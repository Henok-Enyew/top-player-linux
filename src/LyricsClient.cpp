#include "LyricsClient.h"
#include "SubtitleSearch.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QUrlQuery>

#include <algorithm>
#include <cmath>

namespace {

constexpr int kTimeoutMs = 20000;

LyricsClient::Result fromRecord(const QJsonObject &record)
{
    LyricsClient::Result result;
    result.title = record.value(QStringLiteral("trackName")).toString();
    result.artist = record.value(QStringLiteral("artistName")).toString();
    result.album = record.value(QStringLiteral("albumName")).toString();
    result.duration = record.value(QStringLiteral("duration")).toDouble();
    result.instrumental = record.value(QStringLiteral("instrumental")).toBool();
    result.syncedLyrics = record.value(QStringLiteral("syncedLyrics")).toString();
    result.plainLyrics = record.value(QStringLiteral("plainLyrics")).toString();
    result.source = QStringLiteral("LRCLIB");
    return result;
}

} // namespace

LyricsClient::LyricsClient(QObject *parent)
    : QObject(parent)
    , m_network(new QNetworkAccessManager(this))
    , m_lrclib(QStringLiteral("https://lrclib.net"))
    , m_ovh(QStringLiteral("https://api.lyrics.ovh"))
{
    if (const QString url = qEnvironmentVariable("TOPPLAYER_LRCLIB_URL"); !url.isEmpty())
        m_lrclib = QUrl(url);
    if (const QString url = qEnvironmentVariable("TOPPLAYER_LYRICSOVH_URL"); !url.isEmpty())
        m_ovh = QUrl(url);
}

QList<LyricsClient::Result> LyricsClient::parseLrclib(const QByteArray &json)
{
    QList<Result> results;
    const QJsonDocument doc = QJsonDocument::fromJson(json);
    if (doc.isArray()) {
        for (const QJsonValue &value : doc.array()) {
            if (value.isObject())
                results.append(fromRecord(value.toObject()));
        }
    } else if (doc.isObject() && doc.object().contains(QStringLiteral("trackName"))) {
        results.append(fromRecord(doc.object()));
    }
    results.erase(std::remove_if(results.begin(), results.end(),
                                 [](const Result &r) { return r.text().trimmed().isEmpty() && !r.instrumental; }),
                  results.end());
    return results;
}

void LyricsClient::rank(QList<Result> &results, double duration)
{
    std::stable_sort(results.begin(), results.end(), [duration](const Result &a, const Result &b) {
        if (a.isSynced() != b.isSynced())
            return a.isSynced();
        if (duration <= 0)
            return false;
        return std::abs(a.duration - duration) < std::abs(b.duration - duration);
    });
}

void LyricsClient::track(QNetworkReply *reply)
{
    cancel();
    m_reply = reply;
}

void LyricsClient::cancel()
{
    if (QNetworkReply *reply = m_reply.data()) {
        m_reply = nullptr;
        reply->disconnect(this);
        reply->abort();
        reply->deleteLater();
    }
}

void LyricsClient::search(const Query &query)
{
    m_query = query;
    m_query.title = query.title.trimmed();
    m_query.artist = query.artist.trimmed();
    if (m_query.title.isEmpty()) {
        Q_EMIT failed(tr("Enter the song title"));
        return;
    }
    searchLrclib();
}

void LyricsClient::searchLrclib()
{
    QUrl url = m_lrclib;
    url.setPath(url.path() + QStringLiteral("/api/search"));
    QUrlQuery params;
    params.addQueryItem(QStringLiteral("track_name"), m_query.title);
    if (!m_query.artist.isEmpty())
        params.addQueryItem(QStringLiteral("artist_name"), m_query.artist);
    url.setQuery(params);

    QNetworkRequest request(url);
    // LRCLIB asks clients to identify themselves.
    request.setRawHeader("User-Agent", SubtitleSearch::userAgent() + " (https://github.com/Henok-Enyew/top-player-linux)");
    request.setTransferTimeout(kTimeoutMs);
    QNetworkReply *reply = m_network->get(request);
    track(reply);
    connect(reply, &QNetworkReply::finished, this, [this, reply] {
        reply->deleteLater();
        if (reply != m_reply)
            return;
        m_reply = nullptr;
        if (reply->error() != QNetworkReply::NoError) {
            // LRCLIB unreachable: plain lyrics are better than nothing.
            searchLyricsOvh();
            return;
        }
        QList<Result> results = parseLrclib(reply->readAll());
        if (results.isEmpty()) {
            searchLyricsOvh();
            return;
        }
        rank(results, m_query.duration);
        Q_EMIT searchFinished(results);
    });
}

void LyricsClient::searchLyricsOvh()
{
    if (m_query.artist.isEmpty()) {
        Q_EMIT searchFinished({});
        return;
    }
    QUrl url = m_ovh;
    // A slash in a name would read as another path segment.
    auto segment = [](QString text) { return text.replace(QLatin1Char('/'), QLatin1Char(' ')); };
    url.setPath(url.path() + QStringLiteral("/v1/") + segment(m_query.artist) + QLatin1Char('/') + segment(m_query.title));
    QNetworkRequest request(url);
    request.setRawHeader("User-Agent", SubtitleSearch::userAgent());
    request.setTransferTimeout(kTimeoutMs);
    QNetworkReply *reply = m_network->get(request);
    track(reply);
    connect(reply, &QNetworkReply::finished, this, [this, reply] {
        reply->deleteLater();
        if (reply != m_reply)
            return;
        m_reply = nullptr;
        const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        if (reply->error() != QNetworkReply::NoError && status != 404) {
            Q_EMIT failed(tr("Could not reach the lyrics services (%1)").arg(reply->errorString()));
            return;
        }
        const QString lyrics = QJsonDocument::fromJson(reply->readAll()).object().value(QStringLiteral("lyrics")).toString();
        if (lyrics.trimmed().isEmpty()) {
            Q_EMIT searchFinished({});
            return;
        }
        Result result;
        result.title = m_query.title;
        result.artist = m_query.artist;
        result.plainLyrics = lyrics.trimmed();
        result.source = QStringLiteral("lyrics.ovh");
        Q_EMIT searchFinished({result});
    });
}
