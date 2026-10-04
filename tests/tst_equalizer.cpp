// Audio Control & Equalizer: the lavfi filter chain, presets, persistence,
// and the dialog applying effects to mpv's "af" while a file plays. Needs a
// display (run under xvfb-run) and ffmpeg, which generates the test clips.

#include "ResumeManager.h"
#include "AudioControlDialog.h"
#include "AudioEffects.h"
#include "MainWindow.h"
#include "MpvWidget.h"

#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QFile>
#include <QProcess>
#include <QPushButton>
#include <QSlider>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTest>

#include <clocale>

namespace {

// A clip with video and a sine tone, or only the tone.
bool makeClip(const QString &path, bool video)
{
    const QString ffmpeg = QStandardPaths::findExecutable(QStringLiteral("ffmpeg"));
    if (ffmpeg.isEmpty())
        return false;
    QStringList args{QStringLiteral("-loglevel"), QStringLiteral("error"), QStringLiteral("-y")};
    if (video)
        args << QStringLiteral("-f") << QStringLiteral("lavfi") << QStringLiteral("-i")
             << QStringLiteral("testsrc=duration=60:size=64x48:rate=5");
    args << QStringLiteral("-f") << QStringLiteral("lavfi") << QStringLiteral("-i") << QStringLiteral("sine=duration=60");
    if (video)
        args << QStringLiteral("-c:v") << QStringLiteral("mpeg4");
    args << path;
    QProcess process;
    process.start(ffmpeg, args);
    return process.waitForFinished(60000) && process.exitCode() == 0;
}

} // namespace

class EqualizerTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void initTestCase();
    void init();
    void cleanup();

    void filterChain();
    void presets();
    void persistence();
    void openWithF7();
    void bassBoostDuringVideo();
    void bassBoostDuringAudio();
    void presetsAndReset();
    void nightMode();
    void restoredOnStart();

private:
    QVariant prop(const char *name) const { return m_mpv->mpvProperty(QString::fromLatin1(name)); }
    // The lavfi graph of our filter in mpv's "af", or an empty string.
    QString activeGraph() const;
    AudioControlDialog *openDialog();
    // Plays `file` and checks that it keeps playing with the effects on.
    void playAndCheck(const QString &file);
    void createWindow();

    QTemporaryDir m_dir;
    QString m_video;
    QString m_audio;
    MainWindow *m_window = nullptr;
    MpvWidget *m_mpv = nullptr;
};

void EqualizerTest::initTestCase()
{
    QVERIFY(m_dir.isValid());
    m_video = m_dir.filePath(QStringLiteral("movie.mkv"));
    m_audio = m_dir.filePath(QStringLiteral("song.wav"));
    if (!makeClip(m_video, true) || !makeClip(m_audio, false))
        QSKIP("ffmpeg is needed to generate the test clips");
}

void EqualizerTest::createWindow()
{
    m_window = new MainWindow;
    m_window->resize(800, 500);
    m_window->show();
    QVERIFY(QTest::qWaitForWindowExposed(m_window));
    m_mpv = m_window->findChild<MpvWidget *>();
    // Without a sound card, audio would end at once.
    m_mpv->setMpvProperty(QStringLiteral("ao"), QStringLiteral("null"));
}

void EqualizerTest::init()
{
    createWindow();
}

void EqualizerTest::cleanup()
{
    delete m_window;
    m_window = nullptr;
    QFile::remove(AudioEffects::settingsFile());
}

QString EqualizerTest::activeGraph() const
{
    for (const QVariant &filter : prop("af").toList()) {
        const QVariantMap map = filter.toMap();
        if (map.value(QStringLiteral("label")).toString() == AudioEffects::filterLabel())
            return map.value(QStringLiteral("params")).toMap().value(QStringLiteral("graph")).toString();
    }
    return {};
}

AudioControlDialog *EqualizerTest::openDialog()
{
    m_window->activateWindow();
    if (!QTest::qWaitForWindowActive(m_window))
        return nullptr;
    QTest::keyClick(m_mpv, Qt::Key_F7);
    AudioControlDialog *dialog = m_window->audioControlDialog();
    return dialog && QTest::qWaitForWindowExposed(dialog) ? dialog : nullptr;
}

void EqualizerTest::playAndCheck(const QString &file)
{
    m_window->openFile(file);
    QTRY_COMPARE_WITH_TIMEOUT(prop("path").toString(), file, 10000);
    QTRY_VERIFY_WITH_TIMEOUT(prop("time-pos").toDouble() > 0.3, 10000);
    // Audio still flows through the filters, and mpv kept them (a graph that
    // fails to build is dropped from "af").
    const double before = prop("time-pos").toDouble();
    QTest::qWait(400);
    QVERIFY(prop("time-pos").toDouble() > before);
    QVERIFY(prop("audio-params/samplerate").toInt() > 0);
    QVERIFY(!activeGraph().isEmpty());
}

void EqualizerTest::filterChain()
{
    using namespace AudioEffects;
    QCOMPARE(AudioEffects::filterChain({}), QString());
    Settings settings;
    settings.bass = 6;
    QCOMPARE(AudioEffects::filterChain(settings), QStringLiteral("@potfx:lavfi=[bass=g=6:f=110:w=0.6]"));
    settings.preamp = -3;
    settings.treble = 2.5;
    settings.bands[0] = 4;
    settings.bands[9] = -12;
    settings.normalize = true;
    QCOMPARE(AudioEffects::filterChain(settings),
             QStringLiteral("@potfx:lavfi=[volume=-3dB,bass=g=6:f=110:w=0.6,treble=g=2.5:f=3000:w=0.6,"
                            "equalizer=f=31:width_type=o:w=1:g=4,equalizer=f=16000:width_type=o:w=1:g=-12,"
                            "dynaudnorm=f=200:g=15:m=10]"));
}

void EqualizerTest::presets()
{
    QStringList names;
    for (const AudioEffects::Preset &preset : AudioEffects::presets())
        names.append(preset.name);
    QCOMPARE(names, QStringList({QStringLiteral("Flat"), QStringLiteral("Bass Boost"), QStringLiteral("Club"),
                                 QStringLiteral("Rock"), QStringLiteral("Vocal Clear"), QStringLiteral("Cinema/Action")}));
    QCOMPARE(AudioEffects::matchingPreset({}), QStringLiteral("Flat"));
    AudioEffects::Settings custom;
    custom.bands[3] = 1;
    QCOMPARE(AudioEffects::matchingPreset(custom), QString());
}

void EqualizerTest::persistence()
{
    QVERIFY(AudioEffects::settingsFile().startsWith(qEnvironmentVariable("XDG_CONFIG_HOME")));
    QVERIFY(AudioEffects::settingsFile().endsWith(QLatin1String("/audio_settings.json")));
    const QString path = m_dir.filePath(QStringLiteral("audio.json"));
    AudioEffects::Settings settings;
    settings.preamp = 4;
    settings.bass = 20;
    settings.bands = {1, 2, 3, 4, 5, -1, -2, -3, -4, -5};
    settings.normalize = true;
    QVERIFY(AudioEffects::save(settings, path));
    QVERIFY(AudioEffects::load(path) == settings);
    QVERIFY(AudioEffects::load(m_dir.filePath(QStringLiteral("none.json"))) == AudioEffects::Settings());
}

void EqualizerTest::openWithF7()
{
    AudioControlDialog *dialog = openDialog();
    QVERIFY(dialog);
    QCOMPARE(dialog->findChild<QComboBox *>(QStringLiteral("AudioPreset"))->currentText(), QStringLiteral("Flat"));
    auto *bass = dialog->findChild<QSlider *>(QStringLiteral("AudioBass"));
    QCOMPARE(bass->minimum(), -15);
    QCOMPARE(bass->maximum(), 20);
    auto *preamp = dialog->findChild<QSlider *>(QStringLiteral("AudioPreamp"));
    QCOMPARE(preamp->minimum(), -10);
    QCOMPARE(preamp->maximum(), 10);
    for (int i = 0; i < AudioEffects::kBandCount; ++i) {
        auto *band = dialog->findChild<QSlider *>(QStringLiteral("AudioBand%1").arg(i));
        QVERIFY(band);
        QCOMPARE(band->orientation(), Qt::Vertical);
    }
    QVERIFY(activeGraph().isEmpty());
}

void EqualizerTest::bassBoostDuringVideo()
{
    m_window->openFile(m_video);
    QTRY_VERIFY_WITH_TIMEOUT(prop("time-pos").toDouble() > 0.2, 10000);
    AudioControlDialog *dialog = openDialog();
    QVERIFY(dialog);
    // Moving the slider updates the playing file's filters at once.
    dialog->findChild<QSlider *>(QStringLiteral("AudioBass"))->setValue(12);
    QTRY_COMPARE(activeGraph(), QStringLiteral("bass=g=12:f=110:w=0.6"));
    QCOMPARE(dialog->findChild<QComboBox *>(QStringLiteral("AudioPreset"))->currentText(), QStringLiteral("Custom"));
    playAndCheck(m_video);
    QVERIFY(prop("video-params/w").toInt() > 0);
    QVERIFY(QFile::exists(AudioEffects::settingsFile()));
}

void EqualizerTest::bassBoostDuringAudio()
{
    // Audio files run through the visualization graph; the effects apply too.
    AudioControlDialog *dialog = openDialog();
    QVERIFY(dialog);
    dialog->findChild<QSlider *>(QStringLiteral("AudioBass"))->setValue(20);
    dialog->findChild<QSlider *>(QStringLiteral("AudioPreamp"))->setValue(-6);
    QTRY_COMPARE(activeGraph(), QStringLiteral("volume=-6dB,bass=g=20:f=110:w=0.6"));
    playAndCheck(m_audio);
}

void EqualizerTest::presetsAndReset()
{
    AudioControlDialog *dialog = openDialog();
    QVERIFY(dialog);
    auto *preset = dialog->findChild<QComboBox *>(QStringLiteral("AudioPreset"));
    preset->setCurrentText(QStringLiteral("Rock"));
    Q_EMIT preset->activated(preset->currentIndex());
    QCOMPARE(dialog->findChild<QSlider *>(QStringLiteral("AudioBand0"))->value(), 4);
    QCOMPARE(dialog->findChild<QSlider *>(QStringLiteral("AudioBand3"))->value(), -3);
    QTRY_VERIFY(activeGraph().contains(QLatin1String("equalizer=f=250:width_type=o:w=1:g=-3")));
    playAndCheck(m_video);

    dialog->findChild<QPushButton *>(QStringLiteral("AudioReset"))->click();
    QCOMPARE(preset->currentText(), QStringLiteral("Flat"));
    QCOMPARE(dialog->findChild<QSlider *>(QStringLiteral("AudioBand0"))->value(), 0);
    QTRY_VERIFY(activeGraph().isEmpty());
}

void EqualizerTest::nightMode()
{
    AudioControlDialog *dialog = openDialog();
    QVERIFY(dialog);
    dialog->findChild<QCheckBox *>(QStringLiteral("AudioNightMode"))->setChecked(true);
    QTRY_COMPARE(activeGraph(), QStringLiteral("dynaudnorm=f=200:g=15:m=10"));
    playAndCheck(m_audio);
}

void EqualizerTest::restoredOnStart()
{
    AudioControlDialog *dialog = openDialog();
    QVERIFY(dialog);
    dialog->findChild<QSlider *>(QStringLiteral("AudioTreble"))->setValue(5);
    QTRY_VERIFY(!activeGraph().isEmpty());
    m_window->audioEffects()->flush();
    delete m_window;

    createWindow();
    QTRY_COMPARE(activeGraph(), QStringLiteral("treble=g=5:f=3000:w=0.6"));
    playAndCheck(m_video);
    QCOMPARE(openDialog()->findChild<QSlider *>(QStringLiteral("AudioTreble"))->value(), 5);
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
    EqualizerTest test;
    QTEST_SET_MAIN_SOURCE_PATH
    return QTest::qExec(&test, argc, argv);
}

#include "tst_equalizer.moc"
