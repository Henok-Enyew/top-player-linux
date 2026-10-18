#pragma once

#include "MediaDownloader.h"
#include "MediaSearch.h"

#include <QDialog>
#include <QHash>
#include <QMap>
#include <QPixmap>
#include <QPointer>
#include <QSet>

class DownloadQueue;
class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QNetworkAccessManager;
class QNetworkReply;
class QProgressBar;
class QPushButton;
class QTabBar;
class QToolButton;
class QTreeWidget;
class QTreeWidgetItem;

// Search YouTube... / Search Spotify...: finds videos and songs by name, then
// plays them, adds them to the playlist, or downloads them one after another
// in the background (videos in a chosen quality, songs as MP3s; Spotify songs
// are found on YouTube and tagged, like Spotify links in Download from URL).
class MediaSearchDialog : public QDialog
{
    Q_OBJECT

public:
    explicit MediaSearchDialog(MediaSearch::Source source, QWidget *parent = nullptr);
    ~MediaSearchDialog() override;

    MediaSearch::Source source() const { return m_source; }
    MediaSearch *search() const { return m_search; }
    DownloadQueue *downloads() const { return m_queue; }
    QTreeWidget *view() const { return m_view; }

    // Songs, albums or playlists (Spotify), videos (YouTube).
    MediaSearch::Kind currentKind() const;
    void setCurrentKind(MediaSearch::Kind kind);
    // The results listed, and the selected ones.
    QList<MediaSearch::Result> results() const { return m_results.value(currentKind()); }
    QList<MediaSearch::Result> selectedResults() const;
    // "Queued", "45%", "Saved" or "Failed" for a result being downloaded.
    QString status(const MediaSearch::Result &result) const { return m_status.value(result.url); }

    QString directory() const;
    bool playWhenDone() const;
    // The quality "Download Video" and streaming use (YouTube).
    MediaDownloader::Format videoFormat() const;

public Q_SLOTS:
    void setQuery(const QString &query);
    void startSearch();
    void loadMore();
    // Streams the selected results, replacing the playlist.
    void playSelected();
    // Adds the selected results to the end of the playlist.
    void queueSelected();
    void downloadSelected(MediaDownloader::Format format);
    void cancelDownloads();

Q_SIGNALS:
    // A download finished; `play` is "start playing it" (the first one
    // saved while "Play immediately" is ticked), else it is queued.
    void downloaded(const QString &path, bool play);
    // A Spotify album or playlist finished: every song saved, in order.
    void downloadedMany(const QStringList &paths, bool play);
    // Stream these (titled `titles`), replacing the playlist, picking
    // streams with yt-dlp format `format`.
    void tracksStreamRequested(const QStringList &urls, const QStringList &titles, const QString &format);
    // Append these streams to the playlist, without interrupting playback.
    void tracksQueueRequested(const QStringList &urls, const QStringList &titles, const QString &format);

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;
    void reject() override;
    void hideEvent(QHideEvent *event) override;

private:
    void onResults(const MediaSearch::Page &page);
    void showResults();
    void addItems(const QList<MediaSearch::Result> &results);
    void updateHeader();
    void updateState();
    void updateProgress();
    void setStatus(const QString &text, bool error = false);
    void saveSettings();
    void saveCredentials();
    void setAccessPanelOpen(bool open);
    // Plays or queues `results`; Spotify albums and playlists have their
    // songs read first.
    void stream(const QList<MediaSearch::Result> &results, bool append);
    void resolveNext();
    void onStatusChanged(const QString &entry, const QString &status);
    void onDownloadFinished(const QString &entry, bool ok, const QStringList &files, const QString &error);
    // Cover art and thumbnails, loaded once and kept in memory.
    void requestArt(const QUrl &url);
    void cancelArt();
    QPixmap artFor(const QUrl &url) const { return m_art.value(url.toString()); }

    MediaSearch::Source m_source;
    MediaSearch *m_search;
    DownloadQueue *m_queue;
    MediaDownloader *m_resolver;
    QLabel *m_warning;
    QToolButton *m_accessToggle = nullptr;
    QWidget *m_accessPanel = nullptr;
    QLineEdit *m_clientId = nullptr;
    QLineEdit *m_clientSecret = nullptr;
    QLineEdit *m_query;
    QPushButton *m_searchButton;
    QTabBar *m_tabs = nullptr;
    QTreeWidget *m_view;
    QPushButton *m_moreButton;
    QComboBox *m_format = nullptr;
    QLineEdit *m_directory;
    QCheckBox *m_play;
    QProgressBar *m_progress;
    QLabel *m_statusLabel;
    QPushButton *m_cancelButton;
    QPushButton *m_playButton;
    QPushButton *m_queueButton;
    QPushButton *m_videoButton = nullptr;
    QPushButton *m_mp3Button;

    // The last search, its results per kind and whether more can be loaded.
    QString m_lastQuery;
    QMap<MediaSearch::Kind, QList<MediaSearch::Result>> m_results;
    QMap<MediaSearch::Kind, int> m_totals;
    QMap<MediaSearch::Kind, int> m_nextOffset;
    int m_youtubeCount = 0;
    bool m_youtubeMore = false;
    bool m_loadingMore = false;

    // Downloads: each result's state and error, the ones of the current
    // batch (for the overall progress), and whether one has started playing.
    QHash<QString, QString> m_status;
    QHash<QString, QString> m_errors;
    QSet<QString> m_batch;
    int m_saved = 0;
    int m_failed = 0;
    bool m_playedInBatch = false;

    // Albums and playlists whose songs are being read to play or queue them.
    QList<MediaSearch::Result> m_toResolve;
    QStringList m_resolvedUrls;
    QStringList m_resolvedTitles;
    bool m_resolveAppend = false;

    QNetworkAccessManager *m_network;
    QHash<QString, QPixmap> m_art;
    QHash<QString, QPointer<QNetworkReply>> m_artReplies;
};
