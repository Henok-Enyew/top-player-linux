// Resume prompt: positions are remembered per video, and reopening a video
// that was left halfway asks whether to resume or start over (or does one
// of them by itself, as configured). Songs always play from the start. Drives the real window; needs a
// display (run under xvfb-run) and ffmpeg, which generates the test media.

#include "MainWindow.h"
#include "MpvWidget.h"
#include "ResumeManager.h"
#include "ResumePrompt.h"
#include "TestClip.h"

#include <QApplication>
#include <QProcess>
#include <QPushButton>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTest>

#include <clocale>
#include <cmath>

class ResumeTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void initTestCase();
    void init();
    void cleanup();

    void storeKeepsUsefulPositions();
    void asksAndResumes();
    void startOver();
    void keyboardAnswers();
    void countsDown();
    void alwaysAndNever();
    void finishedFileIsForgotten();
    void songsStartOver();

private:
    void open(const QString &file);
    // Plays `file` to `seconds`, then opens another file so it is saved.
    void leaveAt(const QString &file, double seconds);
    double time() const { return m_mpv->mpvProperty(QStringLiteral("time-pos")).toDouble(); }
    bool paused() const { return m_mpv->mpvProperty(QStringLiteral("pause")).toBool(); }

    QTemporaryDir m_dir;
    QString m_long, m_other, m_song;
    MainWindow *m_window = nullptr;
    MpvWidget *m_mpv = nullptr;
    ResumeManager *m_resume = nullptr;
};

void ResumeTest::initTestCase()
{
    QVERIFY(m_dir.isValid());
    m_long = m_dir.filePath(QStringLiteral("long.mkv"));
    m_other = m_dir.filePath(QStringLiteral("other.mkv"));
    if (!makeTestClip(m_long, 120))
        QSKIP("ffmpeg is needed to generate the test media");
    QVERIFY(makeTestClip(m_other, 60));
    // Two minutes of tone: audio without video.
    m_song = m_dir.filePath(QStringLiteral("song.mka"));
    QProcess ffmpeg;
    ffmpeg.start(QStandardPaths::findExecutable(QStringLiteral("ffmpeg")),
                 {QStringLiteral("-loglevel"), QStringLiteral("error"), QStringLiteral("-y"), QStringLiteral("-f"),
                  QStringLiteral("lavfi"), QStringLiteral("-i"), QStringLiteral("sine=frequency=440:duration=120"),
                  QStringLiteral("-c:a"), QStringLiteral("flac"), m_song});
    QVERIFY(ffmpeg.waitForFinished(60000) && ffmpeg.exitCode() == 0);
}

void ResumeTest::init()
{
    ResumeManager::setMode(ResumeManager::Mode::Ask);
    m_window = new MainWindow;
    m_window->resize(800, 450);
    m_window->show();
    m_window->activateWindow();
    QVERIFY(QTest::qWaitForWindowActive(m_window));
    m_mpv = m_window->findChild<MpvWidget *>();
    m_resume = m_window->resume();
    QVERIFY(m_mpv && m_resume);
    m_mpv->setMpvProperty(QStringLiteral("ao"), QStringLiteral("null"));
}

void ResumeTest::cleanup()
{
    delete m_window;
    m_window = nullptr;
    ResumeManager::forget(m_long);
    ResumeManager::forget(m_other);
    ResumeManager::forget(m_song);
}

void ResumeTest::open(const QString &file)
{
    m_window->openFile(file);
    QTRY_COMPARE_WITH_TIMEOUT(m_mpv->mpvPropertyString(QStringLiteral("path")), file, 10000);
    QTRY_VERIFY_WITH_TIMEOUT(m_mpv->mpvProperty(QStringLiteral("time-pos")).isValid(), 10000);
}

void ResumeTest::leaveAt(const QString &file, double seconds)
{
    open(file);
    QTest::qWait(100);
    if (m_resume->prompt()->isVisible())
        m_resume->prompt()->resumeButton()->click();
    m_mpv->command({QStringLiteral("seek"), QString::number(seconds), QStringLiteral("absolute+exact")});
    QTRY_VERIFY_WITH_TIMEOUT(std::abs(time() - seconds) < 1.5, 10000);
    // Let the player hear of the new position, as it would while playing.
    QTest::qWait(400);
    open(file == m_other ? m_long : m_other);
    m_resume->prompt()->dismiss(); // in case the other file asks too
}

void ResumeTest::storeKeepsUsefulPositions()
{
    const QString media = m_dir.filePath(QStringLiteral("stored.mkv"));
    QVERIFY(!ResumeManager::lookup(media));
    ResumeManager::remember(media, 42.5, 600);
    QVERIFY(ResumeManager::lookup(media));
    QCOMPARE(ResumeManager::lookup(media)->position, 42.5);
    QCOMPARE(ResumeManager::lookup(media)->duration, 600.0);
    // Too close to the start or the end, or a stream without length: dropped.
    ResumeManager::remember(media, 3, 600);
    QVERIFY(!ResumeManager::lookup(media));
    ResumeManager::remember(media, 42.5, 600);
    ResumeManager::remember(media, 595, 600);
    QVERIFY(!ResumeManager::lookup(media));
    ResumeManager::remember(QStringLiteral("https://example.com/live.m3u8"), 100, 0);
    QVERIFY(!ResumeManager::lookup(QStringLiteral("https://example.com/live.m3u8")));
    // file:// URLs and paths are the same file.
    ResumeManager::remember(media, 50, 600);
    QVERIFY(ResumeManager::lookup(QUrl::fromLocalFile(media).toString()));
    ResumeManager::forget(media);
    QVERIFY(!ResumeManager::lookup(media));
}

void ResumeTest::asksAndResumes()
{
    leaveAt(m_long, 40);
    QVERIFY(ResumeManager::lookup(m_long));
    QVERIFY(std::abs(ResumeManager::lookup(m_long)->position - 40) < 1.5);

    open(m_long);
    ResumePrompt *prompt = m_resume->prompt();
    QTRY_VERIFY(prompt->isVisible());
    // Held at the start while asking.
    QVERIFY(paused());
    QVERIFY(time() < 2);
    QVERIFY(prompt->resumeButton()->text().contains(QLatin1String("0:40")));
    QTest::mouseClick(prompt->resumeButton(), Qt::LeftButton);
    QVERIFY(!prompt->isVisible());
    QTRY_VERIFY_WITH_TIMEOUT(std::abs(time() - 40) < 2, 10000);
    // Opening played, so resuming plays.
    QVERIFY(!paused());
}

void ResumeTest::startOver()
{
    leaveAt(m_long, 30);
    open(m_long);
    ResumePrompt *prompt = m_resume->prompt();
    QTRY_VERIFY(prompt->isVisible());
    QTest::mouseClick(prompt->startOverButton(), Qt::LeftButton);
    QVERIFY(!prompt->isVisible());
    QTRY_VERIFY(!paused());
    QVERIFY(time() < 5);
    QVERIFY(!ResumeManager::lookup(m_long));
}

void ResumeTest::keyboardAnswers()
{
    leaveAt(m_long, 50);
    open(m_long);
    ResumePrompt *prompt = m_resume->prompt();
    QTRY_VERIFY(prompt->isVisible());
    QTRY_VERIFY(prompt->hasFocus() || prompt->isAncestorOf(QApplication::focusWidget()));
    // Enter resumes instead of toggling fullscreen.
    QTest::keyClick(QApplication::focusWidget(), Qt::Key_Return);
    QVERIFY(!prompt->isVisible());
    QVERIFY(!m_window->isFullScreen());
    QTRY_VERIFY_WITH_TIMEOUT(std::abs(time() - 50) < 2, 10000);

    leaveAt(m_long, 50);
    open(m_long);
    QTRY_VERIFY(prompt->isVisible());
    QTest::keyClick(QApplication::focusWidget(), Qt::Key_Escape);
    QVERIFY(!prompt->isVisible());
    QTRY_VERIFY(!paused());
    QVERIFY(time() < 5);
}

void ResumeTest::countsDown()
{
    leaveAt(m_long, 40);
    open(m_long);
    ResumePrompt *prompt = m_resume->prompt();
    QTRY_VERIFY(prompt->isVisible());
    QCOMPARE(prompt->secondsLeft(), ResumePrompt::kCountdownSeconds);
    QTRY_COMPARE_WITH_TIMEOUT(prompt->secondsLeft(), ResumePrompt::kCountdownSeconds - 1, 2000);
    // Opening another file withdraws the question.
    open(m_other);
    QVERIFY(!prompt->isVisible());
    QVERIFY(!paused());
}

void ResumeTest::alwaysAndNever()
{
    leaveAt(m_long, 35);
    ResumeManager::setMode(ResumeManager::Mode::Always);
    open(m_long);
    QTRY_VERIFY_WITH_TIMEOUT(std::abs(time() - 35) < 2, 10000);
    QVERIFY(!m_resume->prompt()->isVisible());
    QVERIFY(!paused());

    ResumeManager::setMode(ResumeManager::Mode::Never);
    open(m_other);
    open(m_long);
    QTest::qWait(500);
    QVERIFY(!m_resume->prompt()->isVisible());
    QVERIFY(time() < 5);
    ResumeManager::setMode(ResumeManager::Mode::Ask);
}

void ResumeTest::finishedFileIsForgotten()
{
    leaveAt(m_long, 40);
    QVERIFY(ResumeManager::lookup(m_long));
    leaveAt(m_long, 116);
    QVERIFY(!ResumeManager::lookup(m_long));
}

void ResumeTest::songsStartOver()
{
    // A song left halfway is not remembered...
    open(m_song);
    QTRY_VERIFY(m_mpv->isAudioOnly());
    m_mpv->command({QStringLiteral("seek"), QStringLiteral("60"), QStringLiteral("absolute+exact")});
    QTRY_VERIFY_WITH_TIMEOUT(std::abs(time() - 60) < 1.5, 10000);
    QTest::qWait(400);
    open(m_other);
    QTRY_VERIFY(!m_mpv->isAudioOnly());
    QTest::qWait(300);
    QVERIFY(!ResumeManager::lookup(m_song));

    // ...and one saved by an older version plays from the start, unasked.
    ResumeManager::remember(m_song, 60, 120);
    QVERIFY(ResumeManager::lookup(m_song));
    open(m_song);
    QTest::qWait(500);
    QVERIFY(!m_resume->prompt()->isVisible());
    QVERIFY(!paused());
    QVERIFY(time() < 5);
    QVERIFY(!ResumeManager::lookup(m_song));

    // Videos still ask.
    leaveAt(m_long, 40);
    open(m_long);
    QTRY_VERIFY(m_resume->prompt()->isVisible());
}

int main(int argc, char *argv[])
{
    // Keep the settings away from the user's own.
    QTemporaryDir config;
    qputenv("XDG_CONFIG_HOME", config.path().toLocal8Bit());
    QApplication app(argc, argv);
    std::setlocale(LC_NUMERIC, "C");
    ResumeTest test;
    QTEST_SET_MAIN_SOURCE_PATH
    return QTest::qExec(&test, argc, argv);
}

#include "tst_resume.moc"
