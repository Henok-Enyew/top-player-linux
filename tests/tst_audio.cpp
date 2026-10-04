// Audio files: detection, embedded cover art (ID3, FLAC and Vorbis comment
// pictures), folder covers, custom artwork, the visualizations, and that
// seeking, volume and audio track switching work as they do for video.
// Drives the real window and checks the result on mpv's properties. Needs a
// display (run under xvfb-run) and ffmpeg, which generates the test media.

#include "ResumeManager.h"
#include "AudioController.h"
#include "AudioView.h"
#include "MainWindow.h"
#include "MpvWidget.h"
#include "PlayerMenu.h"
#include "TestClip.h"

#include <QApplication>
#include <QBuffer>
#include <QDir>
#include <QFileDialog>
#include <QLineEdit>
#include <QMenu>
#include <QProcess>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>
#include <QtEndian>

#include <clocale>
#include <cmath>

namespace {

// Picks `path` in the next file dialog once it is open.
void acceptFileDialog(const QString &path)
{
    auto *timer = new QTimer;
    timer->setInterval(20);
    QObject::connect(timer, &QTimer::timeout, timer, [timer, path] {
        auto *dialog = qobject_cast<QFileDialog *>(QApplication::activeModalWidget());
        if (!dialog)
            return;
        timer->stop();
        timer->deleteLater();
        // selectFile() leaves the file name field alone while it has focus,
        // which it does in an active window; type the name instead.
        auto *name = dialog->findChild<QLineEdit *>(QStringLiteral("fileNameEdit"));
        QVERIFY(name);
        name->setText(path);
        static_cast<QDialog *>(dialog)->accept(); // public in QDialog, protected in QFileDialog
    });
    timer->start();
}

bool ffmpeg(const QStringList &args)
{
    const QString program = QStandardPaths::findExecutable(QStringLiteral("ffmpeg"));
    if (program.isEmpty())
        return false;
    QProcess process;
    process.start(program, QStringList{QStringLiteral("-loglevel"), QStringLiteral("error"), QStringLiteral("-y")} + args);
    return process.waitForFinished(60000) && process.exitStatus() == QProcess::NormalExit && process.exitCode() == 0;
}

bool solidImage(const QString &path, const QColor &color)
{
    QImage image(120, 120, QImage::Format_RGB32);
    image.fill(color);
    return image.save(path);
}

QStringList tone(int seconds, int frequency = 440)
{
    return {QStringLiteral("-f"), QStringLiteral("lavfi"), QStringLiteral("-i"),
            QStringLiteral("sine=f=%1:d=%2").arg(frequency).arg(seconds)};
}

// A FLAC picture block, as Vorbis comments carry it (METADATA_BLOCK_PICTURE).
QByteArray flacPictureBlock(const QString &jpegPath)
{
    QFile file(jpegPath);
    if (!file.open(QIODevice::ReadOnly))
        return {};
    const QByteArray data = file.readAll();
    QByteArray block;
    auto u32 = [&block](quint32 value) {
        const quint32 big = qToBigEndian(value);
        block.append(reinterpret_cast<const char *>(&big), 4);
    };
    const QByteArray mime("image/jpeg");
    u32(3); // front cover
    u32(mime.size());
    block.append(mime);
    u32(0); // no description
    u32(120);
    u32(120);
    u32(24);
    u32(0);
    u32(data.size());
    block.append(data);
    return block;
}

bool near(const QColor &a, const QColor &b)
{
    return std::abs(a.red() - b.red()) < 40 && std::abs(a.green() - b.green()) < 40 && std::abs(a.blue() - b.blue()) < 40;
}

} // namespace

class AudioTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void initTestCase();
    void init();
    void cleanup();

    void detectsAudioFiles();
    void embeddedArtwork_data();
    void embeddedArtwork();
    void folderCover();
    void fallsBackToSpectrum();
    void customArtwork();
    void customArtworkOverVisualization();
    void dropImageSetsArtwork();
    void visualizationMenu();
    void seekAndVolumeWithVisualization();
    void audioTrackSwitching();
    void videoAfterAudio();

private:
    QVariant prop(const char *name) const { return m_mpv->mpvProperty(QString::fromLatin1(name)); }
    QString propString(const char *name) const { return m_mpv->mpvPropertyString(QString::fromLatin1(name)); }
    // Opens `file` and waits until it plays, paused.
    void open(const QString &file);
    void setVisualization(const QString &text);
    QMenu *audioMenu() const;
    QAction *audioTrackAction(const QString &prefix) const;

    QTemporaryDir m_dir;
    QString m_mp3, m_flac, m_opus, m_folderTrack, m_plain, m_long, m_twoTracks, m_video, m_customImage;
    MainWindow *m_window = nullptr;
    MpvWidget *m_mpv = nullptr;
    AudioController *m_audio = nullptr;
};

void AudioTest::initTestCase()
{
    QVERIFY(m_dir.isValid());
    const QString red = m_dir.filePath(QStringLiteral("red.png"));
    const QString blue = m_dir.filePath(QStringLiteral("blue.png"));
    const QString green = m_dir.filePath(QStringLiteral("green.jpg"));
    QVERIFY(solidImage(red, Qt::red) && solidImage(blue, Qt::blue) && solidImage(green, Qt::green));
    m_customImage = m_dir.filePath(QStringLiteral("custom.png"));
    QVERIFY(solidImage(m_customImage, Qt::magenta));

    // Embedded covers: an ID3 APIC frame, a FLAC picture block, and a Vorbis
    // comment METADATA_BLOCK_PICTURE in an Opus file.
    m_mp3 = m_dir.filePath(QStringLiteral("embedded/song.mp3"));
    m_flac = m_dir.filePath(QStringLiteral("embedded/song.flac"));
    m_opus = m_dir.filePath(QStringLiteral("embedded/song.opus"));
    QVERIFY(QDir().mkpath(m_dir.filePath(QStringLiteral("embedded"))));
    const QStringList tags{QStringLiteral("-metadata"), QStringLiteral("title=Midnight Drive"),
                           QStringLiteral("-metadata"), QStringLiteral("artist=Neon Coast"),
                           QStringLiteral("-metadata"), QStringLiteral("album=Late Summer Tapes")};
    if (!ffmpeg(tone(30) + QStringList{QStringLiteral("-i"), red, QStringLiteral("-map"), QStringLiteral("0"),
                                       QStringLiteral("-map"), QStringLiteral("1"), QStringLiteral("-c:v"), QStringLiteral("mjpeg"),
                                       QStringLiteral("-id3v2_version"), QStringLiteral("3"),
                                       QStringLiteral("-disposition:v"), QStringLiteral("attached_pic")} + tags + QStringList{m_mp3}))
        QSKIP("ffmpeg (with an MP3 encoder) is needed to generate the test media");
    QVERIFY(ffmpeg(tone(30) + QStringList{QStringLiteral("-i"), blue, QStringLiteral("-map"), QStringLiteral("0"),
                                          QStringLiteral("-map"), QStringLiteral("1"), QStringLiteral("-c:a"), QStringLiteral("flac"),
                                          QStringLiteral("-c:v"), QStringLiteral("png"),
                                          QStringLiteral("-disposition:v"), QStringLiteral("attached_pic"), m_flac}));
    const QByteArray picture = flacPictureBlock(green);
    QVERIFY(!picture.isEmpty());
    QVERIFY(ffmpeg(tone(30) + QStringList{QStringLiteral("-c:a"), QStringLiteral("libopus"), QStringLiteral("-metadata"),
                                          QStringLiteral("METADATA_BLOCK_PICTURE=") + QString::fromLatin1(picture.toBase64()),
                                          m_opus}));

    // A cover file next to the track, named in another letter case.
    m_folderTrack = m_dir.filePath(QStringLiteral("album/01 Track.wav"));
    QVERIFY(QDir().mkpath(m_dir.filePath(QStringLiteral("album"))));
    QVERIFY(ffmpeg(tone(30) + QStringList{m_folderTrack}));
    QImage yellow(120, 120, QImage::Format_RGB32);
    yellow.fill(Qt::yellow);
    QVERIFY(yellow.save(m_dir.filePath(QStringLiteral("album/Folder.JPG")), "JPG"));

    // No cover anywhere.
    m_plain = m_dir.filePath(QStringLiteral("plain/plain.wav"));
    QVERIFY(QDir().mkpath(m_dir.filePath(QStringLiteral("plain"))));
    QVERIFY(ffmpeg(tone(30) + QStringList{m_plain}));
    m_long = m_dir.filePath(QStringLiteral("plain/long.wav"));
    QVERIFY(ffmpeg(tone(120) + QStringList{m_long}));
    m_twoTracks = m_dir.filePath(QStringLiteral("plain/two.mka"));
    QVERIFY(ffmpeg(tone(30, 300) + tone(30, 900) + QStringList{QStringLiteral("-map"), QStringLiteral("0"), QStringLiteral("-map"),
                                                               QStringLiteral("1"), QStringLiteral("-c:a"), QStringLiteral("flac"), m_twoTracks}));

    m_video = m_dir.filePath(QStringLiteral("video.mkv"));
    QVERIFY(makeTestClip(m_video, 30));
}

void AudioTest::init()
{
    m_window = new MainWindow;
    m_window->resize(800, 450);
    m_window->show();
    m_window->activateWindow();
    QVERIFY(QTest::qWaitForWindowActive(m_window));
    m_mpv = m_window->findChild<MpvWidget *>();
    m_audio = m_window->audio();
    QVERIFY(m_mpv && m_audio);
    // CI machines have no sound card; without an output mpv would drop the audio track.
    m_mpv->setMpvProperty(QStringLiteral("ao"), QStringLiteral("null"));
    m_audio->setVisualization(AudioArtwork::Visualization::AlbumArt);
}

void AudioTest::cleanup()
{
    delete m_window;
    m_window = nullptr;
    m_mpv = nullptr;
    m_audio = nullptr;
}

void AudioTest::open(const QString &file)
{
    m_window->openFile(file);
    // Opening plays; pause right after (the commands run in order).
    m_mpv->setMpvProperty(QStringLiteral("pause"), QStringLiteral("yes"));
    QTRY_COMPARE_WITH_TIMEOUT(propString("path"), file, 10000);
    QTRY_VERIFY_WITH_TIMEOUT(prop("time-pos").isValid(), 10000);
}

QMenu *AudioTest::audioMenu() const
{
    auto *menu = m_window->findChild<PlayerMenu *>();
    for (QAction *action : menu->actions()) {
        if (action->menu() && action->text() == QLatin1String("Audio"))
            return action->menu();
    }
    return nullptr;
}

void AudioTest::setVisualization(const QString &text)
{
    auto *menu = m_window->findChild<QMenu *>(QStringLiteral("VisualizationMenu"));
    QVERIFY(menu);
    Q_EMIT menu->aboutToShow();
    for (QAction *action : menu->actions()) {
        if (action->text() == text) {
            action->trigger();
            QVERIFY(action->isChecked());
            return;
        }
    }
    QFAIL(qPrintable(QStringLiteral("no action ") + text));
}

QAction *AudioTest::audioTrackAction(const QString &prefix) const
{
    QMenu *tracks = nullptr;
    for (QAction *action : audioMenu()->actions()) {
        if (action->menu() && action->text() == QLatin1String("Audio Track"))
            tracks = action->menu();
    }
    if (!tracks)
        return nullptr;
    Q_EMIT tracks->aboutToShow(); // the menu lists the tracks as it opens
    for (QAction *action : tracks->actions()) {
        if (action->text().startsWith(prefix))
            return action;
    }
    return nullptr;
}

void AudioTest::detectsAudioFiles()
{
    open(m_video);
    QVERIFY(!m_mpv->isAudioOnly());
    QVERIFY(!m_audio->isActive());
    QVERIFY(!m_audio->view()->isVisible());

    open(m_mp3);
    QTRY_VERIFY(m_audio->isActive());
    QVERIFY(m_mpv->isAudioOnly());
    QVERIFY(m_audio->view()->isVisible());
    const AudioArtwork::TrackInfo info = m_audio->view()->trackInfo();
    QCOMPARE(info.title, QStringLiteral("Midnight Drive"));
    QCOMPARE(info.artist, QStringLiteral("Neon Coast"));
    QCOMPARE(info.album, QStringLiteral("Late Summer Tapes"));
    // The cover is drawn by the view; mpv doesn't decode it as video.
    QTRY_COMPARE(propString("current-tracks/video/id"), QString());

    // Without tags, the title is the file name.
    open(m_plain);
    QTRY_COMPARE(m_audio->view()->trackInfo().title, QStringLiteral("plain.wav"));
    QCOMPARE(m_audio->view()->trackInfo().artist, QString());

    // Stopping hides the view.
    m_mpv->stop();
    QTRY_VERIFY(!m_audio->view()->isVisible());
}

void AudioTest::embeddedArtwork_data()
{
    QTest::addColumn<QString>("file");
    QTest::addColumn<QColor>("color");
    QTest::newRow("ID3 APIC (mp3)") << m_mp3 << QColor(Qt::red);
    QTest::newRow("FLAC picture") << m_flac << QColor(Qt::blue);
    QTest::newRow("METADATA_BLOCK_PICTURE (opus)") << m_opus << QColor(Qt::green);
}

void AudioTest::embeddedArtwork()
{
    QFETCH(QString, file);
    QFETCH(QColor, color);
    open(file);
    QTRY_COMPARE_WITH_TIMEOUT(m_audio->artworkSource(), AudioController::ArtworkSource::Embedded, 10000);
    QCOMPARE(m_audio->display(), AudioController::Display::Artwork);
    const QImage art = m_audio->artwork();
    QVERIFY(near(art.pixelColor(art.rect().center()), color));
    QCOMPARE(m_audio->view()->mode(), AudioView::Mode::Artwork);
    QVERIFY(!m_audio->view()->artworkRect().isEmpty());
    // Shown centered, above the text.
    const QRect cover = m_audio->view()->artworkRect();
    QVERIFY(std::abs(cover.center().x() - m_audio->view()->width() / 2) <= 2);
    QCOMPARE(propString("lavfi-complex"), QString());
}

void AudioTest::folderCover()
{
    open(m_folderTrack);
    QTRY_COMPARE_WITH_TIMEOUT(m_audio->artworkSource(), AudioController::ArtworkSource::Folder, 10000);
    QCOMPARE(m_audio->display(), AudioController::Display::Artwork);
    const QImage art = m_audio->artwork();
    QVERIFY(near(art.pixelColor(art.rect().center()), Qt::yellow));
}

void AudioTest::fallsBackToSpectrum()
{
    const QSize windowSize = m_window->size();
    open(m_plain);
    QTRY_COMPARE_WITH_TIMEOUT(m_audio->display(), AudioController::Display::Spectrum, 10000);
    QCOMPARE(m_audio->artworkSource(), AudioController::ArtworkSource::None);
    QVERIFY(propString("lavfi-complex").startsWith(QLatin1String("[aid1]")));
    QVERIFY(propString("lavfi-complex").contains(QLatin1String("showfreqs")));
    QCOMPARE(m_audio->view()->mode(), AudioView::Mode::Visualizer);
    // mpv draws the spectrum as video, and the audio keeps playing.
    QTRY_COMPARE_WITH_TIMEOUT(prop("dwidth").toInt(), 1280, 10000);
    QTRY_COMPARE(propString("current-tracks/audio/id"), QStringLiteral("1"));
    m_mpv->play();
    const double start = prop("time-pos").toDouble();
    QTRY_VERIFY_WITH_TIMEOUT(prop("time-pos").toDouble() > start + 0.5, 5000);
    // The visualization's size is no reason to resize the window.
    QTest::qWait(200);
    QCOMPARE(m_window->size(), windowSize);
}

void AudioTest::customArtwork()
{
    open(m_plain);
    QTRY_COMPARE_WITH_TIMEOUT(m_audio->display(), AudioController::Display::Spectrum, 10000);

    QAction *setAction = nullptr;
    for (QAction *action : audioMenu()->actions()) {
        if (action->text() == QLatin1String("Set Custom Audio Artwork..."))
            setAction = action;
    }
    QVERIFY(setAction);
    acceptFileDialog(m_customImage);
    setAction->trigger();
    QTRY_COMPARE(m_audio->artworkSource(), AudioController::ArtworkSource::Custom);
    QCOMPARE(m_audio->display(), AudioController::Display::Artwork);
    QVERIFY(near(m_audio->artwork().pixelColor(10, 10), Qt::magenta));
    // Dropping the visualization keeps the audio playing.
    QTRY_COMPARE(propString("lavfi-complex"), QString());
    QTRY_COMPARE(propString("current-tracks/audio/id"), QStringLiteral("1"));

    // Remembered for the track.
    open(m_long);
    QTRY_COMPARE_WITH_TIMEOUT(m_audio->display(), AudioController::Display::Spectrum, 10000);
    open(m_plain);
    QTRY_COMPARE(m_audio->artworkSource(), AudioController::ArtworkSource::Custom);

    m_audio->clearCustomArtwork();
    QTRY_COMPARE_WITH_TIMEOUT(m_audio->display(), AudioController::Display::Spectrum, 10000);
    QCOMPARE(m_audio->artworkSource(), AudioController::ArtworkSource::None);
}

void AudioTest::customArtworkOverVisualization()
{
    // Setting a cover while a visualizer runs used to change nothing on screen.
    m_audio->setVisualization(AudioArtwork::Visualization::Waveform);
    open(m_mp3);
    QTRY_COMPARE_WITH_TIMEOUT(m_audio->display(), AudioController::Display::Waveform, 10000);
    QVERIFY(m_audio->setCustomArtwork(m_customImage));
    QCOMPARE(m_audio->display(), AudioController::Display::Artwork);
    QCOMPARE(m_audio->view()->mode(), AudioView::Mode::Artwork);
    QTRY_COMPARE(propString("lavfi-complex"), QString());
    // It is on screen.
    const QImage shot = m_audio->view()->grab().toImage();
    QVERIFY(near(shot.pixelColor(m_audio->view()->artworkRect().center()), Qt::magenta));

    // Reopened, the track shows its cover again.
    open(m_long);
    QTRY_COMPARE_WITH_TIMEOUT(m_audio->display(), AudioController::Display::Waveform, 10000);
    open(m_mp3);
    QTRY_COMPARE(m_audio->display(), AudioController::Display::Artwork);
    QCOMPARE(m_audio->artworkSource(), AudioController::ArtworkSource::Custom);

    // Picking a visualization again shows it.
    setVisualization(QStringLiteral("Frequency Spectrum"));
    QCOMPARE(m_audio->display(), AudioController::Display::Spectrum);
    m_audio->clearCustomArtwork();
}

void AudioTest::dropImageSetsArtwork()
{
    open(m_plain);
    QTRY_COMPARE_WITH_TIMEOUT(m_audio->display(), AudioController::Display::Spectrum, 10000);
    m_window->openUrls({QUrl::fromLocalFile(m_customImage)});
    QCOMPARE(m_audio->artworkSource(), AudioController::ArtworkSource::Custom);
    QCOMPARE(m_audio->display(), AudioController::Display::Artwork);
    // The song keeps playing; the image did not replace it.
    QCOMPARE(propString("path"), m_plain);
    m_audio->clearCustomArtwork();
}

void AudioTest::visualizationMenu()
{
    open(m_mp3);
    QTRY_COMPARE_WITH_TIMEOUT(m_audio->display(), AudioController::Display::Artwork, 10000);

    setVisualization(QStringLiteral("Waveform Visualizer"));
    QCOMPARE(m_audio->display(), AudioController::Display::Waveform);
    QTRY_VERIFY(propString("lavfi-complex").contains(QLatin1String("showwaves")));
    QCOMPARE(m_audio->view()->mode(), AudioView::Mode::Visualizer);

    setVisualization(QStringLiteral("Frequency Spectrum"));
    QCOMPARE(m_audio->display(), AudioController::Display::Spectrum);
    QTRY_VERIFY(propString("lavfi-complex").contains(QLatin1String("showfreqs")));

    setVisualization(QStringLiteral("Off (Minimal Canvas)"));
    QCOMPARE(m_audio->display(), AudioController::Display::Canvas);
    QCOMPARE(m_audio->view()->mode(), AudioView::Mode::Canvas);
    QTRY_COMPARE(propString("lavfi-complex"), QString());
    QTRY_COMPARE(propString("current-tracks/audio/id"), QStringLiteral("1"));

    setVisualization(QStringLiteral("Album Art Mode"));
    QCOMPARE(m_audio->display(), AudioController::Display::Artwork);

    // The choice is saved for the next start.
    setVisualization(QStringLiteral("Waveform Visualizer"));
    QCOMPARE(AudioArtwork::visualization(), AudioArtwork::Visualization::Waveform);
}

void AudioTest::seekAndVolumeWithVisualization()
{
    m_audio->setVisualization(AudioArtwork::Visualization::Waveform);
    open(m_long);
    QTRY_VERIFY(propString("lavfi-complex").contains(QLatin1String("showwaves")));
    QTRY_COMPARE_WITH_TIMEOUT(prop("dwidth").toInt(), 1280, 10000);

    m_mpv->command({QStringLiteral("seek"), QStringLiteral("30"), QStringLiteral("absolute")});
    QTRY_VERIFY_WITH_TIMEOUT(std::abs(prop("time-pos").toDouble() - 30) < 0.5, 5000);
    // The same hotkeys as for video.
    m_mpv->setFocus();
    QTRY_VERIFY(m_mpv->hasFocus());
    QTest::keyClick(m_mpv, Qt::Key_Right);
    QTRY_VERIFY_WITH_TIMEOUT(std::abs(prop("time-pos").toDouble() - 35) < 0.5, 5000);
    QTest::keyClick(m_mpv, Qt::Key_Left, Qt::ControlModifier);
    QTRY_VERIFY_WITH_TIMEOUT(std::abs(prop("time-pos").toDouble() - 5) < 0.5, 5000);

    m_mpv->setMpvProperty(QStringLiteral("volume"), QStringLiteral("50"));
    QTRY_COMPARE(prop("volume").toDouble(), 50.0);
    QTest::keyClick(m_mpv, Qt::Key_Up);
    QTRY_COMPARE(prop("volume").toDouble(), 52.0);
    QTest::keyClick(m_mpv, Qt::Key_M);
    QTRY_VERIFY(prop("mute").toBool());

    // Still the same graph, and still playing audio.
    QVERIFY(propString("lavfi-complex").contains(QLatin1String("showwaves")));
    QCOMPARE(propString("current-tracks/audio/id"), QStringLiteral("1"));
}

void AudioTest::audioTrackSwitching()
{
    m_audio->setVisualization(AudioArtwork::Visualization::Spectrum);
    open(m_twoTracks);
    QTRY_VERIFY(propString("lavfi-complex").startsWith(QLatin1String("[aid1]")));

    QAction *second = audioTrackAction(QStringLiteral("#2"));
    QVERIFY(second);
    QVERIFY(!second->isChecked());
    second->trigger();
    QTRY_COMPARE(propString("current-tracks/audio/id"), QStringLiteral("2"));
    QVERIFY(propString("lavfi-complex").startsWith(QLatin1String("[aid2]")));
    QVERIFY(audioTrackAction(QStringLiteral("#2"))->isChecked());

    // Off drops the visualization along with the audio.
    audioTrackAction(QStringLiteral("Off"))->trigger();
    QTRY_COMPARE(propString("current-tracks/audio/id"), QString());
    QTRY_COMPARE(propString("lavfi-complex"), QString());
    QCOMPARE(m_audio->view()->mode(), AudioView::Mode::Canvas);
    QVERIFY(audioTrackAction(QStringLiteral("Off"))->isChecked());

    // Turning a track back on brings it back.
    audioTrackAction(QStringLiteral("#1"))->trigger();
    QTRY_COMPARE(propString("current-tracks/audio/id"), QStringLiteral("1"));
    QTRY_VERIFY(propString("lavfi-complex").startsWith(QLatin1String("[aid1]")));
    QCOMPARE(m_audio->view()->mode(), AudioView::Mode::Visualizer);

    // Without a visualization, tracks switch through mpv's aid as for video.
    setVisualization(QStringLiteral("Off (Minimal Canvas)"));
    QTRY_COMPARE(propString("lavfi-complex"), QString());
    audioTrackAction(QStringLiteral("#2"))->trigger();
    QTRY_COMPARE(propString("current-tracks/audio/id"), QStringLiteral("2"));
    QCOMPARE(propString("aid"), QStringLiteral("2"));
}

void AudioTest::videoAfterAudio()
{
    m_audio->setVisualization(AudioArtwork::Visualization::Spectrum);
    m_window->openFiles({m_plain, m_video});
    // Opening plays; pause right after (the commands run in order).
    m_mpv->setMpvProperty(QStringLiteral("pause"), QStringLiteral("yes"));
    QTRY_VERIFY_WITH_TIMEOUT(propString("lavfi-complex").contains(QLatin1String("showfreqs")), 10000);

    // The graph and the hidden video track belonged to the audio file only.
    m_mpv->playlistNext();
    QTRY_COMPARE_WITH_TIMEOUT(propString("path"), m_video, 10000);
    QTRY_VERIFY_WITH_TIMEOUT(!m_audio->isActive(), 10000);
    QCOMPARE(propString("lavfi-complex"), QString());
    QTRY_COMPARE(propString("current-tracks/video/id"), QStringLiteral("1"));
    QVERIFY(!m_audio->view()->isVisible());
}

int main(int argc, char *argv[])
{
    // Keep the settings and custom artwork away from the user's own.
    QTemporaryDir config;
    qputenv("XDG_CONFIG_HOME", config.path().toLocal8Bit());
    QApplication app(argc, argv);
    // Reopened files play from the start instead of asking to resume (tst_resume covers that).
    ResumeManager::setMode(ResumeManager::Mode::Never);
    // libmpv requires the C numeric locale; QApplication may have changed it.
    std::setlocale(LC_NUMERIC, "C");
    QApplication::setAttribute(Qt::AA_DontUseNativeDialogs);
    AudioTest test;
    QTEST_SET_MAIN_SOURCE_PATH
    return QTest::qExec(&test, argc, argv);
}

#include "tst_audio.moc"
