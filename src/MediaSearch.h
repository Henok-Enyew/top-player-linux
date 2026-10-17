#pragma once

#include "MediaDownloader.h"

#include <QDateTime>
#include <QList>
#include <QMap>
#include <QObject>
#include <QPointer>
#include <QProcess>
#include <QString>
#include <QStringList>
#include <QUrl>

class QNetworkAccessManager;
class QNetworkReply;

// Finds songs and videos by name: on YouTube through yt-dlp's "ytsearch",
// and on Spotify through its Web API, which needs the user's own app
// credentials (Client ID and Client Secret, Client Credentials flow).
class MediaSearch : public QObject
{
    Q_OBJECT

public:
    enum class Source { YouTube, Spotify };
    enum class Kind { Video, Track, Album, Playlist };
    enum class Error { None, Credentials, RateLimited, Network, Failed };

    // A video, song, album or playlist found.
    struct Result {
        Kind kind = Kind::Video;
        QString id;
        QString title;
        QString artists;      // channel (YouTube), artists or playlist owner (Spotify)
        QString album;        // Spotify songs
        double duration = 0;  // seconds; 0 if unknown
        qint64 views = -1;    // YouTube; -1 if unknown
        int trackCount = 0;   // Spotify albums and playlists
        QString released;     // Spotify albums: the year
        bool live = false;    // YouTube live streams
        QUrl thumbnail;       // cover art or video thumbnail
        QString url;          // the video's or the Spotify item's web page

        // "Artist - Title", or the title alone.
        QString displayName() const;
        // A Spotify song, as the downloader takes it.
        MediaDownloader::SpotifyTrack track() const;
    };

    // One answer to a search.
    struct Page {
        Source source = Source::YouTube;
        QString query;
        QList<Result> results;
        // Spotify: how many results there are in all, per kind searched.
        QMap<Kind, int> totals;
        int offset = 0;
        Error error = Error::None;
        QString errorText;
        int retryAfter = 0; // seconds, for Error::RateLimited
    };

    explicit MediaSearch(QObject *parent = nullptr);
    ~MediaSearch() override;

    // yt-dlp arguments that list the first `count` YouTube results for `query`.
    static QStringList youtubeArguments(const QString &query, int count);
    // The videos in yt-dlp's --flat-playlist --dump-single-json output of a
    // "ytsearchN:" search; thumbnails come from `thumbnailBase`.
    static QList<Result> parseYouTubeSearch(const QByteArray &json,
                                            const QUrl &thumbnailBase = QUrl(QStringLiteral("https://i.ytimg.com")));
    // The songs, albums and playlists in a Spotify /v1/search answer;
    // `totals` gets how many of each there are in all.
    static QList<Result> parseSpotifySearch(const QByteArray &json, QMap<Kind, int> *totals = nullptr);
    // The access token in an accounts.spotify.com /api/token answer, and
    // how many seconds it lasts.
    static QString parseSpotifyToken(const QByteArray &json, int *expiresIn = nullptr);
    // "3:25", "1:02:03"; empty for 0.
    static QString formatDuration(double seconds);
    // "950 views", "12K views", "1.2M views"; empty if unknown.
    static QString formatViews(qint64 views);

    // Lists the first `count` YouTube results for `query`; finished() follows.
    // Returns false if yt-dlp is missing.
    bool searchYouTube(const QString &query, int count);

    void setSpotifyCredentials(const QString &clientId, const QString &clientSecret);
    bool hasSpotifyCredentials() const { return !m_clientId.isEmpty() && !m_clientSecret.isEmpty(); }
    // Where tokens and searches go (https://accounts.spotify.com and
    // https://api.spotify.com); for tests.
    void setSpotifyAccountsUrl(const QUrl &url) { m_accountsBase = url; }
    void setSpotifyApiUrl(const QUrl &url) { m_apiBase = url; }
    // Where YouTube thumbnails are loaded from (https://i.ytimg.com); for tests.
    void setYouTubeThumbnailUrl(const QUrl &url) { m_thumbnailBase = url; }
    // Searches Spotify for `kinds` (songs, albums, playlists), results
    // `offset` onwards; finished() follows.
    void searchSpotify(const QString &query, const QList<Kind> &kinds, int offset = 0);
    // Results asked for per kind and page.
    int spotifyPageSize() const { return m_spotifyLimit; }

    bool isBusy() const;
    // Stops the search; finished() is not emitted.
    void cancel();

Q_SIGNALS:
    void finished(const MediaSearch::Page &page);

private:
    struct SpotifyRequest {
        QString query;
        QList<Kind> kinds;
        int offset = 0;
    };
    void requestToken();
    void onToken(QNetworkReply *reply);
    void sendSpotifySearch();
    void onSpotifySearch(QNetworkReply *reply);
    void finishSpotify(Error error, const QString &text, int retryAfter = 0);
    // A friendly message for a failed Spotify reply.
    QString spotifyErrorText(QNetworkReply *reply, const QByteArray &body, Error *error, int *retryAfter) const;

    QPointer<QProcess> m_process;
    QString m_youtubeQuery;

    QNetworkAccessManager *m_network = nullptr;
    QPointer<QNetworkReply> m_reply;
    QUrl m_accountsBase;
    QUrl m_apiBase;
    QUrl m_thumbnailBase;
    QString m_clientId;
    QString m_clientSecret;
    QString m_token;
    QDateTime m_tokenExpiry;
    SpotifyRequest m_request;
    bool m_spotifyBusy = false;
    // A rejected token is renewed once per search.
    bool m_retried = false;
    int m_spotifyLimit;
};

Q_DECLARE_METATYPE(MediaSearch::Result)
Q_DECLARE_METATYPE(MediaSearch::Page)
