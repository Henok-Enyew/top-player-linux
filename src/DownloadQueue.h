#pragma once

#include "MediaDownloader.h"

#include <QHash>
#include <QList>
#include <QObject>
#include <QString>

#include <algorithm>
#include <optional>

// Saves playlist entries that are streamed from the web (YouTube videos,
// Spotify songs found on YouTube, other pages yt-dlp knows) to the downloads
// folder, one after another with yt-dlp. Each entry's state can be shown
// next to it in the playlist.
class DownloadQueue : public QObject
{
    Q_OBJECT

public:
    explicit DownloadQueue(QObject *parent = nullptr);

    // True if `entry` (a playlist entry's filename) can be downloaded.
    static bool canDownload(const QString &entry);
    // The folder downloads go to: the one last used in Download from URL...
    QString directory() const;
    // Overrides the folder (for tests).
    void setDirectory(const QString &directory) { m_directory = directory; }

    // Queues `entry`, named `title` if given. Returns false if it can't be
    // downloaded, yt-dlp is missing, or it is queued already.
    bool enqueue(const QString &entry, const QString &title, MediaDownloader::Format format);
    // "Queued", "45%", "Saved" or "Failed" for an entry the queue has seen, else empty.
    QString status(const QString &entry) const { return m_status.value(entry); }
    // Entries waiting or downloading.
    int pending() const { return int(m_jobs.size()) + (m_current ? 1 : 0); }
    void cancelAll();

Q_SIGNALS:
    void statusChanged(const QString &entry, const QString &status);
    // `files` are what was saved (an MP3, a video); `error` is empty on success.
    void finished(const QString &entry, bool ok, const QStringList &files, const QString &error);

private:
    struct Job {
        QString entry;
        QString title;
        MediaDownloader::Format format;
    };
    void startNext();
    void setStatus(const QString &entry, const QString &status);

    MediaDownloader *m_downloader;
    QList<Job> m_jobs;
    std::optional<Job> m_current;
    QHash<QString, QString> m_status;
    QString m_directory;
};
