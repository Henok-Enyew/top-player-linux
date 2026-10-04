// Subtitles: showing and hiding them, and the download dialog, run against a
// local mock of podnapisi.net and the OpenSubtitles REST API.
// Needs a display (run under xvfb-run) and ffmpeg, which generates the test clip.

#include "ResumeManager.h"
#include "MainWindow.h"
#include "MpvWidget.h"
#include "OpenSubtitlesClient.h"
#include "OsdWidget.h"
#include "PodnapisiClient.h"
#include "PlayerMenu.h"
#include "SubtitleDownloadDialog.h"
#include "SubtitleHasher.h"
#include "SubtitleSearch.h"
#include "ZipArchive.h"
#include "TestClip.h"

#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QProgressBar>
#include <QPushButton>
#include <QSignalSpy>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>
#include <QTreeWidget>
#include <QUrlQuery>
#include <QtEndian>

#include <zlib.h>

#include <clocale>
#include <functional>

namespace {

const QByteArray kSrt = "1\n00:00:00,000 --> 00:09:00,000\nHello from the mock server\n";

// A minimal HTTP/1.1 server: one request per connection, answered by `handler`.
class MockServer : public QObject
{
public:
    struct Request {
        QByteArray method;
        QUrl url;
        QHash<QByteArray, QByteArray> headers; // lower-case names
        QByteArray body;
    };
    struct Response {
        int status = 200;
        QByteArray body;
        QByteArray contentType = "application/json";
        bool hold = false; // never answer (for cancel tests)
    };

    std::function<Response(const Request &)> handler;
    QList<Request> requests;

    bool listen() { return m_server.listen(QHostAddress::LocalHost); }
    QString url(const QString &path) const
    {
        return QStringLiteral("http://127.0.0.1:%1%2").arg(m_server.serverPort()).arg(path);
    }

    MockServer()
    {
        connect(&m_server, &QTcpServer::newConnection, this, [this] {
            while (QTcpSocket *socket = m_server.nextPendingConnection()) {
                connect(socket, &QTcpSocket::readyRead, this, [this, socket] { onData(socket); });
                connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
            }
        });
    }

private:
    void onData(QTcpSocket *socket)
    {
        QByteArray &buffer = m_buffers[socket];
        buffer += socket->readAll();
        const qsizetype end = buffer.indexOf("\r\n\r\n");
        if (end < 0)
            return;
        Request request;
        const QList<QByteArray> lines = buffer.left(end).split('\n');
        const QList<QByteArray> first = lines.value(0).trimmed().split(' ');
        request.method = first.value(0);
        request.url = QUrl(QString::fromLatin1(first.value(1)));
        for (qsizetype i = 1; i < lines.size(); ++i) {
            const qsizetype colon = lines[i].indexOf(':');
            if (colon > 0)
                request.headers.insert(lines[i].left(colon).trimmed().toLower(), lines[i].mid(colon + 1).trimmed());
        }
        const qsizetype length = request.headers.value("content-length").toLongLong();
        if (buffer.size() < end + 4 + length)
            return;
        request.body = buffer.mid(end + 4, length);
        m_buffers.remove(socket);
        requests.append(request);

        const Response response = handler ? handler(request) : Response{404, "{}"};
        if (response.hold)
            return;
        QByteArray reply = "HTTP/1.1 " + QByteArray::number(response.status) + " X\r\n";
        reply += "Content-Type: " + response.contentType + "\r\n";
        reply += "Content-Length: " + QByteArray::number(response.body.size()) + "\r\nConnection: close\r\n\r\n";
        reply += response.body;
        socket->write(reply);
        socket->disconnectFromHost();
    }

    QTcpServer m_server;
    QHash<QTcpSocket *, QByteArray> m_buffers;
};

QJsonObject result(int fileId, const QString &language, const QString &fileName, int downloads, double rating,
                   bool hearingImpaired, bool hashMatch)
{
    return QJsonObject{
        {QStringLiteral("id"), QString::number(fileId)},
        {QStringLiteral("type"), QStringLiteral("subtitle")},
        {QStringLiteral("attributes"), QJsonObject{
            {QStringLiteral("language"), language},
            {QStringLiteral("download_count"), downloads},
            {QStringLiteral("ratings"), rating},
            {QStringLiteral("hearing_impaired"), hearingImpaired},
            {QStringLiteral("moviehash_match"), hashMatch},
            {QStringLiteral("release"), fileName + QStringLiteral(" release")},
            {QStringLiteral("uploader"), QJsonObject{{QStringLiteral("name"), QStringLiteral("tester")}}},
            {QStringLiteral("files"), QJsonArray{QJsonObject{
                {QStringLiteral("file_id"), fileId},
                {QStringLiteral("file_name"), fileName},
            }}},
        }},
    };
}

// A podnapisi.net search result, as its XML search returns it.
QByteArray podnapisiSubtitle(const QString &pid, const QString &title, int year, const QString &release,
                             const QString &language, double rating, const QString &flags = {})
{
    return QStringLiteral("<subtitle><pid>%1</pid><title>%2</title><year>%3</year>"
                          "<url>http://www.podnapisi.net/subtitles/%1</url><release>%4</release>"
                          "<language>%5</language><rating>%6</rating><flags>%7</flags>"
                          "<downloads>120</downloads><format>SubRip</format><uploaderName>tester</uploaderName></subtitle>")
        .arg(pid, title).arg(year).arg(release, language).arg(rating).arg(flags).toUtf8();
}

QByteArray podnapisiXml(const QList<QByteArray> &subtitles)
{
    QByteArray xml = "<?xml version=\"1.0\" encoding=\"utf-8\"?><results><pagination><current>1</current>"
                     "<count>1</count><results>" + QByteArray::number(subtitles.size()) + "</results></pagination>";
    for (const QByteArray &subtitle : subtitles)
        xml += subtitle;
    return xml + "</results>";
}

// A ZIP archive with the given files, deflated.
QByteArray makeZip(const QList<std::pair<QString, QByteArray>> &files)
{
    QByteArray zip;
    QByteArray directory;
    auto u16 = [](QByteArray &out, quint16 value) { value = qToLittleEndian(value); out.append(reinterpret_cast<const char *>(&value), 2); };
    auto u32 = [](QByteArray &out, quint32 value) { value = qToLittleEndian(value); out.append(reinterpret_cast<const char *>(&value), 4); };
    for (const auto &[name, data] : files) {
        QByteArray packed(compressBound(static_cast<uLong>(data.size())) + 16, Qt::Uninitialized);
        z_stream stream{};
        deflateInit2(&stream, Z_BEST_COMPRESSION, Z_DEFLATED, -MAX_WBITS, 8, Z_DEFAULT_STRATEGY);
        stream.next_in = reinterpret_cast<Bytef *>(const_cast<char *>(data.constData()));
        stream.avail_in = static_cast<uInt>(data.size());
        stream.next_out = reinterpret_cast<Bytef *>(packed.data());
        stream.avail_out = static_cast<uInt>(packed.size());
        deflate(&stream, Z_FINISH);
        packed.resize(static_cast<qsizetype>(stream.total_out));
        deflateEnd(&stream);
        const quint32 crc = crc32(0, reinterpret_cast<const Bytef *>(data.constData()), static_cast<uInt>(data.size()));
        const QByteArray utf8 = name.toUtf8();
        const quint32 offset = static_cast<quint32>(zip.size());
        u32(zip, 0x04034b50); u16(zip, 20); u16(zip, 0); u16(zip, 8); u16(zip, 0); u16(zip, 0);
        u32(zip, crc); u32(zip, packed.size()); u32(zip, data.size()); u16(zip, utf8.size()); u16(zip, 0);
        zip += utf8 + packed;
        u32(directory, 0x02014b50); u16(directory, 20); u16(directory, 20); u16(directory, 0); u16(directory, 8);
        u16(directory, 0); u16(directory, 0); u32(directory, crc); u32(directory, packed.size()); u32(directory, data.size());
        u16(directory, utf8.size()); u16(directory, 0); u16(directory, 0); u16(directory, 0); u16(directory, 0);
        u32(directory, 0); u32(directory, offset);
        directory += utf8;
    }
    const quint32 directoryOffset = static_cast<quint32>(zip.size());
    zip += directory;
    u32(zip, 0x06054b50); u16(zip, 0); u16(zip, 0); u16(zip, files.size()); u16(zip, files.size());
    u32(zip, directory.size()); u32(zip, directoryOffset); u16(zip, 0);
    return zip;
}

const QString kVideoName = QStringLiteral("The.Matrix.1999.1080p.BluRay.x264-GRP");

} // namespace

class SubtitleTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void initTestCase();
    void init();
    void cleanup();

    void parseFileName_data();
    void parseFileName();
    void hasher();
    void sameRelease();
    void zipArchive();
    void podnapisiResults();
    void showAndHideSubtitles();
    void searchesWithoutKey();
    void downloadAndPlay();
    void exactMatchWithKey();
    void exactFallsBackToName();
    void providerFallback();
    void manualSearch();
    void noResults();
    void offline();
    void badDownload();
    void cancelSearch();
    void smallFileSearchesByName();

private:
    QVariant prop(const char *name) const { return m_mpv->mpvProperty(QString::fromLatin1(name)); }
    QString propString(const char *name) const { return m_mpv->mpvPropertyString(QString::fromLatin1(name)); }
    QWidget *keyTarget() const;
    void press(int key, Qt::KeyboardModifiers modifiers = Qt::NoModifier);
    QMenu *subtitleMenu() const;
    QAction *menuAction(QMenu *menu, const QString &text) const;
    // Opens the dialog with D and waits for its first search to end.
    SubtitleDownloadDialog *openDialog();
    QList<MockServer::Request> requests(const QString &pathPrefix) const;
    MockServer::Response defaultResponse(const MockServer::Request &request);

    QTemporaryDir m_dir;
    QString m_video;
    QString m_srt1;
    QString m_srt2;
    MockServer m_server;
    MainWindow *m_window = nullptr;
    MpvWidget *m_mpv = nullptr;
};

void SubtitleTest::initTestCase()
{
    QVERIFY(m_dir.isValid());
    QVERIFY(QDir().mkpath(m_dir.filePath(QStringLiteral("movies"))));
    m_video = m_dir.filePath(QStringLiteral("movies/%1.mkv").arg(kVideoName));
    if (!makeTestClip(m_video))
        QSKIP("ffmpeg is needed to generate the test clip");
    QVERIFY(QFileInfo(m_video).size() >= SubtitleHasher::kMinimumSize);
    m_srt1 = m_dir.filePath(QStringLiteral("first.srt"));
    m_srt2 = m_dir.filePath(QStringLiteral("second.srt"));
    for (const QString &path : {m_srt1, m_srt2}) {
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write(kSrt);
    }

    QVERIFY(m_server.listen());
    qputenv("TOPPLAYER_OPENSUBTITLES_URL", m_server.url(QStringLiteral("/api/v1")).toUtf8());
    qputenv("TOPPLAYER_PODNAPISI_URL", m_server.url(QString()).toUtf8());
}

MockServer::Response SubtitleTest::defaultResponse(const MockServer::Request &request)
{
    const QString path = request.url.path();
    // podnapisi.net: a near-identical release, an exact release match (listed
    // first by the player), and a hearing impaired one.
    if (request.method == "GET" && path == QLatin1String("/subtitles/search/old")) {
        return {200, podnapisiXml({
            podnapisiSubtitle(QStringLiteral("aB1"), QStringLiteral("The Matrix"), 1999, QStringLiteral("The.Matrix.1999.720p.WEB"),
                              QStringLiteral("en"), 8.5),
            podnapisiSubtitle(QStringLiteral("cD2"), QStringLiteral("The Matrix"), 1999, kVideoName, QStringLiteral("en"), 9.0),
            podnapisiSubtitle(QStringLiteral("eF3"), QStringLiteral("The Matrix"), 1999, QString(), QStringLiteral("en"), 0,
                              QStringLiteral("nh")),
        }), "text/xml"};
    }
    if (request.method == "GET" && path.startsWith(QLatin1String("/subtitles/")) && path.endsWith(QLatin1String("/download")))
        return {200, makeZip({{QStringLiteral("readme.nfo"), "release notes"}, {kVideoName + QStringLiteral(".srt"), kSrt}}),
                "application/zip"};
    // OpenSubtitles: one exact match for a hash search, plain results otherwise.
    if (request.method == "GET" && path == QLatin1String("/api/v1/subtitles")) {
        const bool byHash = QUrlQuery(request.url).hasQueryItem(QStringLiteral("moviehash"));
        QJsonArray data{result(101, QStringLiteral("en"), QStringLiteral("Matrix.Popular.srt"), 52000, 8.4, false, false)};
        if (byHash)
            data.append(result(103, QStringLiteral("en"), kVideoName + QStringLiteral(".srt"), 300, 9.1, false, true));
        return {200, QJsonDocument(QJsonObject{{QStringLiteral("data"), data}}).toJson()};
    }
    if (request.method == "POST" && path == QLatin1String("/api/v1/download")) {
        const int fileId = QJsonDocument::fromJson(request.body).object().value(QStringLiteral("file_id")).toInt();
        return {200, QJsonDocument(QJsonObject{{QStringLiteral("link"), m_server.url(QStringLiteral("/files/%1").arg(fileId))}}).toJson()};
    }
    if (request.method == "GET" && path.startsWith(QLatin1String("/files/")))
        return {200, kSrt, "application/x-subrip"};
    return {404, "{\"message\":\"not found\"}"};
}

void SubtitleTest::init()
{
    // No key: the dialog must work without one.
    SubtitleSearch::setUserApiKey(QString());
    SubtitleSearch::setLanguage(QStringLiteral("en"));
    SubtitleSearch::setSaveBesideVideo(false);
    m_server.requests.clear();
    m_server.handler = [this](const MockServer::Request &request) { return defaultResponse(request); };

    m_window = new MainWindow;
    m_window->resize(800, 450);
    m_window->show();
    m_window->activateWindow();
    QVERIFY(QTest::qWaitForWindowActive(m_window));
    m_mpv = m_window->findChild<MpvWidget *>();
    QVERIFY(m_mpv);
    m_window->openFile(m_video);
    // Opening plays; pause right after (the commands run in order).
    m_mpv->setMpvProperty(QStringLiteral("pause"), QStringLiteral("yes"));
    QTRY_VERIFY_WITH_TIMEOUT(prop("time-pos").isValid(), 10000);
    m_mpv->setFocus();
}

void SubtitleTest::cleanup()
{
    while (QWidget *modal = QApplication::activeModalWidget())
        delete modal;
    delete m_window;
    m_window = nullptr;
    m_mpv = nullptr;
    // Downloads would otherwise be picked up next time.
    for (const QString &dir : {m_dir.filePath(QStringLiteral("movies")), SubtitleSearch::cacheDir()}) {
        const QDir folder(dir);
        for (const QString &name : folder.entryList({QStringLiteral("*.srt"), QStringLiteral("*.ass")}, QDir::Files))
            QFile::remove(folder.filePath(name));
    }
}

QWidget *SubtitleTest::keyTarget() const
{
    QWidget *focus = QApplication::focusWidget();
    return focus ? focus : m_window;
}

void SubtitleTest::press(int key, Qt::KeyboardModifiers modifiers)
{
    QTest::keyClick(keyTarget(), static_cast<Qt::Key>(key), modifiers);
}

QMenu *SubtitleTest::subtitleMenu() const
{
    for (QAction *action : m_window->findChild<PlayerMenu *>()->actions()) {
        if (action->menu() && action->text() == QLatin1String("Subtitles"))
            return action->menu();
    }
    return nullptr;
}

QAction *SubtitleTest::menuAction(QMenu *menu, const QString &text) const
{
    Q_EMIT menu->aboutToShow(); // track menus are filled as they open
    for (QAction *action : menu->actions()) {
        if (action->text() == text || (action->menu() && action->menu()->title() == text))
            return action;
    }
    return nullptr;
}

SubtitleDownloadDialog *SubtitleTest::openDialog()
{
    // Without a window manager, closing a dialog doesn't hand activation back.
    m_window->activateWindow();
    m_mpv->setFocus();
    [&] { QTRY_VERIFY(m_window->isActiveWindow()); }();
    press(Qt::Key_D);
    SubtitleDownloadDialog *dialog = nullptr;
    [&] { QTRY_VERIFY((dialog = m_window->findChild<SubtitleDownloadDialog *>()) && dialog->isVisible()); }();
    if (dialog)
        [&] { QTRY_VERIFY_WITH_TIMEOUT(!dialog->isBusy() && !m_server.requests.isEmpty(), 10000); }();
    return dialog;
}

QList<MockServer::Request> SubtitleTest::requests(const QString &pathPrefix) const
{
    QList<MockServer::Request> matching;
    for (const MockServer::Request &request : m_server.requests) {
        if (request.url.path().startsWith(pathPrefix))
            matching.append(request);
    }
    return matching;
}

void SubtitleTest::parseFileName_data()
{
    QTest::addColumn<QString>("file");
    QTest::addColumn<QString>("title");
    QTest::addColumn<int>("year");
    QTest::addColumn<int>("season");
    QTest::addColumn<int>("episode");

    QTest::newRow("movie") << "The.Matrix.1999.1080p.BluRay.x264-GRP.mkv" << "The Matrix" << 1999 << -1 << -1;
    QTest::newRow("brackets") << "[YTS.MX] Dune Part Two (2024) [2160p].mp4" << "Dune Part Two" << 2024 << -1 << -1;
    QTest::newRow("episode") << "Breaking.Bad.S01E02.720p.HDTV.x264.mkv" << "Breaking Bad" << 0 << 1 << 2;
    QTest::newRow("1x02") << "the_office_2x05_hdtv.avi" << "the office" << 0 << 2 << 5;
    QTest::newRow("year title") << "2001.A.Space.Odyssey.1968.REMASTERED.mkv" << "2001 A Space Odyssey" << 1968 << -1 << -1;
    QTest::newRow("no tags") << "/home/me/Videos/My Holiday Video.mp4" << "My Holiday Video" << 0 << -1 << -1;
    QTest::newRow("tag only") << "Inception.WEB-DL.mkv" << "Inception" << 0 << -1 << -1;
}

void SubtitleTest::parseFileName()
{
    QFETCH(QString, file);
    QFETCH(QString, title);
    QFETCH(int, year);
    QFETCH(int, season);
    QFETCH(int, episode);
    const SubtitleSearch::ParsedName parsed = SubtitleSearch::parseFileName(file);
    QCOMPARE(parsed.title, title);
    QCOMPARE(parsed.year, year);
    QCOMPARE(parsed.season, season);
    QCOMPARE(parsed.episode, episode);
}

void SubtitleTest::hasher()
{
    // Size plus the 64-bit words of the first and last 64 KiB: here the first
    // chunk is 0x01 bytes and the last is zeros.
    const QString path = m_dir.filePath(QStringLiteral("hash.bin"));
    QFile file(path);
    QVERIFY(file.open(QIODevice::WriteOnly));
    file.write(QByteArray(65536, '\x01'));
    file.write(QByteArray(2 * 65536, '\0'));
    file.close();
    QCOMPARE(SubtitleHasher::hash(path), QStringLiteral("2020202020232000"));

    // Files under 128 KiB are searched by name instead.
    QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
    file.write(QByteArray(128 * 1024 - 1, 'x'));
    file.close();
    QCOMPARE(SubtitleHasher::hash(path), QString());
    QCOMPARE(SubtitleHasher::hash(m_dir.filePath(QStringLiteral("missing.mkv"))), QString());
}

void SubtitleTest::sameRelease()
{
    const QString video = QStringLiteral("/v/%1.mkv").arg(kVideoName);
    QVERIFY(SubtitleSearch::isSameRelease(kVideoName, video));
    QVERIFY(SubtitleSearch::isSameRelease(kVideoName + QStringLiteral(".srt"), video));
    QVERIFY(SubtitleSearch::isSameRelease(QStringLiteral("the matrix 1999 1080p bluray x264 grp eng"), video));
    QVERIFY(!SubtitleSearch::isSameRelease(QStringLiteral("The.Matrix.1999.720p.WEB"), video));
    QVERIFY(!SubtitleSearch::isSameRelease(QStringLiteral("The Matrix"), video));
    QVERIFY(!SubtitleSearch::isSameRelease(QString(), video));
}

void SubtitleTest::zipArchive()
{
    const QByteArray zip = makeZip({{QStringLiteral("info.nfo"), "x"}, {QStringLiteral("sub/Movie.srt"), kSrt}});
    QVERIFY(ZipArchive::isZip(zip));
    QCOMPARE(ZipArchive::fileNames(zip), (QStringList{QStringLiteral("info.nfo"), QStringLiteral("sub/Movie.srt")}));
    const auto entry = ZipArchive::extract(zip, [](const QString &name) { return name.endsWith(QLatin1String(".srt")); });
    QVERIFY(entry);
    QCOMPARE(entry->name, QStringLiteral("sub/Movie.srt"));
    QCOMPARE(entry->data, kSrt);
    QVERIFY(!ZipArchive::extract(zip, [](const QString &name) { return name.endsWith(QLatin1String(".ass")); }));
    QVERIFY(!ZipArchive::isZip("<html>"));
    QVERIFY(!ZipArchive::extract(zip.left(zip.size() / 2), [](const QString &) { return true; }));
}

void SubtitleTest::podnapisiResults()
{
    const QList<SubtitleResult> results = PodnapisiClient::parseResults(podnapisiXml({
        podnapisiSubtitle(QStringLiteral("aB1"), QStringLiteral("The Matrix"), 1999, QStringLiteral("Rel.One Rel.Two"),
                          QStringLiteral("en"), 8.5, QStringLiteral("hn")),
        podnapisiSubtitle(QStringLiteral("cD2"), QStringLiteral("Amélie"), 2001, QString(), QStringLiteral("fr"), 0),
    }));
    QCOMPARE(results.size(), 2);
    QCOMPARE(results[0].provider, QStringLiteral("Podnapisi"));
    QCOMPARE(results[0].id, QStringLiteral("aB1"));
    QCOMPARE(results[0].fileName, QStringLiteral("Rel.One"));
    QCOMPARE(results[0].language, QStringLiteral("en"));
    QCOMPARE(results[0].rating, 8.5);
    QCOMPARE(results[0].format, QStringLiteral("srt"));
    QVERIFY(results[0].hearingImpaired);
    QCOMPARE(results[0].pageUrl, QUrl(QStringLiteral("http://www.podnapisi.net/subtitles/aB1")));
    // Without a release, the title and year.
    QCOMPARE(results[1].fileName, QStringLiteral("Amélie (2001)"));
    QVERIFY(!results[1].hearingImpaired);
    QVERIFY(PodnapisiClient::parseResults("<results></results>").isEmpty());
}

void SubtitleTest::showAndHideSubtitles()
{
    m_window->loadSubtitle(m_srt1);
    QTRY_COMPARE(propString("sid"), QStringLiteral("1"));
    QVERIFY(prop("sub-visibility").toBool());
    QTRY_COMPARE(propString("sub-text"), QStringLiteral("Hello from the mock server"));

    // Alt+H hides and shows them, and the menu item follows.
    QMenu *subs = subtitleMenu();
    QVERIFY(subs);
    QAction *show = menuAction(subs, QStringLiteral("Show Subtitles"));
    QVERIFY(show);
    press(Qt::Key_H, Qt::AltModifier);
    QTRY_VERIFY(!prop("sub-visibility").toBool());
    Q_EMIT m_window->findChild<PlayerMenu *>()->aboutToShow();
    QVERIFY(!show->isChecked());
    press(Qt::Key_H, Qt::AltModifier);
    QTRY_VERIFY(prop("sub-visibility").toBool());
    Q_EMIT m_window->findChild<PlayerMenu *>()->aboutToShow();
    QVERIFY(show->isChecked());
    show->trigger();
    QTRY_VERIFY(!prop("sub-visibility").toBool());
    show->trigger();
    QTRY_VERIFY(prop("sub-visibility").toBool());

    // Picking Off and a track in the track menu.
    QMenu *tracks = menuAction(subs, QStringLiteral("Subtitle Track"))->menu();
    menuAction(tracks, QStringLiteral("Off"))->trigger();
    QTRY_COMPARE(propString("sid"), QStringLiteral("no"));
    QTRY_COMPARE(propString("sub-text"), QString());
    QAction *first = nullptr;
    Q_EMIT tracks->aboutToShow();
    for (QAction *action : tracks->actions()) {
        if (action->text().startsWith(QLatin1String("#1")))
            first = action;
    }
    QVERIFY(first);
    first->trigger();
    QTRY_COMPARE(propString("sid"), QStringLiteral("1"));
    QTRY_COMPARE(propString("sub-text"), QStringLiteral("Hello from the mock server"));

    // A second track as secondary subtitles, shown and hidden on their own.
    m_window->loadSubtitle(m_srt2);
    QTRY_COMPARE(propString("sid"), QStringLiteral("2"));
    m_mpv->setMpvProperty(QStringLiteral("secondary-sid"), QStringLiteral("1"));
    QTRY_COMPARE(propString("secondary-sid"), QStringLiteral("1"));
    press(Qt::Key_H, Qt::AltModifier | Qt::ShiftModifier);
    QTRY_VERIFY(!prop("secondary-sub-visibility").toBool());
    QVERIFY(prop("sub-visibility").toBool());
    press(Qt::Key_H, Qt::AltModifier | Qt::ShiftModifier);
    QTRY_VERIFY(prop("secondary-sub-visibility").toBool());
}

void SubtitleTest::searchesWithoutKey()
{
    if (!SubtitleSearch::builtInApiKey().isEmpty())
        QSKIP("This build has a built-in OpenSubtitles key");
    SubtitleDownloadDialog *dialog = openDialog();
    QVERIFY(dialog);
    QCOMPARE(dialog->findChild<QLineEdit *>(QStringLiteral("SubtitleQuery"))->text(), QStringLiteral("The Matrix"));
    QCOMPARE(dialog->findChild<QComboBox *>(QStringLiteral("SubtitleLanguage"))->currentData().toString(), QStringLiteral("en"));
    auto *hashButton = dialog->findChild<QPushButton *>(QStringLiteral("SubtitleHashButton"));
    QCOMPARE(hashButton->text(), QStringLiteral("Search by Hash (Exact Match)"));
    QVERIFY(hashButton->isEnabled());
    QCOMPARE(dialog->findChild<QPushButton *>(QStringLiteral("SubtitleNameButton"))->text(), QStringLiteral("Search by Name"));
    QCOMPARE(dialog->movieHash(), SubtitleHasher::hash(m_video));

    // No key, no OpenSubtitles: only podnapisi.net was asked, by name, as
    // TopPlayer.
    QVERIFY(requests(QStringLiteral("/api/")).isEmpty());
    const QList<MockServer::Request> searches = requests(QStringLiteral("/subtitles/search/old"));
    QCOMPARE(searches.size(), 1);
    const QUrlQuery query(searches.first().url);
    QCOMPARE(query.queryItemValue(QStringLiteral("sXML")), QStringLiteral("1"));
    QCOMPARE(query.queryItemValue(QStringLiteral("sK"), QUrl::FullyDecoded), QStringLiteral("The Matrix"));
    QCOMPARE(query.queryItemValue(QStringLiteral("sL")), QStringLiteral("en"));
    QCOMPARE(query.queryItemValue(QStringLiteral("sY")), QStringLiteral("1999"));
    const QByteArray agent = searches.first().headers.value("user-agent");
    QCOMPARE(agent, QByteArray("TopPlayer/" APP_VERSION " (Linux; Qt6)"));
    QVERIFY(!agent.contains("VLC"));
    QVERIFY(!searches.first().headers.contains("api-key"));

    // The release named like the file comes first.
    auto *table = dialog->findChild<QTreeWidget *>(QStringLiteral("SubtitleResults"));
    QStringList headers;
    for (int column = 0; column < table->columnCount(); ++column)
        headers.append(table->headerItem()->text(column));
    QCOMPARE(headers, (QStringList{QStringLiteral("Language"), QStringLiteral("Subtitle Title / Release"),
                                   QStringLiteral("Provider"), QStringLiteral("Rating"), QStringLiteral("Format")}));
    QCOMPARE(table->topLevelItemCount(), 3);
    QTreeWidgetItem *top = table->topLevelItem(0);
    QCOMPARE(top->text(1), kVideoName);
    QCOMPARE(top->data(1, Qt::UserRole + 2).toStringList(), QStringList{QStringLiteral("Release match")});
    QCOMPARE(top->text(0), QStringLiteral("English"));
    QCOMPARE(top->text(2), QStringLiteral("Podnapisi"));
    QCOMPARE(top->text(3), QStringLiteral("9.0"));
    QCOMPARE(top->text(4), QStringLiteral(".srt"));
    QTreeWidgetItem *hearingImpaired = table->topLevelItem(2);
    QCOMPARE(hearingImpaired->text(1), QStringLiteral("The Matrix (1999)"));
    QCOMPARE(hearingImpaired->data(1, Qt::UserRole + 2).toStringList(), QStringList{QStringLiteral("HI")});
    QCOMPARE(hearingImpaired->text(3), QStringLiteral("–"));
    // Says why there are no exact matches.
    auto *status = dialog->findChild<QLabel *>(QStringLiteral("SubtitleStatus"));
    QVERIFY(status->text().contains(QLatin1String("Exact matching needs OpenSubtitles")));
    QVERIFY(status->text().endsWith(QLatin1String("3 subtitle(s) found")));
}

void SubtitleTest::downloadAndPlay()
{
    // Hidden subtitles come back for a subtitle the user just downloaded.
    m_mpv->setMpvProperty(QStringLiteral("sub-visibility"), QStringLiteral("no"));
    SubtitleDownloadDialog *dialog = openDialog();
    QVERIFY(dialog);
    auto *table = dialog->findChild<QTreeWidget *>(QStringLiteral("SubtitleResults"));
    table->setCurrentItem(table->topLevelItem(0));
    m_server.requests.clear();
    auto *button = dialog->findChild<QPushButton *>(QStringLiteral("SubtitleDownloadButton"));
    QCOMPARE(button->text(), QStringLiteral("Download && Play"));
    QTest::mouseClick(button, Qt::LeftButton);
    QTRY_VERIFY_WITH_TIMEOUT(!m_window->findChild<SubtitleDownloadDialog *>(), 10000);

    // Fetched as a ZIP from the result's page, unpacked into the cache.
    QCOMPARE(m_server.requests.size(), 1);
    const MockServer::Request &request = m_server.requests.first();
    QCOMPARE(request.url.path(), QStringLiteral("/subtitles/cD2/download"));
    QCOMPARE(QUrlQuery(request.url).queryItemValue(QStringLiteral("container")), QStringLiteral("zip"));
    QCOMPARE(request.headers.value("referer"), QByteArray("http://www.podnapisi.net/subtitles/cD2"));
    const QString saved = SubtitleSearch::cacheDir() + QStringLiteral("/%1.en.srt").arg(kVideoName);
    QVERIFY(saved.startsWith(qEnvironmentVariable("XDG_CACHE_HOME")));
    QFile file(saved);
    QVERIFY(file.open(QIODevice::ReadOnly));
    QCOMPARE(file.readAll(), kSrt);

    // Selected, shown, and confirmed on the OSD.
    QTRY_COMPARE(propString("current-tracks/sub/external-filename"), saved);
    QTRY_VERIFY(prop("sub-visibility").toBool());
    QTRY_COMPARE(propString("sub-text"), QStringLiteral("Hello from the mock server"));
    auto *osd = m_window->findChild<OsdWidget *>();
    QVERIFY(osd->isVisible());
    QCOMPARE(osd->text(), QStringLiteral("Subtitles loaded: English / %1.en.srt").arg(kVideoName));
}

void SubtitleTest::exactMatchWithKey()
{
    SubtitleSearch::setUserApiKey(QStringLiteral("test-key"));
    SubtitleDownloadDialog *dialog = openDialog();
    QVERIFY(dialog);
    // Only the hash and language: exact matches.
    const QList<MockServer::Request> searches = requests(QStringLiteral("/api/v1/subtitles"));
    QCOMPARE(searches.size(), 1);
    const QUrlQuery query(searches.first().url);
    QCOMPARE(query.queryItemValue(QStringLiteral("moviehash")), SubtitleHasher::hash(m_video));
    QVERIFY(!query.hasQueryItem(QStringLiteral("query")));
    QCOMPARE(searches.first().headers.value("api-key"), QByteArray("test-key"));
    QVERIFY(requests(QStringLiteral("/subtitles/search")).isEmpty());

    auto *table = dialog->findChild<QTreeWidget *>(QStringLiteral("SubtitleResults"));
    QCOMPARE(table->topLevelItemCount(), 1);
    QCOMPARE(table->topLevelItem(0)->text(2), QStringLiteral("OpenSubtitles"));
    QCOMPARE(table->topLevelItem(0)->data(1, Qt::UserRole + 2).toStringList(), QStringList{QStringLiteral("Exact match")});

    dialog->downloadSelected();
    QTRY_VERIFY_WITH_TIMEOUT(!m_window->findChild<SubtitleDownloadDialog *>(), 10000);
    QTRY_VERIFY(propString("current-tracks/sub/external-filename").startsWith(SubtitleSearch::cacheDir()));
}

void SubtitleTest::exactFallsBackToName()
{
    SubtitleSearch::setUserApiKey(QStringLiteral("test-key"));
    m_server.handler = [this](const MockServer::Request &request) -> MockServer::Response {
        // OpenSubtitles knows no exact match.
        if (request.url.path() == QLatin1String("/api/v1/subtitles") && QUrlQuery(request.url).hasQueryItem(QStringLiteral("moviehash")))
            return {200, "{\"data\":[]}"};
        return defaultResponse(request);
    };
    SubtitleDownloadDialog *dialog = openDialog();
    QVERIFY(dialog);
    QTRY_VERIFY(!requests(QStringLiteral("/subtitles/search/old")).isEmpty());
    QTRY_COMPARE(dialog->findChild<QTreeWidget *>(QStringLiteral("SubtitleResults"))->topLevelItemCount(), 3);
    QVERIFY(dialog->findChild<QLabel *>(QStringLiteral("SubtitleStatus"))->text().startsWith(
        QLatin1String("No exact match for this file; these are matches by name.")));
}

void SubtitleTest::providerFallback()
{
    SubtitleSearch::setUserApiKey(QStringLiteral("test-key"));
    m_server.handler = [this](const MockServer::Request &request) -> MockServer::Response {
        if (request.url.path() == QLatin1String("/subtitles/search/old"))
            return {503, "<html>busy</html>", "text/html"};
        return defaultResponse(request);
    };
    SubtitleDownloadDialog *dialog = openDialog();
    QVERIFY(dialog);
    m_server.requests.clear();
    // podnapisi.net is down: OpenSubtitles answers the name search instead.
    dialog->searchByName();
    QTRY_VERIFY_WITH_TIMEOUT(!dialog->isBusy() && !requests(QStringLiteral("/api/v1/subtitles")).isEmpty(), 10000);
    QCOMPARE(requests(QStringLiteral("/subtitles/search/old")).size(), 1);
    QCOMPARE(QUrlQuery(requests(QStringLiteral("/api/v1/subtitles")).first().url).queryItemValue(QStringLiteral("query"), QUrl::FullyDecoded),
             QStringLiteral("the matrix"));
    auto *table = dialog->findChild<QTreeWidget *>(QStringLiteral("SubtitleResults"));
    QTRY_COMPARE(table->topLevelItemCount(), 1);
    QCOMPARE(table->topLevelItem(0)->text(2), QStringLiteral("OpenSubtitles"));
}

void SubtitleTest::manualSearch()
{
    SubtitleDownloadDialog *dialog = openDialog();
    QVERIFY(dialog);
    m_server.requests.clear();
    auto *query = dialog->findChild<QLineEdit *>(QStringLiteral("SubtitleQuery"));
    query->setFocus();
    // (QTest can't type non-ASCII keys.)
    query->setText(QStringLiteral("Tom & Jerry + Friends Été"));
    auto *language = dialog->findChild<QComboBox *>(QStringLiteral("SubtitleLanguage"));
    QVERIFY(language->findData(QStringLiteral("am")) >= 0); // Amharic is offered
    language->setCurrentIndex(language->findData(QStringLiteral("fr")));
    QTest::keyClick(query, Qt::Key_Return);
    QTRY_VERIFY_WITH_TIMEOUT(!dialog->isBusy() && !m_server.requests.isEmpty(), 10000);

    // A typed title: no year from the file name; the language is remembered.
    const QUrlQuery sent(requests(QStringLiteral("/subtitles/search/old")).first().url);
    QCOMPARE(sent.queryItemValue(QStringLiteral("sK"), QUrl::FullyDecoded), QStringLiteral("Tom & Jerry + Friends Été"));
    QCOMPARE(sent.queryItemValue(QStringLiteral("sL")), QStringLiteral("fr"));
    QVERIFY(!sent.hasQueryItem(QStringLiteral("sY")));
    QCOMPARE(SubtitleSearch::language(), QStringLiteral("fr"));
}

void SubtitleTest::noResults()
{
    m_server.handler = [](const MockServer::Request &) -> MockServer::Response {
        return {200, podnapisiXml({}), "text/xml"};
    };
    SubtitleDownloadDialog *dialog = openDialog();
    QVERIFY(dialog);
    auto *status = dialog->findChild<QLabel *>(QStringLiteral("SubtitleStatus"));
    QTRY_VERIFY(status->text().endsWith(QLatin1String("No subtitles found in English. Try another title or language.")));
    QCOMPARE(dialog->findChild<QTreeWidget *>(QStringLiteral("SubtitleResults"))->topLevelItemCount(), 0);
    QVERIFY(!dialog->findChild<QPushButton *>(QStringLiteral("SubtitleDownloadButton"))->isEnabled());
    QVERIFY(dialog->findChild<QPushButton *>(QStringLiteral("SubtitleNameButton"))->isEnabled());
}

void SubtitleTest::offline()
{
    // Nothing listens on port 1.
    qputenv("TOPPLAYER_PODNAPISI_URL", "http://127.0.0.1:1");
    m_window->activateWindow();
    m_mpv->setFocus();
    QTRY_VERIFY(m_window->isActiveWindow());
    press(Qt::Key_D);
    SubtitleDownloadDialog *dialog = nullptr;
    QTRY_VERIFY((dialog = m_window->findChild<SubtitleDownloadDialog *>()));
    auto *status = dialog->findChild<QLabel *>(QStringLiteral("SubtitleStatus"));
    QTRY_COMPARE_WITH_TIMEOUT(status->text(), QStringLiteral("Can't reach podnapisi.net. Check your internet connection."), 10000);
    QVERIFY(!dialog->isBusy());
    QVERIFY(!dialog->findChild<QProgressBar *>(QStringLiteral("SubtitleProgress"))->isVisible());
    QVERIFY(m_mpv->isVisible()); // and the player carries on
    qputenv("TOPPLAYER_PODNAPISI_URL", m_server.url(QString()).toUtf8());
}

void SubtitleTest::badDownload()
{
    m_server.handler = [this](const MockServer::Request &request) -> MockServer::Response {
        if (request.url.path().endsWith(QLatin1String("/download")))
            return {200, "<!DOCTYPE html><html>Please log in</html>", "text/html"};
        return defaultResponse(request);
    };
    SubtitleDownloadDialog *dialog = openDialog();
    QVERIFY(dialog);
    dialog->downloadSelected();
    auto *status = dialog->findChild<QLabel *>(QStringLiteral("SubtitleStatus"));
    QTRY_COMPARE_WITH_TIMEOUT(status->text(), QStringLiteral("podnapisi.net didn't send the subtitle file. Try another one."), 10000);
    QVERIFY(dialog->isVisible());
    QVERIFY(!dialog->isBusy());
    QCOMPARE(propString("sid"), QStringLiteral("no"));
}

void SubtitleTest::cancelSearch()
{
    bool hold = false;
    m_server.handler = [&](const MockServer::Request &request) {
        MockServer::Response response = defaultResponse(request);
        response.hold = hold;
        return response;
    };
    SubtitleDownloadDialog *dialog = openDialog();
    QVERIFY(dialog);
    hold = true;
    dialog->searchByName();
    auto *progress = dialog->findChild<QProgressBar *>(QStringLiteral("SubtitleProgress"));
    QVERIFY(dialog->isBusy());
    QVERIFY(progress->isVisible());
    QCOMPARE(progress->maximum(), 0); // indeterminate
    QVERIFY(!dialog->findChild<QPushButton *>(QStringLiteral("SubtitleNameButton"))->isEnabled());

    // Cancel stops the request first and keeps the dialog; a second one closes it.
    auto *cancel = dialog->findChild<QPushButton *>(QStringLiteral("SubtitleCancelButton"));
    QTest::mouseClick(cancel, Qt::LeftButton);
    QVERIFY(!dialog->isBusy());
    QVERIFY(!progress->isVisible());
    QCOMPARE(dialog->findChild<QLabel *>(QStringLiteral("SubtitleStatus"))->text(), QStringLiteral("Cancelled."));
    QTest::mouseClick(cancel, Qt::LeftButton);
    QTRY_VERIFY(!m_window->findChild<SubtitleDownloadDialog *>());
}

void SubtitleTest::smallFileSearchesByName()
{
    // Under 128 KiB: no hash, so the dialog searches by name.
    const QString small = m_dir.filePath(QStringLiteral("Small.Clip.2020.mkv"));
    QVERIFY(makeTestClip(small, 2));
    QVERIFY(QFileInfo(small).size() < SubtitleHasher::kMinimumSize);
    m_window->openFile(small);
    QTRY_COMPARE_WITH_TIMEOUT(propString("path"), small, 10000);
    // (A request held back by the previous test can still trickle in.)
    QTest::qWait(100);
    m_server.requests.clear();
    SubtitleDownloadDialog *dialog = openDialog();
    QVERIFY(dialog);
    QVERIFY(dialog->movieHash().isEmpty());
    QVERIFY(!dialog->findChild<QPushButton *>(QStringLiteral("SubtitleHashButton"))->isEnabled());
    const QList<MockServer::Request> searches = requests(QStringLiteral("/subtitles/search/old"));
    QVERIFY(!searches.isEmpty());
    QCOMPARE(QUrlQuery(searches.last().url).queryItemValue(QStringLiteral("sK"), QUrl::FullyDecoded), QStringLiteral("Small Clip"));
}

int main(int argc, char *argv[])
{
    // Keep the settings and downloads away from the user's own.
    QTemporaryDir config;
    qputenv("XDG_CONFIG_HOME", (config.path() + QStringLiteral("/config")).toLocal8Bit());
    qputenv("XDG_CACHE_HOME", (config.path() + QStringLiteral("/cache")).toLocal8Bit());
    QApplication app(argc, argv);
    // Reopened files play from the start instead of asking to resume (tst_resume covers that).
    ResumeManager::setMode(ResumeManager::Mode::Never);
    // libmpv requires the C numeric locale; QApplication may have changed it.
    std::setlocale(LC_NUMERIC, "C");
    SubtitleTest test;
    QTEST_SET_MAIN_SOURCE_PATH
    return QTest::qExec(&test, argc, argv);
}

#include "tst_subtitles.moc"
