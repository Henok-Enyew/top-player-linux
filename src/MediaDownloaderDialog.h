#pragma once

#include "MediaDownloader.h"

#include <QDialog>

class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QProgressBar;
class QPushButton;

// Download from URL...: fetches a video or its audio with yt-dlp, or hands
// the URL to mpv to stream. Spotify links are saved (or streamed) as songs
// matched on YouTube.
class MediaDownloaderDialog : public QDialog
{
    Q_OBJECT

public:
    explicit MediaDownloaderDialog(QWidget *parent = nullptr);

    MediaDownloader *downloader() const { return m_downloader; }
    QString url() const;
    MediaDownloader::Format format() const;
    QString directory() const;
    bool playWhenDone() const;
    bool isBusy() const { return m_downloader->isRunning() || m_resolving; }
    bool isSpotify() const { return MediaDownloader::isSpotifyUrl(url()); }

    // ~/Videos, else ~/Downloads, else the home folder; or the last one used.
    static QString defaultDirectory();

public Q_SLOTS:
    void startDownload();
    void streamDirectly();

Q_SIGNALS:
    // A download finished; `play` is the "Play immediately" choice.
    void downloaded(const QString &path, bool play);
    // A Spotify album or playlist finished: every song saved, in order.
    void downloadedMany(const QStringList &paths, bool play);
    // Stream these (Spotify songs found through YouTube), titled `titles`.
    void tracksStreamRequested(const QStringList &urls, const QStringList &titles);
    // Stream `url` in mpv, picking streams with yt-dlp format `format`.
    void streamRequested(const QString &url, const QString &format);

protected:
    void reject() override;

private:
    void updateState();
    void setStatus(const QString &text, bool error = false);
    void setBusy(bool busy);

    MediaDownloader *m_downloader;
    QLineEdit *m_url;
    QLabel *m_site;
    QComboBox *m_format;
    QLineEdit *m_directory;
    QCheckBox *m_play;
    QProgressBar *m_progress;
    QLabel *m_speed;
    QLabel *m_status;
    QLabel *m_warning;
    QPushButton *m_downloadButton;
    QPushButton *m_streamButton;
    // Reading a Spotify link's songs before streaming them.
    bool m_resolving = false;
    // The format chosen before a Spotify link switched it to MP3.
    int m_formatBeforeSpotify = -1;
};
