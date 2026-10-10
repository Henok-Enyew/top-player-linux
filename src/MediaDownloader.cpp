#include "MediaDownloader.h"

#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QUrlQuery>

#include <algorithm>
#include <utility>

namespace {

struct Site {
    const char *domain; // matches the host and its subdomains
    const char *name;
};

const Site kSites[] = {
    {"youtube.com", "YouTube"},
    {"youtu.be", "YouTube"},
    {"youtube-nocookie.com", "YouTube"},
    {"tiktok.com", "TikTok"},
    {"instagram.com", "Instagram"},
    {"twitter.com", "X (Twitter)"},
    {"x.com", "X (Twitter)"},
    {"facebook.com", "Facebook"},
    {"fb.watch", "Facebook"},
    {"vimeo.com", "Vimeo"},
    {"twitch.tv", "Twitch"},
    {"reddit.com", "Reddit"},
    {"redd.it", "Reddit"},
    {"dailymotion.com", "Dailymotion"},
    {"dai.ly", "Dailymotion"},
    {"soundcloud.com", "SoundCloud"},
    {"bilibili.com", "Bilibili"},
    {"pinterest.com", "Pinterest"},
    {"threads.net", "Threads"},
    {"bsky.app", "Bluesky"},
    {"spotify.com", "Spotify"},
    {"spotify.link", "Spotify"},
    {"spotify.app.link", "Spotify"},
};

// Spotify serves its pages only to browsers.
const char kBrowserAgent[] = "Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 (KHTML, like Gecko) "
                             "Chrome/126.0 Safari/537.36";
constexpr int kSpotifyTimeoutMs = 20000;
// yt-dlp's exit code when --max-downloads was reached: the match was saved.
constexpr int kMaxDownloadsReached = 101;
// A YouTube upload matches a Spotify track if its length is this close.
constexpr int kLengthTolerance = 12;
// Results looked through for one of the right length.
constexpr int kSearchResults = 5;

const QStringList kSpotifyTypes{
    QStringLiteral("track"), QStringLiteral("album"), QStringLiteral("playlist"), QStringLiteral("artist"),
};

QString decodeEntities(QString text)
{
    static const QRegularExpression numeric(QStringLiteral("&#(x?)([0-9a-fA-F]+);"));
    QString out;
    qsizetype last = 0;
    for (auto it = numeric.globalMatch(text); it.hasNext();) {
        const QRegularExpressionMatch m = it.next();
        out += text.mid(last, m.capturedStart() - last);
        bool ok = false;
        const uint code = m.captured(2).toUInt(&ok, m.captured(1).isEmpty() ? 10 : 16);
        out += ok ? QString::fromUcs4(reinterpret_cast<const char32_t *>(&code), 1) : m.captured(0);
        last = m.capturedEnd();
    }
    out += text.mid(last);
    out.replace(QLatin1String("&quot;"), QLatin1String("\""));
    out.replace(QLatin1String("&apos;"), QLatin1String("'"));
    out.replace(QLatin1String("&lt;"), QLatin1String("<"));
    out.replace(QLatin1String("&gt;"), QLatin1String(">"));
    out.replace(QLatin1String("&amp;"), QLatin1String("&"));
    return out;
}

QString metaContent(const QString &html, const QString &property)
{
    const QRegularExpression re(QStringLiteral(R"re(<meta\s+[^>]*(?:property|name)="%1"[^>]*content="([^"]*)")re")
                                    .arg(QRegularExpression::escape(property)));
    const QRegularExpressionMatch m = re.match(html);
    return m.hasMatch() ? decodeEntities(m.captured(1)).trimmed() : QString();
}

// The page's data object for the track, album or playlist: where Spotify keeps
// it, else the first object that looks like one.
QJsonObject findEntity(const QJsonValue &value, int depth = 0)
{
    if (depth > 12)
        return {};
    if (value.isObject()) {
        const QJsonObject object = value.toObject();
        if (object.value(QStringLiteral("trackList")).isArray()
            || (object.value(QStringLiteral("type")).toString() == QLatin1String("track")
                && (object.contains(QStringLiteral("name")) || object.contains(QStringLiteral("title")))))
            return object;
        for (auto it = object.begin(); it != object.end(); ++it) {
            const QJsonObject found = findEntity(it.value(), depth + 1);
            if (!found.isEmpty())
                return found;
        }
    } else if (value.isArray()) {
        for (const QJsonValue &item : value.toArray()) {
            const QJsonObject found = findEntity(item, depth + 1);
            if (!found.isEmpty())
                return found;
        }
    }
    return {};
}

QString joinArtists(const QJsonArray &artists)
{
    QStringList names;
    for (const QJsonValue &artist : artists) {
        const QString name = artist.toObject().value(QStringLiteral("name")).toString().trimmed();
        if (!name.isEmpty())
            names << name;
    }
    return names.join(QStringLiteral(", "));
}

// Spotify's lists separate artists with non-breaking spaces.
QString cleanText(QString text)
{
    text.replace(QChar(0x00A0), QLatin1Char(' '));
    return text.simplified();
}

// A literal for yt-dlp's --parse-metadata FROM side: an output template, so
// '%' is doubled, and ':' ends FROM unless escaped. The trailing " |" keeps
// one-word values from being read as field names.
QString metadataLiteral(const QString &value)
{
    QString literal = value;
    literal.replace(QLatin1Char('%'), QStringLiteral("%%"));
    literal.replace(QLatin1Char(':'), QStringLiteral("\\:"));
    return literal + QStringLiteral(" |");
}

QString fileNameFor(QString name)
{
    static const QRegularExpression unsafe(QStringLiteral(R"([/\\\x00-\x1f])"));
    name.replace(unsafe, QStringLiteral("-"));
    name = name.simplified().left(150);
    if (name.isEmpty() || name.startsWith(QLatin1Char('.')))
        return QStringLiteral("%(title).150B [%(id)s].%(ext)s");
    name.replace(QLatin1Char('%'), QStringLiteral("%%"));
    return name + QStringLiteral(".%(ext)s");
}

QString fileNameFor(const MediaDownloader::SpotifyTrack &track)
{
    return fileNameFor(track.artist.isEmpty() ? track.title : track.artist + QStringLiteral(" - ") + track.title);
}

QString formatSelector(MediaDownloader::Format format)
{
    switch (format) {
    case MediaDownloader::Format::Best:
        return QStringLiteral("bv*+ba/b");
    case MediaDownloader::Format::Max2160:
        return QStringLiteral("bv*[height<=2160]+ba/b[height<=2160]/b");
    case MediaDownloader::Format::Max1080:
        return QStringLiteral("bv*[height<=1080]+ba/b[height<=1080]/b");
    case MediaDownloader::Format::Max720:
        return QStringLiteral("bv*[height<=720]+ba/b[height<=720]/b");
    case MediaDownloader::Format::AudioMp3:
        return QStringLiteral("ba/b");
    }
    return QStringLiteral("bv*+ba/b");
}

// The format arguments shared by single videos and playlists.
QStringList formatArguments(MediaDownloader::Format format)
{
    if (format == MediaDownloader::Format::AudioMp3) {
        return {QStringLiteral("-f"), formatSelector(format), QStringLiteral("-x"),
                QStringLiteral("--audio-format"), QStringLiteral("mp3"),
                QStringLiteral("--audio-quality"), QStringLiteral("0")};
    }
    return {QStringLiteral("-f"), formatSelector(format), QStringLiteral("--merge-output-format"), QStringLiteral("mp4")};
}

} // namespace

MediaDownloader::MediaDownloader(QObject *parent)
    : QObject(parent)
    , m_spotifyBase(QStringLiteral("https://open.spotify.com"))
{
    qRegisterMetaType<MediaDownloader::SpotifyTrack>();
    qRegisterMetaType<QList<MediaDownloader::SpotifyTrack>>();
    qRegisterMetaType<MediaDownloader::PlaylistEntry>();
    qRegisterMetaType<QList<MediaDownloader::PlaylistEntry>>();
}

MediaDownloader::~MediaDownloader()
{
    cancel();
}

QString MediaDownloader::executable()
{
    return QStandardPaths::findExecutable(QStringLiteral("yt-dlp"));
}

QStringList MediaDownloader::arguments(const QString &url, Format format, const QString &directory, const QString &name)
{
    QStringList args{
        // One progress line per update, even though --print makes yt-dlp quiet.
        QStringLiteral("--newline"), QStringLiteral("--progress"),
        QStringLiteral("--no-playlist"), QStringLiteral("--no-mtime"),
        QStringLiteral("-P"), directory,
        QStringLiteral("-o"), name.trimmed().isEmpty() ? QStringLiteral("%(title).150B [%(id)s].%(ext)s") : fileNameFor(name),
        // The final file, after merging or audio extraction.
        QStringLiteral("--print"), QStringLiteral("after_move:filepath"),
    };
    args << formatArguments(format);
    // "--" so that a URL can never be read as an option.
    args << QStringLiteral("--") << url;
    return args;
}

QStringList MediaDownloader::playlistArguments(const QString &url, Format format, const QString &directory)
{
    QStringList args{
        QStringLiteral("--newline"), QStringLiteral("--progress"),
        QStringLiteral("--yes-playlist"), QStringLiteral("--no-mtime"),
        // An unavailable video doesn't stop the rest.
        QStringLiteral("--ignore-errors"),
        QStringLiteral("-P"), directory,
        QStringLiteral("-o"),
        QStringLiteral("%(playlist_title,playlist_id|Playlist).100B/%(playlist_index|0)03d - %(title).150B [%(id)s].%(ext)s"),
        QStringLiteral("--print"), QStringLiteral("after_move:filepath"),
    };
    args << formatArguments(format);
    args << QStringLiteral("--") << url;
    return args;
}

QString MediaDownloader::streamFormat(Format format)
{
    return formatSelector(format);
}

QString MediaDownloader::normalizeUrl(const QString &text)
{
    QString candidate = text.trimmed();
    // "Copy Spotify URI" gives spotify:track:<id>.
    static const QRegularExpression spotifyUri(QStringLiteral("^spotify:(track|album|playlist|artist):([A-Za-z0-9]+)$"),
                                               QRegularExpression::CaseInsensitiveOption);
    if (const QRegularExpressionMatch m = spotifyUri.match(candidate); m.hasMatch())
        candidate = QStringLiteral("https://open.spotify.com/%1/%2").arg(m.captured(1).toLower(), m.captured(2));
    if (candidate.isEmpty() || candidate.contains(QLatin1Char('\n')) || candidate.contains(QLatin1Char(' ')))
        return {};
    if (!candidate.contains(QLatin1String("://")))
        candidate.prepend(QStringLiteral("https://"));
    const QUrl url(candidate, QUrl::StrictMode);
    const QString scheme = url.scheme().toLower();
    if (!url.isValid() || url.host().isEmpty() || !url.host().contains(QLatin1Char('.'))
        || (scheme != QLatin1String("http") && scheme != QLatin1String("https"))) {
        return {};
    }
    return url.toString();
}

QString MediaDownloader::platformName(const QString &text)
{
    const QString normalized = normalizeUrl(text);
    if (normalized.isEmpty())
        return {};
    const QString host = QUrl(normalized).host().toLower();
    for (const Site &site : kSites) {
        const QString domain = QString::fromLatin1(site.domain);
        if (host == domain || host.endsWith(QLatin1Char('.') + domain))
            return QString::fromLatin1(site.name);
    }
    return {};
}

std::optional<MediaDownloader::Progress> MediaDownloader::parseProgress(const QString &line)
{
    static const QRegularExpression percent(QStringLiteral("^\\[download\\]\\s+([\\d.]+)%"));
    const QRegularExpressionMatch match = percent.match(line.trimmed());
    if (!match.hasMatch())
        return std::nullopt;
    Progress progress;
    progress.percent = std::clamp(match.captured(1).toDouble(), 0.0, 100.0);
    static const QRegularExpression total(QStringLiteral("\\bof\\s+~?\\s*([\\d.]+\\s*[KMGT]?i?B)\\b"));
    static const QRegularExpression speed(QStringLiteral("\\bat\\s+([\\d.]+\\s*[KMGT]?i?B/s)"));
    static const QRegularExpression eta(QStringLiteral("\\bETA\\s+([\\d:]+)"));
    if (const auto m = total.match(line); m.hasMatch())
        progress.total = m.captured(1).remove(QLatin1Char(' '));
    if (const auto m = speed.match(line); m.hasMatch())
        progress.speed = m.captured(1).remove(QLatin1Char(' '));
    if (const auto m = eta.match(line); m.hasMatch())
        progress.eta = m.captured(1);
    return progress;
}

bool MediaDownloader::isSpotifyUrl(const QString &url)
{
    return platformName(url) == QLatin1String("Spotify");
}

std::optional<MediaDownloader::SpotifyLink> MediaDownloader::parseSpotifyLink(const QString &url)
{
    const QString normalized = normalizeUrl(url);
    if (normalized.isEmpty())
        return std::nullopt;
    const QUrl parsed(normalized);
    const QString host = parsed.host().toLower();
    if (host != QLatin1String("spotify.com") && !host.endsWith(QLatin1String(".spotify.com")))
        return std::nullopt;
    QStringList parts = parsed.path().split(QLatin1Char('/'), Qt::SkipEmptyParts);
    // open.spotify.com/intl-de/track/<id>, open.spotify.com/embed/track/<id>
    while (!parts.isEmpty() && (parts.first().startsWith(QLatin1String("intl-")) || parts.first() == QLatin1String("embed")))
        parts.removeFirst();
    static const QRegularExpression id(QStringLiteral("^[A-Za-z0-9]{8,}$"));
    if (parts.size() < 2 || !kSpotifyTypes.contains(parts[0]) || !id.match(parts[1]).hasMatch())
        return std::nullopt;
    return SpotifyLink{parts[0], parts[1]};
}

QList<MediaDownloader::SpotifyTrack> MediaDownloader::parseSpotifyPage(const QByteArray &data, QString *collection)
{
    QList<SpotifyTrack> tracks;
    const QString html = QString::fromUtf8(data);
    static const QRegularExpression nextData(QStringLiteral(R"(<script[^>]*id="__NEXT_DATA__"[^>]*>(.*?)</script>)"),
                                             QRegularExpression::DotMatchesEverythingOption);
    if (const QRegularExpressionMatch m = nextData.match(html); m.hasMatch()) {
        const QJsonDocument json = QJsonDocument::fromJson(m.captured(1).toUtf8());
        const QJsonObject entity = findEntity(json.object());
        const QJsonArray list = entity.value(QStringLiteral("trackList")).toArray();
        if (!list.isEmpty()) {
            QString name = cleanText(entity.value(QStringLiteral("name")).toString());
            if (name.isEmpty())
                name = cleanText(entity.value(QStringLiteral("title")).toString());
            if (collection)
                *collection = name;
            const bool album = entity.value(QStringLiteral("type")).toString() == QLatin1String("album");
            for (const QJsonValue &value : list) {
                const QJsonObject item = value.toObject();
                SpotifyTrack track;
                track.title = cleanText(item.value(QStringLiteral("title")).toString());
                track.artist = cleanText(item.value(QStringLiteral("subtitle")).toString());
                if (track.artist.isEmpty())
                    track.artist = joinArtists(item.value(QStringLiteral("artists")).toArray());
                track.album = album ? name : QString();
                track.duration = item.value(QStringLiteral("duration")).toDouble() / 1000.0;
                if (!track.title.isEmpty())
                    tracks.append(track);
            }
            return tracks;
        }
        if (!entity.isEmpty()) {
            SpotifyTrack track;
            track.title = cleanText(entity.value(QStringLiteral("name")).toString());
            if (track.title.isEmpty())
                track.title = cleanText(entity.value(QStringLiteral("title")).toString());
            track.artist = joinArtists(entity.value(QStringLiteral("artists")).toArray());
            if (track.artist.isEmpty())
                track.artist = cleanText(entity.value(QStringLiteral("subtitle")).toString());
            track.duration = entity.value(QStringLiteral("duration")).toDouble() / 1000.0;
            if (!track.title.isEmpty()) {
                tracks.append(track);
                return tracks;
            }
        }
    }
    // The web player's page: "Song" pages carry the title, and the artist and
    // album in the description ("Artist · Album · Song · 2020").
    const QString title = metaContent(html, QStringLiteral("og:title"));
    const QString description = metaContent(html, QStringLiteral("og:description"));
    const QString type = metaContent(html, QStringLiteral("og:type"));
    if (title.isEmpty() || (type != QLatin1String("music.song") && !description.contains(QStringLiteral(" · Song"))))
        return tracks;
    SpotifyTrack track;
    track.title = cleanText(title);
    const QStringList parts = description.split(QStringLiteral(" · "));
    if (!parts.isEmpty())
        track.artist = cleanText(parts.value(0));
    if (parts.size() > 2)
        track.album = cleanText(parts.value(1));
    track.duration = metaContent(html, QStringLiteral("music:duration")).toDouble();
    tracks.append(track);
    return tracks;
}

QString MediaDownloader::spotifyQuery(const SpotifyTrack &track)
{
    // The first two artists find the upload; long features lists only hurt.
    const QStringList artists = track.artist.split(QStringLiteral(", "), Qt::SkipEmptyParts);
    const QString artist = artists.mid(0, 2).join(QLatin1Char(' '));
    return artist.isEmpty() ? track.title : artist + QStringLiteral(" - ") + track.title;
}

QStringList MediaDownloader::spotifyArguments(const SpotifyTrack &track, const QString &directory, bool strict)
{
    QStringList args{
        QStringLiteral("--newline"), QStringLiteral("--progress"), QStringLiteral("--no-mtime"),
        QStringLiteral("-P"), directory,
        QStringLiteral("-o"), fileNameFor(track),
        QStringLiteral("--print"), QStringLiteral("after_move:filepath"),
        QStringLiteral("-f"), formatSelector(Format::AudioMp3), QStringLiteral("-x"),
        QStringLiteral("--audio-format"), QStringLiteral("mp3"),
        QStringLiteral("--audio-quality"), QStringLiteral("0"),
        // Tagged with Spotify's names rather than the upload's.
        QStringLiteral("--embed-metadata"),
    };
    const QList<QPair<QString, QString>> tags{
        {track.title, QStringLiteral("meta_title")},
        {track.artist, QStringLiteral("meta_artist")},
        {track.album, QStringLiteral("meta_album")},
    };
    for (const auto &[value, field] : tags) {
        if (!value.isEmpty())
            args << QStringLiteral("--parse-metadata")
                 << QStringLiteral("pre_process:%1:%(%2)s |").arg(metadataLiteral(value), field);
    }
    const QString query = spotifyQuery(track);
    if (strict && track.duration > 0) {
        const int low = std::max(0, static_cast<int>(track.duration) - kLengthTolerance);
        const int high = static_cast<int>(track.duration) + kLengthTolerance;
        args << QStringLiteral("--match-filter")
             << QStringLiteral("duration>=%1 & duration<=%2 & !is_live").arg(low).arg(high)
             << QStringLiteral("--max-downloads") << QStringLiteral("1")
             << QStringLiteral("--") << QStringLiteral("ytsearch%1:%2").arg(kSearchResults).arg(query);
    } else {
        args << QStringLiteral("--") << QStringLiteral("ytsearch1:%1").arg(query);
    }
    return args;
}

QString MediaDownloader::spotifyStreamUrl(const SpotifyTrack &track)
{
    return QStringLiteral("ytdl://ytsearch1:") + spotifyQuery(track);
}

QString MediaDownloader::downloadSource(const QString &entry)
{
    const QString trimmed = entry.trimmed();
    if (trimmed.startsWith(QLatin1String("ytdl://"), Qt::CaseInsensitive))
        return trimmed.mid(7);
    const QUrl url(trimmed);
    const QString scheme = url.scheme().toLower();
    if ((scheme == QLatin1String("http") || scheme == QLatin1String("https")) && !url.host().isEmpty())
        return trimmed;
    return {};
}

bool MediaDownloader::isPlaylistUrl(const QString &text)
{
    const QString normalized = normalizeUrl(text);
    if (platformName(normalized) != QLatin1String("YouTube"))
        return false;
    const QUrlQuery query{QUrl(normalized)};
    return !query.queryItemValue(QStringLiteral("list")).isEmpty();
}

bool MediaDownloader::isPlaylistOnlyUrl(const QString &text)
{
    const QUrl url(normalizeUrl(text));
    return isPlaylistUrl(text) && url.path() == QLatin1String("/playlist");
}

QList<MediaDownloader::PlaylistEntry> MediaDownloader::parsePlaylistJson(const QByteArray &json, QString *title)
{
    QList<PlaylistEntry> entries;
    const QJsonObject root = QJsonDocument::fromJson(json).object();
    if (root.isEmpty())
        return entries;
    const auto entryOf = [](const QJsonObject &item) {
        PlaylistEntry entry;
        entry.title = cleanText(item.value(QStringLiteral("title")).toString());
        entry.duration = item.value(QStringLiteral("duration")).toDouble();
        QString url = item.value(QStringLiteral("webpage_url")).toString();
        if (url.isEmpty())
            url = item.value(QStringLiteral("url")).toString();
        const QString id = item.value(QStringLiteral("id")).toString();
        const QString extractor = item.value(QStringLiteral("ie_key")).toString();
        // Flat entries of some sites carry only the video's id.
        if (!url.contains(QLatin1String("://")) && !id.isEmpty()
            && (extractor.isEmpty() || extractor.compare(QLatin1String("Youtube"), Qt::CaseInsensitive) == 0))
            url = QStringLiteral("https://www.youtube.com/watch?v=") + id;
        entry.url = url;
        return entry;
    };
    const QJsonValue list = root.value(QStringLiteral("entries"));
    if (!list.isArray()) {
        // A single video.
        const PlaylistEntry entry = entryOf(root);
        if (!entry.url.isEmpty())
            entries.append(entry);
        if (title)
            *title = entry.title;
        return entries;
    }
    if (title)
        *title = cleanText(root.value(QStringLiteral("title")).toString());
    for (const QJsonValue &value : list.toArray()) {
        const PlaylistEntry entry = entryOf(value.toObject());
        // Deleted and private videos come as "[Deleted video]" without a usable page.
        if (entry.url.contains(QLatin1String("://")) && entry.title != QLatin1String("[Deleted video]")
            && entry.title != QLatin1String("[Private video]"))
            entries.append(entry);
    }
    return entries;
}

bool MediaDownloader::isResolving() const
{
    return !m_resolver.isNull();
}

bool MediaDownloader::resolvePlaylist(const QString &url)
{
    const QString program = executable();
    if (program.isEmpty())
        return false;
    if (QProcess *old = m_resolver.data()) {
        old->disconnect(this);
        old->kill();
        old->deleteLater();
    }
    auto *process = new QProcess(this);
    m_resolver = process;
    connect(process, &QProcess::finished, this, [this, process](int exitCode, QProcess::ExitStatus status) {
        if (m_resolver != process)
            return;
        m_resolver = nullptr;
        process->deleteLater();
        QString title;
        const QList<PlaylistEntry> entries = parsePlaylistJson(process->readAllStandardOutput(), &title);
        if (!entries.isEmpty()) {
            Q_EMIT playlistResolved(entries, title, {});
            return;
        }
        QString error;
        for (const QString &line : QString::fromUtf8(process->readAllStandardError()).split(QLatin1Char('\n'))) {
            if (line.startsWith(QLatin1String("ERROR:")))
                error = line.mid(6).trimmed();
        }
        if (error.isEmpty())
            error = status == QProcess::NormalExit && exitCode == 0 ? tr("This playlist has no videos that can be played.")
                                                                    : tr("yt-dlp couldn't read the playlist.");
        Q_EMIT playlistResolved({}, {}, error);
    });
    connect(process, &QProcess::errorOccurred, this, [this, process](QProcess::ProcessError error) {
        if (error != QProcess::FailedToStart || m_resolver != process)
            return;
        m_resolver = nullptr;
        process->deleteLater();
        Q_EMIT playlistResolved({}, {}, tr("Could not start yt-dlp: %1").arg(process->errorString()));
    });
    process->start(program, {QStringLiteral("--flat-playlist"), QStringLiteral("--dump-single-json"),
                             QStringLiteral("--no-warnings"), QStringLiteral("--yes-playlist"), QStringLiteral("--"), url});
    return true;
}

bool MediaDownloader::startEntry(const QString &entry, const QString &title, Format format, const QString &directory)
{
    const QString source = downloadSource(entry);
    if (source.isEmpty() || isRunning() || executable().isEmpty())
        return false;
    QDir().mkpath(directory);
    m_files.clear();
    m_failed.clear();
    // A Spotify song streamed through a YouTube search: saved like Spotify downloads.
    static const QRegularExpression search(QStringLiteral("^ytsearch\\d*:(.+)$"));
    if (const QRegularExpressionMatch m = search.match(source); m.hasMatch() && format == Format::AudioMp3) {
        SpotifyTrack track;
        const QString name = title.isEmpty() ? m.captured(1) : title;
        const qsizetype dash = name.indexOf(QStringLiteral(" - "));
        track.artist = dash > 0 ? name.left(dash).trimmed() : QString();
        track.title = dash > 0 ? name.mid(dash + 3).trimmed() : name.trimmed();
        m_spotifyDownload = true;
        m_directory = directory;
        m_tracks = {track};
        m_track = 0;
        m_strict = false;
        startSpotifyTrack();
        return true;
    }
    if (isSpotifyUrl(source))
        return start(source, format, directory);
    return launch(arguments(source, format, directory, title));
}

bool MediaDownloader::startPlaylist(const QString &url, Format format, const QString &directory)
{
    if (isRunning() || executable().isEmpty())
        return false;
    QDir().mkpath(directory);
    m_files.clear();
    m_failed.clear();
    m_playlistDownload = true;
    m_item = 0;
    m_items = 0;
    if (!launch(playlistArguments(url, format, directory))) {
        m_playlistDownload = false;
        return false;
    }
    return true;
}

void MediaDownloader::resolveSpotify(const QString &url)
{
    if (m_reply)
        m_reply->abort();
    m_spotify = true;
    const std::optional<SpotifyLink> link = parseSpotifyLink(url);
    QUrl target;
    if (link) {
        target = m_spotifyBase;
        target.setPath(QStringLiteral("/embed/%1/%2").arg(link->type, link->id));
    } else {
        // A spotify.link short link: follow it to the open.spotify.com page first.
        target = QUrl(normalizeUrl(url));
    }
    fetchSpotify(target, link ? 1 : 2);
}

void MediaDownloader::fetchSpotify(const QUrl &url, int redirectsLeft)
{
    if (!m_network)
        m_network = new QNetworkAccessManager(this);
    QNetworkRequest request(url);
    request.setHeader(QNetworkRequest::UserAgentHeader, QString::fromLatin1(kBrowserAgent));
    request.setRawHeader("Accept-Language", "en");
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
    request.setTransferTimeout(kSpotifyTimeoutMs);
    QNetworkReply *reply = m_network->get(request);
    m_reply = reply;
    connect(reply, &QNetworkReply::finished, this, [this, reply, redirectsLeft] { onSpotifyPage(reply, redirectsLeft); });
}

void MediaDownloader::onSpotifyPage(QNetworkReply *reply, int redirectsLeft)
{
    reply->deleteLater();
    if (reply != m_reply || !m_spotify)
        return;
    m_reply = nullptr;
    const QByteArray data = reply->readAll();
    const QUrl finalUrl = reply->url();
    const bool failed = reply->error() != QNetworkReply::NoError;

    auto fail = [this](const QString &error) {
        const bool download = m_spotifyDownload;
        m_spotify = false;
        m_spotifyDownload = false;
        Q_EMIT spotifyResolved({}, {}, error);
        if (download)
            Q_EMIT finished(false, {}, error);
    };

    QString collection;
    const QList<SpotifyTrack> tracks = failed ? QList<SpotifyTrack>() : parseSpotifyPage(data, &collection);
    if (tracks.isEmpty() && redirectsLeft > 0) {
        // A short link landed on (or names) the real page; an embed page
        // without data is tried again as the web player's page.
        std::optional<SpotifyLink> link = parseSpotifyLink(finalUrl.toString());
        if (!link) {
            static const QRegularExpression inPage(
                QStringLiteral(R"(open\.spotify\.com/(?:intl-[a-z-]+/)?(track|album|playlist|artist)/([A-Za-z0-9]{8,}))"));
            if (const QRegularExpressionMatch m = inPage.match(QString::fromUtf8(data)); m.hasMatch())
                link = SpotifyLink{m.captured(1), m.captured(2)};
        }
        if (link) {
            QUrl next = m_spotifyBase;
            const bool wasEmbed = finalUrl.path().startsWith(QLatin1String("/embed/"));
            next.setPath(wasEmbed ? QStringLiteral("/%1/%2").arg(link->type, link->id)
                                  : QStringLiteral("/embed/%1/%2").arg(link->type, link->id));
            fetchSpotify(next, redirectsLeft - 1);
            return;
        }
    }
    if (tracks.isEmpty()) {
        fail(failed ? tr("Couldn't read the Spotify link: %1").arg(reply->errorString())
                    : tr("No songs found at this Spotify link. Podcasts and private playlists can't be downloaded."));
        return;
    }

    Q_EMIT spotifyResolved(tracks, collection, {});
    if (!m_spotifyDownload) {
        m_spotify = false;
        return;
    }
    m_tracks = tracks;
    m_track = 0;
    m_strict = true;
    startSpotifyTrack();
}

void MediaDownloader::startSpotifyTrack()
{
    if (m_track >= m_tracks.size()) {
        finishSpotify();
        return;
    }
    const SpotifyTrack &track = m_tracks[m_track];
    const QString name = spotifyQuery(track);
    if (m_tracks.size() > 1)
        Q_EMIT stageChanged(tr("Track %1 of %2: %3").arg(m_track + 1).arg(m_tracks.size()).arg(name));
    else
        Q_EMIT stageChanged(m_strict ? tr("Finding \u201c%1\u201d on YouTube...").arg(name)
                                     : tr("Trying the closest match for \u201c%1\u201d...").arg(name));
    m_errors.clear();
    if (!launch(spotifyArguments(track, m_directory, m_strict))) {
        m_spotify = false;
        m_spotifyDownload = false;
    }
}

void MediaDownloader::finishSpotify()
{
    m_spotify = false;
    m_spotifyDownload = false;
    if (!m_files.isEmpty()) {
        Q_EMIT finished(true, m_files.first(), {});
        return;
    }
    QString error = m_errors.join(QLatin1Char('\n'));
    if (error.isEmpty())
        error = m_tracks.size() == 1 ? tr("Couldn't find \u201c%1\u201d on YouTube.").arg(spotifyQuery(m_tracks.first()))
                                     : tr("Couldn't find any of these songs on YouTube.");
    Q_EMIT finished(false, {}, error);
}

bool MediaDownloader::start(const QString &url, Format format, const QString &directory)
{
    const QString program = executable();
    if (isRunning() || program.isEmpty())
        return false;
    QDir().mkpath(directory);
    m_files.clear();
    m_failed.clear();
    if (isSpotifyUrl(url)) {
        // Spotify songs are always saved as MP3s, whatever the format.
        m_spotifyDownload = true;
        m_directory = directory;
        m_tracks.clear();
        m_track = -1;
        Q_EMIT stageChanged(tr("Reading the Spotify link..."));
        resolveSpotify(url);
        return true;
    }
    return launch(arguments(url, format, directory));
}

bool MediaDownloader::launch(const QStringList &args)
{
    const QString program = executable();
    if (program.isEmpty())
        return false;
    m_stdout.clear();
    m_stderr.clear();
    m_path.clear();
    m_paths.clear();
    m_errors.clear();

    auto *process = new QProcess(this);
    m_process = process;
    connect(process, &QProcess::readyReadStandardOutput, this,
            [this, process] { readLines(m_stdout, process->readAllStandardOutput()); });
    connect(process, &QProcess::readyReadStandardError, this,
            [this, process] { readLines(m_stderr, process->readAllStandardError()); });
    connect(process, &QProcess::finished, this, &MediaDownloader::onFinished);
    connect(process, &QProcess::errorOccurred, this, [this, process](QProcess::ProcessError error) {
        if (error != QProcess::FailedToStart || m_process != process)
            return;
        m_process = nullptr;
        m_spotify = false;
        m_spotifyDownload = false;
        m_playlistDownload = false;
        process->deleteLater();
        Q_EMIT finished(false, {}, tr("Could not start yt-dlp: %1").arg(process->errorString()));
    });
    if (!m_spotifyDownload)
        Q_EMIT stageChanged(tr("Starting..."));
    process->start(program, args);
    return true;
}

void MediaDownloader::readLines(QByteArray &buffer, const QByteArray &data)
{
    buffer += data;
    qsizetype end;
    while ((end = buffer.indexOf('\n')) >= 0) {
        const QString line = QString::fromUtf8(buffer.left(end)).trimmed();
        buffer.remove(0, end + 1);
        if (!line.isEmpty())
            handleLine(line);
    }
}

void MediaDownloader::handleLine(const QString &line)
{
    if (auto parsed = parseProgress(line)) {
        // An album or playlist reports its progress as a whole.
        if (m_spotifyDownload && m_tracks.size() > 1)
            parsed->percent = (m_track + parsed->percent / 100.0) * 100.0 / m_tracks.size();
        else if (m_playlistDownload && m_items > 0)
            parsed->percent = (std::max(0, m_item - 1) + parsed->percent / 100.0) * 100.0 / m_items;
        Q_EMIT progress(*parsed);
        return;
    }
    static const QRegularExpression item(QStringLiteral("^\\[download\\] Downloading (?:item|video) (\\d+) of (\\d+)"));
    if (const QRegularExpressionMatch m = item.match(line); m.hasMatch()) {
        m_item = m.captured(1).toInt();
        m_items = m.captured(2).toInt();
        Q_EMIT stageChanged(tr("Video %1 of %2").arg(m_item).arg(m_items));
        return;
    }
    if (line.startsWith(QLatin1String("ERROR:"))) {
        m_errors.append(line.mid(6).trimmed());
    } else if (line.startsWith(QLatin1String("[Merger]"))) {
        Q_EMIT stageChanged(tr("Merging video and audio..."));
    } else if (line.startsWith(QLatin1String("[ExtractAudio]"))) {
        Q_EMIT stageChanged(tr("Converting to MP3..."));
    } else if (line.startsWith(QLatin1String("[download] Destination:"))) {
        Q_EMIT stageChanged(tr("Downloading %1").arg(QFileInfo(line.section(QLatin1Char(':'), 1).trimmed()).fileName()));
    } else if (!line.startsWith(QLatin1Char('[')) && QFileInfo(line).isAbsolute()) {
        // --print after_move:filepath
        m_path = line;
        m_paths.append(line);
    }
}

void MediaDownloader::onFinished(int exitCode, QProcess::ExitStatus status)
{
    QProcess *process = m_process.data();
    if (!process)
        return;
    m_process = nullptr;
    process->deleteLater();
    // Lines without a trailing newline.
    readLines(m_stdout, "\n");
    readLines(m_stderr, "\n");
    const bool saved = !m_path.isEmpty() && QFileInfo::exists(m_path);
    if (m_spotifyDownload) {
        const bool ok = status == QProcess::NormalExit && (exitCode == 0 || exitCode == kMaxDownloadsReached) && saved;
        const SpotifyTrack track = m_tracks.value(m_track);
        if (ok) {
            m_files.append(m_path);
        } else if (m_strict && track.duration > 0 && status == QProcess::NormalExit) {
            // Nothing of the right length: take the best match instead.
            m_strict = false;
            startSpotifyTrack();
            return;
        } else {
            m_failed.append(spotifyQuery(track));
        }
        ++m_track;
        m_strict = true;
        startSpotifyTrack();
        return;
    }
    if (std::exchange(m_playlistDownload, false)) {
        // Some videos of a playlist may be unavailable; the rest count.
        QStringList files;
        for (const QString &path : std::as_const(m_paths)) {
            if (QFileInfo::exists(path) && !files.contains(path))
                files.append(path);
        }
        if (status == QProcess::NormalExit && !files.isEmpty()) {
            m_files = files;
            Q_EMIT finished(true, files.first(), {});
            return;
        }
    } else if (status == QProcess::NormalExit && exitCode == 0 && saved) {
        m_files = {m_path};
        Q_EMIT finished(true, m_path, {});
        return;
    }
    QString error = m_errors.join(QLatin1Char('\n'));
    if (error.isEmpty())
        error = status == QProcess::CrashExit ? tr("yt-dlp crashed.") : tr("yt-dlp failed (exit code %1).").arg(exitCode);
    Q_EMIT finished(false, {}, error);
}

void MediaDownloader::cancel()
{
    m_spotify = false;
    m_spotifyDownload = false;
    m_playlistDownload = false;
    if (QProcess *resolver = m_resolver.data()) {
        m_resolver = nullptr;
        resolver->disconnect(this);
        resolver->kill();
        resolver->waitForFinished(1000);
        resolver->deleteLater();
    }
    if (QNetworkReply *reply = m_reply.data()) {
        m_reply = nullptr;
        reply->abort();
    }
    QProcess *process = m_process.data();
    if (!process)
        return;
    m_process = nullptr;
    process->disconnect(this);
    process->terminate();
    if (!process->waitForFinished(3000))
        process->kill();
    process->deleteLater();
}

bool MediaDownloader::isRunning() const
{
    return !m_process.isNull() || m_spotifyDownload;
}
