// Search YouTube... and Search Spotify...: the yt-dlp search and Spotify Web
// API parsers, the Spotify token flow and its errors against a local stand-in
// server, and both dialogs driving a stand-in yt-dlp (tests have no network).
// Needs a display (run under xvfb-run).

#include "DownloadQueue.h"
#include "EmptyStateWidget.h"
#include "MainWindow.h"
#include "MediaDownloader.h"
#include "MediaSearch.h"
#include "MediaSearchDialog.h"
#include "MpvWidget.h"
#include "PlaylistSession.h"
#include "ResumeManager.h"

#include <QApplication>
#include <QBuffer>
#include <QCheckBox>
#include <QComboBox>
#include <QFile>
#include <QImage>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMessageBox>
#include <QProgressBar>
#include <QPushButton>
#include <QSettings>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QTabBar>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>
#include <QToolButton>
#include <QTreeWidget>
#include <QUrlQuery>

#include <clocale>
#include <functional>

namespace {

bool writeScript(const QString &path, const QByteArray &body)
{
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly))
        return false;
    file.write("#!/bin/sh\n" + body);
    file.close();
    return file.setPermissions(file.permissions() | QFileDevice::ExeOwner | QFileDevice::ExeUser);
}

// Stand-in for yt-dlp: appends its arguments to $FAKE_LOG. A search
// ("ytsearchN:query" with --dump-single-json) prints N videos named after
// the query ("broken" fails); otherwise saves $FAKE_SOURCE under the -o name
// in the -P folder (as .mp3 with -x, else .mp4) and prints it. Names with
// "slow" take long.
const QByteArray kFakeYtDlp =
    "printf '%s\\n' \"$@\" >> \"$FAKE_LOG\"\n"
    "echo '----' >> \"$FAKE_LOG\"\n"
    "dir=''; out=''; prev=''; last=''; json=no; strict=no; audio=no\n"
    "for a in \"$@\"; do\n"
    "  if [ \"$prev\" = \"-P\" ]; then dir=\"$a\"; fi\n"
    "  if [ \"$prev\" = \"-o\" ]; then out=\"$a\"; fi\n"
    "  if [ \"$a\" = \"--dump-single-json\" ]; then json=yes; fi\n"
    "  if [ \"$a\" = \"--match-filter\" ]; then strict=yes; fi\n"
    "  if [ \"$a\" = \"-x\" ]; then audio=yes; fi\n"
    "  prev=\"$a\"; last=\"$a\"\n"
    "done\n"
    "if [ $json = yes ]; then\n"
    "  n=$(printf '%s' \"$last\" | sed 's/^ytsearch\\([0-9]*\\):.*/\\1/')\n"
    "  q=$(printf '%s' \"$last\" | sed 's/^ytsearch[0-9]*://')\n"
    "  case \"$q\" in *broken*) echo 'ERROR: Unable to download API page' >&2; exit 1;; esac\n"
    "  printf '{\"_type\":\"playlist\",\"id\":\"%s\",\"entries\":[' \"$q\"\n"
    "  i=1\n"
    "  while [ $i -le $n ]; do\n"
    "    if [ $i -gt 1 ]; then printf ','; fi\n"
    "    printf '{\"_type\":\"url\",\"ie_key\":\"Youtube\",\"id\":\"vid%03d\",\"url\":\"https://www.youtube.com/watch?v=vid%03d\",'"
    " $i $i\n"
    "    printf '\"title\":\"%s %d\",\"channel\":\"Channel %d\",\"duration\":%d,\"view_count\":%d}' \"$q\" $i $i $((60 + i)) $((i * 1500))\n"
    "    i=$((i + 1))\n"
    "  done\n"
    "  echo ']}'\n"
    "  exit 0\n"
    "fi\n"
    "ext=mp4; if [ $audio = yes ]; then ext=mp3; fi\n"
    "name=$(printf '%s' \"$out\" | sed \"s/%(ext)s/$ext/\")\n"
    "echo '[download]  50.0% of   1.00MiB at    1.00MiB/s ETA 00:01' >&2\n"
    "case \"$name\" in *slow*) sleep 30;; esac\n"
    "sleep 0.2\n"
    "cp \"$FAKE_SOURCE\" \"$dir/$name\"\n"
    "echo \"$dir/$name\"\n"
    "if [ $strict = yes ]; then exit 101; fi\n";

// A local HTTP server: `handler` answers each request.
class FakeHttp : public QObject
{
public:
    struct Request {
        QByteArray method;
        QString path;
        QUrlQuery query;
        QHash<QByteArray, QByteArray> headers; // lower-case names
        QByteArray body;
    };
    struct Response {
        int status = 200;
        QByteArray body;
        QByteArray contentType = "application/json";
        QList<QPair<QByteArray, QByteArray>> headers;
    };

    std::function<Response(const Request &)> handler;
    QList<Request> requests;

    bool listen() { return m_server.listen(QHostAddress::LocalHost); }
    QUrl url() const { return QUrl(QStringLiteral("http://127.0.0.1:%1").arg(m_server.serverPort())); }
    int count(const QString &path) const
    {
        return int(std::count_if(requests.cbegin(), requests.cend(), [&](const Request &r) { return r.path == path; }));
    }

    FakeHttp()
    {
        connect(&m_server, &QTcpServer::newConnection, this, [this] {
            while (QTcpSocket *socket = m_server.nextPendingConnection()) {
                auto buffer = std::make_shared<QByteArray>();
                connect(socket, &QTcpSocket::readyRead, this, [this, socket, buffer] {
                    *buffer += socket->readAll();
                    const qsizetype end = buffer->indexOf("\r\n\r\n");
                    if (end < 0)
                        return;
                    Request request;
                    const QList<QByteArray> lines = buffer->left(end).split('\n');
                    const QList<QByteArray> first = lines.value(0).trimmed().split(' ');
                    request.method = first.value(0);
                    const QUrl target(QString::fromLatin1(first.value(1)));
                    request.path = target.path();
                    request.query = QUrlQuery(target.query());
                    for (qsizetype i = 1; i < lines.size(); ++i) {
                        const qsizetype colon = lines[i].indexOf(':');
                        if (colon > 0)
                            request.headers.insert(lines[i].left(colon).trimmed().toLower(), lines[i].mid(colon + 1).trimmed());
                    }
                    const int length = request.headers.value("content-length").toInt();
                    if (buffer->size() < end + 4 + length)
                        return;
                    request.body = buffer->mid(end + 4, length);
                    buffer->clear();
                    requests.append(request);
                    const Response response = handler ? handler(request) : Response{404, {}, "text/plain", {}};
                    QByteArray out = "HTTP/1.1 " + QByteArray::number(response.status) + " X\r\nContent-Type: "
                                     + response.contentType + "\r\nContent-Length: " + QByteArray::number(response.body.size())
                                     + "\r\nConnection: close\r\n";
                    for (const auto &[name, value] : response.headers)
                        out += name + ": " + value + "\r\n";
                    socket->write(out + "\r\n" + response.body);
                    socket->disconnectFromHost();
                });
                connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
            }
        });
    }

private:
    QTcpServer m_server;
};

QByteArray pngBytes(const QColor &color)
{
    QImage image(32, 32, QImage::Format_RGB32);
    image.fill(color);
    QByteArray data;
    QBuffer buffer(&data);
    buffer.open(QIODevice::WriteOnly);
    image.save(&buffer, "PNG");
    return data;
}

// What yt-dlp --flat-playlist --dump-single-json prints for "ytsearch3:...":
// a video, a live stream with an uploader only, and a channel (skipped).
const QByteArray kYouTubeJson = "{\"_type\":\"playlist\",\"id\":\"believer\",\"title\":\"believer\",\"entries\":["
    " {\"_type\":\"url\",\"ie_key\":\"Youtube\",\"id\":\"7wtfhZwyrcc\",\"url\":\"https://www.youtube.com/watch?v=7wtfhZwyrcc\","
    "  \"title\":\"Imagine Dragons - Believer (Official Music Video)\",\"channel\":\"ImagineDragons\",\"duration\":217.0,"
    "  \"view_count\":2712345678,\"thumbnails\":[{\"url\":\"https://i.ytimg.com/vi/7wtfhZwyrcc/hq720.jpg\",\"height\":404,\"width\":720}]},"
    " {\"_type\":\"url\",\"ie_key\":\"Youtube\",\"id\":\"live0000001\",\"url\":\"https://www.youtube.com/watch?v=live0000001\","
    "  \"title\":\"Believer radio  24/7\",\"uploader\":\"Lofi Girl\",\"duration\":null,\"live_status\":\"is_live\",\"view_count\":950},"
    " {\"_type\":\"url\",\"ie_key\":\"YoutubeTab\",\"id\":\"UC123\",\"url\":\"https://www.youtube.com/channel/UC123\",\"title\":\"Some Channel\"}"
    "]}";

// A Spotify /v1/search answer with a song, an album and playlists (one of
// them a hole, as Spotify sends).
const QByteArray kSpotifyJson = "{"
    " \"tracks\":{\"href\":\"x\",\"limit\":20,\"offset\":0,\"total\":120,\"items\":["
    "  {\"id\":\"0pqnGHJpmpxLKifKRmU6WP\",\"name\":\"Believer\",\"duration_ms\":204346,"
    "   \"artists\":[{\"name\":\"Imagine Dragons\"},{\"name\":\"Lil Wayne\"}],"
    "   \"album\":{\"name\":\"Evolve\",\"images\":[{\"url\":\"IMG/640.jpg\",\"width\":640,\"height\":640},"
    "                                      {\"url\":\"IMG/300.jpg\",\"width\":300,\"height\":300},"
    "                                      {\"url\":\"IMG/64.jpg\",\"width\":64,\"height\":64}]},"
    "   \"external_urls\":{\"spotify\":\"https://open.spotify.com/track/0pqnGHJpmpxLKifKRmU6WP\"}},"
    "  {\"id\":\"5Z9KJZvQzH6PFmb8SNkxuk\",\"name\":\"Thunder\",\"duration_ms\":187146,\"artists\":[{\"name\":\"Imagine Dragons\"}],"
    "   \"album\":{\"name\":\"Evolve\",\"images\":[]}}]},"
    " \"albums\":{\"total\":7,\"items\":["
    "  {\"id\":\"5nFgsH0m3gUo2O4yEBPgrr\",\"name\":\"Evolve\",\"total_tracks\":11,\"release_date\":\"2017-06-23\","
    "   \"artists\":[{\"name\":\"Imagine Dragons\"}],\"images\":[{\"url\":\"IMG/a300.jpg\",\"width\":300,\"height\":300}]}]},"
    " \"playlists\":{\"total\":40,\"items\":[null,"
    "  {\"id\":\"37i9dQZF1DX1lVhptIYRda\",\"name\":\"Hot Hits\",\"owner\":{\"display_name\":\"Spotify\"},\"tracks\":{\"total\":50},"
    "   \"images\":[{\"url\":\"IMG/p.jpg\",\"width\":null,\"height\":null}]},"
    "  {\"id\":\"2rCVYYbIUtWMAzKs5V7Gdk\",\"name\":\"New Style\",\"owner\":{\"display_name\":\"Fan\"},\"items\":{\"total\":12},\"images\":[]}]}"
    "}";

QByteArray tokenJson(const QByteArray &token, int expiresIn = 3600)
{
    return R"({"access_token":")" + token + R"(","token_type":"Bearer","expires_in":)" + QByteArray::number(expiresIn) + "}";
}

} // namespace

class SearchTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void initTestCase();
    void init();
    void cleanup();

    // Parsers
    void youtubeArguments();
    void parseYouTube();
    void parseSpotify();
    void formatting();

    // Spotify Web API
    void spotifyTokenAndSearch();
    void spotifyMissingCredentials();
    void spotifyBadCredentials();
    void spotifyRateLimited();
    void spotifyExpiredToken();
    void spotifyNetworkError();
    void spotifySmallerPages();

    // Dialogs
    void menuAndStartScreen();
    void missingYtDlp();
    void youtubeSearchDialog();
    void youtubeSearchError();
    void youtubePlayAndQueue();
    void youtubeDownloads();
    void closeWhileDownloading();
    void spotifySearchDialog();

private:
    QVariant prop(const char *name) const { return m_mpv->mpvProperty(QString::fromLatin1(name)); }
    void prependPath(const QString &dir) { qputenv("PATH", (dir + QLatin1Char(':')).toLocal8Bit() + m_path); }
    QStringList loggedArgs() const;
    MediaSearchDialog *openDialog(MediaSearch::Source source);
    void selectRows(MediaSearchDialog *dialog, const QList<int> &rows);
    // Saves `dialog` into $TOPPLAYER_SCREENSHOTS, if set.
    void screenshot(QWidget *dialog, const QString &name);
    QStringList playlistTitles() const;

    QTemporaryDir m_dir;
    QString m_source;
    QString m_fakeBin;
    QString m_emptyBin;
    QByteArray m_path;
    FakeHttp *m_art = nullptr;
    MainWindow *m_window = nullptr;
    MpvWidget *m_mpv = nullptr;
};

void SearchTest::initTestCase()
{
    QVERIFY(m_dir.isValid());
    m_path = qgetenv("PATH");
    m_fakeBin = m_dir.filePath(QStringLiteral("fake-bin"));
    m_emptyBin = m_dir.filePath(QStringLiteral("empty-bin"));
    QVERIFY(QDir().mkpath(m_fakeBin) && QDir().mkpath(m_emptyBin));
    QVERIFY(writeScript(m_fakeBin + QStringLiteral("/yt-dlp"), kFakeYtDlp));
    // Downloads are copies of this; a short WAV mpv can play.
    m_source = m_dir.filePath(QStringLiteral("source.wav"));
    QFile wav(m_source);
    QVERIFY(wav.open(QIODevice::WriteOnly));
    const quint32 samples = 8000;
    QByteArray header("RIFF");
    auto le32 = [](quint32 v) { return QByteArray(reinterpret_cast<const char *>(&v), 4); };
    auto le16 = [](quint16 v) { return QByteArray(reinterpret_cast<const char *>(&v), 2); };
    header += le32(36 + samples) + "WAVEfmt " + le32(16) + le16(1) + le16(1) + le32(8000) + le32(8000) + le16(1) + le16(8)
              + "data" + le32(samples);
    wav.write(header + QByteArray(int(samples), char(0x80)));
    wav.close();
    qputenv("FAKE_LOG", m_dir.filePath(QStringLiteral("yt-dlp-args.txt")).toLocal8Bit());
    qputenv("FAKE_SOURCE", m_source.toLocal8Bit());

    // Thumbnails and covers.
    m_art = new FakeHttp;
    m_art->handler = [](const FakeHttp::Request &) {
        return FakeHttp::Response{200, pngBytes(Qt::red), "image/png", {}};
    };
    QVERIFY(m_art->listen());
}

void SearchTest::init()
{
    qputenv("PATH", m_path);
    QFile::remove(QString::fromLocal8Bit(qgetenv("FAKE_LOG")));
    m_window = new MainWindow;
    m_window->resize(800, 450);
    m_window->show();
    m_window->activateWindow();
    QVERIFY(QTest::qWaitForWindowActive(m_window));
    m_mpv = m_window->findChild<MpvWidget *>();
    QVERIFY(m_mpv);
    m_mpv->setMpvProperty(QStringLiteral("ao"), QStringLiteral("null"));
    // Streams are never opened for real: there is no network.
    m_mpv->setMpvProperty(QStringLiteral("ytdl"), QStringLiteral("no"));
}

void SearchTest::cleanup()
{
    qputenv("PATH", m_path);
    while (QWidget *modal = QApplication::activeModalWidget())
        delete modal;
    delete m_window;
    m_window = nullptr;
    m_mpv = nullptr;
}

QStringList SearchTest::loggedArgs() const
{
    QFile log(QString::fromLocal8Bit(qgetenv("FAKE_LOG")));
    if (!log.open(QIODevice::ReadOnly))
        return {};
    return QString::fromUtf8(log.readAll()).split(QLatin1Char('\n'), Qt::SkipEmptyParts);
}

MediaSearchDialog *SearchTest::openDialog(MediaSearch::Source source)
{
    m_window->openMediaSearchDialog(source);
    MediaSearchDialog *dialog = m_window->mediaSearchDialog(source);
    if (dialog) {
        dialog->search()->setYouTubeThumbnailUrl(m_art->url());
        dialog->downloads()->setDirectory(m_dir.filePath(QStringLiteral("unused")));
    }
    return dialog;
}

void SearchTest::selectRows(MediaSearchDialog *dialog, const QList<int> &rows)
{
    dialog->view()->clearSelection();
    for (int row : rows)
        dialog->view()->topLevelItem(row)->setSelected(true);
}

void SearchTest::screenshot(QWidget *dialog, const QString &name)
{
    const QString screenshots = qEnvironmentVariable("TOPPLAYER_SCREENSHOTS");
    if (screenshots.isEmpty())
        return;
    QDir().mkpath(screenshots);
    dialog->grab().save(QStringLiteral("%1/%2.png").arg(screenshots, name));
}

QStringList SearchTest::playlistTitles() const
{
    QStringList titles;
    for (const QVariant &entry : prop("playlist").toList())
        titles << entry.toMap().value(QStringLiteral("title")).toString();
    return titles;
}

// ---- Parsers ----------------------------------------------------------------

void SearchTest::youtubeArguments()
{
    const QStringList args = MediaSearch::youtubeArguments(QStringLiteral("  imagine   dragons "), 20);
    QVERIFY(args.contains(QStringLiteral("--flat-playlist")));
    QVERIFY(args.contains(QStringLiteral("--dump-single-json")));
    QVERIFY(args.contains(QStringLiteral("--no-warnings")));
    // The query comes last, after "--", so it is never read as an option.
    QCOMPARE(args.at(args.size() - 2), QStringLiteral("--"));
    QCOMPARE(args.last(), QStringLiteral("ytsearch20:imagine dragons"));
    QCOMPARE(MediaSearch::youtubeArguments(QStringLiteral("-x"), 40).last(), QStringLiteral("ytsearch40:-x"));
}

void SearchTest::parseYouTube()
{
    const QList<MediaSearch::Result> results = MediaSearch::parseYouTubeSearch(kYouTubeJson);
    QCOMPARE(results.size(), 2);
    const MediaSearch::Result &video = results[0];
    QCOMPARE(video.kind, MediaSearch::Kind::Video);
    QCOMPARE(video.id, QStringLiteral("7wtfhZwyrcc"));
    QCOMPARE(video.title, QStringLiteral("Imagine Dragons - Believer (Official Music Video)"));
    QCOMPARE(video.artists, QStringLiteral("ImagineDragons"));
    QCOMPARE(video.duration, 217.0);
    QCOMPARE(video.views, qint64(2712345678));
    QCOMPARE(video.url, QStringLiteral("https://www.youtube.com/watch?v=7wtfhZwyrcc"));
    QCOMPARE(video.thumbnail, QUrl(QStringLiteral("https://i.ytimg.com/vi/7wtfhZwyrcc/mqdefault.jpg")));
    QVERIFY(!video.live);
    const MediaSearch::Result &live = results[1];
    QCOMPARE(live.title, QStringLiteral("Believer radio 24/7"));
    QCOMPARE(live.artists, QStringLiteral("Lofi Girl"));
    QVERIFY(live.live);
    QCOMPARE(live.duration, 0.0);
    // Thumbnails can come from elsewhere (tests).
    const auto local = MediaSearch::parseYouTubeSearch(kYouTubeJson, QUrl(QStringLiteral("http://127.0.0.1:1")));
    QCOMPARE(local[0].thumbnail, QUrl(QStringLiteral("http://127.0.0.1:1/vi/7wtfhZwyrcc/mqdefault.jpg")));
    QVERIFY(MediaSearch::parseYouTubeSearch("not json").isEmpty());
    QVERIFY(MediaSearch::parseYouTubeSearch(R"({"entries":[]})").isEmpty());
}

void SearchTest::parseSpotify()
{
    QMap<MediaSearch::Kind, int> totals;
    const QList<MediaSearch::Result> results = MediaSearch::parseSpotifySearch(kSpotifyJson, &totals);
    QCOMPARE(totals.value(MediaSearch::Kind::Track), 120);
    QCOMPARE(totals.value(MediaSearch::Kind::Album), 7);
    QCOMPARE(totals.value(MediaSearch::Kind::Playlist), 40);
    QCOMPARE(results.size(), 5);

    const MediaSearch::Result &song = results[0];
    QCOMPARE(song.kind, MediaSearch::Kind::Track);
    QCOMPARE(song.title, QStringLiteral("Believer"));
    QCOMPARE(song.artists, QStringLiteral("Imagine Dragons, Lil Wayne"));
    QCOMPARE(song.album, QStringLiteral("Evolve"));
    QVERIFY(qAbs(song.duration - 204.346) < 0.001);
    QCOMPARE(song.url, QStringLiteral("https://open.spotify.com/track/0pqnGHJpmpxLKifKRmU6WP"));
    // The 300 px cover: big enough, but not the largest.
    QCOMPARE(song.thumbnail.toString(), QStringLiteral("IMG/300.jpg"));
    QCOMPARE(song.displayName(), QStringLiteral("Imagine Dragons, Lil Wayne - Believer"));
    const MediaDownloader::SpotifyTrack track = song.track();
    QCOMPARE(track.title, QStringLiteral("Believer"));
    QCOMPARE(track.artist, QStringLiteral("Imagine Dragons, Lil Wayne"));
    QCOMPARE(track.album, QStringLiteral("Evolve"));
    QVERIFY(qAbs(track.duration - 204.346) < 0.001);
    // Without external_urls the link is built from the id.
    QCOMPARE(results[1].url, QStringLiteral("https://open.spotify.com/track/5Z9KJZvQzH6PFmb8SNkxuk"));
    QVERIFY(results[1].thumbnail.isEmpty());

    const MediaSearch::Result &album = results[2];
    QCOMPARE(album.kind, MediaSearch::Kind::Album);
    QCOMPARE(album.title, QStringLiteral("Evolve"));
    QCOMPARE(album.artists, QStringLiteral("Imagine Dragons"));
    QCOMPARE(album.trackCount, 11);
    QCOMPARE(album.released, QStringLiteral("2017"));
    QCOMPARE(album.url, QStringLiteral("https://open.spotify.com/album/5nFgsH0m3gUo2O4yEBPgrr"));

    // The null playlist is skipped; "tracks" and "items" both give the count.
    QCOMPARE(results[3].kind, MediaSearch::Kind::Playlist);
    QCOMPARE(results[3].title, QStringLiteral("Hot Hits"));
    QCOMPARE(results[3].artists, QStringLiteral("Spotify"));
    QCOMPARE(results[3].trackCount, 50);
    QCOMPARE(results[3].thumbnail.toString(), QStringLiteral("IMG/p.jpg"));
    QCOMPARE(results[4].trackCount, 12);
    QCOMPARE(results[4].url, QStringLiteral("https://open.spotify.com/playlist/2rCVYYbIUtWMAzKs5V7Gdk"));

    // A page of one kind.
    totals.clear();
    const auto albums = MediaSearch::parseSpotifySearch(R"({"albums":{"total":3,"items":[]}})", &totals);
    QVERIFY(albums.isEmpty());
    QCOMPARE(totals.keys(), QList<MediaSearch::Kind>{MediaSearch::Kind::Album});

    int expires = 0;
    QCOMPARE(MediaSearch::parseSpotifyToken(tokenJson("abc", 1800), &expires), QStringLiteral("abc"));
    QCOMPARE(expires, 1800);
    QVERIFY(MediaSearch::parseSpotifyToken(R"({"error":"invalid_client"})").isEmpty());
}

void SearchTest::formatting()
{
    QCOMPARE(MediaSearch::formatDuration(0), QString());
    QCOMPARE(MediaSearch::formatDuration(61), QStringLiteral("1:01"));
    QCOMPARE(MediaSearch::formatDuration(204.346), QStringLiteral("3:24"));
    QCOMPARE(MediaSearch::formatDuration(3723), QStringLiteral("1:02:03"));
    QCOMPARE(MediaSearch::formatViews(-1), QString());
    QCOMPARE(MediaSearch::formatViews(1), QStringLiteral("1 view"));
    QCOMPARE(MediaSearch::formatViews(950), QStringLiteral("950 views"));
    QCOMPARE(MediaSearch::formatViews(1500), QStringLiteral("1.5K views"));
    QCOMPARE(MediaSearch::formatViews(12000), QStringLiteral("12K views"));
    QCOMPARE(MediaSearch::formatViews(2712345678), QStringLiteral("2.7B views"));
    QCOMPARE(MediaSearch::formatViews(1000000), QStringLiteral("1M views"));
}

// ---- Spotify Web API --------------------------------------------------------

void SearchTest::spotifyTokenAndSearch()
{
    FakeHttp server;
    QVERIFY(server.listen());
    server.handler = [](const FakeHttp::Request &request) -> FakeHttp::Response {
        if (request.path == QLatin1String("/api/token")) {
            if (request.method != "POST" || request.headers.value("authorization") != "Basic " + QByteArray("my-id:my-secret").toBase64()
                || request.body != "grant_type=client_credentials"
                || !request.headers.value("content-type").startsWith("application/x-www-form-urlencoded"))
                return {400, R"({"error":"invalid_request"})", "application/json", {}};
            return {200, tokenJson("tok-1"), "application/json", {}};
        }
        if (request.path == QLatin1String("/v1/search") && request.headers.value("authorization") == "Bearer tok-1")
            return {200, kSpotifyJson, "application/json", {}};
        return {401, R"({"error":{"status":401,"message":"No token provided"}})", "application/json", {}};
    };
    MediaSearch search;
    search.setSpotifyAccountsUrl(server.url());
    search.setSpotifyApiUrl(server.url());
    search.setSpotifyCredentials(QStringLiteral(" my-id "), QStringLiteral("my-secret"));
    QVERIFY(search.hasSpotifyCredentials());
    QSignalSpy finished(&search, &MediaSearch::finished);
    search.searchSpotify(QStringLiteral("believer & thunder+1"),
                         {MediaSearch::Kind::Track, MediaSearch::Kind::Album, MediaSearch::Kind::Playlist});
    QVERIFY(search.isBusy());
    QTRY_COMPARE_WITH_TIMEOUT(finished.size(), 1, 10000);
    QVERIFY(!search.isBusy());
    auto page = finished.at(0).at(0).value<MediaSearch::Page>();
    QCOMPARE(page.error, MediaSearch::Error::None);
    QCOMPARE(page.source, MediaSearch::Source::Spotify);
    QCOMPARE(page.results.size(), 5);
    QCOMPARE(page.totals.value(MediaSearch::Kind::Track), 120);
    QCOMPARE(server.count(QStringLiteral("/api/token")), 1);
    const FakeHttp::Request &request = server.requests.last();
    QCOMPARE(request.method, QByteArray("GET"));
    // '&' and '+' stay part of the words.
    QCOMPARE(request.query.queryItemValue(QStringLiteral("q"), QUrl::FullyDecoded), QStringLiteral("believer & thunder+1"));
    QCOMPARE(request.query.queryItemValue(QStringLiteral("type")), QStringLiteral("track,album,playlist"));
    QCOMPARE(request.query.queryItemValue(QStringLiteral("limit")), QStringLiteral("20"));
    QCOMPARE(request.query.queryItemValue(QStringLiteral("offset")), QStringLiteral("0"));

    // The token is kept for the next search (a page of albums).
    search.searchSpotify(QStringLiteral("believer"), {MediaSearch::Kind::Album}, 20);
    QTRY_COMPARE_WITH_TIMEOUT(finished.size(), 2, 10000);
    page = finished.at(1).at(0).value<MediaSearch::Page>();
    QCOMPARE(page.error, MediaSearch::Error::None);
    QCOMPARE(page.offset, 20);
    QCOMPARE(server.count(QStringLiteral("/api/token")), 1);
    QCOMPARE(server.requests.last().query.queryItemValue(QStringLiteral("type")), QStringLiteral("album"));
    QCOMPARE(server.requests.last().query.queryItemValue(QStringLiteral("offset")), QStringLiteral("20"));

    // Other credentials need their own token.
    search.setSpotifyCredentials(QStringLiteral("other"), QStringLiteral("secret"));
    search.searchSpotify(QStringLiteral("believer"), {MediaSearch::Kind::Track});
    QTRY_COMPARE_WITH_TIMEOUT(finished.size(), 3, 10000);
    QCOMPARE(server.count(QStringLiteral("/api/token")), 2);
    QCOMPARE(finished.at(2).at(0).value<MediaSearch::Page>().error, MediaSearch::Error::Credentials);
}

void SearchTest::spotifyMissingCredentials()
{
    MediaSearch search;
    search.setSpotifyAccountsUrl(QUrl(QStringLiteral("http://127.0.0.1:1")));
    QSignalSpy finished(&search, &MediaSearch::finished);
    search.searchSpotify(QStringLiteral("believer"), {MediaSearch::Kind::Track});
    QCOMPARE(finished.size(), 1);
    const auto page = finished.at(0).at(0).value<MediaSearch::Page>();
    QCOMPARE(page.error, MediaSearch::Error::Credentials);
    QVERIFY(page.errorText.contains(QLatin1String("Client ID")));
    QVERIFY(!search.isBusy());
}

void SearchTest::spotifyBadCredentials()
{
    FakeHttp server;
    QVERIFY(server.listen());
    server.handler = [](const FakeHttp::Request &) {
        return FakeHttp::Response{400, R"({"error":"invalid_client","error_description":"Invalid client"})", "application/json", {}};
    };
    MediaSearch search;
    search.setSpotifyAccountsUrl(server.url());
    search.setSpotifyApiUrl(server.url());
    search.setSpotifyCredentials(QStringLiteral("id"), QStringLiteral("wrong"));
    QSignalSpy finished(&search, &MediaSearch::finished);
    search.searchSpotify(QStringLiteral("believer"), {MediaSearch::Kind::Track});
    QTRY_COMPARE_WITH_TIMEOUT(finished.size(), 1, 10000);
    const auto page = finished.at(0).at(0).value<MediaSearch::Page>();
    QCOMPARE(page.error, MediaSearch::Error::Credentials);
    QVERIFY2(page.errorText.contains(QLatin1String("didn't accept")), qPrintable(page.errorText));
    QCOMPARE(server.count(QStringLiteral("/v1/search")), 0);
}

void SearchTest::spotifyRateLimited()
{
    FakeHttp server;
    QVERIFY(server.listen());
    server.handler = [](const FakeHttp::Request &request) -> FakeHttp::Response {
        if (request.path == QLatin1String("/api/token"))
            return {200, tokenJson("tok"), "application/json", {}};
        return {429, R"({"error":{"status":429,"message":"API rate limit exceeded"}})", "application/json", {{"Retry-After", "7"}}};
    };
    MediaSearch search;
    search.setSpotifyAccountsUrl(server.url());
    search.setSpotifyApiUrl(server.url());
    search.setSpotifyCredentials(QStringLiteral("id"), QStringLiteral("secret"));
    QSignalSpy finished(&search, &MediaSearch::finished);
    search.searchSpotify(QStringLiteral("believer"), {MediaSearch::Kind::Track});
    QTRY_COMPARE_WITH_TIMEOUT(finished.size(), 1, 10000);
    const auto page = finished.at(0).at(0).value<MediaSearch::Page>();
    QCOMPARE(page.error, MediaSearch::Error::RateLimited);
    QCOMPARE(page.retryAfter, 7);
    QVERIFY2(page.errorText.contains(QLatin1String("7 second")), qPrintable(page.errorText));
}

void SearchTest::spotifyExpiredToken()
{
    FakeHttp server;
    QVERIFY(server.listen());
    int tokens = 0;
    server.handler = [&tokens](const FakeHttp::Request &request) -> FakeHttp::Response {
        if (request.path == QLatin1String("/api/token"))
            return {200, tokenJson("tok-" + QByteArray::number(++tokens)), "application/json", {}};
        // The first token is refused, as an expired one is.
        if (request.headers.value("authorization") == "Bearer tok-1")
            return {401, R"({"error":{"status":401,"message":"The access token expired"}})", "application/json",
                    {{"WWW-Authenticate", "Bearer realm=\"spotify\""}}};
        return {200, kSpotifyJson, "application/json", {}};
    };
    MediaSearch search;
    search.setSpotifyAccountsUrl(server.url());
    search.setSpotifyApiUrl(server.url());
    search.setSpotifyCredentials(QStringLiteral("id"), QStringLiteral("secret"));
    QSignalSpy finished(&search, &MediaSearch::finished);
    search.searchSpotify(QStringLiteral("believer"), {MediaSearch::Kind::Track});
    QTRY_COMPARE_WITH_TIMEOUT(finished.size(), 1, 10000);
    const auto page = finished.at(0).at(0).value<MediaSearch::Page>();
    QCOMPARE(page.error, MediaSearch::Error::None);
    QCOMPARE(page.results.size(), 5);
    QCOMPARE(tokens, 2);

    // A token refused again isn't retried for ever.
    server.handler = [](const FakeHttp::Request &request) -> FakeHttp::Response {
        if (request.path == QLatin1String("/api/token"))
            return {200, tokenJson("tok-x"), "application/json", {}};
        return {401, R"({"error":{"status":401,"message":"Invalid access token"}})", "application/json", {}};
    };
    search.searchSpotify(QStringLiteral("believer"), {MediaSearch::Kind::Track});
    QTRY_COMPARE_WITH_TIMEOUT(finished.size(), 2, 10000);
    const auto refused = finished.at(1).at(0).value<MediaSearch::Page>();
    QCOMPARE(refused.error, MediaSearch::Error::Credentials);
    QVERIFY2(refused.errorText.contains(QLatin1String("Invalid access token")), qPrintable(refused.errorText));
}

void SearchTest::spotifyNetworkError()
{
    // Nothing listens there.
    QTcpServer closed;
    QVERIFY(closed.listen(QHostAddress::LocalHost));
    const QUrl url(QStringLiteral("http://127.0.0.1:%1").arg(closed.serverPort()));
    closed.close();
    MediaSearch search;
    search.setSpotifyAccountsUrl(url);
    search.setSpotifyApiUrl(url);
    search.setSpotifyCredentials(QStringLiteral("id"), QStringLiteral("secret"));
    QSignalSpy finished(&search, &MediaSearch::finished);
    search.searchSpotify(QStringLiteral("believer"), {MediaSearch::Kind::Track});
    QTRY_COMPARE_WITH_TIMEOUT(finished.size(), 1, 10000);
    const auto page = finished.at(0).at(0).value<MediaSearch::Page>();
    QCOMPARE(page.error, MediaSearch::Error::Network);
    QVERIFY2(page.errorText.startsWith(QLatin1String("Couldn't reach Spotify")), qPrintable(page.errorText));

    // Cancelled: nothing follows.
    search.searchSpotify(QStringLiteral("believer"), {MediaSearch::Kind::Track});
    search.cancel();
    QTest::qWait(300);
    QCOMPARE(finished.size(), 1);
}

void SearchTest::spotifySmallerPages()
{
    FakeHttp server;
    QVERIFY(server.listen());
    server.handler = [](const FakeHttp::Request &request) -> FakeHttp::Response {
        if (request.path == QLatin1String("/api/token"))
            return {200, tokenJson("tok"), "application/json", {}};
        if (request.query.queryItemValue(QStringLiteral("limit")).toInt() > 10)
            return {400, R"({"error":{"status":400,"message":"Invalid limit"}})", "application/json", {}};
        return {200, kSpotifyJson, "application/json", {}};
    };
    MediaSearch search;
    search.setSpotifyAccountsUrl(server.url());
    search.setSpotifyApiUrl(server.url());
    search.setSpotifyCredentials(QStringLiteral("id"), QStringLiteral("secret"));
    QSignalSpy finished(&search, &MediaSearch::finished);
    search.searchSpotify(QStringLiteral("believer"), {MediaSearch::Kind::Track});
    QTRY_COMPARE_WITH_TIMEOUT(finished.size(), 1, 10000);
    QCOMPARE(finished.at(0).at(0).value<MediaSearch::Page>().error, MediaSearch::Error::None);
    QCOMPARE(search.spotifyPageSize(), 10);
}

// ---- Dialogs ----------------------------------------------------------------

void SearchTest::menuAndStartScreen()
{
    // Both searches have their own entry, and their own window.
    auto *menu = m_window->findChild<QMenu *>();
    QVERIFY(menu);
    QStringList texts;
    for (QMenu *m : m_window->findChildren<QMenu *>()) {
        for (QAction *action : m->actions())
            texts << action->text();
    }
    QVERIFY(texts.contains(QStringLiteral("Search YouTube...")));
    QVERIFY(texts.contains(QStringLiteral("Search Spotify...")));

    QTest::keyClick(m_window, Qt::Key_F, Qt::ControlModifier);
    MediaSearchDialog *youtube = m_window->mediaSearchDialog(MediaSearch::Source::YouTube);
    QVERIFY(youtube);
    QTRY_VERIFY(youtube->isVisible());
    QCOMPARE(youtube->windowTitle(), QStringLiteral("Search YouTube"));
    youtube->hide();
    m_window->activateWindow();
    QVERIFY(QTest::qWaitForWindowActive(m_window));
    QTest::keyClick(m_window, Qt::Key_S, Qt::ControlModifier | Qt::ShiftModifier);
    MediaSearchDialog *spotify = m_window->mediaSearchDialog(MediaSearch::Source::Spotify);
    QVERIFY(spotify);
    QTRY_VERIFY(spotify->isVisible());
    QCOMPARE(spotify->windowTitle(), QStringLiteral("Search Spotify"));
    QVERIFY(spotify != youtube);
    spotify->hide();

    // The start screen offers them too; the window is reused.
    auto *empty = m_window->findChild<EmptyStateWidget *>();
    auto *button = empty->findChild<QPushButton *>(QStringLiteral("SearchYouTubeButton"));
    QVERIFY(button && button->isVisible());
    button->click();
    QVERIFY(youtube->isVisible());
    QCOMPARE(m_window->mediaSearchDialog(MediaSearch::Source::YouTube), youtube);
    youtube->hide();
    empty->findChild<QPushButton *>(QStringLiteral("SearchSpotifyButton"))->click();
    QVERIFY(spotify->isVisible());
}

void SearchTest::missingYtDlp()
{
    qputenv("PATH", m_emptyBin.toLocal8Bit());
    MediaSearchDialog *dialog = openDialog(MediaSearch::Source::YouTube);
    auto *warning = dialog->findChild<QLabel *>(QStringLiteral("MediaSearchWarning"));
    QVERIFY(warning->isVisible());
    QVERIFY(warning->text().contains(QLatin1String("sudo apt install yt-dlp")));
    dialog->setQuery(QStringLiteral("believer"));
    QVERIFY(!dialog->findChild<QPushButton *>(QStringLiteral("MediaSearchButton"))->isEnabled());

    // Spotify still searches without it, but nothing can be played or saved.
    MediaSearchDialog *spotify = openDialog(MediaSearch::Source::Spotify);
    QVERIFY(spotify->findChild<QLabel *>(QStringLiteral("MediaSearchWarning"))->isVisible());
    spotify->setQuery(QStringLiteral("believer"));
    QVERIFY(spotify->findChild<QPushButton *>(QStringLiteral("MediaSearchButton"))->isEnabled());
    QVERIFY(!spotify->findChild<QPushButton *>(QStringLiteral("MediaSearchPlay"))->isEnabled());
}

void SearchTest::youtubeSearchDialog()
{
    prependPath(m_fakeBin);
    MediaSearchDialog *dialog = openDialog(MediaSearch::Source::YouTube);
    QVERIFY(dialog);
    QVERIFY(!dialog->findChild<QLabel *>(QStringLiteral("MediaSearchWarning"))->isVisible());
    // No Spotify parts.
    QVERIFY(!dialog->findChild<QTabBar *>(QStringLiteral("MediaSearchTabs")));
    QVERIFY(!dialog->findChild<QWidget *>(QStringLiteral("SpotifyAccessPanel")));

    auto *query = dialog->findChild<QLineEdit *>(QStringLiteral("MediaSearchQuery"));
    query->setText(QStringLiteral("lofi beats"));
    QTest::keyClick(query, Qt::Key_Return);
    QTreeWidget *view = dialog->view();
    auto *status = dialog->findChild<QLabel *>(QStringLiteral("MediaSearchStatus"));
    QTRY_VERIFY2_WITH_TIMEOUT(view->topLevelItemCount() == 20, qPrintable(status->text()), 10000);
    QVERIFY(loggedArgs().contains(QStringLiteral("ytsearch20:lofi beats")));
    QVERIFY(loggedArgs().contains(QStringLiteral("--flat-playlist")));
    QTreeWidgetItem *first = view->topLevelItem(0);
    QCOMPARE(first->text(0), QStringLiteral("lofi beats 1"));
    QCOMPARE(first->text(1), QStringLiteral("Channel 1"));
    QCOMPARE(first->text(2), QStringLiteral("1:01"));
    QCOMPARE(first->text(3), QStringLiteral("1.5K views"));
    QCOMPARE(view->headerItem()->text(1), QStringLiteral("Channel"));
    // Thumbnails arrive one by one.
    QTRY_VERIFY_WITH_TIMEOUT(!view->topLevelItem(19)->icon(0).isNull(), 10000);
    selectRows(dialog, {1});
    screenshot(dialog, QStringLiteral("search-youtube"));
    QVERIFY(m_art->count(QStringLiteral("/vi/vid001/mqdefault.jpg")) >= 1);
    QCOMPARE(dialog->findChild<QLabel *>(QStringLiteral("MediaSearchStatus"))->text(), QStringLiteral("20 video(s) found."));

    // Load More asks for a longer search and lists the new ones.
    auto *more = dialog->findChild<QPushButton *>(QStringLiteral("MediaSearchMore"));
    QVERIFY(more->isVisible());
    more->click();
    QTRY_COMPARE_WITH_TIMEOUT(view->topLevelItemCount(), 40, 10000);
    QVERIFY(loggedArgs().contains(QStringLiteral("ytsearch40:lofi beats")));
    QCOMPARE(view->topLevelItem(20)->text(0), QStringLiteral("lofi beats 21"));
    // Thumbnails seen already aren't fetched again.
    const int fetched = m_art->count(QStringLiteral("/vi/vid001/mqdefault.jpg"));
    QTRY_VERIFY_WITH_TIMEOUT(!view->topLevelItem(39)->icon(0).isNull(), 10000);
    QCOMPARE(m_art->count(QStringLiteral("/vi/vid001/mqdefault.jpg")), fetched);

    // A new search starts over.
    dialog->setQuery(QStringLiteral("rain"));
    dialog->startSearch();
    QTRY_COMPARE_WITH_TIMEOUT(view->topLevelItemCount(), 20, 10000);
    QTRY_COMPARE(view->topLevelItem(0)->text(0), QStringLiteral("rain 1"));
}

void SearchTest::youtubeSearchError()
{
    prependPath(m_fakeBin);
    MediaSearchDialog *dialog = openDialog(MediaSearch::Source::YouTube);
    dialog->setQuery(QStringLiteral("broken"));
    dialog->startSearch();
    auto *status = dialog->findChild<QLabel *>(QStringLiteral("MediaSearchStatus"));
    QTRY_VERIFY_WITH_TIMEOUT(status->text().contains(QLatin1String("Unable to download API page")), 10000);
    QVERIFY(status->property("error").toBool());
    QCOMPARE(dialog->view()->topLevelItemCount(), 0);
}

void SearchTest::youtubePlayAndQueue()
{
    prependPath(m_fakeBin);
    MediaSearchDialog *dialog = openDialog(MediaSearch::Source::YouTube);
    dialog->setQuery(QStringLiteral("lofi"));
    dialog->startSearch();
    QTRY_COMPARE_WITH_TIMEOUT(dialog->view()->topLevelItemCount(), 20, 10000);
    auto *format = dialog->findChild<QComboBox *>(QStringLiteral("MediaSearchFormat"));
    format->setCurrentIndex(format->findData(int(MediaDownloader::Format::Max720)));

    // Play Now streams the selection, in the quality chosen.
    QSignalSpy play(dialog, &MediaSearchDialog::tracksStreamRequested);
    selectRows(dialog, {2, 0});
    dialog->findChild<QPushButton *>(QStringLiteral("MediaSearchPlay"))->click();
    QCOMPARE(play.size(), 1);
    QCOMPARE(play.at(0).at(0).toStringList(),
             QStringList({QStringLiteral("https://www.youtube.com/watch?v=vid001"), QStringLiteral("https://www.youtube.com/watch?v=vid003")}));
    QCOMPARE(play.at(0).at(1).toStringList(), QStringList({QStringLiteral("lofi 1"), QStringLiteral("lofi 3")}));
    QCOMPARE(play.at(0).at(2).toString(), MediaDownloader::streamFormat(MediaDownloader::Format::Max720));
    QTRY_COMPARE_WITH_TIMEOUT(playlistTitles(), QStringList({QStringLiteral("lofi 1"), QStringLiteral("lofi 3")}), 10000);
    QCOMPARE(m_mpv->mpvPropertyString(QStringLiteral("ytdl-format")), MediaDownloader::streamFormat(MediaDownloader::Format::Max720));

    // Add to Playlist appends, keeping what is there.
    QSignalSpy queue(dialog, &MediaSearchDialog::tracksQueueRequested);
    selectRows(dialog, {4});
    dialog->queueSelected();
    QCOMPARE(queue.size(), 1);
    QTRY_COMPARE_WITH_TIMEOUT(playlistTitles(),
                              QStringList({QStringLiteral("lofi 1"), QStringLiteral("lofi 3"), QStringLiteral("lofi 5")}), 10000);

    // Double-click plays that one.
    QTreeWidgetItem *item = dialog->view()->topLevelItem(6);
    Q_EMIT dialog->view()->itemDoubleClicked(item, 0);
    QCOMPARE(play.size(), 2);
    QCOMPARE(play.at(1).at(1).toStringList(), QStringList{QStringLiteral("lofi 7")});
}

void SearchTest::youtubeDownloads()
{
    prependPath(m_fakeBin);
    MediaSearchDialog *dialog = openDialog(MediaSearch::Source::YouTube);
    const QString folder = m_dir.filePath(QStringLiteral("youtube-downloads"));
    dialog->findChild<QLineEdit *>(QStringLiteral("MediaSearchDirectory"))->setText(folder);
    dialog->findChild<QCheckBox *>(QStringLiteral("MediaSearchPlayWhenDone"))->setChecked(true);
    dialog->setQuery(QStringLiteral("song"));
    dialog->startSearch();
    QTRY_COMPARE_WITH_TIMEOUT(dialog->view()->topLevelItemCount(), 20, 10000);
    QFile::remove(QString::fromLocal8Bit(qgetenv("FAKE_LOG")));

    // Two MP3s, one after another: each row shows its state, the first one
    // saved plays and the next is queued.
    QSignalSpy downloaded(dialog, &MediaSearchDialog::downloaded);
    selectRows(dialog, {0, 1});
    dialog->findChild<QPushButton *>(QStringLiteral("MediaSearchDownloadMp3"))->click();
    QTreeWidget *view = dialog->view();
    QCOMPARE(dialog->downloads()->pending(), 2);
    QCOMPARE(view->topLevelItem(1)->text(4), QStringLiteral("Queued"));
    auto *progress = dialog->findChild<QProgressBar *>(QStringLiteral("MediaSearchProgress"));
    QVERIFY(progress->isVisible());
    QVERIFY(dialog->findChild<QPushButton *>(QStringLiteral("MediaSearchCancel"))->isVisible());
    QTRY_COMPARE_WITH_TIMEOUT(downloaded.size(), 2, 15000);
    QCOMPARE(view->topLevelItem(0)->text(4), QStringLiteral("Saved"));
    QCOMPARE(view->topLevelItem(1)->text(4), QStringLiteral("Saved"));
    QCOMPARE(downloaded.at(0).at(0).toString(), folder + QStringLiteral("/song 1.mp3"));
    QVERIFY(downloaded.at(0).at(1).toBool());
    QCOMPARE(downloaded.at(1).at(0).toString(), folder + QStringLiteral("/song 2.mp3"));
    QVERIFY(!downloaded.at(1).at(1).toBool());
    QVERIFY(QFileInfo::exists(folder + QStringLiteral("/song 1.mp3")));
    QVERIFY(!progress->isVisible());
    QTRY_VERIFY_WITH_TIMEOUT(m_mpv->mpvPropertyString(QStringLiteral("path")).endsWith(QLatin1String("song 1.mp3")), 10000);
    QTRY_COMPARE(prop("playlist-count").toInt(), 2);
    QStringList args = loggedArgs();
    QVERIFY(args.contains(QStringLiteral("-x")));
    QVERIFY(args.contains(QStringLiteral("https://www.youtube.com/watch?v=vid001")));
    // The folder is remembered, as in Download from URL.
    QSettings settings(PlaylistSession::configDir() + QStringLiteral("/settings.ini"), QSettings::IniFormat);
    QCOMPARE(settings.value(QStringLiteral("downloader/directory")).toString(), folder);

    // A video, in the quality chosen.
    QFile::remove(QString::fromLocal8Bit(qgetenv("FAKE_LOG")));
    auto *format = dialog->findChild<QComboBox *>(QStringLiteral("MediaSearchFormat"));
    format->setCurrentIndex(format->findData(int(MediaDownloader::Format::Max1080)));
    dialog->findChild<QCheckBox *>(QStringLiteral("MediaSearchPlayWhenDone"))->setChecked(false);
    selectRows(dialog, {2});
    dialog->findChild<QPushButton *>(QStringLiteral("MediaSearchDownloadVideo"))->click();
    QTRY_COMPARE_WITH_TIMEOUT(downloaded.size(), 3, 15000);
    QCOMPARE(downloaded.at(2).at(0).toString(), folder + QStringLiteral("/song 3.mp4"));
    QVERIFY(!downloaded.at(2).at(1).toBool());
    args = loggedArgs();
    QVERIFY(args.contains(MediaDownloader::streamFormat(MediaDownloader::Format::Max1080)));
    QVERIFY(!args.contains(QStringLiteral("-x")));
    // The state survives a new search.
    dialog->setQuery(QStringLiteral("song"));
    dialog->startSearch();
    QTRY_COMPARE_WITH_TIMEOUT(view->topLevelItemCount(), 20, 10000);
    QCOMPARE(view->topLevelItem(2)->text(4), QStringLiteral("Saved"));
}

void SearchTest::closeWhileDownloading()
{
    prependPath(m_fakeBin);
    MediaSearchDialog *dialog = openDialog(MediaSearch::Source::YouTube);
    dialog->findChild<QLineEdit *>(QStringLiteral("MediaSearchDirectory"))->setText(m_dir.filePath(QStringLiteral("slow")));
    dialog->setQuery(QStringLiteral("slow"));
    dialog->startSearch();
    QTRY_COMPARE_WITH_TIMEOUT(dialog->view()->topLevelItemCount(), 20, 10000);
    selectRows(dialog, {0, 1});
    dialog->downloadSelected(MediaDownloader::Format::AudioMp3);
    QTRY_COMPARE_WITH_TIMEOUT(dialog->view()->topLevelItem(0)->text(4), QStringLiteral("50%"), 10000);

    // Closing asks; "Keep Downloading" closes and keeps them going.
    const auto answer = [this](const QString &button) {
        QTimer::singleShot(200, this, [button] {
            auto *box = qobject_cast<QMessageBox *>(QApplication::activeModalWidget());
            QVERIFY(box);
            QCOMPARE(box->objectName(), QStringLiteral("MediaSearchCloseQuestion"));
            for (QAbstractButton *b : box->buttons()) {
                if (b->text() == button)
                    b->click();
            }
        });
    };
    answer(QStringLiteral("Keep Downloading"));
    static_cast<QDialog *>(dialog)->reject();
    QVERIFY(!dialog->isVisible());
    QCOMPARE(dialog->downloads()->pending(), 2);

    // Back again: Esc asks again; Cancel stays.
    m_window->openMediaSearchDialog(MediaSearch::Source::YouTube);
    QVERIFY(dialog->isVisible());
    QCOMPARE(dialog->view()->topLevelItem(1)->text(4), QStringLiteral("Queued"));
    answer(QStringLiteral("Cancel"));
    static_cast<QDialog *>(dialog)->reject();
    QVERIFY(dialog->isVisible());
    QCOMPARE(dialog->downloads()->pending(), 2);

    // "Stop Downloads" stops them and closes.
    answer(QStringLiteral("Stop Downloads"));
    static_cast<QDialog *>(dialog)->reject();
    QVERIFY(!dialog->isVisible());
    QCOMPARE(dialog->downloads()->pending(), 0);
    QCOMPARE(dialog->view()->topLevelItem(0)->text(4), QString());

    // Without downloads it just closes.
    m_window->openMediaSearchDialog(MediaSearch::Source::YouTube);
    static_cast<QDialog *>(dialog)->reject();
    QVERIFY(!dialog->isVisible());
}

void SearchTest::spotifySearchDialog()
{
    prependPath(m_fakeBin);
    FakeHttp server;
    QVERIFY(server.listen());
    const QByteArray json = QByteArray(kSpotifyJson).replace("IMG/", server.url().toString().toUtf8() + "/img/");
    server.handler = [&json](const FakeHttp::Request &request) -> FakeHttp::Response {
        if (request.path == QLatin1String("/api/token")) {
            if (request.headers.value("authorization") != "Basic " + QByteArray("dash-id:dash-secret").toBase64())
                return {401, R"({"error":"invalid_client","error_description":"Invalid client secret"})", "application/json", {}};
            return {200, tokenJson("tok"), "application/json", {}};
        }
        if (request.path == QLatin1String("/v1/search"))
            return {200, json, "application/json", {}};
        if (request.path.startsWith(QLatin1String("/img/")))
            return {200, pngBytes(Qt::green), "image/png", {}};
        return {404, {}, "text/plain", {}};
    };
    MediaSearchDialog *dialog = openDialog(MediaSearch::Source::Spotify);
    dialog->search()->setSpotifyAccountsUrl(server.url());
    dialog->search()->setSpotifyApiUrl(server.url());

    // No credentials yet: the access panel is open, with a link to the dashboard.
    auto *panel = dialog->findChild<QWidget *>(QStringLiteral("SpotifyAccessPanel"));
    QVERIFY(panel->isVisible());
    auto *help = dialog->findChild<QLabel *>(QStringLiteral("SpotifyAccessHelp"));
    QVERIFY(help->text().contains(QLatin1String("https://developer.spotify.com/dashboard")));
    QVERIFY(help->openExternalLinks());
    auto *clientId = dialog->findChild<QLineEdit *>(QStringLiteral("SpotifyClientId"));
    auto *secret = dialog->findChild<QLineEdit *>(QStringLiteral("SpotifyClientSecret"));
    QCOMPARE(secret->echoMode(), QLineEdit::Password);
    auto *status = dialog->findChild<QLabel *>(QStringLiteral("MediaSearchStatus"));

    // Searching without them says what to do.
    dialog->setQuery(QStringLiteral("believer"));
    dialog->startSearch();
    QTRY_VERIFY(status->text().contains(QLatin1String("Client ID")));
    QVERIFY(status->property("error").toBool());

    // Wrong ones are refused in plain words, and the panel stays open.
    auto *toggle = dialog->findChild<QToolButton *>(QStringLiteral("SpotifyAccessToggle"));
    toggle->setChecked(false);
    QVERIFY(!panel->isVisible());
    clientId->setText(QStringLiteral("dash-id"));
    secret->setText(QStringLiteral("wrong"));
    dialog->startSearch();
    QTRY_VERIFY_WITH_TIMEOUT(status->text().contains(QLatin1String("didn't accept")), 10000);
    QVERIFY(panel->isVisible());

    secret->setText(QStringLiteral("dash-secret"));
    dialog->startSearch();
    QTreeWidget *view = dialog->view();
    QTRY_COMPARE_WITH_TIMEOUT(view->topLevelItemCount(), 2, 10000);
    QVERIFY(!status->property("error").toBool());
    // They work: the panel makes room for the results.
    QVERIFY(!panel->isVisible());
    // Stored for next time.
    QSettings settings(PlaylistSession::configDir() + QStringLiteral("/settings.ini"), QSettings::IniFormat);
    QCOMPARE(settings.value(QStringLiteral("spotify/clientId")).toString(), QStringLiteral("dash-id"));
    QCOMPARE(settings.value(QStringLiteral("spotify/clientSecret")).toString(), QStringLiteral("dash-secret"));

    // Songs, then albums and playlists in their tabs.
    auto *tabs = dialog->findChild<QTabBar *>(QStringLiteral("MediaSearchTabs"));
    QCOMPARE(tabs->tabText(0), QStringLiteral("Songs (120)"));
    QCOMPARE(tabs->tabText(1), QStringLiteral("Albums (7)"));
    QCOMPARE(tabs->tabText(2), QStringLiteral("Playlists (40)"));
    QTreeWidgetItem *song = view->topLevelItem(0);
    QCOMPARE(song->text(0), QStringLiteral("Believer"));
    QCOMPARE(song->text(1), QStringLiteral("Imagine Dragons, Lil Wayne"));
    QCOMPARE(song->text(2), QStringLiteral("Evolve"));
    QCOMPARE(song->text(3), QStringLiteral("3:24"));
    QTRY_VERIFY_WITH_TIMEOUT(!song->icon(0).isNull(), 10000);
    screenshot(dialog, QStringLiteral("search-spotify"));
    QCOMPARE(server.count(QStringLiteral("/img/300.jpg")), 1);
    QVERIFY(dialog->findChild<QPushButton *>(QStringLiteral("MediaSearchMore"))->isVisible());
    QVERIFY(!dialog->findChild<QPushButton *>(QStringLiteral("MediaSearchDownloadVideo")));

    dialog->setCurrentKind(MediaSearch::Kind::Album);
    QCOMPARE(view->topLevelItemCount(), 1);
    QCOMPARE(view->topLevelItem(0)->text(0), QStringLiteral("Evolve"));
    QCOMPARE(view->topLevelItem(0)->text(2), QStringLiteral("11"));
    QCOMPARE(view->topLevelItem(0)->text(3), QStringLiteral("2017"));
    QCOMPARE(view->headerItem()->text(0), QStringLiteral("Album"));
    dialog->setCurrentKind(MediaSearch::Kind::Playlist);
    QCOMPARE(view->topLevelItemCount(), 2);
    QCOMPARE(view->topLevelItem(0)->text(1), QStringLiteral("Spotify"));
    QVERIFY(view->isColumnHidden(3));

    // Load More asks for the next page of this tab only.
    dialog->loadMore();
    QTRY_COMPARE_WITH_TIMEOUT(server.requests.last().query.queryItemValue(QStringLiteral("offset")), QStringLiteral("20"), 10000);
    QCOMPARE(server.requests.last().query.queryItemValue(QStringLiteral("type")), QStringLiteral("playlist"));
    // (The stand-in answers the same page: nothing new.)
    QTRY_VERIFY(!dialog->search()->isBusy());
    QCOMPARE(view->topLevelItemCount(), 2);

    // A song plays as its YouTube match, titled "Artist - Title".
    dialog->setCurrentKind(MediaSearch::Kind::Track);
    QSignalSpy play(dialog, &MediaSearchDialog::tracksStreamRequested);
    selectRows(dialog, {0});
    dialog->playSelected();
    QCOMPARE(play.size(), 1);
    QCOMPARE(play.at(0).at(0).toStringList(),
             QStringList{QStringLiteral("ytdl://ytsearch1:Imagine Dragons Lil Wayne - Believer")});
    QCOMPARE(play.at(0).at(1).toStringList(), QStringList{QStringLiteral("Imagine Dragons, Lil Wayne - Believer")});
    QCOMPARE(play.at(0).at(2).toString(), MediaDownloader::streamFormat(MediaDownloader::Format::AudioMp3));
    QSignalSpy queue(dialog, &MediaSearchDialog::tracksQueueRequested);
    selectRows(dialog, {1});
    dialog->queueSelected();
    QCOMPARE(queue.size(), 1);
    QTRY_COMPARE_WITH_TIMEOUT(playlistTitles(),
                              QStringList({QStringLiteral("Imagine Dragons, Lil Wayne - Believer"),
                                           QStringLiteral("Imagine Dragons - Thunder")}), 10000);

    // Download MP3: found on YouTube by length, tagged with Spotify's names.
    const QString folder = m_dir.filePath(QStringLiteral("spotify-downloads"));
    dialog->findChild<QLineEdit *>(QStringLiteral("MediaSearchDirectory"))->setText(folder);
    dialog->findChild<QCheckBox *>(QStringLiteral("MediaSearchPlayWhenDone"))->setChecked(false);
    QFile::remove(QString::fromLocal8Bit(qgetenv("FAKE_LOG")));
    QSignalSpy downloaded(dialog, &MediaSearchDialog::downloaded);
    selectRows(dialog, {0});
    dialog->findChild<QPushButton *>(QStringLiteral("MediaSearchDownloadMp3"))->click();
    QCOMPARE(view->topLevelItem(0)->text(4), QStringLiteral("0%"));
    screenshot(dialog, QStringLiteral("search-spotify-downloading"));
    QTRY_COMPARE_WITH_TIMEOUT(downloaded.size(), 1, 15000);
    QCOMPARE(view->topLevelItem(0)->text(4), QStringLiteral("Saved"));
    QCOMPARE(downloaded.at(0).at(0).toString(), folder + QStringLiteral("/Imagine Dragons, Lil Wayne - Believer.mp3"));
    QVERIFY(!downloaded.at(0).at(1).toBool());
    const QStringList args = loggedArgs();
    QVERIFY(args.contains(QStringLiteral("--match-filter")));
    QVERIFY(args.contains(QStringLiteral("ytsearch5:Imagine Dragons Lil Wayne - Believer")));
    QVERIFY(args.contains(QStringLiteral("--embed-metadata")));
    QVERIFY(std::any_of(args.cbegin(), args.cend(), [](const QString &a) {
        return a.startsWith(QLatin1String("pre_process:Evolve |:%(meta_album)s"));
    }));
    QTRY_VERIFY(QFileInfo::exists(folder + QStringLiteral("/Imagine Dragons, Lil Wayne - Believer.mp3")));
}

int main(int argc, char *argv[])
{
    // Keep the settings away from the user's own.
    QTemporaryDir config;
    qputenv("XDG_CONFIG_HOME", config.path().toLocal8Bit());
    QApplication app(argc, argv);
    ResumeManager::setMode(ResumeManager::Mode::Never);
    // libmpv requires the C numeric locale; QApplication may have changed it.
    std::setlocale(LC_NUMERIC, "C");
    QApplication::setAttribute(Qt::AA_DontUseNativeDialogs);
    SearchTest test;
    QTEST_SET_MAIN_SOURCE_PATH
    return QTest::qExec(&test, argc, argv);
}

#include "tst_search.moc"
