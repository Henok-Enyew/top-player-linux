#pragma once

#include <QCache>
#include <QImage>
#include <QObject>
#include <QTimer>

struct mpv_handle;
struct mpv_render_context;

// Generates seek preview thumbnails with a second, headless libmpv instance
// that decodes the same file and renders through mpv's software render API.
// The instance is started on the first request and shut down again after a
// while without one, so it costs nothing while previews are not used.
class ThumbnailGenerator : public QObject
{
    Q_OBJECT

public:
    explicit ThumbnailGenerator(QObject *parent = nullptr);
    ~ThumbnailGenerator() override;

    // Sets the file previews come from; an empty path disables them. Nothing
    // is opened or decoded until the first request().
    void setFile(const QString &path);
    bool isAvailable() const { return !m_broken && !m_path.isEmpty(); }
    // True while the decoder is running (between a request and the idle shutdown).
    bool isRunning() const { return m_mpv; }
    // True once the file has been opened for a request.
    bool isOpen() const { return m_opened; }

    // Requests the frame near `seconds`, opening the file on the first request.
    // thumbnailReady() fires when it is available, immediately if cached. Only
    // the latest request is kept.
    void request(double seconds);

Q_SIGNALS:
    void thumbnailReady(int second, const QImage &image);

private Q_SLOTS:
    void processEvents();
    void onRenderUpdate();

private:
    static void onWakeup(void *ctx);
    static void onRenderUpdateCallback(void *ctx);

    // Starts the decoder if it isn't running; false if libmpv can't.
    bool ensureMpv();
    void releaseMpv();
    void startNext();
    void finishCurrent(bool render);

    mpv_handle *m_mpv = nullptr;
    mpv_render_context *m_renderCtx = nullptr;
    // libmpv failed to start: previews are off for the rest of the run.
    bool m_broken = false;
    QCache<int, QImage> m_cache; // costs in KiB
    QSize m_size{256, 144};
    QString m_path;
    // The file was sent to mpv; it is opened at most once per setFile().
    bool m_opened = false;
    bool m_loaded = false;
    int m_inFlight = -1;
    int m_pending = -1;
    QTimer m_watchdog;
    QTimer m_idleTimer;
};
