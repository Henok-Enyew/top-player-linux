// Drives the real main window through its buttons and keyboard, and checks
// the result on mpv's properties. Needs a display (run under xvfb-run) and
// ffmpeg, which generates the test clip.

#include "ControlBar.h"
#include "Icons.h"
#include "MainWindow.h"
#include "PlaylistDrawer.h"
#include "PlaylistSession.h"
#include "ResumeManager.h"
#include "MpvWidget.h"
#ifdef TOPPLAYER_HAVE_DBUS
#include "MprisService.h"
#include <QDBusAbstractAdaptor>
#include <QDBusObjectPath>
#endif
#include "SeekBar.h"
#include "ThumbnailGenerator.h"
#include "TestClip.h"

#include <QApplication>
#include <QDialog>
#include <QFile>
#include <QKeyEvent>
#include <QLabel>
#include <QListWidget>
#include <QSettings>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <QSlider>
#include <QToolButton>
#include <QWheelEvent>
#include <QMouseEvent>

#include <clocale>
#include <cmath>

namespace {

constexpr int kEntries = 3;

QImage iconImage(const QIcon &icon)
{
    return icon.pixmap(QSize(20, 20)).toImage();
}

} // namespace

class TransportTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void initTestCase();
    void init();
    void cleanup();

    void playPauseButton();
    void stopButton();
    void previousNextButtons();
    void muteButton();
    void mediaKeys();
    void mediaKeysWithListFocus();
    void mpris();
    void seekHotkeys_data();
    void seekHotkeys();
    void volumeHotkeys();
    void pageUpPageDown();
    void playlistKeepsArrowKeys();
    void clickTogglesPause();
    void swipeSeeks();
    void horizontalWheelSeeks();
    void doubleClickTogglesFullScreen();
    void playlistInFullScreen();
    void shuffleButton();
    void repeatButton();
    void aspectButton();
    void modeButtonsFollowMenu();
    void aboutDialog();
    void thumbnailsOnlyOnHover();
    void controlsRememberState();
    void fullScreenControlsFloat();
    void miniPlayer();

private:
    QVariant prop(const char *name) const { return m_mpv->mpvProperty(QString::fromLatin1(name)); }
    void set(const char *name, const QString &value) { m_mpv->setMpvProperty(QString::fromLatin1(name), value); }
    QWidget *keyTarget() const;
    void press(int key, Qt::KeyboardModifiers modifiers = Qt::NoModifier);
    // Media keys have no text, which QTest::keyClick() refuses.
    void pressMediaKey(int key);
    void click(const char *buttonName);
    // Pauses playback at `seconds` of the current entry.
    void pauseAt(double seconds);

    QTemporaryDir m_dir;
    QString m_clip;
    MainWindow *m_window = nullptr;
    MpvWidget *m_mpv = nullptr;
};

void TransportTest::initTestCase()
{
    QVERIFY(m_dir.isValid());
    m_clip = m_dir.filePath(QStringLiteral("clip.mkv"));
    if (!makeTestClip(m_clip))
        QSKIP("ffmpeg is needed to generate the test clip");
}

void TransportTest::init()
{
    m_window = new MainWindow;
    m_window->resize(800, 450);
    m_window->show();
    m_window->activateWindow();
    QVERIFY(QTest::qWaitForWindowActive(m_window));
    m_mpv = m_window->findChild<MpvWidget *>();
    QVERIFY(m_mpv);

    m_window->openFiles(QStringList(kEntries, m_clip));
    QTRY_COMPARE_WITH_TIMEOUT(prop("playlist-count").toInt(), kEntries, 10000);
    QTRY_VERIFY_WITH_TIMEOUT(prop("time-pos").isValid(), 10000);
    QTRY_VERIFY(!m_mpv->isIdle());
    QCOMPARE(prop("playlist-pos").toInt(), 0);
}

void TransportTest::cleanup()
{
    delete m_window;
    m_window = nullptr;
    m_mpv = nullptr;
    // Each test starts from the default volume and modes, and a fresh mini player.
    QSettings settings(PlaylistSession::configDir() + QStringLiteral("/settings.ini"), QSettings::IniFormat);
    settings.remove(QStringLiteral("player"));
    settings.remove(QStringLiteral("miniPlayer"));
}

QWidget *TransportTest::keyTarget() const
{
    QWidget *focus = QApplication::focusWidget();
    return focus ? focus : m_window;
}

void TransportTest::press(int key, Qt::KeyboardModifiers modifiers)
{
    QTest::keyClick(keyTarget(), static_cast<Qt::Key>(key), modifiers);
}

void TransportTest::pressMediaKey(int key)
{
    QKeyEvent press(QEvent::KeyPress, key, Qt::NoModifier);
    QKeyEvent release(QEvent::KeyRelease, key, Qt::NoModifier);
    QApplication::sendEvent(keyTarget(), &press);
    QApplication::sendEvent(keyTarget(), &release);
}

void TransportTest::click(const char *buttonName)
{
    auto *button = m_window->findChild<QToolButton *>(QString::fromLatin1(buttonName));
    QVERIFY2(button, buttonName);
    QTest::mouseClick(button, Qt::LeftButton);
}

void TransportTest::pauseAt(double seconds)
{
    set("pause", QStringLiteral("yes"));
    m_mpv->command({QStringLiteral("seek"), QString::number(seconds), QStringLiteral("absolute")});
    QTRY_VERIFY_WITH_TIMEOUT(std::abs(prop("time-pos").toDouble() - seconds) < 0.5, 5000);
    QTRY_VERIFY(prop("pause").toBool());
}

void TransportTest::playPauseButton()
{
    auto *button = m_window->findChild<QToolButton *>(QStringLiteral("PlayButton"));
    QVERIFY(button);
    QTRY_COMPARE(iconImage(button->icon()), iconImage(skinIcon(IconType::Pause)));

    click("PlayButton");
    QTRY_VERIFY(prop("pause").toBool());
    QTRY_COMPARE(iconImage(button->icon()), iconImage(skinIcon(IconType::Play)));

    click("PlayButton");
    QTRY_VERIFY(!prop("pause").toBool());
    QTRY_COMPARE(iconImage(button->icon()), iconImage(skinIcon(IconType::Pause)));
}

void TransportTest::stopButton()
{
    pauseAt(100);
    if (QTest::currentTestFailed())
        return;
    auto *timeLabel = m_window->findChild<QLabel *>(QStringLiteral("TimeLabel"));
    QVERIFY(timeLabel);
    QTRY_VERIFY(timeLabel->text().contains(QStringLiteral("00:01:40")));

    click("StopButton");
    QTRY_VERIFY(m_mpv->isIdle());
    // The playlist survives, the seekbar and time rewind, and the video is blanked.
    QCOMPARE(prop("playlist-count").toInt(), kEntries);
    QTRY_COMPARE(timeLabel->text().count(QStringLiteral("00:00:00")), 2);
    auto *playButton = m_window->findChild<QToolButton *>(QStringLiteral("PlayButton"));
    QTRY_COMPARE(iconImage(playButton->icon()), iconImage(skinIcon(IconType::Play)));
    const QImage frame = m_mpv->grabFramebuffer();
    QCOMPARE(frame.pixelColor(frame.rect().center()).rgb(), m_mpv->palette().color(QPalette::Window).rgb());

    // Play starts the stopped entry again.
    click("PlayButton");
    QTRY_VERIFY(!m_mpv->isIdle());
    QTRY_COMPARE(prop("playlist-pos").toInt(), 0);
    QTRY_VERIFY(!prop("pause").toBool());
}

void TransportTest::previousNextButtons()
{
    click("NextButton");
    QTRY_COMPARE(prop("playlist-pos").toInt(), 1);
    click("NextButton");
    QTRY_COMPARE(prop("playlist-pos").toInt(), 2);
    click("PreviousButton");
    QTRY_COMPARE(prop("playlist-pos").toInt(), 1);

    // mpv has no current entry once stopped; the buttons step from the last one.
    click("StopButton");
    QTRY_VERIFY(m_mpv->isIdle());
    click("NextButton");
    QTRY_VERIFY(!m_mpv->isIdle());
    QTRY_COMPARE(prop("playlist-pos").toInt(), 2);
    click("StopButton");
    QTRY_VERIFY(m_mpv->isIdle());
    click("PreviousButton");
    QTRY_COMPARE(prop("playlist-pos").toInt(), 1);
}

void TransportTest::muteButton()
{
    auto *button = m_window->findChild<QToolButton *>(QStringLiteral("MuteButton"));
    QVERIFY(button);
    QVERIFY(!prop("mute").toBool());

    click("MuteButton");
    QTRY_VERIFY(prop("mute").toBool());
    QTRY_COMPARE(iconImage(button->icon()), iconImage(skinIcon(IconType::Muted)));

    press(Qt::Key_M);
    QTRY_VERIFY(!prop("mute").toBool());
    QTRY_COMPARE(iconImage(button->icon()), iconImage(skinIcon(IconType::Volume)));
}

// Like a real key press: tried as a shortcut first. (QTest::keyClick can't
// type keys without text, such as the media keys.)
static void mediaKeyClick(QWidget *widget, Qt::Key key)
{
    QTest::sendKeyEvent(QTest::Press, widget, key, QString(), Qt::NoModifier);
    QTest::sendKeyEvent(QTest::Release, widget, key, QString(), Qt::NoModifier);
}

void TransportTest::mediaKeysWithListFocus()
{
    // The media keys must work while a list (the playlist) has the keyboard.
    m_window->setPlaylistVisible(true);
    auto *list = m_window->findChild<QListWidget *>(QStringLiteral("PlaylistView"));
    QVERIFY(list);
    QTRY_VERIFY(list->isVisible());
    list->setFocus();
    QTRY_COMPARE(QApplication::focusWidget(), list);
    mediaKeyClick(list, Qt::Key_MediaNext);
    QTRY_COMPARE(prop("playlist-pos").toInt(), 1);
    mediaKeyClick(list, Qt::Key_MediaPrevious);
    QTRY_COMPARE(prop("playlist-pos").toInt(), 0);
    mediaKeyClick(list, Qt::Key_MediaTogglePlayPause);
    QTRY_VERIFY(prop("pause").toBool());
    mediaKeyClick(list, Qt::Key_MediaPlay);
    QTRY_VERIFY(!prop("pause").toBool());
    mediaKeyClick(list, Qt::Key_MediaStop);
    QTRY_VERIFY(m_mpv->isIdle());
}

void TransportTest::mpris()
{
#ifndef TOPPLAYER_HAVE_DBUS
    QSKIP("built without D-Bus");
#else
    auto *service = m_window->findChild<MprisService *>();
    QVERIFY(service);
    QDBusAbstractAdaptor *player = nullptr;
    for (auto *adaptor : service->findChildren<QDBusAbstractAdaptor *>()) {
        if (QString::fromLatin1(adaptor->metaObject()->className()) == QLatin1String("MprisPlayerAdaptor"))
            player = adaptor;
    }
    QVERIFY(player);
    const auto call = [player](const char *method) { QVERIFY(QMetaObject::invokeMethod(player, method)); };
    QTRY_COMPARE(player->property("PlaybackStatus").toString(), QStringLiteral("Playing"));
    QVERIFY(player->property("CanGoNext").toBool());
    QVERIFY(player->property("Metadata").toMap().contains(QStringLiteral("mpris:trackid")));

    call("PlayPause");
    QTRY_VERIFY(prop("pause").toBool());
    QTRY_COMPARE(player->property("PlaybackStatus").toString(), QStringLiteral("Paused"));
    call("Play");
    QTRY_VERIFY(!prop("pause").toBool());
    call("Next");
    QTRY_COMPARE(prop("playlist-pos").toInt(), 1);
    call("Previous");
    QTRY_COMPARE(prop("playlist-pos").toInt(), 0);
    QTRY_VERIFY(prop("seekable").toBool());
    QVERIFY(QMetaObject::invokeMethod(player, "Seek", Q_ARG(qlonglong, 30 * 1000000LL)));
    QTRY_VERIFY(prop("time-pos").toDouble() >= 29);
    call("Pause");
    QTRY_VERIFY(prop("pause").toBool());
    call("Stop");
    QTRY_VERIFY(m_mpv->isIdle());
    QTRY_COMPARE(player->property("PlaybackStatus").toString(), QStringLiteral("Stopped"));
#endif
}

void TransportTest::mediaKeys()
{
    pressMediaKey(Qt::Key_MediaTogglePlayPause);
    QTRY_VERIFY(prop("pause").toBool());
    pressMediaKey(Qt::Key_MediaTogglePlayPause);
    QTRY_VERIFY(!prop("pause").toBool());

    pressMediaKey(Qt::Key_MediaPause);
    QTRY_VERIFY(prop("pause").toBool());
    pressMediaKey(Qt::Key_MediaPause);
    QTest::qWait(100);
    QVERIFY(prop("pause").toBool());
    pressMediaKey(Qt::Key_MediaPlay);
    QTRY_VERIFY(!prop("pause").toBool());

    pressMediaKey(Qt::Key_MediaNext);
    QTRY_COMPARE(prop("playlist-pos").toInt(), 1);
    pressMediaKey(Qt::Key_MediaPrevious);
    QTRY_COMPARE(prop("playlist-pos").toInt(), 0);
    pressMediaKey(Qt::Key_MediaNext);
    QTRY_COMPARE(prop("playlist-pos").toInt(), 1);

    pressMediaKey(Qt::Key_MediaStop);
    QTRY_VERIFY(m_mpv->isIdle());
    QCOMPARE(prop("playlist-count").toInt(), kEntries);
    // Pause while stopped must not make the next start paused.
    pressMediaKey(Qt::Key_MediaPause);
    pressMediaKey(Qt::Key_MediaPlay);
    QTRY_VERIFY(!m_mpv->isIdle());
    QTRY_COMPARE(prop("playlist-pos").toInt(), 1);
    QTRY_VERIFY(prop("time-pos").isValid());
    QVERIFY(!prop("pause").toBool());
}

void TransportTest::seekHotkeys_data()
{
    QTest::addColumn<int>("key");
    QTest::addColumn<int>("modifiers");
    QTest::addColumn<double>("delta");

    QTest::newRow("Right") << int(Qt::Key_Right) << int(Qt::NoModifier) << 5.0;
    QTest::newRow("Left") << int(Qt::Key_Left) << int(Qt::NoModifier) << -5.0;
    QTest::newRow("Ctrl+Right") << int(Qt::Key_Right) << int(Qt::ControlModifier) << 30.0;
    QTest::newRow("Ctrl+Left") << int(Qt::Key_Left) << int(Qt::ControlModifier) << -30.0;
    QTest::newRow("Shift+Right") << int(Qt::Key_Right) << int(Qt::ShiftModifier) << 60.0;
    QTest::newRow("Shift+Left") << int(Qt::Key_Left) << int(Qt::ShiftModifier) << -60.0;
}

void TransportTest::seekHotkeys()
{
    QFETCH(int, key);
    QFETCH(int, modifiers);
    QFETCH(double, delta);

    pauseAt(200);
    if (QTest::currentTestFailed())
        return;
    press(key, Qt::KeyboardModifiers(modifiers));
    QTRY_VERIFY2_WITH_TIMEOUT(std::abs(prop("time-pos").toDouble() - (200 + delta)) < 0.5,
                              qPrintable(QString::number(prop("time-pos").toDouble())), 5000);
}

void TransportTest::volumeHotkeys()
{
    set("volume", QStringLiteral("50"));
    QTRY_COMPARE(prop("volume").toDouble(), 50.0);
    press(Qt::Key_Up);
    QTRY_COMPARE(prop("volume").toDouble(), 52.0);
    press(Qt::Key_Down);
    press(Qt::Key_Down);
    QTRY_COMPARE(prop("volume").toDouble(), 48.0);
}

void TransportTest::pageUpPageDown()
{
    // PotPlayer: PgDn plays the next file, PgUp the previous one.
    press(Qt::Key_PageDown);
    QTRY_COMPARE(prop("playlist-pos").toInt(), 1);
    press(Qt::Key_PageDown);
    QTRY_COMPARE(prop("playlist-pos").toInt(), 2);
    press(Qt::Key_PageUp);
    QTRY_COMPARE(prop("playlist-pos").toInt(), 1);
}

void TransportTest::playlistKeepsArrowKeys()
{
    set("volume", QStringLiteral("50"));
    QTRY_COMPARE(prop("volume").toDouble(), 50.0);

    m_window->setPlaylistVisible(true);
    auto *view = m_window->findChild<QListWidget *>(QStringLiteral("PlaylistView"));
    QVERIFY(view);
    QTRY_COMPARE(view->count(), kEntries);
    QTRY_VERIFY(view->isVisible());
    view->setFocus();
    QTRY_VERIFY(view->hasFocus());
    view->setCurrentRow(0);

    // Up/Down move through the list and leave the volume alone.
    press(Qt::Key_Down);
    QCOMPARE(view->currentRow(), 1);
    press(Qt::Key_Down);
    QCOMPARE(view->currentRow(), 2);
    press(Qt::Key_Up);
    QCOMPARE(view->currentRow(), 1);
    QTest::qWait(200);
    QCOMPARE(prop("volume").toDouble(), 50.0);

    // Return plays the selected entry instead of toggling fullscreen.
    press(Qt::Key_Return);
    QTRY_COMPARE(prop("playlist-pos").toInt(), 1);
    QVERIFY(!m_window->isFullScreen());

    // Player keys that a list has no use for still reach the player.
    press(Qt::Key_M);
    QTRY_VERIFY(prop("mute").toBool());
    press(Qt::Key_M);
    QTRY_VERIFY(!prop("mute").toBool());

    // Clicking the video hands the arrow keys back to the player.
    QTest::mouseClick(m_mpv, Qt::LeftButton, Qt::NoModifier, m_mpv->rect().center());
    QTRY_VERIFY(m_mpv->hasFocus());
    press(Qt::Key_Up);
    QTRY_COMPARE(prop("volume").toDouble(), 52.0);

    // So does closing the drawer while the list has focus.
    view->setFocus();
    QTRY_VERIFY(view->hasFocus());
    m_window->setPlaylistVisible(false);
    QTRY_VERIFY(!view->hasFocus());
    press(Qt::Key_Down);
    QTRY_COMPARE(prop("volume").toDouble(), 50.0);
}

void TransportTest::clickTogglesPause()
{
    QVERIFY(!prop("pause").toBool());
    QTest::mouseClick(m_mpv, Qt::LeftButton, Qt::NoModifier, m_mpv->rect().center());
    // Not at once: the click could still become a double click.
    QVERIFY(!prop("pause").toBool());
    QTRY_VERIFY(prop("pause").toBool());
    // QTest stamps events with its own clock: space the clicks so they stay single.
    QTest::mouseClick(m_mpv, Qt::LeftButton, Qt::NoModifier, m_mpv->rect().center(),
                      QApplication::doubleClickInterval() + 100);
    QTRY_VERIFY(!prop("pause").toBool());
}

void TransportTest::swipeSeeks()
{
    // Hold and drag sideways over the video: scrubs through the file.
    m_mpv->command({QStringLiteral("seek"), QStringLiteral("100"), QStringLiteral("absolute+exact")});
    QTRY_VERIFY(std::abs(prop("time-pos").toDouble() - 100) < 1);
    const QRect geometry = m_window->geometry();
    const QPoint start = m_mpv->rect().center();
    const int quarter = m_mpv->width() / 4; // a quarter of the 180 s span: 45 s
    QTest::mousePress(m_mpv, Qt::LeftButton, Qt::NoModifier, start);
    for (int step = 1; step <= 10; ++step) {
        QMouseEvent move(QEvent::MouseMove, QPointF(start + QPoint(quarter * step / 10, 2)),
                         m_mpv->mapToGlobal(QPointF(start + QPoint(quarter * step / 10, 2))),
                         Qt::NoButton, Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(m_mpv, &move);
        QTest::qWait(20);
    }
    QTest::mouseRelease(m_mpv, Qt::LeftButton, Qt::NoModifier, start + QPoint(quarter, 2));
    QTRY_VERIFY(std::abs(prop("time-pos").toDouble() - 145) < 2);
    // A seek, not a click: no pause, and the window did not move.
    QTest::qWait(QApplication::doubleClickInterval() + 100);
    QVERIFY(!prop("pause").toBool());
    QCOMPARE(m_window->geometry(), geometry);

    // Backwards, then cancelled with Esc: back where it started.
    QTest::mousePress(m_mpv, Qt::LeftButton, Qt::NoModifier, start);
    QMouseEvent back(QEvent::MouseMove, QPointF(start - QPoint(quarter, 0)), m_mpv->mapToGlobal(QPointF(start - QPoint(quarter, 0))),
                     Qt::NoButton, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(m_mpv, &back);
    QTest::keyClick(m_window, Qt::Key_Escape);
    QTest::mouseRelease(m_mpv, Qt::LeftButton, Qt::NoModifier, start - QPoint(quarter, 0));
    QTest::qWait(300);
    QTRY_VERIFY(prop("time-pos").toDouble() > 140);
}

void TransportTest::horizontalWheelSeeks()
{
    m_mpv->setMpvProperty(QStringLiteral("pause"), QStringLiteral("yes"));
    m_mpv->command({QStringLiteral("seek"), QStringLiteral("100"), QStringLiteral("absolute+exact")});
    QTRY_VERIFY(std::abs(prop("time-pos").toDouble() - 100) < 1);
    const double volume = prop("volume").toDouble();
    // Synthesized wheel events don't propagate from the video to the window
    // like real ones, so go to the window directly.
    const QPointF pos = m_mpv->mapTo(m_window, QPointF(m_mpv->rect().center()));
    const auto wheel = [&](QPoint angle) {
        QWheelEvent event(pos, m_window->mapToGlobal(pos), QPoint(), angle, Qt::NoButton, Qt::NoModifier,
                          Qt::NoScrollPhase, false);
        QApplication::sendEvent(m_window, &event);
    };
    // Scrolling right: 5 s forward per notch.
    wheel(QPoint(-240, 0));
    QTRY_VERIFY(std::abs(prop("time-pos").toDouble() - 110) < 1.5);
    // Small touchpad steps add up.
    for (int i = 0; i < 6; ++i)
        wheel(QPoint(20, 0));
    QTRY_VERIFY(std::abs(prop("time-pos").toDouble() - 105) < 1.5);
    QCOMPARE(prop("volume").toDouble(), volume);
}

void TransportTest::doubleClickTogglesFullScreen()
{
    const QRect normalGeometry = m_window->geometry();

    QTest::mouseDClick(m_mpv, Qt::LeftButton, Qt::NoModifier, m_mpv->rect().center());
    QTRY_VERIFY(m_window->isFullScreen());
    // The double click's first click must not pause.
    QTest::qWait(QApplication::doubleClickInterval() + 300);
    QVERIFY(!prop("pause").toBool());

    QTest::mouseDClick(m_mpv, Qt::LeftButton, Qt::NoModifier, m_mpv->rect().center());
    QTRY_VERIFY(!m_window->isFullScreen());
    QTRY_COMPARE(m_window->geometry(), normalGeometry);
    QTest::qWait(QApplication::doubleClickInterval() + 300);
    QVERIFY(!prop("pause").toBool());

    // Esc leaves fullscreen even while another widget has the keyboard.
    m_window->toggleFullScreen();
    QTRY_VERIFY(m_window->isFullScreen());
    auto *view = m_window->findChild<QListWidget *>(QStringLiteral("PlaylistView"));
    QVERIFY(view);
    view->setFocus();
    QTest::keyClick(view, Qt::Key_Escape);
    QTRY_VERIFY(!m_window->isFullScreen());
    QTRY_COMPARE(m_window->geometry(), normalGeometry);
}

void TransportTest::playlistInFullScreen()
{
    auto *drawer = m_window->findChild<PlaylistDrawer *>();
    auto *button = m_window->findChild<QToolButton *>(QStringLiteral("PlaylistButton"));
    QVERIFY(drawer && button);
    QVERIFY(!drawer->isExpanded());

    m_window->toggleFullScreen();
    QTRY_VERIFY(m_window->isFullScreen());
    // F6 opens the drawer in fullscreen, beside the video.
    press(Qt::Key_F6);
    QVERIFY(drawer->isExpanded());
    QTRY_VERIFY(drawer->isVisible() && drawer->width() > 100);
    QVERIFY(m_window->rect().contains(drawer->mapTo(m_window, drawer->rect().center())));
    QVERIFY(button->isChecked());
    // And closes it again, with the button in step.
    press(Qt::Key_F6);
    QVERIFY(!drawer->isExpanded());
    QVERIFY(!button->isChecked());
    QTRY_VERIFY(!drawer->isVisible());

    // The bottom bar's button works the same.
    button->show(); // the bar hides itself in fullscreen until the mouse comes near
    QTest::mouseClick(button, Qt::LeftButton);
    QVERIFY(drawer->isExpanded());
    QTRY_VERIFY(drawer->isVisible() && drawer->width() > 100);
    QVERIFY(m_window->isPlaylistVisible());

    // Leaving and entering fullscreen leaves an open drawer open.
    m_window->toggleFullScreen();
    QTRY_VERIFY(!m_window->isFullScreen());
    QVERIFY(drawer->isExpanded());
    QVERIFY(button->isChecked());
    m_window->toggleFullScreen();
    QTRY_VERIFY(m_window->isFullScreen());
    QVERIFY(drawer->isExpanded());
    QTRY_VERIFY(drawer->isVisible());
    m_window->toggleFullScreen();
    QTRY_VERIFY(!m_window->isFullScreen());
}

void TransportTest::shuffleButton()
{
    // Distinct entries, so that the order shows.
    QStringList files;
    for (int i = 0; i < 12; ++i) {
        const QString link = m_dir.filePath(QStringLiteral("clip-%1.mkv").arg(i));
        QVERIFY(QFile::exists(link) || QFile::link(m_clip, link));
        files.append(link);
    }
    m_window->openFiles(files);
    const auto order = [this] {
        QStringList result;
        for (const QVariant &entry : prop("playlist").toList())
            result.append(entry.toMap().value(QStringLiteral("filename")).toString());
        return result;
    };
    QTRY_COMPARE_WITH_TIMEOUT(order(), files, 10000);

    auto *button = m_window->findChild<QToolButton *>(QStringLiteral("ShuffleButton"));
    QVERIFY(button);
    // Opening files fits the window to the (small) clip; the mode buttons need room.
    m_window->resize(800, 450);
    QTRY_VERIFY(button->isVisible());
    QVERIFY(!button->isChecked());
    click("ShuffleButton");
    QTRY_VERIFY(prop("shuffle").toBool());
    QTRY_VERIFY(button->isChecked());
    QTRY_VERIFY(order() != files);
    QCOMPARE(prop("playlist-count").toInt(), files.size());
    // The accent-colored icon marks it on.
    const QImage on = button->icon().pixmap(QSize(20, 20), QIcon::Normal, QIcon::On).toImage();
    bool accent = false;
    for (int y = 0; y < on.height() && !accent; ++y) {
        for (int x = 0; x < on.width() && !accent; ++x)
            accent = on.pixelColor(x, y) == QColor(0x00, 0xD2, 0xFF);
    }
    QVERIFY(accent);

    // Off restores the order.
    click("ShuffleButton");
    QTRY_VERIFY(!prop("shuffle").toBool());
    QTRY_COMPARE(order(), files);
    QTRY_VERIFY(!button->isChecked());
}

void TransportTest::repeatButton()
{
    auto *button = m_window->findChild<QToolButton *>(QStringLiteral("RepeatButton"));
    auto *bar = m_window->findChild<ControlBar *>();
    QVERIFY(button && bar);
    QCOMPARE(bar->repeat(), ControlBar::Repeat::Off);
    QVERIFY(!button->isChecked());
    const QImage loopIcon = iconImage(button->icon());

    click("RepeatButton");
    QTRY_COMPARE(bar->repeat(), ControlBar::Repeat::All);
    QCOMPARE(prop("loop-playlist").toString(), QStringLiteral("inf"));
    QVERIFY(!prop("loop-file").toBool());
    QVERIFY(button->isChecked());
    QCOMPARE(button->toolTip(), QStringLiteral("Repeat: All"));

    click("RepeatButton");
    QTRY_COMPARE(bar->repeat(), ControlBar::Repeat::One);
    QCOMPARE(prop("loop-file").toString(), QStringLiteral("inf"));
    QVERIFY(!prop("loop-playlist").toBool());
    QVERIFY(button->isChecked());
    QVERIFY(iconImage(button->icon()) != loopIcon); // shows the "1"

    click("RepeatButton");
    QTRY_COMPARE(bar->repeat(), ControlBar::Repeat::Off);
    QVERIFY(!prop("loop-file").toBool());
    QVERIFY(!prop("loop-playlist").toBool());
    QVERIFY(!button->isChecked());
    QCOMPARE(iconImage(button->icon()), loopIcon);
}

void TransportTest::aspectButton()
{
    auto *bar = m_window->findChild<ControlBar *>();
    QCOMPARE(bar->aspect(), ControlBar::Aspect::Fit);
    click("AspectButton");
    QTRY_COMPARE(bar->aspect(), ControlBar::Aspect::Wide);
    QVERIFY(std::abs(prop("video-aspect-override").toDouble() - 16.0 / 9.0) < 0.01);
    QTRY_VERIFY(std::abs(prop("video-params/aspect").toDouble() - 16.0 / 9.0) < 0.01);
    click("AspectButton");
    QTRY_COMPARE(bar->aspect(), ControlBar::Aspect::Original);
    QVERIFY(prop("video-unscaled").toBool());
    QCOMPARE(prop("video-aspect-override").toDouble(), -1.0);
    click("AspectButton");
    QTRY_COMPARE(bar->aspect(), ControlBar::Aspect::Fit);
    QVERIFY(!prop("video-unscaled").toBool());
    QCOMPARE(prop("video-aspect-override").toDouble(), -1.0);
}

void TransportTest::modeButtonsFollowMenu()
{
    // Changes made elsewhere (the menu's Loop File, its Aspect Ratio) show on the buttons.
    auto *bar = m_window->findChild<ControlBar *>();
    press(Qt::Key_L, Qt::ControlModifier | Qt::ShiftModifier); // Loop File
    QTRY_COMPARE(bar->repeat(), ControlBar::Repeat::One);
    QTRY_VERIFY(m_window->findChild<QToolButton *>(QStringLiteral("RepeatButton"))->isChecked());
    set("video-aspect-override", QStringLiteral("16:9"));
    QTRY_COMPARE(bar->aspect(), ControlBar::Aspect::Wide);
    set("video-aspect-override", QStringLiteral("4:3"));
    QTRY_COMPARE(bar->aspect(), ControlBar::Aspect::Fit);

    // Narrow windows drop the mode buttons, not the essentials.
    m_window->resize(m_window->minimumWidth(), m_window->height());
    QTRY_VERIFY(!m_window->findChild<QToolButton *>(QStringLiteral("ShuffleButton"))->isVisible());
    QVERIFY(m_window->findChild<QToolButton *>(QStringLiteral("PlaylistButton"))->isVisible());
    QVERIFY(m_window->findChild<QToolButton *>(QStringLiteral("FullScreenButton"))->isVisible());
    QCOMPARE(m_window->width(), m_window->minimumWidth());
    m_window->resize(800, 450);
    QTRY_VERIFY(m_window->findChild<QToolButton *>(QStringLiteral("ShuffleButton"))->isVisible());
}

void TransportTest::aboutDialog()
{
    press(Qt::Key_F1);
    QDialog *about = nullptr;
    QTRY_VERIFY((about = m_window->findChild<QDialog *>(QStringLiteral("AboutDialog"))) && about->isVisible());
    QCOMPARE(about->windowTitle(), QStringLiteral("About Top Player"));

    auto *title = about->findChild<QLabel *>(QStringLiteral("AboutTitle"));
    QVERIFY(title);
    QCOMPARE(title->accessibleName(), QStringLiteral("Top Player — Version " APP_VERSION));
    QCOMPARE(QStringLiteral(APP_VERSION), QStringLiteral("1.0.5"));
    QVERIFY(title->text().contains(QLatin1String("Version 1.0.5")));
    auto *links = about->findChild<QLabel *>(QStringLiteral("AboutLinks"));
    QVERIFY(links);
    QVERIFY(links->openExternalLinks());
    QVERIFY(links->text().contains(QStringLiteral("https://github.com/Henok-Enyew/pot-player-linux")));
    QVERIFY(links->text().contains(QStringLiteral("https://t.me/enoch90s")));

    auto *qt = about->findChild<QLabel *>(QStringLiteral("AboutQtVersion"));
    QVERIFY(qt);
    QCOMPARE(qt->text(), QString::fromLatin1(qVersion()));
    auto *mpv = about->findChild<QLabel *>(QStringLiteral("AboutMpvVersion"));
    QVERIFY(mpv);
    QVERIFY2(mpv->text().startsWith(QStringLiteral("mpv ")), qPrintable(mpv->text()));
    auto *hwdec = about->findChild<QLabel *>(QStringLiteral("AboutHwdec"));
    QVERIFY(hwdec);
    QVERIFY(!hwdec->text().isEmpty());

    // F1 again doesn't stack a second dialog.
    m_window->showAbout();
    QCOMPARE(m_window->findChildren<QDialog *>(QStringLiteral("AboutDialog")).size(), 1);

    if (const QString dir = qEnvironmentVariable("TOPPLAYER_SCREENSHOTS"); !dir.isEmpty())
        about->grab().save(dir + QStringLiteral("/about.png"));
    about->close();
    QTRY_VERIFY(!m_window->findChild<QDialog *>(QStringLiteral("AboutDialog")));
}

void TransportTest::thumbnailsOnlyOnHover()
{
    auto *thumbnails = m_window->findChild<ThumbnailGenerator *>();
    auto *seekBar = m_window->findChild<SeekBar *>();
    QVERIFY(thumbnails && seekBar);
    // The playing file is known, but nothing is decoded for previews yet.
    QTRY_VERIFY(thumbnails->isAvailable());
    QTest::qWait(300);
    QVERIFY(!thumbnails->isOpen());

    QSignalSpy ready(thumbnails, &ThumbnailGenerator::thumbnailReady);
    QTest::mouseMove(seekBar, QPoint(seekBar->width() / 2, seekBar->height() / 2));
    QTRY_VERIFY(thumbnails->isOpen());
    QTRY_VERIFY_WITH_TIMEOUT(!ready.isEmpty(), 10000);
    QVERIFY(!ready.first().at(1).value<QImage>().isNull());

    // Stopping closes the preview decoder too.
    m_mpv->stop();
    QTRY_VERIFY(!thumbnails->isAvailable());
    QVERIFY(!thumbnails->isOpen());
}

void TransportTest::controlsRememberState()
{
    set("volume", QStringLiteral("37"));
    set("mute", QStringLiteral("yes"));
    click("ShuffleButton");
    click("RepeatButton"); // Off -> All
    QTRY_COMPARE(prop("volume").toDouble(), 37.0);
    QTRY_VERIFY(prop("shuffle").toBool());
    QTRY_COMPARE(m_window->findChild<ControlBar *>()->repeat(), ControlBar::Repeat::All);

    // Saved shortly after the changes, and set again in the next window.
    const QString file = PlaylistSession::configDir() + QStringLiteral("/settings.ini");
    QTRY_COMPARE(QSettings(file, QSettings::IniFormat).value(QStringLiteral("player/volume")).toDouble(), 37.0);
    delete m_window;
    m_window = new MainWindow;
    m_window->show();
    m_mpv = m_window->findChild<MpvWidget *>();
    auto *bar = m_window->findChild<ControlBar *>();
    QTRY_COMPARE(prop("volume").toDouble(), 37.0);
    QTRY_VERIFY(prop("mute").toBool());
    QTRY_VERIFY(prop("shuffle").toBool());
    QTRY_VERIFY(bar->isShuffle());
    QTRY_COMPARE(bar->repeat(), ControlBar::Repeat::All);
    QCOMPARE(m_window->findChild<QSlider *>(QStringLiteral("VolumeSlider"))->value(), 37);

    // Repeat One comes back as One.
    bar->setRepeat(ControlBar::Repeat::One);
    QTRY_COMPARE(bar->repeat(), ControlBar::Repeat::One);
    bar->saveState();
    QCOMPARE(QSettings(file, QSettings::IniFormat).value(QStringLiteral("player/repeat")).toString(), QStringLiteral("one"));
}

void TransportTest::fullScreenControlsFloat()
{
    auto *bar = m_window->findChild<ControlBar *>();
    QVERIFY(!m_window->areControlsOverlaid());
    m_window->toggleFullScreen();
    QTRY_VERIFY(m_window->isFullScreen());
    QVERIFY(m_window->areControlsOverlaid());
    QTRY_VERIFY(!bar->isVisible());
    // The video takes the whole screen; the bar is not in the way of the layout.
    QTRY_COMPARE(m_mpv->geometry().bottom(), m_mpv->parentWidget()->rect().bottom());
    const QRect video = m_mpv->geometry();

    // Near the bottom edge the controls appear over the video, which stays put.
    QMouseEvent move(QEvent::MouseMove, QPointF(video.width() / 2.0, video.height() - 4),
                     m_mpv->mapToGlobal(QPointF(video.width() / 2.0, video.height() - 4)), Qt::NoButton,
                     Qt::NoButton, Qt::NoModifier);
    QApplication::sendEvent(m_mpv, &move);
    QTRY_VERIFY(bar->isVisible());
    QCOMPARE(m_mpv->geometry(), video);
    QCOMPARE(bar->geometry().bottom(), video.bottom());
    QCOMPARE(bar->geometry().width(), video.width());
    QVERIFY(bar->geometry().top() > video.top());
    // A click between the floating buttons doesn't pause.
    QTest::mouseClick(bar, Qt::LeftButton, Qt::NoModifier, QPoint(bar->width() / 2, 2));
    QTest::qWait(QApplication::doubleClickInterval() + 200);
    QVERIFY(!prop("pause").toBool());

    // Back in a window, the bar sits below the video again.
    m_window->toggleFullScreen();
    QTRY_VERIFY(!m_window->isFullScreen());
    QVERIFY(!m_window->areControlsOverlaid());
    QTRY_VERIFY(bar->isVisible());
    QTRY_VERIFY(bar->geometry().top() > m_mpv->geometry().bottom());
}

void TransportTest::miniPlayer()
{
    const QSize normal = m_window->size();
    auto *titleBar = m_window->findChild<QWidget *>(QStringLiteral("TitleBar"));
    auto *restore = m_window->findChild<QToolButton *>(QStringLiteral("MiniRestoreButton"));
    QVERIFY(titleBar && restore);

    // Ctrl+M: small, on top, no title bar, the controls float.
    press(Qt::Key_M, Qt::ControlModifier);
    QVERIFY(m_window->isMiniPlayer());
    QVERIFY(m_window->windowFlags().testFlag(Qt::WindowStaysOnTopHint));
    QTRY_VERIFY(!titleBar->isVisible());
    QVERIFY(m_window->areControlsOverlaid());
    QTRY_COMPARE(m_window->width(), 400);
    QVERIFY(m_window->height() < normal.height());

    // The pointer over it brings the controls and the way back; few buttons fit.
    QMouseEvent move(QEvent::MouseMove, QPointF(20, 20), m_mpv->mapToGlobal(QPointF(20, 20)), Qt::NoButton,
                     Qt::NoButton, Qt::NoModifier);
    QApplication::sendEvent(m_mpv, &move);
    QTRY_VERIFY(restore->isVisible());
    auto *bar = m_window->findChild<ControlBar *>();
    QTRY_VERIFY(bar->isVisible());
    QVERIFY(m_window->findChild<QToolButton *>(QStringLiteral("PlayButton"))->isVisible());
    QVERIFY(!m_window->findChild<QToolButton *>(QStringLiteral("OpenButton"))->isVisible());

    // Its size can be picked; it is remembered.
    m_window->setMiniPlayerWidth(300);
    QTRY_COMPARE(m_window->width(), 300);
    const QSize mini = m_window->size();

    // Esc brings the full window back.
    QTest::keyClick(m_window, Qt::Key_Escape);
    QVERIFY(!m_window->isMiniPlayer());
    QVERIFY(!m_window->windowFlags().testFlag(Qt::WindowStaysOnTopHint));
    QTRY_VERIFY(titleBar->isVisible());
    QTRY_COMPARE(m_window->size(), normal);
    QVERIFY(!m_window->areControlsOverlaid());
    QVERIFY(!restore->isVisible());

    // Reopened at the size it had; a double click on the video opens it back up.
    m_window->setMiniPlayer(true);
    QTRY_COMPARE(m_window->size(), mini);
    QTest::mouseDClick(m_mpv, Qt::LeftButton, Qt::NoModifier, m_mpv->rect().center());
    QTRY_VERIFY(!m_window->isMiniPlayer());
    QVERIFY(!m_window->isFullScreen());
    QTRY_COMPARE(m_window->size(), normal);
}

int main(int argc, char *argv[])
{
    // Keep the settings away from the user's own.
    QTemporaryDir config;
    qputenv("XDG_CONFIG_HOME", config.path().toLocal8Bit());
    QApplication app(argc, argv);
    // The same clip is reopened after seeking: play it from the start each
    // time instead of asking to resume (tst_resume covers that).
    ResumeManager::setMode(ResumeManager::Mode::Never);
    // libmpv requires the C numeric locale; QApplication may have changed it.
    std::setlocale(LC_NUMERIC, "C");
    TransportTest test;
    QTEST_SET_MAIN_SOURCE_PATH
    return QTest::qExec(&test, argc, argv);
}

#include "tst_transport.moc"
