#include "MediaSearch.h"
#include "YtDlpOptions.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocale>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>

#include <algorithm>
#include <utility>
#include <cmath>

namespace {

constexpr int kTimeoutMs = 20000;
// Results asked of Spotify per kind, and what is tried when an app may ask
// for fewer.
constexpr int kSpotifyLimit = 20;
constexpr int kSpotifySmallLimit = 10;
// A token is renewed this long before Spotify says it expires.
constexpr int kTokenMarginSecs = 60;
// Cover art about this wide is picked from the sizes Spotify offers.
constexpr int kCoverWidth = 160;

QString cleanText(QString text)
{
    text.replace(QChar(0x00A0), QLatin1Char(' '));
    return text.simplified();
}

QString joinArtists(const QJsonArray &artists)
{
    QStringList names;
    for (const QJsonValue &artist : artists) {
        const QString name = cleanText(artist.toObject().value(QStringLiteral("name")).toString());
        if (!name.isEmpty())
            names << name;
    }
    return names.join(QStringLiteral(", "));
}

// The smallest image at least kCoverWidth wide, else the largest.
QUrl pickImage(const QJsonArray &images)
{
    QUrl best;
    int bestWidth = 0;
    QUrl largest;
    int largestWidth = -1;
    for (const QJsonValue &value : images) {
        const QJsonObject image = value.toObject();
        const QUrl url(image.value(QStringLiteral("url")).toString());
        if (!url.isValid() || url.isEmpty())
            continue;
        const int width = image.value(QStringLiteral("width")).toInt();
        if (width > largestWidth) {
            largest = url;
            largestWidth = width;
        }
        if (width >= kCoverWidth && (best.isEmpty() || width < bestWidth)) {
            best = url;
            bestWidth = width;
        }
    }
    return best.isEmpty() ? largest : best;
}

QString spotifyUrl(const QJsonObject &item, const QString &type, const QString &id)
{
    const QString url = item.value(QStringLiteral("external_urls")).toObject().value(QStringLiteral("spotify")).toString();
    return url.isEmpty() ? QStringLiteral("https://open.spotify.com/%1/%2").arg(type, id) : url;
}

const char *typeName(MediaSearch::Kind kind)
{
    switch (kind) {
    case MediaSearch::Kind::Track:
        return "track";
    case MediaSearch::Kind::Album:
        return "album";
    case MediaSearch::Kind::Playlist:
        return "playlist";
    case MediaSearch::Kind::Video:
        break;
    }
    return "track";
}

} // namespace

QString MediaSearch::Result::displayName() const
{
    return artists.isEmpty() || kind != Kind::Track ? title : artists + QStringLiteral(" - ") + title;
}

MediaDownloader::SpotifyTrack MediaSearch::Result::track() const
{
    MediaDownloader::SpotifyTrack track;
    track.title = title;
    track.artist = artists;
    track.album = album;
    track.duration = duration;
    return track;
}

MediaSearch::MediaSearch(QObject *parent)
    : QObject(parent)
    , m_accountsBase(QStringLiteral("https://accounts.spotify.com"))
    , m_apiBase(QStringLiteral("https://api.spotify.com"))
    , m_thumbnailBase(QStringLiteral("https://i.ytimg.com"))
    , m_deezerBase(QStringLiteral("https://api.deezer.com"))
    , m_spotifyLimit(kSpotifyLimit)
{
    qRegisterMetaType<MediaSearch::Result>();
    qRegisterMetaType<MediaSearch::Page>();
}

MediaSearch::~MediaSearch()
{
    cancel();
}

QStringList MediaSearch::youtubeArguments(const QString &query, int count)
{
    // "--" so that a query can never be read as an option.
    return {QStringLiteral("--flat-playlist"), QStringLiteral("--dump-single-json"), QStringLiteral("--no-warnings"),
            QStringLiteral("--"), QStringLiteral("ytsearch%1:%2").arg(std::max(1, count)).arg(query.simplified())};
}

QList<MediaSearch::Result> MediaSearch::parseYouTubeSearch(const QByteArray &json, const QUrl &thumbnailBase)
{
    QList<Result> results;
    const QJsonObject root = QJsonDocument::fromJson(json).object();
    const QJsonArray entries = root.value(QStringLiteral("entries")).toArray();
    for (const QJsonValue &value : entries) {
        const QJsonObject item = value.toObject();
        Result result;
        result.kind = Kind::Video;
        result.id = item.value(QStringLiteral("id")).toString();
        result.title = cleanText(item.value(QStringLiteral("title")).toString());
        // Only videos: search results may also name channels and playlists.
        const QString url = item.value(QStringLiteral("url")).toString();
        if (result.id.isEmpty() || result.title.isEmpty() || url.contains(QLatin1String("/channel/"))
            || url.contains(QLatin1String("list=")) || result.title == QLatin1String("[Private video]")
            || result.title == QLatin1String("[Deleted video]"))
            continue;
        result.artists = cleanText(item.value(QStringLiteral("channel")).toString());
        if (result.artists.isEmpty())
            result.artists = cleanText(item.value(QStringLiteral("uploader")).toString());
        result.duration = item.value(QStringLiteral("duration")).toDouble();
        const QJsonValue views = item.value(QStringLiteral("view_count"));
        result.views = views.isDouble() ? static_cast<qint64>(views.toDouble()) : -1;
        result.live = item.value(QStringLiteral("live_status")).toString() == QLatin1String("is_live")
                      || item.value(QStringLiteral("is_live")).toBool();
        result.url = url.startsWith(QLatin1String("https://www.youtube.com/watch")) ? url
                                                                                    : QStringLiteral("https://www.youtube.com/watch?v=") + result.id;
        QUrl thumbnail = thumbnailBase;
        thumbnail.setPath(QStringLiteral("/vi/%1/mqdefault.jpg").arg(result.id));
        result.thumbnail = thumbnail;
        results.append(result);
    }
    return results;
}

QList<MediaSearch::Result> MediaSearch::parseSpotifySearch(const QByteArray &json, QMap<Kind, int> *totals)
{
    QList<Result> results;
    const QJsonObject root = QJsonDocument::fromJson(json).object();
    const QList<QPair<Kind, QString>> sections{
        {Kind::Track, QStringLiteral("tracks")},
        {Kind::Album, QStringLiteral("albums")},
        {Kind::Playlist, QStringLiteral("playlists")},
    };
    for (const auto &[kind, key] : sections) {
        if (!root.contains(key))
            continue;
        const QJsonObject section = root.value(key).toObject();
        if (totals)
            totals->insert(kind, section.value(QStringLiteral("total")).toInt());
        for (const QJsonValue &value : section.value(QStringLiteral("items")).toArray()) {
            // Spotify leaves holes (null) where it hides a playlist.
            const QJsonObject item = value.toObject();
            Result result;
            result.kind = kind;
            result.id = item.value(QStringLiteral("id")).toString();
            result.title = cleanText(item.value(QStringLiteral("name")).toString());
            if (result.id.isEmpty() || result.title.isEmpty())
                continue;
            result.url = spotifyUrl(item, QString::fromLatin1(typeName(kind)), result.id);
            switch (kind) {
            case Kind::Track: {
                result.artists = joinArtists(item.value(QStringLiteral("artists")).toArray());
                const QJsonObject album = item.value(QStringLiteral("album")).toObject();
                result.album = cleanText(album.value(QStringLiteral("name")).toString());
                result.duration = item.value(QStringLiteral("duration_ms")).toDouble() / 1000.0;
                result.thumbnail = pickImage(album.value(QStringLiteral("images")).toArray());
                break;
            }
            case Kind::Album:
                result.artists = joinArtists(item.value(QStringLiteral("artists")).toArray());
                result.trackCount = item.value(QStringLiteral("total_tracks")).toInt();
                result.released = item.value(QStringLiteral("release_date")).toString().left(4);
                result.thumbnail = pickImage(item.value(QStringLiteral("images")).toArray());
                break;
            case Kind::Playlist: {
                result.artists = cleanText(item.value(QStringLiteral("owner")).toObject().value(QStringLiteral("display_name")).toString());
                // "tracks" in older answers, "items" in newer ones.
                QJsonObject songs = item.value(QStringLiteral("tracks")).toObject();
                if (songs.isEmpty())
                    songs = item.value(QStringLiteral("items")).toObject();
                result.trackCount = songs.value(QStringLiteral("total")).toInt();
                result.thumbnail = pickImage(item.value(QStringLiteral("images")).toArray());
                break;
            }
            case Kind::Video:
                break;
            }
            results.append(result);
        }
    }
    return results;
}

QList<MediaSearch::Result> MediaSearch::parseDeezerSearch(const QByteArray &json, Kind kind, int *total)
{
    QList<Result> results;
    const QJsonObject root = QJsonDocument::fromJson(json).object();
    if (total)
        *total = root.value(QStringLiteral("total")).toInt();
    for (const QJsonValue &value : root.value(QStringLiteral("data")).toArray()) {
        const QJsonObject item = value.toObject();
        Result result;
        result.kind = kind;
        result.id = QString::number(item.value(QStringLiteral("id")).toVariant().toLongLong());
        result.title = cleanText(item.value(QStringLiteral("title")).toString());
        result.url = item.value(QStringLiteral("link")).toString();
        switch (kind) {
        case Kind::Track: {
            const QJsonObject album = item.value(QStringLiteral("album")).toObject();
            result.artists = cleanText(item.value(QStringLiteral("artist")).toObject().value(QStringLiteral("name")).toString());
            result.album = cleanText(album.value(QStringLiteral("title")).toString());
            result.duration = item.value(QStringLiteral("duration")).toDouble();
            result.thumbnail = QUrl(album.value(QStringLiteral("cover_medium")).toString());
            if (result.url.isEmpty())
                result.url = QStringLiteral("https://www.deezer.com/track/") + result.id;
            break;
        }
        case Kind::Album:
            result.artists = cleanText(item.value(QStringLiteral("artist")).toObject().value(QStringLiteral("name")).toString());
            result.trackCount = item.value(QStringLiteral("nb_tracks")).toInt();
            result.thumbnail = QUrl(item.value(QStringLiteral("cover_medium")).toString());
            if (result.url.isEmpty())
                result.url = QStringLiteral("https://www.deezer.com/album/") + result.id;
            break;
        case Kind::Playlist:
            result.artists = cleanText(item.value(QStringLiteral("user")).toObject().value(QStringLiteral("name")).toString());
            result.trackCount = item.value(QStringLiteral("nb_tracks")).toInt();
            result.thumbnail = QUrl(item.value(QStringLiteral("picture_medium")).toString());
            if (result.url.isEmpty())
                result.url = QStringLiteral("https://www.deezer.com/playlist/") + result.id;
            break;
        case Kind::Video:
            break;
        }
        if (!result.title.isEmpty() && result.id != QLatin1String("0"))
            results.append(result);
    }
    return results;
}

void MediaSearch::searchDeezer(const QString &notice)
{
    if (!m_network)
        m_network = new QNetworkAccessManager(this);
    m_spotifyBusy = true;
    m_deezerNotice = notice;
    m_deezerReplies.clear();
    QList<Kind> kinds = m_request.kinds;
    if (kinds.isEmpty())
        kinds << Kind::Track;
    for (Kind kind : std::as_const(kinds)) {
        QUrl url = m_deezerBase;
        url.setPath(QStringLiteral("/search/") + QString::fromLatin1(typeName(kind)));
        // Encoded by hand, as for Spotify.
        url.setQuery(QStringLiteral("q=") + QString::fromLatin1(QUrl::toPercentEncoding(m_request.query))
                     + QStringLiteral("&index=") + QString::number(m_request.offset)
                     + QStringLiteral("&limit=") + QString::number(m_spotifyLimit));
        QNetworkRequest request(url);
        request.setAttribute(QNetworkRequest::User, int(kind));
        request.setTransferTimeout(kTimeoutMs);
        QNetworkReply *reply = m_network->get(request);
        m_deezerReplies.append(reply);
        connect(reply, &QNetworkReply::finished, this, &MediaSearch::onDeezerSearch);
    }
}

void MediaSearch::onDeezerSearch()
{
    // Wait for every kind's answer, then report them together.
    for (const QPointer<QNetworkReply> &reply : std::as_const(m_deezerReplies)) {
        if (reply && !reply->isFinished())
            return;
    }
    const QList<QPointer<QNetworkReply>> replies = std::exchange(m_deezerReplies, {});
    Page page;
    page.source = Source::Spotify;
    page.query = m_request.query;
    page.offset = m_request.offset;
    page.notice = m_deezerNotice;
    QString failure;
    for (const QPointer<QNetworkReply> &reply : replies) {
        if (!reply) {
            m_spotifyBusy = false; // cancelled
            return;
        }
        reply->deleteLater();
        const Kind kind = static_cast<Kind>(reply->request().attribute(QNetworkRequest::User).toInt());
        const QByteArray body = reply->readAll();
        if (reply->error() != QNetworkReply::NoError) {
            failure = tr("Couldn't reach Deezer: %1").arg(reply->errorString());
            continue;
        }
        int total = 0;
        page.results += parseDeezerSearch(body, kind, &total);
        page.totals.insert(kind, total);
    }
    m_spotifyBusy = false;
    if (page.results.isEmpty() && !failure.isEmpty()) {
        page.error = Error::Network;
        page.errorText = m_deezerNotice.isEmpty() ? failure : m_deezerNotice + QLatin1Char(' ') + failure;
    }
    Q_EMIT finished(page);
}

QString MediaSearch::parseSpotifyToken(const QByteArray &json, int *expiresIn)
{
    const QJsonObject root = QJsonDocument::fromJson(json).object();
    if (expiresIn)
        *expiresIn = root.value(QStringLiteral("expires_in")).toInt(3600);
    return root.value(QStringLiteral("access_token")).toString();
}

QString MediaSearch::formatDuration(double seconds)
{
    const qint64 total = std::llround(std::max(0.0, seconds));
    if (total == 0)
        return {};
    if (total >= 3600) {
        return QStringLiteral("%1:%2:%3")
            .arg(total / 3600)
            .arg((total / 60) % 60, 2, 10, QLatin1Char('0'))
            .arg(total % 60, 2, 10, QLatin1Char('0'));
    }
    return QStringLiteral("%1:%2").arg(total / 60).arg(total % 60, 2, 10, QLatin1Char('0'));
}

QString MediaSearch::formatViews(qint64 views)
{
    if (views < 0)
        return {};
    const auto scaled = [](qint64 value, double unit, const char *suffix) {
        const double v = value / unit;
        // One decimal below 10 ("1.2M"), none above ("12M").
        QString number = QString::number(v, 'f', v < 10 ? 1 : 0);
        if (number.endsWith(QLatin1String(".0")))
            number.chop(2);
        return number + QLatin1String(suffix);
    };
    QString count;
    if (views >= 1000000000)
        count = scaled(views, 1e9, "B");
    else if (views >= 1000000)
        count = scaled(views, 1e6, "M");
    else if (views >= 1000)
        count = scaled(views, 1e3, "K");
    else
        count = QString::number(views);
    return views == 1 ? tr("1 view") : tr("%1 views").arg(count);
}

bool MediaSearch::isBusy() const
{
    return !m_process.isNull() || m_spotifyBusy;
}

bool MediaSearch::searchYouTube(const QString &query, int count)
{
    const QString program = MediaDownloader::executable();
    if (program.isEmpty())
        return false;
    cancel();
    m_youtubeQuery = query;
    m_youtubeCount = count;
    auto *process = new QProcess(this);
    m_process = process;
    const auto fail = [this](const QString &error) {
        Page page;
        page.source = Source::YouTube;
        page.query = m_youtubeQuery;
        page.error = Error::Failed;
        page.errorText = error;
        Q_EMIT finished(page);
    };
    connect(process, &QProcess::finished, this, [this, process, fail](int exitCode, QProcess::ExitStatus status) {
        if (m_process != process)
            return;
        m_process = nullptr;
        process->deleteLater();
        const QByteArray output = process->readAllStandardOutput();
        Page page;
        page.source = Source::YouTube;
        page.query = m_youtubeQuery;
        page.results = parseYouTubeSearch(output, m_thumbnailBase);
        if (!page.results.isEmpty() || (status == QProcess::NormalExit && exitCode == 0)) {
            Q_EMIT finished(page);
            return;
        }
        QString error;
        for (const QString &line : QString::fromUtf8(process->readAllStandardError()).split(QLatin1Char('\n'))) {
            if (line.startsWith(QLatin1String("ERROR:")))
                error = line.mid(6).trimmed();
        }
        // YouTube's bot check: once more with the browser's cookies.
        if (YtDlpOptions::isBotCheck(error) && YtDlpOptions::noteBotCheck()) {
            searchYouTube(m_youtubeQuery, m_youtubeCount);
            return;
        }
        fail(error.isEmpty() ? tr("yt-dlp couldn't search YouTube (exit code %1).").arg(exitCode)
                             : tr("YouTube search failed: %1").arg(MediaDownloader::explainError(error)));
    });
    connect(process, &QProcess::errorOccurred, this, [this, process, fail](QProcess::ProcessError error) {
        if (error != QProcess::FailedToStart || m_process != process)
            return;
        m_process = nullptr;
        process->deleteLater();
        fail(tr("Could not start yt-dlp: %1").arg(process->errorString()));
    });
    process->start(program, YtDlpOptions::arguments() + youtubeArguments(query, count));
    return true;
}

void MediaSearch::setSpotifyCredentials(const QString &clientId, const QString &clientSecret)
{
    const QString id = clientId.trimmed();
    const QString secret = clientSecret.trimmed();
    if (id == m_clientId && secret == m_clientSecret)
        return;
    m_clientId = id;
    m_clientSecret = secret;
    // A token belongs to the app that asked for it.
    m_token.clear();
    m_tokenExpiry = {};
}

void MediaSearch::searchSpotify(const QString &query, const QList<Kind> &kinds, int offset)
{
    cancel();
    m_request = {query.simplified(), kinds, std::max(0, offset)};
    m_retried = false;
    m_spotifyBusy = true;
    if (!hasSpotifyCredentials()) {
        searchDeezer(tr("Results from Deezer's catalog: add Spotify API access to search Spotify itself. "
                        "Songs are found on YouTube and saved the same way."));
        return;
    }
    if (!m_token.isEmpty() && QDateTime::currentDateTimeUtc() < m_tokenExpiry)
        sendSpotifySearch();
    else
        requestToken();
}

void MediaSearch::requestToken()
{
    if (!m_network)
        m_network = new QNetworkAccessManager(this);
    QUrl url = m_accountsBase;
    url.setPath(QStringLiteral("/api/token"));
    QNetworkRequest request(url);
    request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/x-www-form-urlencoded"));
    request.setRawHeader("Authorization", "Basic " + (m_clientId + QLatin1Char(':') + m_clientSecret).toUtf8().toBase64());
    request.setTransferTimeout(kTimeoutMs);
    QNetworkReply *reply = m_network->post(request, QByteArrayLiteral("grant_type=client_credentials"));
    m_reply = reply;
    connect(reply, &QNetworkReply::finished, this, [this, reply] { onToken(reply); });
}

void MediaSearch::onToken(QNetworkReply *reply)
{
    reply->deleteLater();
    if (reply != m_reply)
        return;
    m_reply = nullptr;
    const QByteArray body = reply->readAll();
    const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    int expiresIn = 0;
    const QString token = status == 200 ? parseSpotifyToken(body, &expiresIn) : QString();
    if (token.isEmpty()) {
        Error error = Error::Failed;
        int retryAfter = 0;
        QString text = spotifyErrorText(reply, body, &error, &retryAfter);
        // The accounts service answers bad credentials with 400 or 401.
        const QString code = QJsonDocument::fromJson(body).object().value(QStringLiteral("error")).toString();
        if (status == 400 || status == 401 || code == QLatin1String("invalid_client")) {
            searchDeezer(tr("Spotify didn't accept the Client ID and Client Secret (check them under “Spotify API "
                            "access”). Showing results from Deezer's catalog instead."));
            return;
        }
        finishSpotify(error, text, retryAfter);
        return;
    }
    m_token = token;
    m_tokenExpiry = QDateTime::currentDateTimeUtc().addSecs(std::max(0, expiresIn - kTokenMarginSecs));
    sendSpotifySearch();
}

void MediaSearch::sendSpotifySearch()
{
    if (!m_network)
        m_network = new QNetworkAccessManager(this);
    QStringList types;
    for (Kind kind : std::as_const(m_request.kinds))
        types << QString::fromLatin1(typeName(kind));
    if (types.isEmpty())
        types << QStringLiteral("track");
    QUrl url = m_apiBase;
    url.setPath(QStringLiteral("/v1/search"));
    // Encoded by hand: QUrlQuery would leave a '+' in the words, read as a space.
    // (Concatenated: arg() would read the "%20"s of the words as markers.)
    url.setQuery(QStringLiteral("q=") + QString::fromLatin1(QUrl::toPercentEncoding(m_request.query))
                 + QStringLiteral("&type=") + types.join(QLatin1Char(','))
                 + QStringLiteral("&limit=") + QString::number(m_spotifyLimit)
                 + QStringLiteral("&offset=") + QString::number(m_request.offset));
    QNetworkRequest request(url);
    request.setRawHeader("Authorization", "Bearer " + m_token.toUtf8());
    request.setRawHeader("Accept", "application/json");
    request.setTransferTimeout(kTimeoutMs);
    QNetworkReply *reply = m_network->get(request);
    m_reply = reply;
    connect(reply, &QNetworkReply::finished, this, [this, reply] { onSpotifySearch(reply); });
}

void MediaSearch::onSpotifySearch(QNetworkReply *reply)
{
    reply->deleteLater();
    if (reply != m_reply)
        return;
    m_reply = nullptr;
    const QByteArray body = reply->readAll();
    const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    if (status == 401 && !m_retried) {
        // The token expired early or was revoked: get a new one, once.
        m_retried = true;
        m_token.clear();
        requestToken();
        return;
    }
    if (status == 400 && m_spotifyLimit > kSpotifySmallLimit && body.contains("limit")) {
        // Some apps may ask for fewer results per page.
        m_spotifyLimit = kSpotifySmallLimit;
        sendSpotifySearch();
        return;
    }
    if (status != 200) {
        Error error = Error::Failed;
        int retryAfter = 0;
        const QString text = spotifyErrorText(reply, body, &error, &retryAfter);
        if (status == 401 || status == 403) {
            m_token.clear();
            searchDeezer(tr("%1 Showing results from Deezer's catalog instead; songs are found on YouTube and saved "
                            "the same way.").arg(text));
            return;
        }
        finishSpotify(error, text, retryAfter);
        return;
    }
    Page page;
    page.source = Source::Spotify;
    page.query = m_request.query;
    page.offset = m_request.offset;
    page.results = parseSpotifySearch(body, &page.totals);
    m_spotifyBusy = false;
    Q_EMIT finished(page);
}

QString MediaSearch::spotifyErrorText(QNetworkReply *reply, const QByteArray &body, Error *error, int *retryAfter) const
{
    const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    const QJsonValue detail = QJsonDocument::fromJson(body).object().value(QStringLiteral("error"));
    QString message = detail.isObject() ? detail.toObject().value(QStringLiteral("message")).toString()
                                        : QJsonDocument::fromJson(body).object().value(QStringLiteral("error_description")).toString();
    // Some refusals come as plain text ("Active premium subscription required for the owner of the app.").
    if (message.isEmpty() && !body.trimmed().startsWith('{') && !body.trimmed().startsWith('<'))
        message = QString::fromUtf8(body).simplified().left(200);
    if (status == 0) {
        *error = Error::Network;
        return tr("Couldn't reach Spotify: %1").arg(reply->errorString());
    }
    if (status == 429) {
        *error = Error::RateLimited;
        *retryAfter = reply->rawHeader("Retry-After").trimmed().toInt();
        return *retryAfter > 0 ? tr("Spotify is limiting searches right now. Try again in %n second(s).", nullptr, *retryAfter)
                               : tr("Spotify is limiting searches right now. Try again in a little while.");
    }
    if (status == 401 || status == 403) {
        *error = Error::Credentials;
        // Since February 2026 Spotify answers only apps whose owner has Premium.
        if (status == 403 && message.isEmpty())
            return tr("Spotify refused the search (HTTP 403): it now answers only apps whose owner has Spotify Premium.");
        return message.isEmpty() ? tr("Spotify refused the search (HTTP %1).").arg(status)
                                 : tr("Spotify refused the search: %1.").arg(message.endsWith(QLatin1Char('.')) ? message.chopped(1) : message);
    }
    *error = Error::Failed;
    if (status >= 500)
        return tr("Spotify is having trouble (HTTP %1). Try again later.").arg(status);
    return message.isEmpty() ? tr("Spotify search failed (HTTP %1).").arg(status)
                             : tr("Spotify search failed: %1").arg(message);
}

void MediaSearch::finishSpotify(Error error, const QString &text, int retryAfter)
{
    m_spotifyBusy = false;
    Page page;
    page.source = Source::Spotify;
    page.query = m_request.query;
    page.offset = m_request.offset;
    page.error = error;
    page.errorText = text;
    page.retryAfter = retryAfter;
    Q_EMIT finished(page);
}

void MediaSearch::cancel()
{
    if (QProcess *process = m_process.data()) {
        m_process = nullptr;
        process->disconnect(this);
        process->kill();
        process->waitForFinished(1000);
        process->deleteLater();
    }
    m_spotifyBusy = false;
    if (QNetworkReply *reply = m_reply.data()) {
        m_reply = nullptr;
        reply->abort();
    }
    for (const QPointer<QNetworkReply> &reply : std::exchange(m_deezerReplies, {})) {
        if (reply) {
            reply->disconnect(this);
            reply->abort();
            reply->deleteLater();
        }
    }
}
