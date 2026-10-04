#pragma once

#include <QList>
#include <QObject>
#include <QPointer>
#include <QUrl>

class QNetworkAccessManager;
class QNetworkReply;

// Finds song lyrics online without any account or key: LRCLIB
// (lrclib.net, an open database of time-synced LRC lyrics), with
// lyrics.ovh as a fallback for plain lyrics when LRCLIB has nothing.
class LyricsClient : public QObject
{
    Q_OBJECT

public:
    struct Query {
        QString title;
        QString artist;
        QString album;
        double duration = 0; // seconds; 0 if unknown
    };

    struct Result {
        QString title;
        QString artist;
        QString album;
        double duration = 0;
        bool instrumental = false;
        QString syncedLyrics; // LRC, may be empty
        QString plainLyrics;
        QString source;       // "LRCLIB", "lyrics.ovh"

        bool isSynced() const { return !syncedLyrics.trimmed().isEmpty(); }
        // The best text to use: synced LRC if any, else the plain lyrics.
        QString text() const { return isSynced() ? syncedLyrics : plainLyrics; }
    };

    explicit LyricsClient(QObject *parent = nullptr);

    // https://lrclib.net, or $TOPPLAYER_LRCLIB_URL.
    QUrl lrclibUrl() const { return m_lrclib; }
    void setLrclibUrl(const QUrl &url) { m_lrclib = url; }
    // https://api.lyrics.ovh, or $TOPPLAYER_LYRICSOVH_URL.
    QUrl lyricsOvhUrl() const { return m_ovh; }
    void setLyricsOvhUrl(const QUrl &url) { m_ovh = url; }

    // Searches by title and artist; searchFinished() or failed() follows.
    // Results with synced lyrics and a duration close to the query's come first.
    void search(const Query &query);
    void cancel();
    bool isBusy() const { return !m_reply.isNull(); }

    // Parses LRCLIB's JSON (an array of records, or one record).
    static QList<Result> parseLrclib(const QByteArray &json);
    // Best matches first: synced, then by how close the duration is.
    static void rank(QList<Result> &results, double duration);

Q_SIGNALS:
    void searchFinished(const QList<LyricsClient::Result> &results);
    void failed(const QString &message);

private:
    void searchLrclib();
    void searchLyricsOvh();
    void track(QNetworkReply *reply);

    QNetworkAccessManager *m_network;
    QUrl m_lrclib;
    QUrl m_ovh;
    Query m_query;
    QPointer<QNetworkReply> m_reply;
};
