#include "ArtworkExtractor.h"
#include "MpvHelpers.h"

#include <QMetaObject>

#include <mpv/client.h>
#include <mpv/render.h>

#include <algorithm>
#include <cmath>
#include <utility>

namespace {

// Covers are scaled down to fit this size; the view never shows them larger.
constexpr int kMaxSide = 1024;
constexpr int kCacheEntries = 16;
constexpr int kWatchdogMs = 5000;

} // namespace

ArtworkExtractor::ArtworkExtractor(QObject *parent)
    : QObject(parent)
    , m_cache(kCacheEntries)
{
    m_mpv = mpv_create();
    if (!m_mpv)
        return;

    // Only the embedded cover is decoded: no audio, and no cover files from
    // the folder (those are looked up separately).
    const char *options[][2] = {
        {"vo", "libmpv"},
        {"ao", "null"},
        {"aid", "no"},
        {"sid", "no"},
        {"audio-display", "embedded-first"},
        {"cover-art-auto", "no"},
        {"pause", "yes"},
        {"idle", "yes"},
        {"hwdec", "no"},
        {"cache", "no"},
        {"sub-auto", "no"},
        {"audio-file-auto", "no"},
        {"load-scripts", "no"},
        {"ytdl", "no"},
        {"config", "no"},
        {"terminal", "no"},
    };
    for (const auto &option : options)
        mpv_set_option_string(m_mpv, option[0], option[1]);

    if (mpv_initialize(m_mpv) < 0) {
        mpv_terminate_destroy(m_mpv);
        m_mpv = nullptr;
        return;
    }
    mpv_render_param params[]{
        {MPV_RENDER_PARAM_API_TYPE, const_cast<char *>(MPV_RENDER_API_TYPE_SW)},
        {MPV_RENDER_PARAM_INVALID, nullptr},
    };
    if (mpv_render_context_create(&m_renderCtx, m_mpv, params) < 0) {
        mpv_terminate_destroy(m_mpv);
        m_mpv = nullptr;
        return;
    }
    mpv_render_context_set_update_callback(m_renderCtx, &ArtworkExtractor::onRenderUpdateCallback, this);
    mpv_set_wakeup_callback(m_mpv, &ArtworkExtractor::onWakeup, this);

    m_watchdog.setSingleShot(true);
    m_watchdog.setInterval(kWatchdogMs);
    connect(&m_watchdog, &QTimer::timeout, this, [this] { finish({}); });
}

ArtworkExtractor::~ArtworkExtractor()
{
    if (!m_mpv)
        return;
    mpv_set_wakeup_callback(m_mpv, nullptr, nullptr);
    mpv_render_context_free(m_renderCtx);
    mpv_terminate_destroy(m_mpv);
}

void ArtworkExtractor::request(const QString &path)
{
    if (const QImage *cached = m_cache.object(path)) {
        // Emitted later, like a fresh result, so callers see one behavior.
        const QImage image = *cached;
        QMetaObject::invokeMethod(this, [this, path, image] { Q_EMIT artworkReady(path, image); }, Qt::QueuedConnection);
        return;
    }
    if (!m_mpv) {
        QMetaObject::invokeMethod(this, [this, path] { Q_EMIT artworkReady(path, {}); }, Qt::QueuedConnection);
        return;
    }
    m_current = path;
    m_hasCover = false;
    m_entry = -1;
    m_watchdog.start();
    mpvCommandAsync(m_mpv, {QStringLiteral("loadfile"), path, QStringLiteral("replace")});
}

void ArtworkExtractor::finish(const QImage &image)
{
    m_watchdog.stop();
    const QString path = std::exchange(m_current, QString());
    if (path.isEmpty())
        return;
    mpvCommandAsync(m_mpv, {QStringLiteral("stop")});
    m_cache.insert(path, new QImage(image));
    Q_EMIT artworkReady(path, image);
}

QImage ArtworkExtractor::render()
{
    int64_t w = 0;
    int64_t h = 0;
    mpv_get_property(m_mpv, "dwidth", MPV_FORMAT_INT64, &w);
    mpv_get_property(m_mpv, "dheight", MPV_FORMAT_INT64, &h);
    if (w <= 0 || h <= 0)
        return {};
    QSize size(static_cast<int>(w), static_cast<int>(h));
    if (size.width() > kMaxSide || size.height() > kMaxSide)
        size.scale(kMaxSide, kMaxSide, Qt::KeepAspectRatio);

    QImage image(size, QImage::Format_RGBX8888);
    int renderSize[2] = {image.width(), image.height()};
    size_t stride = static_cast<size_t>(image.bytesPerLine());
    mpv_render_param params[]{
        {MPV_RENDER_PARAM_SW_SIZE, renderSize},
        {MPV_RENDER_PARAM_SW_FORMAT, const_cast<char *>("rgb0")},
        {MPV_RENDER_PARAM_SW_STRIDE, &stride},
        {MPV_RENDER_PARAM_SW_POINTER, image.bits()},
        {MPV_RENDER_PARAM_INVALID, nullptr},
    };
    if (mpv_render_context_render(m_renderCtx, params) < 0)
        return {};
    // The padding byte is 0; RGB32 makes the image opaque.
    image.convertTo(QImage::Format_RGB32);
    return image;
}

void ArtworkExtractor::processEvents()
{
    while (m_mpv) {
        mpv_event *event = mpv_wait_event(m_mpv, 0);
        if (event->event_id == MPV_EVENT_NONE)
            break;
        if (m_current.isEmpty())
            continue;

        switch (event->event_id) {
        case MPV_EVENT_START_FILE:
            // Requests run in order: the latest file to start is the one asked for.
            m_entry = static_cast<const mpv_event_start_file *>(event->data)->playlist_entry_id;
            break;
        case MPV_EVENT_FILE_LOADED: {
            char *path = mpv_get_property_string(m_mpv, "path");
            const bool isCurrent = path && QString::fromUtf8(path) == m_current;
            mpv_free(path);
            if (!isCurrent)
                break;
            int albumArt = 0;
            mpv_get_property(m_mpv, "current-tracks/video/albumart", MPV_FORMAT_FLAG, &albumArt);
            m_hasCover = albumArt != 0;
            if (!m_hasCover)
                finish({});
            break;
        }
        case MPV_EVENT_PLAYBACK_RESTART:
            // The cover is decoded and ready to render.
            if (m_hasCover)
                finish(render());
            break;
        case MPV_EVENT_END_FILE: {
            // Files without a cover end right away: nothing is selected to play.
            const auto *end = static_cast<mpv_event_end_file *>(event->data);
            if (end->playlist_entry_id != m_entry)
                break;
            if (end->reason == MPV_END_FILE_REASON_ERROR || (end->reason == MPV_END_FILE_REASON_EOF && !m_hasCover))
                finish({});
            break;
        }
        default:
            break;
        }
    }
}

void ArtworkExtractor::onRenderUpdate()
{
    if (m_renderCtx)
        mpv_render_context_update(m_renderCtx);
}

void ArtworkExtractor::onWakeup(void *ctx)
{
    QMetaObject::invokeMethod(static_cast<ArtworkExtractor *>(ctx), "processEvents", Qt::QueuedConnection);
}

void ArtworkExtractor::onRenderUpdateCallback(void *ctx)
{
    QMetaObject::invokeMethod(static_cast<ArtworkExtractor *>(ctx), "onRenderUpdate", Qt::QueuedConnection);
}
