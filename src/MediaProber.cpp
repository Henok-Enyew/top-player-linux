#include "MediaProber.h"
#include "MediaFiles.h"
#include "MpvHelpers.h"

#include <QFileInfo>
#include <QMetaObject>

#include <mpv/client.h>

#include <utility>

namespace {

constexpr int kWatchdogMs = 5000;
// The prober (a second player) is shut down this long after its queue ran
// empty, instead of keeping the last file open for the rest of the run.
constexpr int kIdleReleaseMs = 3000;

} // namespace

MediaProber::MediaProber(QObject *parent)
    : QObject(parent)
{
    m_watchdog.setSingleShot(true);
    m_watchdog.setInterval(kWatchdogMs);
    connect(&m_watchdog, &QTimer::timeout, this, [this] {
        // A file that takes this long may be on a mount that stopped
        // answering, and would hold up every file after it: the next one
        // starts in a fresh prober.
        destroyMpvDetached(std::exchange(m_mpv, nullptr));
        finishCurrent(-1);
    });
    m_idleTimer.setSingleShot(true);
    m_idleTimer.setInterval(kIdleReleaseMs);
    connect(&m_idleTimer, &QTimer::timeout, this, [this] {
        if (!isBusy())
            destroyMpvDetached(std::exchange(m_mpv, nullptr));
    });
}

MediaProber::~MediaProber()
{
    if (!m_mpv)
        return;
    mpv_set_wakeup_callback(m_mpv, nullptr, nullptr);
    mpv_terminate_destroy(m_mpv);
}

bool MediaProber::ensureMpv()
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

    // Open files paused into null outputs: the duration is known once the
    // demuxer has read the headers, and at most a frame is decoded. (With every
    // track disabled, mpv would refuse to load the file.)
    const char *options[][2] = {
        {"vo", "null"},
        {"ao", "null"},
        {"sid", "no"},
        {"audio-display", "no"},
        {"hwdec", "no"},
        {"vd-lavc-threads", "1"},
        {"pause", "yes"},
        {"idle", "yes"},
        {"cache", "no"},
        {"demuxer-max-bytes", "4MiB"},
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

    if (mpv_initialize(m_mpv) < 0) {
        mpv_terminate_destroy(m_mpv);
        m_mpv = nullptr;
        m_broken = true;
        return false;
    }
    mpv_set_wakeup_callback(m_mpv, &MediaProber::onWakeup, this);
    return true;
}

void MediaProber::probe(const QStringList &entries)
{
    if (m_broken)
        return;
    for (const QString &entry : entries) {
        if (m_durations.contains(entry) || entry == m_current || m_queued.contains(entry))
            continue;
        // Streams would mean network traffic; only local media files are probed.
        // Missing files are not checked for here, which would stat every entry
        // of a long playlist on the GUI thread: mpv fails to open them instead.
        if (MediaFiles::localPath(entry).isEmpty())
            continue;
        m_queue.append(entry);
        m_queued.insert(entry);
    }
    startNext();
}

void MediaProber::setDuration(const QString &entry, double seconds)
{
    if (seconds < 0)
        return;
    m_durations.insert(entry, seconds);
    if (m_queued.remove(entry))
        m_queue.removeAll(entry);
}

double MediaProber::duration(const QString &entry) const
{
    return m_durations.value(entry, -1);
}

void MediaProber::startNext()
{
    if (!m_current.isEmpty())
        return;
    if (m_queue.isEmpty()) {
        if (m_mpv)
            m_idleTimer.start();
        Q_EMIT finished();
        return;
    }
    if (!ensureMpv()) {
        m_queue.clear();
        m_queued.clear();
        Q_EMIT finished();
        return;
    }
    m_idleTimer.stop();
    m_current = m_queue.takeFirst();
    m_queued.remove(m_current);
    m_watchdog.start();
    mpvCommandAsync(m_mpv, {QStringLiteral("loadfile"), MediaFiles::localPath(m_current), QStringLiteral("replace")});
}

void MediaProber::finishCurrent(double seconds)
{
    m_watchdog.stop();
    const QString entry = std::exchange(m_current, QString());
    if (entry.isEmpty())
        return;
    m_durations.insert(entry, seconds);
    if (seconds >= 0)
        Q_EMIT durationKnown(entry, seconds);
    startNext();
}

void MediaProber::processEvents()
{
    while (m_mpv) {
        mpv_event *event = mpv_wait_event(m_mpv, 0);
        if (event->event_id == MPV_EVENT_NONE)
            break;

        switch (event->event_id) {
        case MPV_EVENT_FILE_LOADED: {
            // Only the file being probed counts; events of a file the watchdog
            // gave up on may still trickle in.
            char *path = mpv_get_property_string(m_mpv, "path");
            const bool isCurrent = path && !m_current.isEmpty()
                && QString::fromUtf8(path) == MediaFiles::localPath(m_current);
            mpv_free(path);
            if (!isCurrent)
                break;
            double seconds = -1;
            if (mpv_get_property(m_mpv, "duration", MPV_FORMAT_DOUBLE, &seconds) < 0)
                seconds = -1;
            finishCurrent(seconds);
            break;
        }
        case MPV_EVENT_END_FILE: {
            const auto *end = static_cast<mpv_event_end_file *>(event->data);
            // A file that failed to open never reports FILE_LOADED.
            if (end->reason == MPV_END_FILE_REASON_ERROR)
                finishCurrent(-1);
            break;
        }
        default:
            break;
        }
    }
}

void MediaProber::onWakeup(void *ctx)
{
    QMetaObject::invokeMethod(static_cast<MediaProber *>(ctx), "processEvents", Qt::QueuedConnection);
}
