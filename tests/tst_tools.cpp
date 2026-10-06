// The media cutter (In/Out points, the export dialog, ffmpeg) and the yt-dlp
// downloader. The cutter runs the real ffmpeg on a generated clip; the
// downloader runs a stand-in yt-dlp script, since tests have no network.
// Needs a display (run under xvfb-run) and ffmpeg.

#include "ResumeManager.h"
#include "ControlBar.h"
#include "MainWindow.h"
#include "MediaCutter.h"
#include "MediaCutterDialog.h"
#include "MediaDownloader.h"
#include "MediaDownloaderDialog.h"
#include "MpvWidget.h"
#include "OsdWidget.h"
#include "SeekBar.h"

#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QComboBox>
#include <QFile>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QProcess>
#include <QProgressBar>
#include <QProgressDialog>
#include <QPushButton>
#include <QRadioButton>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QTest>

#include <clocale>
#include <cmath>

namespace {

bool runFfmpeg(const QStringList &args)
{
    QProcess process;
    process.start(QStandardPaths::findExecutable(QStringLiteral("ffmpeg")),
                  QStringList{QStringLiteral("-loglevel"), QStringLiteral("error"), QStringLiteral("-y")} + args);
    return process.waitForFinished(60000) && process.exitCode() == 0;
}

double probeDuration(const QString &path)
{
    QProcess process;
    process.start(QStandardPaths::findExecutable(QStringLiteral("ffprobe")),
                  {QStringLiteral("-v"), QStringLiteral("error"), QStringLiteral("-show_entries"), QStringLiteral("format=duration"),
                   QStringLiteral("-of"), QStringLiteral("default=nw=1:nk=1"), path});
    process.waitForFinished(30000);
    return process.readAllStandardOutput().trimmed().toDouble();
}

QStringList probeCodecs(const QString &path)
{
    QProcess process;
    process.start(QStandardPaths::findExecutable(QStringLiteral("ffprobe")),
                  {QStringLiteral("-v"), QStringLiteral("error"), QStringLiteral("-show_entries"), QStringLiteral("stream=codec_name"),
                   QStringLiteral("-of"), QStringLiteral("default=nw=1:nk=1"), path});
    process.waitForFinished(30000);
    return QString::fromUtf8(process.readAllStandardOutput()).split(QLatin1Char('\n'), Qt::SkipEmptyParts);
}

bool writeScript(const QString &path, const QByteArray &body)
{
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly))
        return false;
    file.write("#!/bin/sh\n" + body);
    file.close();
    return file.setPermissions(file.permissions() | QFileDevice::ExeOwner | QFileDevice::ExeUser);
}

// Stand-in for yt-dlp: logs its arguments, prints progress like
// `yt-dlp --newline --progress`, copies $FAKE_SOURCE into the -P folder and
// prints the file as `--print after_move:filepath` does.
const QByteArray kFakeYtDlp =
    "printf '%s\\n' \"$@\" > \"$FAKE_LOG\"\n"
    "dir=\"\"; prev=\"\"; url=\"\"\n"
    "for a in \"$@\"; do\n"
    "  if [ \"$prev\" = \"-P\" ]; then dir=\"$a\"; fi\n"
    "  prev=\"$a\"; url=\"$a\"\n"
    "done\n"
    "case \"$url\" in *unavailable*) echo \"ERROR: [youtube] abc123: Video unavailable\" >&2; exit 1;; esac\n"
    "echo \"[youtube] Extracting URL: $url\" >&2\n"
    "echo \"[download] Destination: $dir/Test Video [abc123].f137.mp4\" >&2\n"
    "for p in 12.5 47.3 100.0; do\n"
    "  echo \"[download]  $p% of   1.50MiB at    2.04MiB/s ETA 00:01\" >&2\n"
    "  sleep 0.2\n"
    "done\n"
    "case \"$url\" in *slow*) sleep 30;; esac\n"
    "echo \"[Merger] Merging formats into \\\"$dir/Test Video [abc123].mp4\\\"\" >&2\n"
    "cp \"$FAKE_SOURCE\" \"$dir/Test Video [abc123].mp4\"\n"
    "echo \"$dir/Test Video [abc123].mp4\"\n";

// Stand-in for yt-dlp searching YouTube for Spotify songs: appends its
// arguments to $FAKE_LOG, saves $FAKE_SOURCE under the -o name (as an .mp3)
// and prints it. Songs with "NoMatch" in their name have no upload of the
// right length (the length-checked search finds nothing); "Missing" ones are
// not found at all.
const QByteArray kFakeSpotifyYtDlp =
    "printf '%s\\n' \"$@\" >> \"$FAKE_LOG\"\n"
    "echo '----' >> \"$FAKE_LOG\"\n"
    "dir=\"\"; out=\"\"; prev=\"\"; query=\"\"; strict=no\n"
    "for a in \"$@\"; do\n"
    "  if [ \"$prev\" = \"-P\" ]; then dir=\"$a\"; fi\n"
    "  if [ \"$prev\" = \"-o\" ]; then out=\"$a\"; fi\n"
    "  if [ \"$a\" = \"--match-filter\" ]; then strict=yes; fi\n"
    "  prev=\"$a\"; query=\"$a\"\n"
    "done\n"
    "case \"$query\" in *Missing*) echo \"ERROR: no results for $query\" >&2; exit 1;; esac\n"
    "case \"$query\" in *NoMatch*) if [ $strict = yes ]; then exit 0; fi;; esac\n"
    "name=$(printf '%s' \"$out\" | sed 's/%(ext)s/mp3/')\n"
    "echo \"[download]  50.0% of   1.00MiB at    1.00MiB/s ETA 00:01\" >&2\n"
    "cp \"$FAKE_SOURCE\" \"$dir/$name\"\n"
    "echo \"$dir/$name\"\n"
    "if [ $strict = yes ]; then exit 101; fi\n";

// Serves Spotify-like pages: `pages` maps a path to its HTML.
class FakeSpotify : public QObject
{
public:
    QHash<QString, QByteArray> pages;
    QStringList requests;

    bool listen() { return m_server.listen(QHostAddress::LocalHost); }
    QUrl url() const { return QUrl(QStringLiteral("http://127.0.0.1:%1").arg(m_server.serverPort())); }

    FakeSpotify()
    {
        connect(&m_server, &QTcpServer::newConnection, this, [this] {
            while (QTcpSocket *socket = m_server.nextPendingConnection()) {
                connect(socket, &QTcpSocket::readyRead, this, [this, socket] {
                    const QByteArray request = socket->readAll();
                    const QString path = QString::fromLatin1(request.split(' ').value(1));
                    requests << path;
                    const QByteArray body = pages.value(path);
                    const QByteArray status = body.isEmpty() ? "404 Not Found" : "200 OK";
                    socket->write("HTTP/1.1 " + status + "\r\nContent-Type: text/html; charset=utf-8\r\nContent-Length: "
                                  + QByteArray::number(body.size()) + "\r\nConnection: close\r\n\r\n" + body);
                    socket->disconnectFromHost();
                });
                connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
            }
        });
    }

private:
    QTcpServer m_server;
};

QByteArray nextDataPage(const QByteArray &entity)
{
    return "<!DOCTYPE html><html><head><title>Spotify</title></head><body><div id=\"__next\"></div>"
           "<script id=\"__NEXT_DATA__\" type=\"application/json\">{\"props\":{\"pageProps\":{\"state\":{\"data\":{\"entity\":"
           + entity + "}}}}}</script></body></html>";
}

const QByteArray kTrackEntity =
    "{\"type\":\"track\",\"name\":\"Believer\",\"uri\":\"spotify:track:0pqnGHJpmpxLKifKRmU6WP\","
    "\"artists\":[{\"name\":\"Imagine Dragons\",\"uri\":\"spotify:artist:x\"}],\"duration\":204346}";

const QByteArray kAlbumEntity =
    "{\"type\":\"album\",\"name\":\"Night Drive\",\"subtitle\":\"Neon Coast\",\"trackList\":["
    "{\"uri\":\"spotify:track:a\",\"title\":\"Song One\",\"subtitle\":\"Neon Coast\",\"duration\":180000},"
    "{\"uri\":\"spotify:track:b\",\"title\":\"NoMatch Two\",\"subtitle\":\"Neon Coast,\u00a0Guest\",\"duration\":200000},"
    "{\"uri\":\"spotify:track:c\",\"title\":\"Missing Three\",\"subtitle\":\"Neon Coast\",\"duration\":150000},"
    "{\"uri\":\"spotify:track:d\",\"title\":\"Song Four\",\"subtitle\":\"Neon Coast\",\"duration\":0}]}";

// Stand-in for an ffmpeg that takes its time: reports once, creates the
// output and waits.
const QByteArray kSlowFfmpeg =
    "for a in \"$@\"; do out=\"$a\"; done\n"
    "echo \"frame=    2 fps=0.0 q=-1.0 size=       0kB time=00:00:01.00 bitrate=N/A speed=N/A\" >&2\n"
    ": > \"$out\"\n"
    "sleep 30\n";

} // namespace

class ToolsTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void initTestCase();
    void init();
    void cleanup();

    // Cutter
    void timestamps();
    void ffmpegProgress();
    void ffmpegArguments();
    void inOutMarkers();
    void cutFiveSecondClip();
    void extractAudio();
    void exportValidation();
    void cancelExport();
    void missingFfmpeg();

    // Downloader
    void recognizesLinks_data();
    void recognizesLinks();
    void ytDlpArguments();
    void ytDlpProgress();
    void pastesLinkFromClipboard();
    void missingYtDlp();
    void downloadAndPlay();
    void downloadError();
    void cancelDownload();
    void directStream();
    void spotifyLinks();
    void spotifyPages();
    void spotifyArguments();
    void spotifyDownloadsAlbum();
    void spotifyDialog();
    void spotifyStream();

private:
    QVariant prop(const char *name) const { return m_mpv->mpvProperty(QString::fromLatin1(name)); }
    QString propString(const char *name) const { return m_mpv->mpvPropertyString(QString::fromLatin1(name)); }
    void press(int key, Qt::KeyboardModifiers modifiers = Qt::NoModifier);
    void pauseAt(double seconds);
    template <typename T>
    T *waitForDialog();
    void usePath(const QString &path) { qputenv("PATH", path.toLocal8Bit()); }
    // `dir` first, then the real $PATH (the scripts need sleep, cp, ...).
    void prependPath(const QString &dir) { usePath(dir + QLatin1Char(':') + QString::fromLocal8Bit(m_path)); }

    QTemporaryDir m_dir;
    QString m_clip;    // 60 s, video + audio
    QString m_fakeBin; // stand-in yt-dlp
    QString m_spotifyBin; // stand-in yt-dlp searching YouTube
    QString m_slowBin; // stand-in slow ffmpeg
    QString m_emptyBin;
    QByteArray m_path; // the real $PATH
    MainWindow *m_window = nullptr;
    MpvWidget *m_mpv = nullptr;
};

void ToolsTest::initTestCase()
{
    QVERIFY(m_dir.isValid());
    if (QStandardPaths::findExecutable(QStringLiteral("ffmpeg")).isEmpty())
        QSKIP("ffmpeg is needed for these tests");
    m_clip = m_dir.filePath(QStringLiteral("clip.mkv"));
    // A keyframe every second, so stream copy cuts land on the marks.
    QVERIFY(runFfmpeg({QStringLiteral("-f"), QStringLiteral("lavfi"), QStringLiteral("-i"), QStringLiteral("testsrc=duration=60:size=64x48:rate=2"),
                       QStringLiteral("-f"), QStringLiteral("lavfi"), QStringLiteral("-i"), QStringLiteral("sine=f=440:d=60"),
                       QStringLiteral("-c:v"), QStringLiteral("mpeg4"), QStringLiteral("-g"), QStringLiteral("2"),
                       QStringLiteral("-c:a"), QStringLiteral("aac"), m_clip}));

    m_path = qgetenv("PATH");
    m_fakeBin = m_dir.filePath(QStringLiteral("fake-bin"));
    m_slowBin = m_dir.filePath(QStringLiteral("slow-bin"));
    m_emptyBin = m_dir.filePath(QStringLiteral("empty-bin"));
    QVERIFY(QDir().mkpath(m_fakeBin) && QDir().mkpath(m_slowBin) && QDir().mkpath(m_emptyBin));
    QVERIFY(writeScript(m_fakeBin + QStringLiteral("/yt-dlp"), kFakeYtDlp));
    m_spotifyBin = m_dir.filePath(QStringLiteral("spotify-bin"));
    QVERIFY(QDir().mkpath(m_spotifyBin));
    QVERIFY(writeScript(m_spotifyBin + QStringLiteral("/yt-dlp"), kFakeSpotifyYtDlp));
    QVERIFY(writeScript(m_slowBin + QStringLiteral("/ffmpeg"), kSlowFfmpeg));
    qputenv("FAKE_LOG", m_dir.filePath(QStringLiteral("yt-dlp-args.txt")).toLocal8Bit());
    qputenv("FAKE_SOURCE", m_clip.toLocal8Bit());
}

void ToolsTest::init()
{
    qputenv("PATH", m_path);
    m_window = new MainWindow;
    m_window->resize(800, 450);
    m_window->show();
    m_window->activateWindow();
    QVERIFY(QTest::qWaitForWindowActive(m_window));
    m_mpv = m_window->findChild<MpvWidget *>();
    QVERIFY(m_mpv);
    m_mpv->setMpvProperty(QStringLiteral("ao"), QStringLiteral("null"));
}

void ToolsTest::cleanup()
{
    qputenv("PATH", m_path);
    while (QWidget *modal = QApplication::activeModalWidget())
        delete modal;
    delete m_window;
    m_window = nullptr;
    m_mpv = nullptr;
}

void ToolsTest::press(int key, Qt::KeyboardModifiers modifiers)
{
    m_window->activateWindow();
    QWidget *focus = QApplication::focusWidget();
    QTest::keyClick(focus ? focus : m_window, static_cast<Qt::Key>(key), modifiers);
}

void ToolsTest::pauseAt(double seconds)
{
    m_mpv->setMpvProperty(QStringLiteral("pause"), QStringLiteral("yes"));
    m_mpv->command({QStringLiteral("seek"), QString::number(seconds), QStringLiteral("absolute+exact")});
    QTRY_VERIFY_WITH_TIMEOUT(std::abs(prop("time-pos").toDouble() - seconds) < 0.05, 5000);
}

template <typename T>
T *ToolsTest::waitForDialog()
{
    T *dialog = nullptr;
    [&] { QTRY_VERIFY((dialog = m_window->findChild<T *>()) && dialog->isVisible()); }();
    return dialog;
}

// ---- Cutter ---------------------------------------------------------------

void ToolsTest::timestamps()
{
    QCOMPARE(MediaCutter::formatTimestamp(0), QStringLiteral("00:00:00.000"));
    QCOMPARE(MediaCutter::formatTimestamp(3725.5), QStringLiteral("01:02:05.500"));
    QCOMPARE(MediaCutter::formatTimestamp(59.9996), QStringLiteral("00:01:00.000"));
    QCOMPARE(MediaCutter::parseTimestamp(QStringLiteral("01:02:05.500")), 3725.5);
    QCOMPARE(MediaCutter::parseTimestamp(QStringLiteral("02:05")), 125.0);
    QCOMPARE(MediaCutter::parseTimestamp(QStringLiteral("02:05,25")), 125.25);
    QCOMPARE(MediaCutter::parseTimestamp(QStringLiteral("75.5")), 75.5);
    QCOMPARE(MediaCutter::parseTimestamp(QStringLiteral(" 10 ")), 10.0);
    QCOMPARE(MediaCutter::parseTimestamp(QStringLiteral("1:75")), -1.0);
    QCOMPARE(MediaCutter::parseTimestamp(QStringLiteral("1:60:00")), -1.0);
    QCOMPARE(MediaCutter::parseTimestamp(QStringLiteral("abc")), -1.0);
    QCOMPARE(MediaCutter::parseTimestamp(QString()), -1.0);
}

void ToolsTest::ffmpegProgress()
{
    QCOMPARE(MediaCutter::parseProgress("frame=   10 fps=0.0 q=-1.0 size=     256kB time=00:00:02.50 bitrate= 838.9kbits/s"), 2.5);
    // Several updates in one chunk (ffmpeg separates them with \r): the last counts.
    QCOMPARE(MediaCutter::parseProgress("size=1kB time=00:00:01.00 x\rsize=2kB time=00:01:03.25 x\r"), 63.25);
    QCOMPARE(MediaCutter::parseProgress("size=N/A time=N/A bitrate=N/A"), -1.0);
    QCOMPARE(MediaCutter::parseProgress("time=-00:00:00.04"), 0.0);
    QCOMPARE(MediaCutter::parseProgress("out_time_us=3500000\nprogress=continue\n"), 3.5);
    QCOMPARE(MediaCutter::parseProgress("Input #0, matroska"), -1.0);
}

void ToolsTest::ffmpegArguments()
{
    MediaCutter::Job job;
    job.input = QStringLiteral("/v/in.mkv");
    job.output = QStringLiteral("/v/out.mkv");
    job.start = 10;
    job.end = 15.5;
    const QStringList copy = MediaCutter::arguments(job);
    // A fast input seek to 5 s before the In-point, then the exact range.
    QVERIFY(copy.join(QLatin1Char(' ')).contains(QLatin1String("-ss 5.000 -i /v/in.mkv -ss 5.000 -to 10.500")));
    QVERIFY(copy.join(QLatin1Char(' ')).contains(QLatin1String("-map 0 -c copy")));
    QCOMPARE(copy.last(), QStringLiteral("/v/out.mkv"));

    job.output = QStringLiteral("/v/out.mp4");
    QVERIFY(MediaCutter::arguments(job).contains(QStringLiteral("-sn")));
    // Near the start there is nothing to seek past.
    job.start = 2;
    QVERIFY(MediaCutter::arguments(job).join(QLatin1Char(' ')).contains(QLatin1String("-y -i /v/in.mkv -ss 2.000 -to 15.500")));

    job.mode = MediaCutter::Mode::AudioOnly;
    job.output = QStringLiteral("/v/out.mp3");
    const QString audio = MediaCutter::arguments(job).join(QLatin1Char(' '));
    QVERIFY(audio.contains(QLatin1String("-vn")));
    QVERIFY(audio.contains(QLatin1String("-c:a libmp3lame")));
    job.audioFormat = MediaCutter::AudioFormat::Flac;
    QVERIFY(MediaCutter::arguments(job).join(QLatin1Char(' ')).contains(QLatin1String("-c:a flac")));
    job.audioFormat = MediaCutter::AudioFormat::Aac;
    QVERIFY(MediaCutter::arguments(job).join(QLatin1Char(' ')).contains(QLatin1String("-c:a aac -b:a 192k -f adts")));
}

void ToolsTest::inOutMarkers()
{
    m_window->openFile(m_clip);
    QTRY_VERIFY_WITH_TIMEOUT(prop("duration").toDouble() > 59, 10000);
    SeekBar *seekBar = m_window->findChild<ControlBar *>()->seekBar();
    auto *osd = m_window->findChild<OsdWidget *>();

    pauseAt(10);
    press(Qt::Key_BracketLeft, Qt::ControlModifier);
    QCOMPARE(m_window->clipIn(), 10.0);
    QCOMPARE(seekBar->clipIn(), 10.0);
    QCOMPARE(osd->text(), QStringLiteral("In-Point (A) 00:00:10.000"));
    pauseAt(15);
    press(Qt::Key_BracketRight, Qt::ControlModifier);
    QCOMPARE(m_window->clipOut(), 15.0);
    QCOMPARE(seekBar->clipOut(), 15.0);
    QCOMPARE(osd->text(), QStringLiteral("Out-Point (B) 00:00:15.000"));
    // [ and ] alone still move the subtitle delay.
    press(Qt::Key_BracketRight);
    QTRY_COMPARE(prop("sub-delay").toDouble(), 0.5);
    QCOMPARE(m_window->clipOut(), 15.0);

    // The brackets and the band between them are drawn on the seekbar.
    const QImage image = seekBar->grab().toImage();
    const QColor clipColor = seekBar->property("clipColor").value<QColor>();
    auto columnHas = [&](int x, const std::function<bool(const QColor &)> &test) {
        for (int y = 0; y < image.height(); ++y) {
            if (test(image.pixelColor(x * image.width() / seekBar->width(), y)))
                return true;
        }
        return false;
    };
    const qreal left = 6;
    const qreal width = seekBar->width() - 12;
    const int inX = qRound(left + width * 10 / prop("duration").toDouble());
    const int midX = qRound(left + width * 12.5 / prop("duration").toDouble());
    const int afterX = qRound(left + width * 30 / prop("duration").toDouble());
    QVERIFY(columnHas(inX, [&](const QColor &c) { return c == clipColor; }));
    QVERIFY(columnHas(midX, [](const QColor &c) { return c.blue() > c.red() + 30; })); // the band
    QVERIFY(!columnHas(afterX, [](const QColor &c) { return c.blue() > c.red() + 30; }));

    // An Out-point before the In-point drops the In-point.
    pauseAt(5);
    press(Qt::Key_BracketRight, Qt::ControlModifier);
    QCOMPARE(m_window->clipOut(), 5.0);
    QCOMPARE(m_window->clipIn(), -1.0);
    // And an In-point after the Out-point drops the Out-point.
    pauseAt(20);
    press(Qt::Key_BracketLeft, Qt::ControlModifier);
    QCOMPARE(m_window->clipIn(), 20.0);
    QCOMPARE(m_window->clipOut(), -1.0);

    // Another file starts without marks.
    const QString other = m_dir.filePath(QStringLiteral("other.mkv"));
    QVERIFY(QFile::copy(m_clip, other) || QFileInfo::exists(other));
    m_window->openFile(other);
    QTRY_COMPARE(m_window->clipIn(), -1.0);
    QCOMPARE(seekBar->clipIn(), -1.0);
}

void ToolsTest::cutFiveSecondClip()
{
    m_window->openFile(m_clip);
    QTRY_VERIFY_WITH_TIMEOUT(prop("duration").toDouble() > 59, 10000);
    pauseAt(10);
    press(Qt::Key_BracketLeft, Qt::ControlModifier);
    pauseAt(15);
    press(Qt::Key_BracketRight, Qt::ControlModifier);

    press(Qt::Key_X, Qt::ControlModifier);
    auto *dialog = waitForDialog<MediaCutterDialog>();
    QVERIFY(dialog);
    QCOMPARE(dialog->findChild<QLineEdit *>(QStringLiteral("CutterStart"))->text(), QStringLiteral("00:00:10.000"));
    QCOMPARE(dialog->findChild<QLineEdit *>(QStringLiteral("CutterEnd"))->text(), QStringLiteral("00:00:15.000"));
    QCOMPARE(dialog->findChild<QLabel *>(QStringLiteral("CutterDuration"))->text(), QStringLiteral("00:00:05.000"));
    QVERIFY(dialog->findChild<QRadioButton *>(QStringLiteral("CutterCopyMode"))->isChecked());
    const QString output = m_dir.filePath(QStringLiteral("clip_clip.mkv"));
    QCOMPARE(dialog->findChild<QLineEdit *>(QStringLiteral("CutterOutput"))->text(), output);

    QSignalSpy progress(dialog->cutter(), &MediaCutter::progress);
    QTest::mouseClick(dialog->findChild<QPushButton *>(QStringLiteral("CutterExportButton")), Qt::LeftButton);
    QTRY_VERIFY_WITH_TIMEOUT(!m_window->findChild<MediaCutterDialog *>(), 30000);

    // A 5-second clip with the video and audio copied as they were.
    QVERIFY(QFileInfo::exists(output));
    QVERIFY2(std::abs(probeDuration(output) - 5.0) < 0.05, qPrintable(QString::number(probeDuration(output))));
    QCOMPARE(probeCodecs(output), (QStringList{QStringLiteral("mpeg4"), QStringLiteral("aac")}));
    QVERIFY(!progress.isEmpty());
    // Reports grow to 100%. (Their order can interleave: a modal progress
    // dialog processes events while it updates.)
    double highest = 0;
    for (const QList<QVariant> &report : std::as_const(progress))
        highest = std::max(highest, report.first().toDouble());
    QCOMPARE(highest, 1.0);

    // Confirmed on the OSD, with a choice to open it.
    QCOMPARE(m_window->findChild<OsdWidget *>()->text(), QStringLiteral("Clip saved: clip_clip.mkv"));
    QMessageBox *box = nullptr;
    QTRY_VERIFY((box = m_window->findChild<QMessageBox *>(QStringLiteral("ClipSavedMessage"))) && box->isVisible());
    QVERIFY(box->findChild<QPushButton *>(QStringLiteral("ClipShowButton")));
    QTest::mouseClick(box->findChild<QPushButton *>(QStringLiteral("ClipOpenButton")), Qt::LeftButton);
    QTRY_COMPARE_WITH_TIMEOUT(propString("path"), output, 10000);
    QTRY_VERIFY_WITH_TIMEOUT(std::abs(prop("duration").toDouble() - 5.0) < 0.05, 10000);

    // The next export doesn't overwrite it.
    QCOMPARE(MediaCutterDialog::defaultOutput(m_clip, QStringLiteral("clip.mkv"), QStringLiteral("mkv")),
             m_dir.filePath(QStringLiteral("clip_clip2.mkv")));
    QFile::remove(output);
}

void ToolsTest::extractAudio()
{
    m_window->openFile(m_clip);
    QTRY_VERIFY_WITH_TIMEOUT(prop("duration").toDouble() > 59, 10000);
    pauseAt(20);
    press(Qt::Key_X, Qt::ControlModifier);
    auto *dialog = waitForDialog<MediaCutterDialog>();
    QVERIFY(dialog);
    // Without marks the range is the whole file; "Use Current Time" takes the position.
    QCOMPARE(dialog->findChild<QLineEdit *>(QStringLiteral("CutterStart"))->text(), QStringLiteral("00:00:00.000"));
    QTest::mouseClick(dialog->findChild<QPushButton *>(QStringLiteral("CutterStartNow")), Qt::LeftButton);
    QCOMPARE(dialog->findChild<QLineEdit *>(QStringLiteral("CutterStart"))->text(), QStringLiteral("00:00:20.000"));
    dialog->findChild<QLineEdit *>(QStringLiteral("CutterEnd"))->setText(QStringLiteral("25.000"));
    QCOMPARE(dialog->findChild<QLabel *>(QStringLiteral("CutterDuration"))->text(), QStringLiteral("00:00:05.000"));

    // Picking an audio format switches the mode and the extension.
    auto *format = dialog->findChild<QComboBox *>(QStringLiteral("CutterAudioFormat"));
    format->setCurrentIndex(format->findData(int(MediaCutter::AudioFormat::Flac)));
    QVERIFY(dialog->findChild<QRadioButton *>(QStringLiteral("CutterAudioMode"))->isChecked());
    const QString output = m_dir.filePath(QStringLiteral("clip_clip.flac"));
    QCOMPARE(dialog->findChild<QLineEdit *>(QStringLiteral("CutterOutput"))->text(), output);
    format->setCurrentIndex(format->findData(int(MediaCutter::AudioFormat::Mp3)));
    const QString mp3 = m_dir.filePath(QStringLiteral("clip_clip.mp3"));
    QCOMPARE(dialog->findChild<QLineEdit *>(QStringLiteral("CutterOutput"))->text(), mp3);

    dialog->startExport();
    QTRY_VERIFY_WITH_TIMEOUT(!m_window->findChild<MediaCutterDialog *>(), 30000);
    QCOMPARE(probeCodecs(mp3), QStringList{QStringLiteral("mp3")});
    QVERIFY2(std::abs(probeDuration(mp3) - 5.0) < 0.2, qPrintable(QString::number(probeDuration(mp3))));
    QFile::remove(mp3);
}

void ToolsTest::exportValidation()
{
    m_window->openFile(m_clip);
    QTRY_VERIFY_WITH_TIMEOUT(prop("duration").toDouble() > 59, 10000);
    press(Qt::Key_X, Qt::ControlModifier);
    auto *dialog = waitForDialog<MediaCutterDialog>();
    QVERIFY(dialog);
    auto *exportButton = dialog->findChild<QPushButton *>(QStringLiteral("CutterExportButton"));
    auto *status = dialog->findChild<QLabel *>(QStringLiteral("CutterStatus"));
    QVERIFY(exportButton->isEnabled());

    dialog->findChild<QLineEdit *>(QStringLiteral("CutterStart"))->setText(QStringLiteral("00:00:30.000"));
    dialog->findChild<QLineEdit *>(QStringLiteral("CutterEnd"))->setText(QStringLiteral("00:00:20.000"));
    QVERIFY(!exportButton->isEnabled());
    QCOMPARE(status->text(), QStringLiteral("The end must be after the start."));
    dialog->findChild<QLineEdit *>(QStringLiteral("CutterEnd"))->setText(QStringLiteral("00:00:40.000"));
    dialog->findChild<QLineEdit *>(QStringLiteral("CutterOutput"))->setText(m_clip);
    QVERIFY(!exportButton->isEnabled());
    QCOMPARE(status->text(), QStringLiteral("The clip can't replace the file it is cut from."));
}

void ToolsTest::cancelExport()
{
    m_window->openFile(m_clip);
    QTRY_VERIFY_WITH_TIMEOUT(prop("duration").toDouble() > 59, 10000);
    prependPath(m_slowBin);
    press(Qt::Key_X, Qt::ControlModifier);
    auto *dialog = waitForDialog<MediaCutterDialog>();
    QVERIFY(dialog);
    const QString output = dialog->findChild<QLineEdit *>(QStringLiteral("CutterOutput"))->text();
    dialog->startExport();

    // The progress dialog follows ffmpeg's report (1 s of 60).
    QProgressDialog *progress = nullptr;
    QTRY_VERIFY((progress = dialog->findChild<QProgressDialog *>(QStringLiteral("CutterProgress"))) && progress->isVisible());
    QTRY_VERIFY(progress->value() > 0);
    QVERIFY(std::abs(progress->value() - 1000 / 60) <= 1);
    QTRY_VERIFY(QFileInfo::exists(output));
    QVERIFY(dialog->cutter()->isRunning());

    // Cancel stops ffmpeg, removes the partial file and keeps the dialog.
    Q_EMIT progress->canceled();
    QVERIFY(!dialog->cutter()->isRunning());
    QVERIFY(!QFileInfo::exists(output));
    QCOMPARE(dialog->findChild<QLabel *>(QStringLiteral("CutterStatus"))->text(), QStringLiteral("Export cancelled."));
    QVERIFY(dialog->isVisible());
    QTRY_VERIFY(!dialog->findChild<QProgressDialog *>(QStringLiteral("CutterProgress")));
}

void ToolsTest::missingFfmpeg()
{
    m_window->openFile(m_clip);
    QTRY_VERIFY_WITH_TIMEOUT(prop("duration").toDouble() > 59, 10000);
    usePath(m_emptyBin);
    press(Qt::Key_X, Qt::ControlModifier);
    auto *dialog = waitForDialog<MediaCutterDialog>();
    QVERIFY(dialog);
    QVERIFY(!dialog->findChild<QPushButton *>(QStringLiteral("CutterExportButton"))->isEnabled());
    QVERIFY(dialog->findChild<QLabel *>(QStringLiteral("CutterStatus"))->text().contains(QLatin1String("sudo dnf install ffmpeg")));
}

// ---- Downloader -----------------------------------------------------------

void ToolsTest::recognizesLinks_data()
{
    QTest::addColumn<QString>("text");
    QTest::addColumn<QString>("url");
    QTest::addColumn<QString>("site");

    QTest::newRow("youtube watch") << "https://www.youtube.com/watch?v=dQw4w9WgXcQ"
                                   << "https://www.youtube.com/watch?v=dQw4w9WgXcQ" << "YouTube";
    QTest::newRow("youtu.be") << "  youtu.be/dQw4w9WgXcQ?t=42 " << "https://youtu.be/dQw4w9WgXcQ?t=42" << "YouTube";
    QTest::newRow("youtube shorts") << "https://youtube.com/shorts/abcDEF12345" << "https://youtube.com/shorts/abcDEF12345" << "YouTube";
    QTest::newRow("youtube music") << "https://music.youtube.com/watch?v=x" << "https://music.youtube.com/watch?v=x" << "YouTube";
    QTest::newRow("youtube mobile") << "m.youtube.com/watch?v=x" << "https://m.youtube.com/watch?v=x" << "YouTube";
    QTest::newRow("tiktok video") << "https://www.tiktok.com/@scout2015/video/6718335390845095173"
                                  << "https://www.tiktok.com/@scout2015/video/6718335390845095173" << "TikTok";
    QTest::newRow("tiktok short link") << "https://vm.tiktok.com/ZMeAbCdEf/" << "https://vm.tiktok.com/ZMeAbCdEf/" << "TikTok";
    QTest::newRow("instagram reel") << "https://www.instagram.com/reel/C1a2b3c4d5/" << "https://www.instagram.com/reel/C1a2b3c4d5/" << "Instagram";
    QTest::newRow("x") << "https://x.com/user/status/1" << "https://x.com/user/status/1" << "X (Twitter)";
    QTest::newRow("other site") << "https://example.org/video.mp4" << "https://example.org/video.mp4" << "";
    QTest::newRow("look-alike") << "https://notyoutube.com/watch?v=x" << "https://notyoutube.com/watch?v=x" << "";
    QTest::newRow("spotify track") << "https://open.spotify.com/track/0pqnGHJpmpxLKifKRmU6WP?si=abc"
                                   << "https://open.spotify.com/track/0pqnGHJpmpxLKifKRmU6WP?si=abc" << "Spotify";
    QTest::newRow("spotify uri") << "spotify:track:0pqnGHJpmpxLKifKRmU6WP"
                                 << "https://open.spotify.com/track/0pqnGHJpmpxLKifKRmU6WP" << "Spotify";
    QTest::newRow("spotify short link") << "https://spotify.link/AbCdEfGh" << "https://spotify.link/AbCdEfGh" << "Spotify";
    QTest::newRow("words") << "check this out" << "" << "";
    QTest::newRow("ftp") << "ftp://youtube.com/x" << "" << "";
    QTest::newRow("empty") << "" << "" << "";
}

void ToolsTest::recognizesLinks()
{
    QFETCH(QString, text);
    QFETCH(QString, url);
    QFETCH(QString, site);
    QCOMPARE(MediaDownloader::normalizeUrl(text), url);
    QCOMPARE(MediaDownloader::platformName(text), site);
}

void ToolsTest::ytDlpArguments()
{
    const QString url = QStringLiteral("https://youtu.be/x");
    const QStringList best = MediaDownloader::arguments(url, MediaDownloader::Format::Best, QStringLiteral("/d"));
    const QString joined = best.join(QLatin1Char(' '));
    QVERIFY(joined.contains(QLatin1String("--newline --progress")));
    QVERIFY(joined.contains(QLatin1String("-P /d")));
    QVERIFY(joined.contains(QLatin1String("--print after_move:filepath")));
    QVERIFY(joined.contains(QLatin1String("-f bv*+ba/b --merge-output-format mp4")));
    QCOMPARE(best.mid(best.size() - 2), (QStringList{QStringLiteral("--"), url}));

    const QString audio = MediaDownloader::arguments(url, MediaDownloader::Format::AudioMp3, QStringLiteral("/d")).join(QLatin1Char(' '));
    QVERIFY(audio.contains(QLatin1String("-x --audio-format mp3 --audio-quality 0")));
    QVERIFY(!audio.contains(QLatin1String("--merge-output-format")));
    QVERIFY(MediaDownloader::arguments(url, MediaDownloader::Format::Max720, QStringLiteral("/d")).contains(
        QStringLiteral("bv*[height<=720]+ba/b[height<=720]/b")));
    QVERIFY(MediaDownloader::arguments(url, MediaDownloader::Format::Max2160, QStringLiteral("/d")).join(QLatin1Char(' ')).contains(
        QLatin1String("height<=2160")));
}

void ToolsTest::ytDlpProgress()
{
    auto progress = MediaDownloader::parseProgress(QStringLiteral("[download]  45.3% of   10.03MiB at    2.04MiB/s ETA 00:05"));
    QVERIFY(progress);
    QCOMPARE(progress->percent, 45.3);
    QCOMPARE(progress->total, QStringLiteral("10.03MiB"));
    QCOMPARE(progress->speed, QStringLiteral("2.04MiB/s"));
    QCOMPARE(progress->eta, QStringLiteral("00:05"));
    progress = MediaDownloader::parseProgress(QStringLiteral("[download]   3.0% of ~  52.50MiB at  512.00KiB/s ETA 01:41 (frag 2/60)"));
    QVERIFY(progress);
    QCOMPARE(progress->total, QStringLiteral("52.50MiB"));
    QCOMPARE(progress->speed, QStringLiteral("512.00KiB/s"));
    progress = MediaDownloader::parseProgress(QStringLiteral("[download] 100% of   10.03MiB in 00:00:05 at 1.98MiB/s"));
    QVERIFY(progress);
    QCOMPARE(progress->percent, 100.0);
    QCOMPARE(progress->eta, QString());
    QVERIFY(!MediaDownloader::parseProgress(QStringLiteral("[download] Destination: /d/a.mp4")));
    QVERIFY(!MediaDownloader::parseProgress(QStringLiteral("[youtube] Extracting URL")));
}

void ToolsTest::pastesLinkFromClipboard()
{
    prependPath(m_fakeBin);
    QGuiApplication::clipboard()->setText(QStringLiteral("https://youtu.be/dQw4w9WgXcQ"));
    press(Qt::Key_D, Qt::ControlModifier | Qt::ShiftModifier);
    auto *dialog = waitForDialog<MediaDownloaderDialog>();
    QVERIFY(dialog);
    QCOMPARE(dialog->findChild<QLineEdit *>(QStringLiteral("DownloaderUrl"))->text(), QStringLiteral("https://youtu.be/dQw4w9WgXcQ"));
    QCOMPARE(dialog->findChild<QLabel *>(QStringLiteral("DownloaderSite"))->text(), QStringLiteral("YouTube link"));
    QVERIFY(dialog->findChild<QPushButton *>(QStringLiteral("DownloaderDownloadButton"))->isEnabled());
    QVERIFY(!dialog->findChild<QLabel *>(QStringLiteral("DownloaderWarning"))->isVisible());
    // Typing another link updates the site.
    dialog->findChild<QLineEdit *>(QStringLiteral("DownloaderUrl"))->setText(QStringLiteral("www.tiktok.com/@a/video/1"));
    QCOMPARE(dialog->findChild<QLabel *>(QStringLiteral("DownloaderSite"))->text(), QStringLiteral("TikTok link"));
    QCOMPARE(dialog->url(), QStringLiteral("https://www.tiktok.com/@a/video/1"));
    delete dialog;

    // A TikTok link is picked up too; other clipboard text is not.
    QGuiApplication::clipboard()->setText(QStringLiteral("https://vm.tiktok.com/ZMeAbCdEf/"));
    dialog = new MediaDownloaderDialog(m_window);
    QCOMPARE(dialog->findChild<QLineEdit *>(QStringLiteral("DownloaderUrl"))->text(), QStringLiteral("https://vm.tiktok.com/ZMeAbCdEf/"));
    QCOMPARE(dialog->findChild<QLabel *>(QStringLiteral("DownloaderSite"))->text(), QStringLiteral("TikTok link"));
    delete dialog;
    QGuiApplication::clipboard()->setText(QStringLiteral("just some text"));
    dialog = new MediaDownloaderDialog(m_window);
    QCOMPARE(dialog->findChild<QLineEdit *>(QStringLiteral("DownloaderUrl"))->text(), QString());
    QVERIFY(!dialog->findChild<QPushButton *>(QStringLiteral("DownloaderDownloadButton"))->isEnabled());
    delete dialog;
}

void ToolsTest::missingYtDlp()
{
    usePath(m_emptyBin);
    QGuiApplication::clipboard()->setText(QStringLiteral("https://youtu.be/dQw4w9WgXcQ"));
    auto *dialog = new MediaDownloaderDialog(m_window);
    dialog->show();
    auto *warning = dialog->findChild<QLabel *>(QStringLiteral("DownloaderWarning"));
    QVERIFY(warning->isVisible());
    QVERIFY(warning->text().contains(QLatin1String("sudo dnf install yt-dlp")));
    QVERIFY(warning->text().contains(QLatin1String("pip install")));
    QVERIFY(!dialog->findChild<QPushButton *>(QStringLiteral("DownloaderDownloadButton"))->isEnabled());
    QVERIFY(!dialog->findChild<QPushButton *>(QStringLiteral("DownloaderStreamButton"))->isEnabled());
    delete dialog;
}

void ToolsTest::downloadAndPlay()
{
    prependPath(m_fakeBin);
    const QString downloads = m_dir.filePath(QStringLiteral("downloads"));
    QGuiApplication::clipboard()->setText(QStringLiteral("https://www.youtube.com/watch?v=abc123"));
    press(Qt::Key_D, Qt::ControlModifier | Qt::ShiftModifier);
    auto *dialog = waitForDialog<MediaDownloaderDialog>();
    QVERIFY(dialog);
    dialog->findChild<QLineEdit *>(QStringLiteral("DownloaderDirectory"))->setText(downloads);
    auto *format = dialog->findChild<QComboBox *>(QStringLiteral("DownloaderFormat"));
    format->setCurrentIndex(format->findData(int(MediaDownloader::Format::Max1080)));
    QVERIFY(dialog->findChild<QCheckBox *>(QStringLiteral("DownloaderPlay"))->isChecked());

    auto *progress = dialog->findChild<QProgressBar *>(QStringLiteral("DownloaderProgress"));
    auto *speed = dialog->findChild<QLabel *>(QStringLiteral("DownloaderSpeed"));
    QSignalSpy updates(dialog->downloader(), &MediaDownloader::progress);
    QTest::mouseClick(dialog->findChild<QPushButton *>(QStringLiteral("DownloaderDownloadButton")), Qt::LeftButton);
    QVERIFY(dialog->isBusy());
    QVERIFY(progress->isVisible());
    // Live progress and speed while it runs.
    QTRY_VERIFY_WITH_TIMEOUT(progress->value() == 473, 5000);
    QVERIFY(speed->text().contains(QLatin1String("2.04MiB/s")));
    QVERIFY(speed->text().contains(QLatin1String("ETA 00:01")));

    // Done: the dialog closes and the file plays.
    QTRY_VERIFY_WITH_TIMEOUT(!m_window->findChild<MediaDownloaderDialog *>(), 10000);
    QCOMPARE(updates.size(), 3);
    const QString file = downloads + QStringLiteral("/Test Video [abc123].mp4");
    QVERIFY(QFileInfo::exists(file));
    QTRY_COMPARE_WITH_TIMEOUT(propString("path"), file, 10000);
    QCOMPARE(m_window->findChild<OsdWidget *>()->text(), QStringLiteral("Downloaded: Test Video [abc123].mp4"));

    // What yt-dlp was asked for.
    QFile log(QString::fromLocal8Bit(qgetenv("FAKE_LOG")));
    QVERIFY(log.open(QIODevice::ReadOnly));
    const QStringList args = QString::fromUtf8(log.readAll()).split(QLatin1Char('\n'), Qt::SkipEmptyParts);
    QCOMPARE(args, MediaDownloader::arguments(QStringLiteral("https://www.youtube.com/watch?v=abc123"),
                                              MediaDownloader::Format::Max1080, downloads));
    // The choices are remembered.
    QCOMPARE(MediaDownloaderDialog::defaultDirectory(), downloads);
}

void ToolsTest::downloadError()
{
    prependPath(m_fakeBin);
    auto *dialog = new MediaDownloaderDialog(m_window);
    dialog->show();
    dialog->findChild<QLineEdit *>(QStringLiteral("DownloaderUrl"))->setText(QStringLiteral("https://youtu.be/unavailable"));
    dialog->startDownload();
    auto *status = dialog->findChild<QLabel *>(QStringLiteral("DownloaderStatus"));
    QTRY_COMPARE_WITH_TIMEOUT(status->text(), QStringLiteral("[youtube] abc123: Video unavailable"), 5000);
    QVERIFY(!dialog->isBusy());
    QVERIFY(dialog->isVisible());
    QVERIFY(dialog->findChild<QPushButton *>(QStringLiteral("DownloaderDownloadButton"))->isEnabled());
    delete dialog;
}

void ToolsTest::cancelDownload()
{
    prependPath(m_fakeBin);
    auto *dialog = new MediaDownloaderDialog(m_window);
    dialog->show();
    dialog->findChild<QLineEdit *>(QStringLiteral("DownloaderUrl"))->setText(QStringLiteral("https://youtu.be/slow"));
    dialog->startDownload();
    QTRY_VERIFY(dialog->findChild<QProgressBar *>(QStringLiteral("DownloaderProgress"))->value() == 1000);
    QVERIFY(dialog->isBusy());
    // Esc stops yt-dlp and keeps the dialog; a second Esc closes it.
    QTest::keyClick(dialog, Qt::Key_Escape);
    QVERIFY(!dialog->isBusy());
    QCOMPARE(dialog->findChild<QLabel *>(QStringLiteral("DownloaderStatus"))->text(), QStringLiteral("Download cancelled."));
    QVERIFY(dialog->isVisible());
    QTest::keyClick(dialog, Qt::Key_Escape);
    QVERIFY(!dialog->isVisible());
    delete dialog;
}

void ToolsTest::directStream()
{
    prependPath(m_fakeBin);
    QGuiApplication::clipboard()->setText(QStringLiteral("https://www.tiktok.com/@scout2015/video/6718335390845095173"));
    press(Qt::Key_D, Qt::ControlModifier | Qt::ShiftModifier);
    auto *dialog = waitForDialog<MediaDownloaderDialog>();
    QVERIFY(dialog);
    auto *format = dialog->findChild<QComboBox *>(QStringLiteral("DownloaderFormat"));
    format->setCurrentIndex(format->findData(int(MediaDownloader::Format::Max720)));
    QTest::mouseClick(dialog->findChild<QPushButton *>(QStringLiteral("DownloaderStreamButton")), Qt::LeftButton);
    QTRY_VERIFY(!m_window->findChild<MediaDownloaderDialog *>());
    // mpv gets the page URL and picks streams through yt-dlp in that format.
    QTRY_COMPARE(propString("ytdl-format"), MediaDownloader::streamFormat(MediaDownloader::Format::Max720));
    const QVariantList playlist = prop("playlist").toList();
    QVERIFY(!playlist.isEmpty());
    QCOMPARE(playlist.first().toMap().value(QStringLiteral("filename")).toString(),
             QStringLiteral("https://www.tiktok.com/@scout2015/video/6718335390845095173"));
}

// ---- Spotify ----------------------------------------------------------------

void ToolsTest::spotifyLinks()
{
    QVERIFY(MediaDownloader::isSpotifyUrl(QStringLiteral("open.spotify.com/album/1DFixLWuPkv3KT3TnV35m3")));
    QVERIFY(!MediaDownloader::isSpotifyUrl(QStringLiteral("https://www.youtube.com/watch?v=x")));
    auto link = MediaDownloader::parseSpotifyLink(QStringLiteral("https://open.spotify.com/intl-de/track/0pqnGHJpmpxLKifKRmU6WP?si=1"));
    QVERIFY(link);
    QCOMPARE(link->type, QStringLiteral("track"));
    QCOMPARE(link->id, QStringLiteral("0pqnGHJpmpxLKifKRmU6WP"));
    link = MediaDownloader::parseSpotifyLink(QStringLiteral("https://open.spotify.com/embed/playlist/37i9dQZF1DXcBWIGoYBM5M"));
    QVERIFY(link);
    QCOMPARE(link->type, QStringLiteral("playlist"));
    link = MediaDownloader::parseSpotifyLink(QStringLiteral("spotify:album:1DFixLWuPkv3KT3TnV35m3"));
    QVERIFY(link);
    QCOMPARE(link->type, QStringLiteral("album"));
    // Podcasts, short links (until followed) and other sites are not songs.
    QVERIFY(!MediaDownloader::parseSpotifyLink(QStringLiteral("https://open.spotify.com/episode/0pqnGHJpmpxLKifKRmU6WP")));
    QVERIFY(!MediaDownloader::parseSpotifyLink(QStringLiteral("https://spotify.link/AbCdEfGh")));
    QVERIFY(!MediaDownloader::parseSpotifyLink(QStringLiteral("https://example.com/track/0pqnGHJpmpxLKifKRmU6WP")));
}

void ToolsTest::spotifyPages()
{
    // A track's embed page.
    QString collection = QStringLiteral("unset");
    QList<MediaDownloader::SpotifyTrack> tracks = MediaDownloader::parseSpotifyPage(nextDataPage(kTrackEntity), &collection);
    QCOMPARE(tracks.size(), 1);
    QCOMPARE(tracks[0].title, QStringLiteral("Believer"));
    QCOMPARE(tracks[0].artist, QStringLiteral("Imagine Dragons"));
    QVERIFY(std::abs(tracks[0].duration - 204.346) < 0.001);
    QCOMPARE(collection, QStringLiteral("unset"));

    // An album: every track, with the album's name; non-breaking spaces cleaned.
    tracks = MediaDownloader::parseSpotifyPage(nextDataPage(kAlbumEntity), &collection);
    QCOMPARE(tracks.size(), 4);
    QCOMPARE(collection, QStringLiteral("Night Drive"));
    QCOMPARE(tracks[1].title, QStringLiteral("NoMatch Two"));
    QCOMPARE(tracks[1].artist, QStringLiteral("Neon Coast, Guest"));
    QCOMPARE(tracks[1].album, QStringLiteral("Night Drive"));
    QCOMPARE(tracks[0].duration, 180.0);

    // The web player's page: og: tags.
    const QByteArray web =
        "<html><head><meta property=\"og:title\" content=\"Don&#x27;t Stop Me Now\"/>"
        "<meta property=\"og:description\" content=\"Queen &amp; Friends \xc2\xb7 Jazz \xc2\xb7 Song \xc2\xb7 1978\"/>"
        "<meta property=\"og:type\" content=\"music.song\"/><meta name=\"music:duration\" content=\"209\"/></head></html>";
    tracks = MediaDownloader::parseSpotifyPage(web);
    QCOMPARE(tracks.size(), 1);
    QCOMPARE(tracks[0].title, QStringLiteral("Don't Stop Me Now"));
    QCOMPARE(tracks[0].artist, QStringLiteral("Queen & Friends"));
    QCOMPARE(tracks[0].album, QStringLiteral("Jazz"));
    QCOMPARE(tracks[0].duration, 209.0);

    // Anything else holds no songs.
    QVERIFY(MediaDownloader::parseSpotifyPage("<html><body>Page not found</body></html>").isEmpty());
    QVERIFY(MediaDownloader::parseSpotifyPage(QByteArray()).isEmpty());
}

void ToolsTest::spotifyArguments()
{
    MediaDownloader::SpotifyTrack track{QStringLiteral("Believer"), QStringLiteral("Imagine Dragons, Lil Wayne, Someone"),
                                        QStringLiteral("Evolve: 100% Deluxe"), 204};
    QCOMPARE(MediaDownloader::spotifyQuery(track), QStringLiteral("Imagine Dragons Lil Wayne - Believer"));
    const QStringList strict = MediaDownloader::spotifyArguments(track, QStringLiteral("/d"), true);
    const QString joined = strict.join(QLatin1Char('\n'));
    QVERIFY(joined.contains(QLatin1String("-P\n/d")));
    QVERIFY(joined.contains(QLatin1String("-o\nImagine Dragons, Lil Wayne, Someone - Believer.%(ext)s")));
    QVERIFY(joined.contains(QLatin1String("-x\n--audio-format\nmp3")));
    QVERIFY(joined.contains(QLatin1String("--embed-metadata")));
    // Tags carry Spotify's names; ':' and '%' are escaped for yt-dlp.
    QVERIFY(strict.contains(QStringLiteral("pre_process:Believer |:%(meta_title)s |")));
    QVERIFY(strict.contains(QStringLiteral("pre_process:Evolve\\: 100%% Deluxe |:%(meta_album)s |")));
    // The length decides between search results.
    QVERIFY(strict.contains(QStringLiteral("duration>=192 & duration<=216 & !is_live")));
    QCOMPARE(strict.mid(strict.size() - 2), (QStringList{QStringLiteral("--"), QStringLiteral("ytsearch5:Imagine Dragons Lil Wayne - Believer")}));
    const QStringList loose = MediaDownloader::spotifyArguments(track, QStringLiteral("/d"), false);
    QVERIFY(!loose.contains(QStringLiteral("--match-filter")));
    QCOMPARE(loose.last(), QStringLiteral("ytsearch1:Imagine Dragons Lil Wayne - Believer"));
    // A slash can't make a folder of the name.
    track.title = QStringLiteral("AC/DC Song");
    QVERIFY(MediaDownloader::spotifyArguments(track, QStringLiteral("/d"), false).contains(
        QStringLiteral("Imagine Dragons, Lil Wayne, Someone - AC-DC Song.%(ext)s")));
    QCOMPARE(MediaDownloader::spotifyStreamUrl(track), QStringLiteral("ytdl://ytsearch1:Imagine Dragons Lil Wayne - AC/DC Song"));
}

void ToolsTest::spotifyDownloadsAlbum()
{
    prependPath(m_spotifyBin);
    QFile::remove(QString::fromLocal8Bit(qgetenv("FAKE_LOG")));
    FakeSpotify spotify;
    QVERIFY(spotify.listen());
    spotify.pages.insert(QStringLiteral("/embed/album/1DFixLWuPkv3KT3TnV35m3"), nextDataPage(kAlbumEntity));
    const QString downloads = m_dir.filePath(QStringLiteral("spotify"));

    MediaDownloader downloader;
    downloader.setSpotifyBaseUrl(spotify.url());
    QSignalSpy resolved(&downloader, &MediaDownloader::spotifyResolved);
    QSignalSpy finished(&downloader, &MediaDownloader::finished);
    QSignalSpy progress(&downloader, &MediaDownloader::progress);
    // Whatever format is asked for, Spotify songs become MP3s.
    QVERIFY(downloader.start(QStringLiteral("https://open.spotify.com/album/1DFixLWuPkv3KT3TnV35m3"),
                             MediaDownloader::Format::Best, downloads));
    QVERIFY(downloader.isRunning());
    QTRY_COMPARE_WITH_TIMEOUT(finished.size(), 1, 15000);
    QVERIFY(!downloader.isRunning());
    QCOMPARE(resolved.size(), 1);
    QCOMPARE(resolved.first().at(1).toString(), QStringLiteral("Night Drive"));
    QVERIFY(finished.first().at(0).toBool());

    // The missing song is skipped; the others are saved in album order, the
    // one without an upload of the right length from the closest match.
    const QStringList files = downloader.files();
    QCOMPARE(files, (QStringList{downloads + QStringLiteral("/Neon Coast - Song One.mp3"),
                                 downloads + QStringLiteral("/Neon Coast, Guest - NoMatch Two.mp3"),
                                 downloads + QStringLiteral("/Neon Coast - Song Four.mp3")}));
    for (const QString &file : files)
        QVERIFY(QFileInfo::exists(file));
    QCOMPARE(finished.first().at(1).toString(), files.first());
    // Progress covers the whole album: the first song's half is an eighth of it.
    QVERIFY(!progress.isEmpty());
    QCOMPARE(progress.first().at(0).value<MediaDownloader::Progress>().percent, 12.5);

    QFile log(QString::fromLocal8Bit(qgetenv("FAKE_LOG")));
    QVERIFY(log.open(QIODevice::ReadOnly));
    const QString calls = QString::fromUtf8(log.readAll());
    QVERIFY(calls.contains(QLatin1String("ytsearch5:Neon Coast - Song One")));
    // Retried without the length check, then the next one.
    QVERIFY(calls.contains(QLatin1String("ytsearch5:Neon Coast Guest - NoMatch Two")));
    QVERIFY(calls.contains(QLatin1String("ytsearch1:Neon Coast Guest - NoMatch Two")));
    QVERIFY(calls.contains(QLatin1String("ytsearch1:Neon Coast - Missing Three")));
    // Without a length, the best match is taken at once.
    QVERIFY(calls.contains(QLatin1String("ytsearch1:Neon Coast - Song Four")));
    QVERIFY(!calls.contains(QLatin1String("ytsearch5:Neon Coast - Song Four")));
    QVERIFY(calls.contains(QLatin1String("pre_process:Night Drive |:%(meta_album)s |")));

    // A link that isn't there: an error, not a hang.
    finished.clear();
    QVERIFY(downloader.start(QStringLiteral("https://open.spotify.com/track/0000000000000000000000"),
                             MediaDownloader::Format::AudioMp3, downloads));
    QTRY_COMPARE_WITH_TIMEOUT(finished.size(), 1, 15000);
    QVERIFY(!finished.first().at(0).toBool());
    QVERIFY(!finished.first().at(2).toString().isEmpty());
    QVERIFY(!downloader.isRunning());
}

void ToolsTest::spotifyDialog()
{
    prependPath(m_spotifyBin);
    QGuiApplication::clipboard()->setText(QStringLiteral("spotify:track:0pqnGHJpmpxLKifKRmU6WP"));
    auto *dialog = new MediaDownloaderDialog(m_window);
    dialog->show();
    auto *url = dialog->findChild<QLineEdit *>(QStringLiteral("DownloaderUrl"));
    auto *format = dialog->findChild<QComboBox *>(QStringLiteral("DownloaderFormat"));
    auto *site = dialog->findChild<QLabel *>(QStringLiteral("DownloaderSite"));
    // The URI from "Copy Spotify URI" is taken from the clipboard.
    QCOMPARE(url->text(), QStringLiteral("https://open.spotify.com/track/0pqnGHJpmpxLKifKRmU6WP"));
    QVERIFY(site->text().startsWith(QLatin1String("Spotify link")));
    QVERIFY(dialog->isSpotify());
    // Songs only: MP3, and the choice is put back for other links.
    QCOMPARE(dialog->format(), MediaDownloader::Format::AudioMp3);
    QVERIFY(!format->isEnabled());
    url->setText(QStringLiteral("https://open.spotify.com/playlist/37i9dQZF1DXcBWIGoYBM5M"));
    QVERIFY(site->text().contains(QLatin1String("playlist")));
    url->setText(QStringLiteral("https://youtu.be/x"));
    QVERIFY(format->isEnabled());
    QCOMPARE(site->text(), QStringLiteral("YouTube link"));
    delete dialog;
}

void ToolsTest::spotifyStream()
{
    prependPath(m_spotifyBin);
    FakeSpotify spotify;
    QVERIFY(spotify.listen());
    spotify.pages.insert(QStringLiteral("/embed/album/1DFixLWuPkv3KT3TnV35m3"), nextDataPage(kAlbumEntity));
    auto *dialog = new MediaDownloaderDialog(m_window);
    dialog->downloader()->setSpotifyBaseUrl(spotify.url());
    dialog->findChild<QLineEdit *>(QStringLiteral("DownloaderUrl"))->setText(QStringLiteral("https://open.spotify.com/album/1DFixLWuPkv3KT3TnV35m3"));
    QSignalSpy streams(dialog, &MediaDownloaderDialog::tracksStreamRequested);
    dialog->show();
    dialog->streamDirectly();
    QVERIFY(dialog->isBusy());
    QTRY_COMPARE_WITH_TIMEOUT(streams.size(), 1, 10000);
    const QStringList urls = streams.first().at(0).toStringList();
    const QStringList titles = streams.first().at(1).toStringList();
    QCOMPARE(urls.size(), 4);
    QCOMPARE(urls.first(), QStringLiteral("ytdl://ytsearch1:Neon Coast - Song One"));
    QCOMPARE(titles.first(), QStringLiteral("Neon Coast - Song One"));
    QTRY_VERIFY(!dialog->isVisible());
    delete dialog;
}

int main(int argc, char *argv[])
{
    // Keep the settings away from the user's own.
    QTemporaryDir config;
    qputenv("XDG_CONFIG_HOME", config.path().toLocal8Bit());
    QApplication app(argc, argv);
    // Reopened files play from the start instead of asking to resume (tst_resume covers that).
    ResumeManager::setMode(ResumeManager::Mode::Never);
    // libmpv requires the C numeric locale; QApplication may have changed it.
    std::setlocale(LC_NUMERIC, "C");
    QApplication::setAttribute(Qt::AA_DontUseNativeDialogs);
    ToolsTest test;
    QTEST_SET_MAIN_SOURCE_PATH
    return QTest::qExec(&test, argc, argv);
}

#include "tst_tools.moc"
