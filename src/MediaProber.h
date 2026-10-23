#pragma once

#include <QHash>
#include <QObject>
#include <QSet>
#include <QStringList>
#include <QTimer>

struct mpv_handle;

// Reads the durations of local media files, one at a time, with a second,
// headless libmpv instance that opens each file without decoding it. The
// instance runs only while there is something to probe.
class MediaProber : public QObject
{
    Q_OBJECT

public:
    explicit MediaProber(QObject *parent = nullptr);
    ~MediaProber() override;

    // Queues the local files among `entries` (paths or URLs) that were not probed yet.
    void probe(const QStringList &entries);
    // Records a duration known from elsewhere (e.g. the saved session).
    void setDuration(const QString &entry, double seconds);
    // Seconds, or a negative value if unknown (not probed yet, or unreadable).
    double duration(const QString &entry) const;
    bool hasResult(const QString &entry) const { return m_durations.contains(entry); }
    bool isBusy() const { return !m_current.isEmpty() || !m_queue.isEmpty(); }

Q_SIGNALS:
    void durationKnown(const QString &entry, double seconds);
    // The queue ran empty.
    void finished();

private Q_SLOTS:
    void processEvents();

private:
    static void onWakeup(void *ctx);
    // Starts the prober if it isn't running; false if libmpv can't.
    bool ensureMpv();
    void startNext();
    void finishCurrent(double seconds);

    mpv_handle *m_mpv = nullptr;
    bool m_broken = false;
    QHash<QString, double> m_durations; // includes failures, as -1
    QStringList m_queue;
    // Entries in m_queue, for constant-time lookups in long playlists.
    QSet<QString> m_queued;
    QString m_current;
    QTimer m_watchdog;
    QTimer m_idleTimer;
};
