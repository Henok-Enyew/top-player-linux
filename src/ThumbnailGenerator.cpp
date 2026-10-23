#include "ThumbnailGenerator.h"
#include "MpvHelpers.h"

#include <QMetaObject>

#include <mpv/client.h>
#include <mpv/render.h>

#include <algorithm>
#include <cmath>
#include <utility>

namespace {

constexpr int kThumbnailWidth = 256;
// Previews kept for the file, by size: about a hundred 16:9 frames.
constexpr int kCacheKiB = 16 * 1024;
constexpr int kWatchdogMs = 3000;
// The decoder (a whole second player, with its threads and buffers) is shut
// down this long after the last preview, and started again on the next.
constexpr int kIdleReleaseMs = 20000;

} // namespace

ThumbnailGenerator::ThumbnailGenerator(QObject *parent)
    : QObject(parent)
    , m_cache(kCacheKiB)
{
    m_watchdog.setSingleShot(true);
    m_watchdog.setInterval(kWatchdogMs);
    connect(&m_watchdog, &QTimer::timeout, this, [this] { finishCurrent(false); });
    m_idleTimer.setSingleShot(true);
    m_idleTimer.setInterval(kIdleReleaseMs);
    connect(&m_idleTimer, &QTimer::timeout, this, [this] {
        if (m_inFlight >= 0)
            m_idleTimer.start();
        else
            releaseMpv();
    });
}

ThumbnailGenerator::~ThumbnailGenerator()
{
    // Here and now, not in a thread that could still run as the process exits.
    if (!m_mpv)
        return;
    mpv_set_wakeup_callback(m_mpv, nullptr, nullptr);
    mpv_render_context_free(m_renderCtx);
    mpv_terminate_destroy(m_mpv);
}

bool ThumbnailGenerator::ensureMpv()
{
    if (m_mpv)
        return true;
    if (m_broken)
        return false;
    m_mpv = mpv_create();
    if (!m_mpv) {
        m_broken = true;
        return false;
    }

    // Decode as cheaply as possible: no audio, subtitles, scripts or caching,
    // and reduced-quality decoding. Seeks are exact so the preview matches the
    // hovered time rather than the nearest keyframe.
    const char *options[][2] = {
        {"vo", "libmpv"},
        {"ao", "null"},
        {"aid", "no"},
        {"sid", "no"},
        {"pause", "yes"},
        {"keep-open", "always"},
        {"hwdec", "no"},
        {"hr-seek-framedrop", "yes"},
        {"vd-lavc-fast", "yes"},
        {"vd-lavc-skiploopfilter", "all"},
        {"vd-lavc-threads", "2"},
        {"cache", "no"},
        {"demuxer-max-bytes", "8MiB"},
        {"demuxer-max-back-bytes", "0"},
        {"sub-auto", "no"},
        {"audio-file-auto", "no"},
        {"load-scripts", "no"},
        {"ytdl", "no"},
        {"config", "no"},
        {"terminal", "no"},
    };
    for (const auto &option : options)
        mpv_set_option_string(m_mpv, option[0], option[1]);

    mpv_render_param params[]{
        {MPV_RENDER_PARAM_API_TYPE, const_cast<char *>(MPV_RENDER_API_TYPE_SW)},
        {MPV_RENDER_PARAM_INVALID, nullptr},
    };
    if (mpv_initialize(m_mpv) < 0 || mpv_render_context_create(&m_renderCtx, m_mpv, params) < 0) {
        mpv_terminate_destroy(m_mpv);
        m_mpv = nullptr;
        m_renderCtx = nullptr;
        m_broken = true;
        return false;
    }
    mpv_render_context_set_update_callback(m_renderCtx, &ThumbnailGenerator::onRenderUpdateCallback, this);
    mpv_set_wakeup_callback(m_mpv, &ThumbnailGenerator::onWakeup, this);
    return true;
}

void ThumbnailGenerator::releaseMpv()
{
    m_watchdog.stop();
    m_idleTimer.stop();
    destroyMpvDetached(std::exchange(m_mpv, nullptr), std::exchange(m_renderCtx, nullptr));
    // The file opens again on the next request; the previews made stay.
    m_opened = false;
    m_loaded = false;
    m_inFlight = -1;
}

void ThumbnailGenerator::setFile(const QString &path)
{
    if (path == m_path)
        return;
    m_cache.clear();
    m_loaded = false;
    m_inFlight = -1;
    m_pending = -1;
    m_watchdog.stop();
    m_path = path;
    // Close the previous file; the new one opens on the first hover.
    if (std::exchange(m_opened, false) && m_mpv)
        mpvCommandAsync(m_mpv, {QStringLiteral("stop")});
    // Nothing to preview: the decoder can go now.
    if (m_path.isEmpty())
        releaseMpv();
}

void ThumbnailGenerator::request(double seconds)
{
    if (!isAvailable())
        return;
    const int second = std::max(0, static_cast<int>(seconds));
    if (const QImage *cached = m_cache.object(second)) {
        Q_EMIT thumbnailReady(second, *cached);
        return;
    }
    if (!ensureMpv())
        return;
    m_idleTimer.start();
    if (!std::exchange(m_opened, true))
        mpvCommandAsync(m_mpv, {QStringLiteral("loadfile"), m_path});
    m_pending = second;
    startNext();
}

void ThumbnailGenerator::startNext()
{
    while (m_loaded && m_inFlight < 0 && m_pending >= 0) {
        const int second = std::exchange(m_pending, -1);
        if (const QImage *cached = m_cache.object(second)) {
            Q_EMIT thumbnailReady(second, *cached);
            continue;
        }
        m_inFlight = second;
        m_watchdog.start();
        mpvCommandAsync(m_mpv, {QStringLiteral("seek"), QString::number(second), QStringLiteral("absolute+exact")});
    }
}

void ThumbnailGenerator::finishCurrent(bool render)
{
    m_watchdog.stop();
    const int second = std::exchange(m_inFlight, -1);
    if (render && second >= 0) {
        QImage image(m_size, QImage::Format_RGBX8888);
        int size[2] = {image.width(), image.height()};
        size_t stride = static_cast<size_t>(image.bytesPerLine());
        mpv_render_param params[]{
            {MPV_RENDER_PARAM_SW_SIZE, size},
            {MPV_RENDER_PARAM_SW_FORMAT, const_cast<char *>("rgb0")},
            {MPV_RENDER_PARAM_SW_STRIDE, &stride},
            {MPV_RENDER_PARAM_SW_POINTER, image.bits()},
            {MPV_RENDER_PARAM_INVALID, nullptr},
        };
        if (mpv_render_context_render(m_renderCtx, params) >= 0) {
            // mpv leaves the padding byte at 0; RGB32 makes it opaque so the frame
            // is not blended away when composited over the video surface.
            image.convertTo(QImage::Format_RGB32);
            m_cache.insert(second, new QImage(image), static_cast<int>(image.sizeInBytes() / 1024));
            Q_EMIT thumbnailReady(second, image);
        }
    }
    startNext();
}

void ThumbnailGenerator::processEvents()
{
    while (m_mpv) {
        mpv_event *event = mpv_wait_event(m_mpv, 0);
        if (event->event_id == MPV_EVENT_NONE)
            break;

        switch (event->event_id) {
        case MPV_EVENT_START_FILE:
            m_loaded = false;
            break;
        case MPV_EVENT_FILE_LOADED:
            m_loaded = true;
            startNext();
            break;
        case MPV_EVENT_VIDEO_RECONFIG: {
            int64_t w = 0;
            int64_t h = 0;
            mpv_get_property(m_mpv, "dwidth", MPV_FORMAT_INT64, &w);
            mpv_get_property(m_mpv, "dheight", MPV_FORMAT_INT64, &h);
            if (w > 0 && h > 0) {
                const int height = std::clamp(static_cast<int>(std::lround(kThumbnailWidth * double(h) / double(w))), 32, kThumbnailWidth);
                // The decoder restarting on the same file keeps the previews made.
                if (const QSize size(kThumbnailWidth, height & ~1); size != m_size) {
                    m_size = size;
                    m_cache.clear();
                }
            }
            break;
        }
        case MPV_EVENT_END_FILE:
            m_loaded = false;
            break;
        case MPV_EVENT_PLAYBACK_RESTART:
            // The seek finished and the target frame is ready to render.
            if (m_inFlight >= 0)
                finishCurrent(true);
            break;
        default:
            break;
        }
    }
}

void ThumbnailGenerator::onRenderUpdate()
{
    // Required by the render API; frames are rendered on demand in finishCurrent().
    if (m_renderCtx)
        mpv_render_context_update(m_renderCtx);
}

void ThumbnailGenerator::onWakeup(void *ctx)
{
    QMetaObject::invokeMethod(static_cast<ThumbnailGenerator *>(ctx), "processEvents", Qt::QueuedConnection);
}

void ThumbnailGenerator::onRenderUpdateCallback(void *ctx)
{
    QMetaObject::invokeMethod(static_cast<ThumbnailGenerator *>(ctx), "onRenderUpdate", Qt::QueuedConnection);
}
