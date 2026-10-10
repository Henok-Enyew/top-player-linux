// Lyrics: LRC / SRT parsing and writing, the online search (LRCLIB and
// lyrics.ovh, against a local mock server), the karaoke-style lyrics view
// following playback, loading and downloading lyrics, the AI prompt, and the
// sync editor for lyrics and subtitles. Drives the real window; needs a
// display (run under xvfb-run) and ffmpeg, which generates the test media.

#include "LyricsClient.h"
#include "LyricsController.h"
#include "LyricsDialogs.h"
#include "LyricsView.h"
#include "MainWindow.h"
#include "MpvWidget.h"
#include "SyncEditorDialog.h"
#include "TestClip.h"

#include <QApplication>
#include <QClipboard>
#include <QDialogButtonBox>
#include <QSlider>
#include <QWheelEvent>
#include <QDir>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPlainTextEdit>
#include <QProcess>
#include <QPushButton>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QScreen>
#include <QTest>
#include <QTreeWidget>
#include <QUrlQuery>

#include <clocale>
#include <cmath>
#include <functional>

namespace {

bool ffmpeg(const QStringList &args)
{
    const QString program = QStandardPaths::findExecutable(QStringLiteral("ffmpeg"));
    if (program.isEmpty())
        return false;
    QProcess process;
    process.start(program, QStringList{QStringLiteral("-loglevel"), QStringLiteral("error"), QStringLiteral("-y")} + args);
    return process.waitForFinished(60000) && process.exitStatus() == QProcess::NormalExit && process.exitCode() == 0;
}

bool writeFile(const QString &path, const QByteArray &data)
{
    QFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write(data) == data.size();
}

const char kSongLrc[] =
    "[ti:Midnight Drive]\n"
    "[ar:Neon Coast]\n"
    "[00:01.00]First line\n"
    "[00:05.00]Second line\n"
    "[00:10.00]Third line\n"
    "[00:15.00]\n"
    "[00:20.00]Fourth line\n"
    "[00:25.00]Last line\n";

// Answers HTTP requests with `handler`, like the lyrics services would.
class MockServer : public QObject
{
public:
    std::function<QPair<int, QByteArray>(const QUrl &)> handler;
    QList<QUrl> requests;

    bool listen() { return m_server.listen(QHostAddress::LocalHost); }
    QString url() const { return QStringLiteral("http://127.0.0.1:%1").arg(m_server.serverPort()); }

    MockServer()
    {
        connect(&m_server, &QTcpServer::newConnection, this, [this] {
            while (QTcpSocket *socket = m_server.nextPendingConnection()) {
                connect(socket, &QTcpSocket::readyRead, this, [this, socket] {
                    m_buffers[socket] += socket->readAll();
                    const QByteArray &buffer = m_buffers[socket];
                    if (!buffer.contains("\r\n\r\n"))
                        return;
                    const QUrl url(QString::fromLatin1(buffer.left(buffer.indexOf('\n')).trimmed().split(' ').value(1)));
                    m_buffers.remove(socket);
                    requests.append(url);
                    const auto [status, body] = handler ? handler(url) : qMakePair(404, QByteArray("{}"));
                    socket->write("HTTP/1.1 " + QByteArray::number(status) + " X\r\nContent-Type: application/json\r\n"
                                  "Content-Length: " + QByteArray::number(body.size()) + "\r\nConnection: close\r\n\r\n" + body);
                    socket->disconnectFromHost();
                });
                connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
            }
        });
    }

private:
    QTcpServer m_server;
    QHash<QTcpSocket *, QByteArray> m_buffers;
};

QJsonObject record(const QString &title, double duration, const QString &synced, const QString &plain)
{
    return QJsonObject{
        {QStringLiteral("trackName"), title},
        {QStringLiteral("artistName"), QStringLiteral("Neon Coast")},
        {QStringLiteral("albumName"), QStringLiteral("Late Summer Tapes")},
        {QStringLiteral("duration"), duration},
        {QStringLiteral("instrumental"), false},
        {QStringLiteral("syncedLyrics"), synced.isEmpty() ? QJsonValue() : QJsonValue(synced)},
        {QStringLiteral("plainLyrics"), plain},
    };
}

} // namespace

class LyricsTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void initTestCase();
    void init();
    void cleanup();

    // Formats.
    void parseLrc();
    void parsePlainLyrics();
    void parseSrtAndVtt();
    void writeLrcAndSrt();
    void activeLine();
    void decodeEncodings();

    // Online search.
    void lrclibSearch();
    void lyricsOvhFallback();
    void downloadDialogUsesSelection();

    // In the player.
    void sidecarLyricsFollowPlayback();
    void viewCentersActiveLine();
    void loadApplyAndRemove();
    void offsetAdjust();
    void queryFromFileName();
    void aiPrompt();
    void aiPromptPasteAnswer();
    void clickLineSeeks();
    void browseAndReturn();
    void plainLyricsOnlyScroll();
    void appearance();
    void smallWindowShowsFewerLines();
    void backgroundOverVideo();

    // Sync editor.
    void syncLyricsByTapping();
    void syncSubtitles();

private:
    void open(const QString &file);

    QTemporaryDir m_dir;
    QString m_song;       // has "<name>.lrc" beside it
    QString m_bareSong;   // no lyrics, tagged
    QString m_namedSong;  // "Artist - Title.wav", untagged
    QString m_video;
    MainWindow *m_window = nullptr;
    MpvWidget *m_mpv = nullptr;
    LyricsController *m_lyrics = nullptr;
};

void LyricsTest::initTestCase()
{
    QVERIFY(m_dir.isValid());
    const QStringList tone{QStringLiteral("-f"), QStringLiteral("lavfi"), QStringLiteral("-i"),
                           QStringLiteral("sine=f=440:d=60")};
    m_song = m_dir.filePath(QStringLiteral("songs/song.wav"));
    m_bareSong = m_dir.filePath(QStringLiteral("songs/bare.mka"));
    m_namedSong = m_dir.filePath(QStringLiteral("songs/Neon Coast - Midnight Drive.wav"));
    QVERIFY(QDir().mkpath(m_dir.filePath(QStringLiteral("songs"))));
    if (!ffmpeg(tone + QStringList{m_song}))
        QSKIP("ffmpeg is needed to generate the test media");
    QVERIFY(writeFile(m_dir.filePath(QStringLiteral("songs/song.lrc")), kSongLrc));
    QVERIFY(ffmpeg(tone + QStringList{QStringLiteral("-c:a"), QStringLiteral("flac"), QStringLiteral("-metadata"),
                                      QStringLiteral("title=Night Bus"), QStringLiteral("-metadata"),
                                      QStringLiteral("artist=Neon Coast"), QStringLiteral("-metadata"),
                                      QStringLiteral("album=Late Summer Tapes"), m_bareSong}));
    QVERIFY(ffmpeg(tone + QStringList{m_namedSong}));
    m_video = m_dir.filePath(QStringLiteral("video.mkv"));
    QVERIFY(makeTestClip(m_video, 60));
}

void LyricsTest::init()
{
    m_window = new MainWindow;
    m_window->resize(800, 500);
    m_window->show();
    m_window->activateWindow();
    QVERIFY(QTest::qWaitForWindowActive(m_window));
    m_mpv = m_window->findChild<MpvWidget *>();
    m_lyrics = m_window->lyrics();
    QVERIFY(m_mpv && m_lyrics);
    m_mpv->setMpvProperty(QStringLiteral("ao"), QStringLiteral("null"));
}

void LyricsTest::cleanup()
{
    for (QWidget *widget : QApplication::topLevelWidgets()) {
        if (widget != m_window && qobject_cast<QDialog *>(widget))
            widget->close();
    }
    delete m_window;
    m_window = nullptr;
}

void LyricsTest::open(const QString &file)
{
    m_window->openFile(file);
    m_mpv->setMpvProperty(QStringLiteral("pause"), QStringLiteral("yes"));
    QTRY_COMPARE_WITH_TIMEOUT(m_mpv->mpvPropertyString(QStringLiteral("path")), file, 10000);
    QTRY_VERIFY_WITH_TIMEOUT(m_mpv->mpvProperty(QStringLiteral("time-pos")).isValid(), 10000);
    // The lyrics are looked up once the file has loaded.
    QTRY_COMPARE_WITH_TIMEOUT(m_lyrics->trackPath(), file, 10000);
}

void LyricsTest::parseLrc()
{
    const Lyrics::Document doc = Lyrics::parseLrc(QStringLiteral(
        "\uFEFF[ar: Artist ]\r\n[ti:Song]\n[al:Album]\n[offset:+500]\n[length:03:20]\n"
        "[00:20.50][01:10.00]Chorus line\n"
        "[00:05.123]<00:05.12>Word <00:05.60>stamps\n"
        "[00:01.5]First\n"
        "not a lyric without time\n"));
    QCOMPARE(doc.artist, QStringLiteral("Artist"));
    QCOMPARE(doc.title, QStringLiteral("Song"));
    QCOMPARE(doc.album, QStringLiteral("Album"));
    QCOMPARE(doc.offset, 0.5);
    QCOMPARE(doc.length, 200.0);
    QVERIFY(doc.isSynced());
    QCOMPARE(doc.lines.size(), 4);
    QCOMPARE(doc.lines[0].text, QStringLiteral("First"));
    QCOMPARE(doc.lines[0].start, 1.5);
    QCOMPARE(doc.lines[1].text, QStringLiteral("Word stamps"));
    QVERIFY(std::abs(doc.lines[1].start - 5.123) < 1e-6);
    QCOMPARE(doc.lines[2].start, 20.5);
    QCOMPARE(doc.lines[3].start, 70.0);
    QCOMPARE(doc.lines[3].text, QStringLiteral("Chorus line"));
}

void LyricsTest::parsePlainLyrics()
{
    const Lyrics::Document doc = Lyrics::parseLrc(QStringLiteral("\n\nVerse one\nline two\n\nChorus\n\n"));
    QVERIFY(!doc.isSynced());
    QCOMPARE(doc.lines.size(), 4);
    QCOMPARE(doc.lines[0].text, QStringLiteral("Verse one"));
    QCOMPARE(doc.lines[2].text, QString());
    QCOMPARE(doc.lines[3].text, QStringLiteral("Chorus"));
    QCOMPARE(Lyrics::activeLine(doc, 10), -1);
}

void LyricsTest::parseSrtAndVtt()
{
    const Lyrics::Document srt = Lyrics::parseSrt(QStringLiteral(
        "1\r\n00:00:01,000 --> 00:00:02,500\r\nHello\r\nthere\r\n\r\n2\r\n00:00:04,000 --> 00:00:05,000\r\n<i>Bye</i>\r\n"));
    QCOMPARE(srt.lines.size(), 2);
    QCOMPARE(srt.lines[0].start, 1.0);
    QCOMPARE(srt.lines[0].end, 2.5);
    QCOMPARE(srt.lines[0].text, QStringLiteral("Hello\nthere"));
    QCOMPARE(srt.lines[1].text, QStringLiteral("<i>Bye</i>"));

    const Lyrics::Document vtt = Lyrics::parseSrt(QStringLiteral(
        "WEBVTT\n\nNOTE comment\n\n00:01.000 --> 00:02.000 align:start\nOne\n\n01:00:00.250 --> 01:00:01.000\nTwo\n"));
    QCOMPARE(vtt.lines.size(), 2);
    QCOMPARE(vtt.lines[0].start, 1.0);
    QCOMPARE(vtt.lines[1].start, 3600.25);
}

void LyricsTest::writeLrcAndSrt()
{
    Lyrics::Document doc = Lyrics::parseLrc(QString::fromLatin1(kSongLrc));
    doc.offset = -0.25;
    const QString lrc = Lyrics::toLrc(doc);
    QVERIFY(lrc.contains(QLatin1String("[ti:Midnight Drive]")));
    QVERIFY(lrc.contains(QLatin1String("[offset:-250]")));
    QVERIFY(lrc.contains(QLatin1String("[00:10.00]Third line")));
    const Lyrics::Document back = Lyrics::parseLrc(lrc);
    QCOMPARE(back.lines.size(), doc.lines.size());
    QCOMPARE(back.offset, -0.25);
    for (int i = 0; i < doc.lines.size(); ++i) {
        QCOMPARE(back.lines[i].start, doc.lines[i].start);
        QCOMPARE(back.lines[i].text, doc.lines[i].text);
    }

    // Cues without an end run to the next cue, at most 4 s; empty lines are dropped.
    const QString srt = Lyrics::toSrt(doc);
    QVERIFY(srt.startsWith(QLatin1String("1\n00:00:01,000 --> 00:00:05,000\nFirst line\n\n")));
    QVERIFY(srt.contains(QLatin1String("00:00:25,000 --> 00:00:29,000\nLast line")));
    QCOMPARE(Lyrics::parseSrt(srt).lines.size(), 5);

    const QString path = m_dir.filePath(QStringLiteral("out/written.srt"));
    QVERIFY(Lyrics::save(doc, path));
    QCOMPARE(Lyrics::load(path).lines.size(), 5);

    QCOMPARE(Lyrics::formatLrcTime(83.456), QStringLiteral("01:23.46"));
    QCOMPARE(Lyrics::formatSrtTime(3723.5), QStringLiteral("01:02:03,500"));
    QCOMPARE(Lyrics::parseTime(QStringLiteral("01:02:03,500")), 3723.5);
    QCOMPARE(Lyrics::parseTime(QStringLiteral("bad")), -1.0);
}

void LyricsTest::activeLine()
{
    Lyrics::Document doc = Lyrics::parseLrc(QString::fromLatin1(kSongLrc));
    QCOMPARE(Lyrics::activeLine(doc, 0.5), -1);
    QCOMPARE(Lyrics::activeLine(doc, 1.0), 0);
    QCOMPARE(Lyrics::activeLine(doc, 7), 1);
    QCOMPARE(Lyrics::activeLine(doc, 100), 5);
    // A positive offset shows lines earlier.
    doc.offset = 1;
    QCOMPARE(Lyrics::activeLine(doc, 4.2), 1);
    // A subtitle gap shows nothing.
    const Lyrics::Document srt = Lyrics::parseSrt(QStringLiteral("1\n00:00:01,000 --> 00:00:02,000\nA\n"));
    QCOMPARE(Lyrics::activeLine(srt, 1.5), 0);
    QCOMPARE(Lyrics::activeLine(srt, 3), -1);
}

void LyricsTest::decodeEncodings()
{
    const QString text = QStringLiteral("[00:01.00]Café ♪");
    QCOMPARE(Lyrics::decode(text.toUtf8()), text);
    QCOMPARE(Lyrics::decode("\xEF\xBB\xBF" + text.toUtf8()), text);
    QByteArray utf16("\xFF\xFE");
    utf16.append(reinterpret_cast<const char *>(text.utf16()), text.size() * 2);
    QCOMPARE(Lyrics::decode(utf16), text);
    QCOMPARE(Lyrics::decode("Caf\xE9"), QStringLiteral("Café")); // Latin-1
}

void LyricsTest::lrclibSearch()
{
    MockServer server;
    QVERIFY(server.listen());
    server.handler = [](const QUrl &url) {
        if (url.path() != QLatin1String("/api/search"))
            return qMakePair(404, QByteArray("{}"));
        const QJsonArray results{
            record(QStringLiteral("Midnight Drive (Live)"), 300, {}, QStringLiteral("plain only")),
            record(QStringLiteral("Midnight Drive (Edit)"), 150, QStringLiteral("[00:01.00]edit"), QStringLiteral("edit")),
            record(QStringLiteral("Midnight Drive"), 61, QStringLiteral("[00:01.00]album"), QStringLiteral("album")),
            record(QStringLiteral("Empty"), 61, {}, {}),
        };
        return qMakePair(200, QJsonDocument(results).toJson());
    };
    LyricsClient client;
    client.setLrclibUrl(QUrl(server.url()));
    QSignalSpy finished(&client, &LyricsClient::searchFinished);
    client.search({QStringLiteral("Midnight Drive"), QStringLiteral("Neon Coast"), {}, 60});
    QVERIFY(finished.wait(10000));
    const auto results = finished.first().first().value<QList<LyricsClient::Result>>();
    // Synced first, the closest length first among them; empty records dropped.
    QCOMPARE(results.size(), 3);
    QCOMPARE(results[0].title, QStringLiteral("Midnight Drive"));
    QVERIFY(results[0].isSynced());
    QCOMPARE(results[1].title, QStringLiteral("Midnight Drive (Edit)"));
    QVERIFY(!results[2].isSynced());
    QCOMPARE(results[2].text(), QStringLiteral("plain only"));
    const QUrlQuery query(server.requests.first());
    QCOMPARE(query.queryItemValue(QStringLiteral("track_name")), QStringLiteral("Midnight Drive"));
    QCOMPARE(query.queryItemValue(QStringLiteral("artist_name")), QStringLiteral("Neon Coast"));

    QSignalSpy failed(&client, &LyricsClient::failed);
    client.search({});
    QCOMPARE(failed.size(), 1);
}

void LyricsTest::lyricsOvhFallback()
{
    MockServer server;
    QVERIFY(server.listen());
    server.handler = [](const QUrl &url) {
        if (url.path() == QLatin1String("/api/search"))
            return qMakePair(200, QByteArray("[]"));
        if (url.path() == QLatin1String("/v1/AC DC/Back In Black"))
            return qMakePair(200, QByteArray(R"({"lyrics":"Back in black\nI hit the sack"})"));
        return qMakePair(404, QByteArray(R"({"error":"No lyrics found"})"));
    };
    LyricsClient client;
    client.setLrclibUrl(QUrl(server.url()));
    client.setLyricsOvhUrl(QUrl(server.url()));
    QSignalSpy finished(&client, &LyricsClient::searchFinished);
    client.search({QStringLiteral("Back In Black"), QStringLiteral("AC/DC"), {}, 0});
    QVERIFY(finished.wait(10000));
    auto results = finished.first().first().value<QList<LyricsClient::Result>>();
    QCOMPARE(results.size(), 1);
    QCOMPARE(results[0].source, QStringLiteral("lyrics.ovh"));
    QVERIFY(results[0].plainLyrics.startsWith(QLatin1String("Back in black")));

    finished.clear();
    client.search({QStringLiteral("Unknown"), QStringLiteral("Nobody"), {}, 0});
    QVERIFY(finished.wait(10000));
    QVERIFY(finished.first().first().value<QList<LyricsClient::Result>>().isEmpty());
}

void LyricsTest::downloadDialogUsesSelection()
{
    MockServer server;
    QVERIFY(server.listen());
    server.handler = [](const QUrl &) {
        const QJsonArray results{record(QStringLiteral("Night Bus"), 60, QStringLiteral("[00:02.00]Downloaded line\n"
                                                                                         "[00:04.00]Second downloaded"),
                                        QStringLiteral("Downloaded line"))};
        return qMakePair(200, QJsonDocument(results).toJson());
    };
    qputenv("TOPPLAYER_LRCLIB_URL", server.url().toUtf8());
    open(m_bareSong);
    m_lyrics->openDownloadDialog();
    LyricsDownloadDialog *dialog = nullptr;
    QTRY_VERIFY((dialog = m_window->findChild<LyricsDownloadDialog *>()));
    // Seeded from the tags, and searched right away.
    QCOMPARE(dialog->findChild<QLineEdit *>(QStringLiteral("LyricsTitleEdit"))->text(), QStringLiteral("Night Bus"));
    QCOMPARE(dialog->findChild<QLineEdit *>(QStringLiteral("LyricsArtistEdit"))->text(), QStringLiteral("Neon Coast"));
    QTRY_COMPARE_WITH_TIMEOUT(dialog->resultList()->topLevelItemCount(), 1, 10000);
    QVERIFY(dialog->findChild<QPlainTextEdit *>(QStringLiteral("LyricsPreview"))->toPlainText().contains(
        QLatin1String("Downloaded line")));
    dialog->useSelected();
    QTRY_VERIFY(m_lyrics->document().isSynced());
    QCOMPARE(m_lyrics->document().lines.first().text, QStringLiteral("Downloaded line"));
    QVERIFY(m_lyrics->isShown());
    QVERIFY(m_lyrics->view()->isVisible());
    // Saved for the next time the song plays.
    QVERIFY(QFileInfo(Lyrics::storagePathFor(m_bareSong)).isFile());
    QCOMPARE(Lyrics::findFor(m_bareSong), Lyrics::storagePathFor(m_bareSong));
    m_lyrics->removeLyrics();
    QVERIFY(Lyrics::findFor(m_bareSong).isEmpty());
    qunsetenv("TOPPLAYER_LRCLIB_URL");
}

void LyricsTest::sidecarLyricsFollowPlayback()
{
    open(m_song);
    // Found next to the song and shown by themselves.
    QTRY_VERIFY_WITH_TIMEOUT(m_lyrics->isShown(), 5000);
    QCOMPARE(m_lyrics->lyricsPath(), m_dir.filePath(QStringLiteral("songs/song.lrc")));
    QVERIFY(m_lyrics->view()->isVisible());
    QCOMPARE(m_lyrics->document().lines.size(), 6);

    m_mpv->command({QStringLiteral("seek"), QStringLiteral("11"), QStringLiteral("absolute+exact")});
    QTRY_COMPARE_WITH_TIMEOUT(m_lyrics->view()->activeLine(), 2, 5000);
    m_mpv->command({QStringLiteral("seek"), QStringLiteral("26"), QStringLiteral("absolute+exact")});
    QTRY_COMPARE_WITH_TIMEOUT(m_lyrics->view()->activeLine(), 5, 5000);
    m_mpv->play();
    m_mpv->command({QStringLiteral("seek"), QStringLiteral("4.5"), QStringLiteral("absolute+exact")});
    QTRY_COMPARE_WITH_TIMEOUT(m_lyrics->view()->activeLine(), 1, 5000);

    // Y hides and shows them.
    QTest::keyClick(m_window, Qt::Key_Y);
    QVERIFY(!m_lyrics->isShown());
    QVERIFY(!m_lyrics->view()->isVisible());
    QTest::keyClick(m_window, Qt::Key_Y);
    QVERIFY(m_lyrics->view()->isVisible());

    // A video with no lyrics doesn't show them.
    open(m_video);
    QTRY_VERIFY(!m_lyrics->view()->isVisible());
    QVERIFY(m_lyrics->document().isEmpty());
}

void LyricsTest::viewCentersActiveLine()
{
    open(m_song);
    QTRY_VERIFY_WITH_TIMEOUT(m_lyrics->view()->isVisible(), 5000);
    LyricsView *view = m_lyrics->view();
    m_mpv->command({QStringLiteral("seek"), QStringLiteral("21"), QStringLiteral("absolute+exact")});
    QTRY_COMPARE_WITH_TIMEOUT(view->activeLine(), 4, 5000);
    // After the scroll animation, the active line sits in the middle.
    QTest::qWait(700);
    const QRect active = view->lineRect(4);
    QVERIFY2(std::abs(active.center().y() - (view->height() - 64) / 2) <= active.height(),
             qPrintable(QStringLiteral("%1 vs %2").arg(active.center().y()).arg(view->height())));
    QVERIFY(view->lineRect(5).top() > active.bottom());
    QVERIFY(view->lineRect(2).bottom() < active.top());
    // It draws: the active line is bright, a far line much dimmer.
    const QImage shot = view->grab().toImage();
    auto brightest = [&shot](const QRect &rect) {
        int best = 0;
        const QRect r = rect.intersected(shot.rect());
        for (int y = r.top(); y <= r.bottom(); y += 2)
            for (int x = r.left(); x <= r.right(); x += 2)
                best = std::max(best, qGray(shot.pixel(x, y)));
        return best;
    };
    QVERIFY(brightest(active) > 230);
    QVERIFY(brightest(view->lineRect(1)) < brightest(active) - 40);
}

void LyricsTest::loadApplyAndRemove()
{
    open(m_bareSong);
    QVERIFY(!m_lyrics->isShown());
    QVERIFY(m_lyrics->document().isEmpty());
    // Showing lyrics with none: a hint on how to get them.
    m_lyrics->setShown(true);
    QVERIFY(m_lyrics->view()->isVisible());
    m_lyrics->setShown(false);

    const QString file = m_dir.filePath(QStringLiteral("loaded.lrc"));
    QVERIFY(writeFile(file, "[00:03.00]Loaded from disk\n"));
    QVERIFY(m_lyrics->loadFile(file));
    QVERIFY(m_lyrics->view()->isVisible());
    QCOMPARE(m_lyrics->document().lines.first().text, QStringLiteral("Loaded from disk"));
    // Remembered for the track.
    QCOMPARE(Lyrics::findFor(m_bareSong), file);
    open(m_song);
    open(m_bareSong);
    QCOMPARE(m_lyrics->lyricsPath(), file);
    QVERIFY(m_lyrics->isShown());

    QVERIFY(!m_lyrics->applyText(QStringLiteral("   \n  "), QStringLiteral("nothing")));
    QVERIFY(m_lyrics->applyText(QStringLiteral("Plain words\nMore words"), QStringLiteral("typed")));
    QVERIFY(!m_lyrics->document().isSynced());
    QCOMPARE(m_lyrics->document().title, QStringLiteral("Night Bus"));
    m_lyrics->removeLyrics();
    QVERIFY(m_lyrics->document().isEmpty());
    QVERIFY(Lyrics::findFor(m_bareSong).isEmpty());
}

void LyricsTest::offsetAdjust()
{
    open(m_bareSong);
    QVERIFY(m_lyrics->applyText(QStringLiteral("[00:10.00]Ten\n[00:20.00]Twenty"), QStringLiteral("t")));
    m_lyrics->adjustOffset(0.5); // later
    QCOMPARE(m_lyrics->document().offset, -0.5);
    QCOMPARE(Lyrics::activeLine(m_lyrics->document(), 10.2), -1);
    QCOMPARE(Lyrics::activeLine(m_lyrics->document(), 10.6), 0);
    // Saved with the lyrics.
    QCOMPARE(Lyrics::load(Lyrics::findFor(m_bareSong)).offset, -0.5);
    m_lyrics->removeLyrics();
}

void LyricsTest::queryFromFileName()
{
    open(m_namedSong);
    const LyricsClient::Query q = m_lyrics->query();
    QCOMPARE(q.title, QStringLiteral("Midnight Drive"));
    QCOMPARE(q.artist, QStringLiteral("Neon Coast"));
    QVERIFY(std::abs(q.duration - 60) < 1);
    open(m_bareSong);
    QCOMPARE(m_lyrics->query().title, QStringLiteral("Night Bus"));
    QCOMPARE(m_lyrics->query().album, QStringLiteral("Late Summer Tapes"));
}

void LyricsTest::aiPrompt()
{
    const QString prompt = LyricsController::aiPrompt(
        {QStringLiteral("Midnight Drive"), QStringLiteral("Neon Coast"), QStringLiteral("Late Summer Tapes"), 225.4});
    QVERIFY(prompt.contains(QLatin1String("- Title: Midnight Drive")));
    QVERIFY(prompt.contains(QLatin1String("- Artist: Neon Coast")));
    QVERIFY(prompt.contains(QLatin1String("- Album: Late Summer Tapes")));
    QVERIFY(prompt.contains(QLatin1String("- Length: 03:45 (225 seconds)")));
    QVERIFY(prompt.contains(QLatin1String("[ti:Midnight Drive]")));
    QVERIFY(prompt.contains(QLatin1String("[length:03:45]")));
    QVERIFY(prompt.contains(QLatin1String("Neon Coast - Midnight Drive.lrc")));
    QVERIFY(prompt.contains(QLatin1String("[mm:ss.xx]")));
    // Without a length, none is made up.
    QVERIFY(!LyricsController::aiPrompt({QStringLiteral("X"), {}, {}, 0}).contains(QLatin1String("Length")));
}

void LyricsTest::aiPromptPasteAnswer()
{
    open(m_bareSong);
    m_lyrics->openAiPromptDialog();
    AiLyricsPromptDialog *dialog = nullptr;
    QTRY_VERIFY((dialog = m_window->findChild<AiLyricsPromptDialog *>()));
    QVERIFY(dialog->prompt().contains(QLatin1String("Night Bus")));
    QVERIFY(dialog->prompt().contains(QLatin1String("- Length: 01:00")));
    dialog->findChild<QPushButton *>(QStringLiteral("CopyPromptButton"))->click();
    QCOMPARE(QGuiApplication::clipboard()->text(), dialog->prompt());

    // A chat answer: words around a fenced LRC block.
    QGuiApplication::clipboard()->setText(QStringLiteral(
        "Here you go:\n```lrc\n[ti:Night Bus]\n[00:02.00]From the AI\n[00:06.00]Line two\n```\nEnjoy!"));
    QPointer<AiLyricsPromptDialog> guard(dialog);
    dialog->findChild<QPushButton *>(QStringLiteral("PasteAnswerButton"))->click();
    QTRY_VERIFY(guard.isNull() || !guard->isVisible());
    QVERIFY(m_lyrics->document().isSynced());
    QCOMPARE(m_lyrics->document().lines.size(), 2);
    QCOMPARE(m_lyrics->document().lines[0].text, QStringLiteral("From the AI"));
    m_lyrics->removeLyrics();
}

void LyricsTest::syncLyricsByTapping()
{
    open(m_bareSong);
    m_lyrics->openSyncEditor(false);
    SyncEditorDialog *editor = nullptr;
    QTRY_VERIFY((editor = m_window->findChild<SyncEditorDialog *>()));
    QCOMPARE(editor->mode(), SyncEditorDialog::Mode::Lyrics);
    editor->setText(QStringLiteral("One\nTwo\nThree\nFour"));
    QCOMPARE(editor->document().lines.size(), 4);
    QVERIFY(!editor->document().isSynced());

    editor->tap(1.0);
    editor->tap(3.0);
    QCOMPARE(editor->selectedLine(), 2);
    // Tapping with the playback time.
    m_mpv->command({QStringLiteral("seek"), QStringLiteral("5"), QStringLiteral("absolute+exact")});
    QTRY_VERIFY(std::abs(m_mpv->mpvProperty(QStringLiteral("time-pos")).toDouble() - 5) < 0.05);
    QTest::keyClick(editor->findChild<QTreeWidget *>(QStringLiteral("SyncLines")), Qt::Key_Space);
    QVERIFY(std::abs(editor->document().lines[2].start - 5) < 0.05);
    editor->tap(7.0);
    QVERIFY(editor->document().isSynced());
    // The lyrics view follows the edit live.
    QVERIFY(m_lyrics->isShown());
    QCOMPARE(m_lyrics->document().lines[3].start, 7.0);

    // Backspace undoes the last tap.
    QTest::keyClick(editor->findChild<QTreeWidget *>(QStringLiteral("SyncLines")), Qt::Key_Backspace);
    QCOMPARE(editor->document().lines[3].start, -1.0);
    QCOMPARE(editor->selectedLine(), 3);
    editor->tap(7.5);

    // Nudging one line, then it and all after it.
    editor->selectLine(1);
    editor->nudge(0.1);
    QVERIFY(std::abs(editor->document().lines[1].start - 3.1) < 1e-6);
    QCOMPARE(editor->document().lines[2].start, editor->document().lines[2].start);
    editor->setShiftFollowing(true);
    const double third = editor->document().lines[2].start;
    editor->nudge(-0.1);
    QVERIFY(std::abs(editor->document().lines[1].start - 3.0) < 1e-6);
    QVERIFY(std::abs(editor->document().lines[2].start - (third - 0.1)) < 1e-6);
    QVERIFY(std::abs(editor->document().lines[3].start - 7.4) < 1e-6);

    QCOMPARE(editor->save(), QStringLiteral("lyrics"));
    QCOMPARE(Lyrics::load(Lyrics::findFor(m_bareSong)).lines.size(), 4);
    QCOMPARE(Lyrics::load(Lyrics::findFor(m_bareSong)).lines[0].start, 1.0);
    editor->close();
    m_lyrics->removeLyrics();
}

void LyricsTest::syncSubtitles()
{
    // Subtitles that are all 2 s late.
    const QString srt = m_dir.filePath(QStringLiteral("subs/video.srt"));
    QVERIFY(QDir().mkpath(m_dir.filePath(QStringLiteral("subs"))));
    QVERIFY(writeFile(srt, "1\n00:00:03,000 --> 00:00:04,000\nFirst\n\n"
                           "2\n00:00:07,000 --> 00:00:09,000\nSecond\n\n"
                           "3\n00:00:12,000 --> 00:00:13,500\nThird\n"));
    open(m_video);
    m_window->loadSubtitle(srt);
    QTRY_COMPARE_WITH_TIMEOUT(m_mpv->tracks(QStringLiteral("sub")).size(), 1, 5000);
    QTRY_COMPARE(m_mpv->mpvPropertyString(QStringLiteral("sid")), QStringLiteral("1"));

    m_lyrics->openSyncEditor(true);
    SyncEditorDialog *editor = nullptr;
    QTRY_VERIFY((editor = m_window->findChild<SyncEditorDialog *>()));
    QCOMPARE(editor->mode(), SyncEditorDialog::Mode::Subtitles);
    QCOMPARE(QFileInfo(editor->sourceFile()).canonicalFilePath(), QFileInfo(srt).canonicalFilePath());
    QCOMPARE(editor->document().lines.size(), 3);

    // Heard the first line at 1 s: everything moves 2 s earlier, keeping lengths.
    editor->selectLine(0);
    editor->tap(1.0);
    const Lyrics::Document doc = editor->document();
    QCOMPARE(doc.lines[0].start, 1.0);
    QCOMPARE(doc.lines[0].end, 2.0);
    QCOMPARE(doc.lines[1].start, 5.0);
    QCOMPARE(doc.lines[1].end, 7.0);
    QCOMPARE(doc.lines[2].end, 11.5);

    QSignalSpy saved(editor, &SyncEditorDialog::subtitlesSaved);
    const QString path = editor->save();
    QCOMPARE(path, m_dir.filePath(QStringLiteral("subs/video.synced.srt")));
    QCOMPARE(saved.size(), 1);
    QCOMPARE(Lyrics::load(path).lines[1].start, 5.0);
    // Loaded into the player and selected.
    QTRY_COMPARE_WITH_TIMEOUT(m_mpv->tracks(QStringLiteral("sub")).size(), 2, 5000);
    QTRY_COMPARE(m_mpv->mpvPropertyString(QStringLiteral("sid")), QStringLiteral("2"));
}

void LyricsTest::clickLineSeeks()
{
    open(m_song);
    QTRY_VERIFY_WITH_TIMEOUT(m_lyrics->view()->isVisible(), 5000);
    LyricsView *view = m_lyrics->view();
    m_mpv->command({QStringLiteral("seek"), QStringLiteral("2"), QStringLiteral("absolute+exact")});
    QTRY_COMPARE_WITH_TIMEOUT(view->activeLine(), 0, 5000);
    QTest::qWait(700);
    // Lines with a time can be played from; the empty pause line can't be hit.
    QVERIFY(std::abs(view->seekTime(4) - 20) < 0.05);
    QCOMPARE(view->lineAt(view->lineRect(4).center()), 4);
    QCOMPARE(view->lineAt(QPoint(2, view->lineRect(4).center().y())), -1);

    QTest::mouseClick(view, Qt::LeftButton, Qt::NoModifier, view->lineRect(4).center());
    QTRY_VERIFY_WITH_TIMEOUT(std::abs(m_mpv->mpvProperty(QStringLiteral("time-pos")).toDouble() - 20) < 1, 5000);
    QTRY_COMPARE(view->activeLine(), 4);
    // Picking a line plays it.
    QTRY_VERIFY(!m_mpv->mpvProperty(QStringLiteral("pause")).toBool());
    QVERIFY(!view->isBrowsing());

    // The lyrics offset is taken into account.
    m_lyrics->adjustOffset(0.5);
    QVERIFY(std::abs(view->seekTime(4) - 20.5) < 0.05);
    m_lyrics->adjustOffset(-0.5);
}

void LyricsTest::browseAndReturn()
{
    open(m_song);
    QTRY_VERIFY_WITH_TIMEOUT(m_lyrics->view()->isVisible(), 5000);
    LyricsView *view = m_lyrics->view();
    m_mpv->command({QStringLiteral("seek"), QStringLiteral("6"), QStringLiteral("absolute+exact")});
    QTRY_COMPARE_WITH_TIMEOUT(view->activeLine(), 1, 5000);
    QTest::qWait(700);
    const int centerY = view->lineRect(1).center().y();
    const double volume = m_mpv->mpvProperty(QStringLiteral("volume")).toDouble();

    // The wheel over the lyrics looks further down the song, not the volume.
    const QPointF pos = view->lineRect(1).center();
    QWheelEvent wheel(pos, view->mapToGlobal(pos), QPoint(), QPoint(0, -240), Qt::NoButton, Qt::NoModifier,
                      Qt::NoScrollPhase, false);
    QApplication::sendEvent(view, &wheel);
    QVERIFY(view->isBrowsing());
    QTRY_VERIFY(view->lineRect(1).center().y() < centerY - 40);
    QCOMPARE(m_mpv->mpvProperty(QStringLiteral("volume")).toDouble(), volume);
    // A few seconds later the lyrics glide back to the sung line.
    QTRY_VERIFY_WITH_TIMEOUT(!view->isBrowsing(), LyricsView::kBrowseHoldMs + 2000);
    QTRY_VERIFY(std::abs(view->lineRect(1).center().y() - centerY) <= 2);

    // Dragging a line scrolls too, and isn't a click.
    const QPoint start = view->lineRect(2).center();
    QTest::mousePress(view, Qt::LeftButton, Qt::NoModifier, start);
    for (int step = 1; step <= 6; ++step) {
        const QPointF at = start - QPoint(0, step * 15);
        QMouseEvent move(QEvent::MouseMove, at, view->mapToGlobal(at), Qt::NoButton, Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(view, &move);
    }
    QTest::mouseRelease(view, Qt::LeftButton, Qt::NoModifier, start - QPoint(0, 90));
    QVERIFY(view->isBrowsing());
    QVERIFY(std::abs(view->lineRect(1).center().y() - (centerY - 90)) <= 2);
    QCOMPARE(view->activeLine(), 1);
    QVERIFY(m_mpv->mpvProperty(QStringLiteral("time-pos")).toDouble() < 9);
    // "Back to current line".
    view->followPlayback();
    QVERIFY(!view->isBrowsing());
    QTRY_VERIFY(std::abs(view->lineRect(1).center().y() - centerY) <= 2);
}

void LyricsTest::plainLyricsOnlyScroll()
{
    open(m_bareSong);
    QVERIFY(m_lyrics->applyText(QStringLiteral("First plain line\nSecond plain line\nThird plain line\nFourth plain line\n"),
                                QStringLiteral("test")));
    LyricsView *view = m_lyrics->view();
    QTRY_VERIFY(view->isVisible());
    QVERIFY(!view->document().isSynced());
    m_mpv->command({QStringLiteral("seek"), QStringLiteral("3"), QStringLiteral("absolute+exact")});
    QTRY_VERIFY_WITH_TIMEOUT(std::abs(m_mpv->mpvProperty(QStringLiteral("time-pos")).toDouble() - 3) < 0.5, 5000);
    QTest::qWait(500);

    // Lines without times only scroll: a click on one changes nothing.
    QCOMPARE(view->seekTime(1), -1.0);
    const int line = view->lineAt(view->lineRect(1).center());
    QCOMPARE(line, 1);
    QTest::mouseClick(view, Qt::LeftButton, Qt::NoModifier, view->lineRect(1).center());
    QTest::qWait(QApplication::doubleClickInterval() + 300);
    QVERIFY(std::abs(m_mpv->mpvProperty(QStringLiteral("time-pos")).toDouble() - 3) < 0.5);
    QVERIFY(m_mpv->mpvProperty(QStringLiteral("pause")).toBool());

    // They can be dragged around, and follow playback again afterwards.
    const QPoint start = view->lineRect(1).center();
    QTest::mousePress(view, Qt::LeftButton, Qt::NoModifier, start);
    for (int step = 1; step <= 4; ++step) {
        const QPointF at = start - QPoint(0, step * 10);
        QMouseEvent move(QEvent::MouseMove, at, view->mapToGlobal(at), Qt::NoButton, Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(view, &move);
    }
    QTest::mouseRelease(view, Qt::LeftButton, Qt::NoModifier, start - QPoint(0, 40));
    QVERIFY(view->isBrowsing());
    view->followPlayback();
    QVERIFY(!view->isBrowsing());
    m_lyrics->removeLyrics();
}

void LyricsTest::appearance()
{
    open(m_song);
    QTRY_VERIFY_WITH_TIMEOUT(m_lyrics->view()->isVisible(), 5000);
    LyricsView *view = m_lyrics->view();
    m_mpv->command({QStringLiteral("seek"), QStringLiteral("21"), QStringLiteral("absolute+exact")});
    QTRY_COMPARE_WITH_TIMEOUT(view->activeLine(), 4, 5000);
    QTest::qWait(700);
    const int height = view->lineRect(5).height();
    QCOMPARE(view->lyricsStyle(), LyricsStyle());

    // Changes show at once; Cancel puts the old look back.
    m_lyrics->openStyleDialog();
    auto *dialog = m_window->findChild<LyricsStyleDialog *>();
    QVERIFY(dialog);
    QTRY_VERIFY(dialog->isVisible());
    LyricsStyle bigger;
    bigger.scale = 1.6;
    bigger.align = Qt::AlignLeft;
    bigger.highlight = QColor(0xFF, 0xD1, 0x66);
    dialog->setLyricsStyle(bigger);
    QCOMPARE(view->lyricsStyle(), bigger);
    QVERIFY(view->lineRect(5).height() > height);
    dialog->reject();
    QCOMPARE(view->lyricsStyle(), LyricsStyle());
    QTRY_VERIFY(!m_window->findChild<LyricsStyleDialog *>());

    // OK keeps (and saves) it.
    m_lyrics->openStyleDialog();
    dialog = m_window->findChild<LyricsStyleDialog *>();
    QVERIFY(dialog);
    dialog->findChild<QSlider *>(QStringLiteral("LyricsSizeSlider"))->setValue(150);
    dialog->findChild<QSlider *>(QStringLiteral("LyricsDimSlider"))->setValue(40);
    QCOMPARE(view->lyricsStyle().scale, 1.5);
    dialog->findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Ok)->click();
    QCOMPARE(view->lyricsStyle().scale, 1.5);
    QCOMPARE(LyricsStyle::load().scale, 1.5);
    QCOMPARE(LyricsStyle::load().dim, 40);

    // Ctrl+wheel over the lyrics resizes them.
    const QPointF pos = view->rect().center();
    QWheelEvent wheel(pos, view->mapToGlobal(pos), QPoint(), QPoint(0, 120), Qt::NoButton, Qt::ControlModifier,
                      Qt::NoScrollPhase, false);
    QApplication::sendEvent(view, &wheel);
    QCOMPARE(view->lyricsStyle().scale, 1.6);
    QCOMPARE(LyricsStyle::load().scale, 1.6);
    // The new look is kept for the next window.
    delete m_window;
    m_window = new MainWindow;
    m_lyrics = m_window->lyrics();
    m_mpv = m_window->findChild<MpvWidget *>();
    QCOMPARE(m_lyrics->view()->lyricsStyle().scale, 1.6);
    LyricsStyle().save();
}

void LyricsTest::backgroundOverVideo()
{
    // Lyrics beside a video show over it once turned on.
    const QString lrc = QFileInfo(m_video).absolutePath() + QStringLiteral("/") + QFileInfo(m_video).completeBaseName()
                        + QStringLiteral(".lrc");
    QVERIFY(writeFile(lrc, kSongLrc));
    open(m_video);
    LyricsView *view = m_lyrics->view();
    m_lyrics->setShown(true);
    QTRY_VERIFY(view->isVisible());
    m_mpv->command({QStringLiteral("seek"), QStringLiteral("30"), QStringLiteral("absolute+exact")});

    // What reaches the screen in the top-left corner of the picture, away from the lyrics.
    const auto brightness = [this] {
        const QPoint corner = m_mpv->mapToGlobal(QPoint(m_mpv->width() / 10, m_mpv->height() / 10));
        const QImage shot = m_mpv->screen()->grabWindow(0, corner.x(), corner.y(), 8, 8).toImage();
        int sum = 0;
        for (int y = 0; y < shot.height(); ++y) {
            for (int x = 0; x < shot.width(); ++x)
                sum += qGray(shot.pixel(x, y));
        }
        return sum / std::max(1, int(shot.width() * shot.height()));
    };
    LyricsStyle style = view->lyricsStyle();
    style.dim = 0;
    view->setLyricsStyle(style);
    QTRY_VERIFY_WITH_TIMEOUT(brightness() > 60, 5000);
    const int bright = brightness();
    // Darken video: the picture behind the lyrics goes dark.
    style.dim = 90;
    view->setLyricsStyle(style);
    QTRY_VERIFY(brightness() < bright / 3);

    // Blur video: a filter on the video while the lyrics show over it.
    const auto filters = [this] { return m_mpv->mpvPropertyString(QStringLiteral("vf")); };
    QVERIFY(!filters().contains(QStringLiteral("lyricsblur")));
    style.videoBlur = 50;
    view->setLyricsStyle(style);
    QTRY_VERIFY(filters().contains(QStringLiteral("lyricsblur")));
    m_lyrics->setShown(false);
    QTRY_VERIFY(!filters().contains(QStringLiteral("lyricsblur")));
    m_lyrics->setShown(true);
    QTRY_VERIFY(filters().contains(QStringLiteral("lyricsblur")));
    style.videoBlur = 0;
    view->setLyricsStyle(style);
    QTRY_VERIFY(!filters().contains(QStringLiteral("lyricsblur")));
    QFile::remove(lrc);
}

void LyricsTest::smallWindowShowsFewerLines()
{
    open(m_song);
    QTRY_VERIFY_WITH_TIMEOUT(m_lyrics->view()->isVisible(), 5000);
    LyricsView *view = m_lyrics->view();
    m_mpv->command({QStringLiteral("seek"), QStringLiteral("21"), QStringLiteral("absolute+exact")});
    QTRY_COMPARE_WITH_TIMEOUT(view->activeLine(), 4, 5000);
    QVERIFY(!view->isCompact());
    QCOMPARE(view->visibleRadius(), -1);

    // The mini player at its smallest: the title strip goes, and only the
    // lines around the sung one are drawn, all of it on screen.
    m_window->setMiniPlayer(true);
    m_window->setMiniPlayerWidth(220);
    QTRY_VERIFY(view->height() < 140);
    QVERIFY(view->isCompact());
    QVERIFY(view->visibleRadius() >= 0);
    QVERIFY(view->visibleRadius() <= 2);
    QTest::qWait(700);
    QVERIFY(view->rect().contains(view->lineRect(4)));
    QVERIFY(!view->grab().isNull());
    m_window->setMiniPlayer(false);
    QTRY_VERIFY(!view->isCompact());
    QCOMPARE(view->visibleRadius(), -1);
}

int main(int argc, char *argv[])
{
    // Keep the settings and lyrics away from the user's own.
    QTemporaryDir config;
    qputenv("XDG_CONFIG_HOME", config.path().toLocal8Bit());
    QTemporaryDir cache;
    qputenv("XDG_CACHE_HOME", cache.path().toLocal8Bit());
    // No network in tests.
    qputenv("TOPPLAYER_LYRICSOVH_URL", "http://127.0.0.1:1");
    QApplication app(argc, argv);
    std::setlocale(LC_NUMERIC, "C");
    QApplication::setAttribute(Qt::AA_DontUseNativeDialogs);
    LyricsTest test;
    QTEST_SET_MAIN_SOURCE_PATH
    return QTest::qExec(&test, argc, argv);
}

#include "tst_lyrics.moc"
