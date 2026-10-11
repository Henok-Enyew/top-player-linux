#pragma once

#include <QCache>
#include <QImage>
#include <QObject>
#include <QTimer>

struct mpv_handle;
struct mpv_render_context;

// Extracts the cover art embedded in audio files (ID3 APIC frames, FLAC and
// Vorbis comment pictures, MP4 covr atoms, ...) with a second, headless
// libmpv instance that decodes only the cover track and renders it through
// mpv's software render API.
class ArtworkExtractor : public QObject
{
    Q_OBJECT

public:
    explicit ArtworkExtractor(QObject *parent = nullptr);
    ~ArtworkExtractor() override;

    // Starts extracting the cover of `path`; artworkReady() follows, with a
    // null image if there is none. Replaces an unfinished request.
    void request(const QString &path);

Q_SIGNALS:
    void artworkReady(const QString &path, const QImage &image);

private Q_SLOTS:
    void processEvents();
    void onRenderUpdate();

private:
    static void onWakeup(void *ctx);
    static void onRenderUpdateCallback(void *ctx);

    void finish(const QImage &image);
    QImage render();

    mpv_handle *m_mpv = nullptr;
    mpv_render_context *m_renderCtx = nullptr;
    QString m_current;
    bool m_hasCover = false;
    // mpv's playlist entry opened for m_current, once it started; -1 before.
    // The end of an earlier file (reported after the next request) must not
    // count as this one having no cover.
    qint64 m_entry = -1;
    QCache<QString, QImage> m_cache;
    QTimer m_watchdog;
};
