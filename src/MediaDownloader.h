#pragma once

#include <QList>
#include <QObject>
#include <QPointer>
#include <QProcess>
#include <QString>
#include <QStringList>
#include <QUrl>

#include <optional>

// Downloads videos and audio from YouTube, TikTok, Instagram and the other
// sites yt-dlp supports, by running the yt-dlp command-line tool.
//
// Spotify links (tracks, albums, playlists) can't be fetched directly: the
// tracks are read from Spotify's public embed page, each is matched on
// YouTube by artist, title and length, and saved as an MP3 tagged with the
// Spotify title, artist and album.
class QNetworkAccessManager;
class QNetworkReply;

class MediaDownloader : public QObject
{
    Q_OBJECT

public:
    enum class Format { Best, Max2160, Max1080, Max720, AudioMp3 };

    struct Progress {
        double percent = -1; // 0..100
        QString total;       // "10.03MiB"
        QString speed;       // "2.04MiB/s"
        QString eta;         // "00:03"
    };

    // A song named by a Spotify link.
    struct SpotifyTrack {
        QString title;
        QString artist;
        QString album;
        double duration = 0; // seconds; 0 if unknown
    };
    // What a Spotify link points at: "track", "album", "playlist", ... and its id.
    struct SpotifyLink {
        QString type;
        QString id;
    };
    // A video of a playlist read with yt-dlp.
    struct PlaylistEntry {
        QString url;
        QString title;
        double duration = 0; // seconds; 0 if unknown
    };

    explicit MediaDownloader(QObject *parent = nullptr);
    ~MediaDownloader() override;

    // The yt-dlp found on $PATH, or empty.
    static QString executable();
    // yt-dlp arguments that save `url` in `format`, named after its title, or
    // `name` (plus the extension) if given.
    static QStringList arguments(const QString &url, Format format, const QString &directory,
                                 const QString &name = QString());
    // mpv's ytdl-format for streaming in `format`.
    static QString streamFormat(Format format);

    // Cleans pasted text into a URL ("youtu.be/x" -> "https://youtu.be/x");
    // empty if it isn't one.
    static QString normalizeUrl(const QString &text);
    // "YouTube", "TikTok", ... for the social and video sites the dialog knows,
    // else empty (yt-dlp may still support the URL).
    static QString platformName(const QString &url);
    static bool isKnownSite(const QString &url) { return !platformName(url).isEmpty(); }
    // Parses a "[download]  45.3% of 10.00MiB at 1.2MiB/s ETA 00:05" line.
    static std::optional<Progress> parseProgress(const QString &line);

    // Spotify: open.spotify.com, spotify.link and "spotify:track:..." links.
    static bool isSpotifyUrl(const QString &url);
    // The type and id in an open.spotify.com link (with or without /intl-xx/).
    static std::optional<SpotifyLink> parseSpotifyLink(const QString &url);
    // The tracks on a Spotify embed or web page: its __NEXT_DATA__ JSON, else
    // its og: tags. `collection` gets the album or playlist name, if any.
    static QList<SpotifyTrack> parseSpotifyPage(const QByteArray &html, QString *collection = nullptr);
    // "Artist - Title", as searched for on YouTube.
    static QString spotifyQuery(const SpotifyTrack &track);
    // yt-dlp arguments that find `track` on YouTube and save it as a tagged
    // MP3 named "Artist - Title.mp3". `strict` searches a few results and
    // takes the first whose length is within a few seconds of the track's.
    static QStringList spotifyArguments(const SpotifyTrack &track, const QString &directory, bool strict);
    // What mpv plays to stream `track`: a YouTube search through yt-dlp.
    static QString spotifyStreamUrl(const SpotifyTrack &track);
    // What yt-dlp fetches for a playlist entry: a web page as it is, a
    // "ytdl://" entry (a Spotify song's YouTube search) without the prefix;
    // empty for local files and other things that can't be downloaded.
    static QString downloadSource(const QString &entry);
    // A YouTube playlist: ".../playlist?list=...", or a video in one ("&list=").
    static bool isPlaylistUrl(const QString &url);
    // A link to the playlist alone, not to a video in it.
    static bool isPlaylistOnlyUrl(const QString &url);
    // The videos in yt-dlp's --flat-playlist --dump-single-json output (a
    // single video gives one); `title` gets the playlist's name.
    static QList<PlaylistEntry> parsePlaylistJson(const QByteArray &json, QString *title = nullptr);
    // yt-dlp arguments that save every video of a playlist, in order, into a
    // folder named after the playlist.
    static QStringList playlistArguments(const QString &url, Format format, const QString &directory);
    // Reads the videos of a playlist link; playlistResolved() follows.
    bool resolvePlaylist(const QString &url);
    bool isResolving() const;
    // Downloads a playlist entry (see downloadSource()). A Spotify song's
    // search ("ytdl://ytsearch1:Artist - Title") saved as audio becomes a
    // tagged "Artist - Title.mp3", as Spotify downloads are.
    bool startEntry(const QString &entry, const QString &title, Format format, const QString &directory);
    // Downloads every video of a playlist link (files() lists them).
    bool startPlaylist(const QString &url, Format format, const QString &directory);

    // Where Spotify pages are read from (https://open.spotify.com); for tests.
    void setSpotifyBaseUrl(const QUrl &url) { m_spotifyBase = url; }
    // Reads the tracks of a Spotify link; spotifyResolved() follows.
    void resolveSpotify(const QString &url);
    // The files the last download saved (several for a Spotify album or playlist).
    QStringList files() const { return m_files; }

    bool start(const QString &url, Format format, const QString &directory);
    // Stops yt-dlp; finished() is not emitted. Partial ".part" files stay
    // for yt-dlp to resume next time.
    void cancel();
    bool isRunning() const;

Q_SIGNALS:
    void progress(const MediaDownloader::Progress &progress);
    // What yt-dlp is doing: "Downloading", "Merging formats", "Extracting audio".
    void stageChanged(const QString &stage);
    // `path` is the downloaded file (the first one, for a Spotify album or
    // playlist: see files()); `error` is empty on success.
    void finished(bool ok, const QString &path, const QString &error);
    // The tracks of a Spotify link (resolveSpotify(), or a download starting);
    // empty with an error if the link couldn't be read.
    void spotifyResolved(const QList<MediaDownloader::SpotifyTrack> &tracks, const QString &collection,
                         const QString &error);
    // The videos of a playlist link (resolvePlaylist()); empty with an error
    // if it couldn't be read.
    void playlistResolved(const QList<MediaDownloader::PlaylistEntry> &entries, const QString &title,
                          const QString &error);

private:
    bool launch(const QStringList &args);
    void fetchSpotify(const QUrl &url, int redirectsLeft);
    void onSpotifyPage(QNetworkReply *reply, int redirectsLeft);
    void startSpotifyTrack();
    void finishSpotify();
    void readLines(QByteArray &buffer, const QByteArray &data);
    void handleLine(const QString &line);
    void onFinished(int exitCode, QProcess::ExitStatus status);

    QPointer<QProcess> m_process;
    QPointer<QProcess> m_resolver;
    // A whole playlist being saved: the files it printed, and which item of
    // how many is downloading.
    bool m_playlistDownload = false;
    QStringList m_paths;
    int m_item = 0;
    int m_items = 0;
    QByteArray m_stdout;
    QByteArray m_stderr;
    QString m_path;
    QStringList m_errors;
    QStringList m_files;

    QNetworkAccessManager *m_network = nullptr;
    QPointer<QNetworkReply> m_reply;
    QUrl m_spotifyBase;
    // A Spotify download: the tracks, the one being fetched, whether this is
    // its first (length-checked) attempt, and the tracks that failed.
    bool m_spotify = false;
    bool m_spotifyDownload = false;
    QString m_directory;
    QList<SpotifyTrack> m_tracks;
    int m_track = -1;
    bool m_strict = true;
    QStringList m_failed;
};

Q_DECLARE_METATYPE(MediaDownloader::SpotifyTrack)
Q_DECLARE_METATYPE(MediaDownloader::PlaylistEntry)
