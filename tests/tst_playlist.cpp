// The playlist manager: folder scanning and natural order, sorting, shuffle,
// cleanup, the search filter, saving and opening .m3u playlists, and the
// session restored on the next start. Drives the real window and checks the
// result on mpv's playlist. Needs a display (run under xvfb-run) and ffmpeg,
// which generates the test clips.

#include "ResumeManager.h"
#include "MainWindow.h"
#include "MediaFiles.h"
#include "MpvWidget.h"
#include "PlaylistController.h"
#include "PlaylistDrawer.h"
#include "PlaylistOps.h"
#include "PlaylistSession.h"
#include "TestClip.h"

#include <QApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileDialog>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QProcess>
#include <QRandomGenerator>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>
#include <QToolButton>

#include <algorithm>
#include <clocale>
#include <cmath>
#include <functional>
#include <numeric>

namespace {

// Runs `handle` on the next modal dialog of type T once it is open.
template <typename T>
void whenDialogOpens(std::function<void(T *)> handle)
{
    auto *timer = new QTimer;
    timer->setInterval(20);
    QObject::connect(timer, &QTimer::timeout, timer, [timer, handle] {
        if (auto *dialog = qobject_cast<T *>(QApplication::activeModalWidget())) {
            timer->deleteLater();
            timer->stop();
            handle(dialog);
        }
    });
    timer->start();
}

bool makeTestTone(const QString &path, int seconds)
{
    const QString ffmpeg = QStandardPaths::findExecutable(QStringLiteral("ffmpeg"));
    if (ffmpeg.isEmpty())
        return false;
    QProcess process;
    process.start(ffmpeg, {QStringLiteral("-loglevel"), QStringLiteral("error"), QStringLiteral("-y"),
                           QStringLiteral("-f"), QStringLiteral("lavfi"),
                           QStringLiteral("-i"), QStringLiteral("sine=duration=%1").arg(seconds), path});
    return process.waitForFinished(60000) && process.exitStatus() == QProcess::NormalExit && process.exitCode() == 0;
}

} // namespace

class PlaylistTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void initTestCase();
    void init();
    void cleanup();

    void naturalOrder();
    void folderScan();
    void movesForOrder();
    void addFolderButton();
    void sortByName();
    void sortByDuration();
    void sortByPathAndSize();
    void reverseOrder();
    void shuffle();
    void shiftOrder();
    void moveButtons();
    void playingEntryUpdatesInPlace();
    void filterKeepsPlaylist();
    void filteredMove();
    void removeMissingAndDuplicates();
    void saveAndOpenPlaylist();
    void readPlaylistEntries();
    void openPlaylistKeepsTitlesAndSkipsMissing();
    void openPlaylistFeedback();
    void folderFeedback();
    void openFolderReplacesPlayingPlaylist();
    void staleOpenIsDropped();
    void openStartsPlayback();
    void savePlaylistDialogAddsSuffix();
    void sessionRestore();
    void sessionDisabled();
    void largeFolderStaysResponsive();

private:
    QVariant prop(const char *name) const { return m_mpv->mpvProperty(QString::fromLatin1(name)); }
    QStringList mpvFiles() const;
    QStringList viewFiles() const;
    // Loads `files` paused, with entry `current` loaded.
    void load(const QStringList &files, int current = 0);
    void triggerMenuAction(const char *menuName, const QString &text);
    void createWindow();

    QTemporaryDir m_dir;
    QString m_show; // folder of test media
    QStringList m_expected; // its media files, in natural folder order
    QHash<QString, int> m_seconds;
    MainWindow *m_window = nullptr;
    MpvWidget *m_mpv = nullptr;
    PlaylistDrawer *m_drawer = nullptr;
    QListWidget *m_view = nullptr;
};

void PlaylistTest::initTestCase()
{
    QVERIFY(m_dir.isValid());
    m_show = m_dir.filePath(QStringLiteral("show"));
    QVERIFY(QDir().mkpath(m_show + QStringLiteral("/Extras")));

    // Distinct durations, so that sorting by duration differs from the name order.
    const QList<std::pair<QString, int>> clips{
        {QStringLiteral("episode 1.mkv"), 2},
        {QStringLiteral("Episode 2.mkv"), 3},
        {QStringLiteral("Episode 10.mkv"), 1},
        {QStringLiteral("track.wav"), 4},
        {QStringLiteral("Extras/Bonus 1.mkv"), 5},
    };
    for (const auto &[name, seconds] : clips) {
        const QString path = m_show + QLatin1Char('/') + name;
        const bool made = name.endsWith(QLatin1String(".wav")) ? makeTestTone(path, seconds) : makeTestClip(path, seconds);
        if (!made)
            QSKIP("ffmpeg is needed to generate the test clips");
        m_expected.append(path);
        m_seconds.insert(path, seconds);
    }
    QFile notes(m_show + QStringLiteral("/notes.txt"));
    QVERIFY(notes.open(QIODevice::WriteOnly));
}

void PlaylistTest::createWindow()
{
    m_window = new MainWindow;
    m_window->resize(900, 500);
    m_window->show();
    QVERIFY(QTest::qWaitForWindowExposed(m_window));
    m_mpv = m_window->findChild<MpvWidget *>();
    m_drawer = m_window->findChild<PlaylistDrawer *>();
    m_view = m_window->findChild<QListWidget *>(QStringLiteral("PlaylistView"));
    QVERIFY(m_mpv && m_drawer && m_view);
    // Without the slide-in animation, so the drawer's buttons can be clicked right away.
    m_drawer->setExpanded(true, false);
}

void PlaylistTest::init()
{
    createWindow();
}

void PlaylistTest::cleanup()
{
    delete m_window;
    m_window = nullptr;
    m_mpv = nullptr;
    m_drawer = nullptr;
    m_view = nullptr;
    QFile::remove(PlaylistSession::sessionFile());
}

QStringList PlaylistTest::mpvFiles() const
{
    QStringList files;
    for (const QVariant &entry : prop("playlist").toList())
        files.append(entry.toMap().value(QStringLiteral("filename")).toString());
    return files;
}

QStringList PlaylistTest::viewFiles() const
{
    QStringList files;
    for (int row = 0; row < m_view->count(); ++row)
        files.append(m_view->item(row)->toolTip());
    return files;
}

void PlaylistTest::load(const QStringList &files, int current)
{
    m_window->openFiles(files);
    // Opening plays; short clips would otherwise play through the whole
    // playlist mid-test, so pause right after (the commands run in order).
    m_mpv->setMpvProperty(QStringLiteral("pause"), QStringLiteral("yes"));
    QTRY_COMPARE_WITH_TIMEOUT(mpvFiles(), files, 10000);
    // playlist-pos moves to a new entry before the old one is unloaded;
    // playlist-playing-pos only once the new one opens.
    QTRY_VERIFY_WITH_TIMEOUT(prop("playlist-playing-pos").toInt() == 0 && prop("time-pos").isValid(), 10000);
    if (current != 0) {
        m_mpv->command({QStringLiteral("playlist-play-index"), QString::number(current)});
        QTRY_VERIFY_WITH_TIMEOUT(prop("playlist-playing-pos").toInt() == current && prop("time-pos").isValid(), 10000);
    }
    QCOMPARE(prop("path").toString(), files[current]);
    QTRY_COMPARE(viewFiles(), files);
}

void PlaylistTest::triggerMenuAction(const char *menuName, const QString &text)
{
    auto *menu = m_drawer->findChild<QMenu *>(QString::fromLatin1(menuName));
    QVERIFY2(menu, menuName);
    for (QAction *action : menu->actions()) {
        if (action->text() == text) {
            action->trigger();
            return;
        }
    }
    QFAIL(qPrintable(QStringLiteral("no action ") + text));
}

void PlaylistTest::naturalOrder()
{
    QVERIFY(MediaFiles::naturalLess(QStringLiteral("Episode 2"), QStringLiteral("Episode 10")));
    QVERIFY(!MediaFiles::naturalLess(QStringLiteral("Episode 10"), QStringLiteral("Episode 2")));
    QVERIFY(MediaFiles::naturalLess(QStringLiteral("episode 1"), QStringLiteral("Episode 2")));
    QVERIFY(MediaFiles::naturalLess(QStringLiteral("track9.mp3"), QStringLiteral("track10.mp3")));
    QVERIFY(MediaFiles::naturalLess(QStringLiteral("a"), QStringLiteral("B")));
    // Different strings never compare equal, so sorting is deterministic.
    QVERIFY(MediaFiles::naturalCompare(QStringLiteral("a"), QStringLiteral("A")) != 0);
    QCOMPARE(MediaFiles::naturalCompare(QStringLiteral("x"), QStringLiteral("x")), 0);
}

void PlaylistTest::folderScan()
{
    QCOMPARE(MediaFiles::mediaFilesInFolder(m_show), m_expected);
}

void PlaylistTest::movesForOrder()
{
    // Applying the moves with mpv's semantics yields the requested order.
    for (int round = 0; round < 50; ++round) {
        const int size = QRandomGenerator::global()->bounded(12);
        QList<int> order(size);
        std::iota(order.begin(), order.end(), 0);
        std::shuffle(order.begin(), order.end(), *QRandomGenerator::global());

        QList<int> list(size);
        std::iota(list.begin(), list.end(), 0);
        for (const auto &[from, to] : PlaylistOps::movesForOrder(order)) {
            QVERIFY(from > to); // every move goes up, so it lands exactly at `to`
            list.move(from, to);
        }
        QCOMPARE(list, order);
    }
}

void PlaylistTest::addFolderButton()
{
    bool directoryMode = false;
    whenDialogOpens<QFileDialog>([&](QFileDialog *dialog) {
        directoryMode = dialog->fileMode() == QFileDialog::Directory;
        dialog->setDirectory(m_dir.path());
        chooseInDialog(dialog, m_show);
        static_cast<QDialog *>(dialog)->accept(); // public in QDialog, protected in QFileDialog
    });
    // ADD > Add Folder..., at the bottom of the playlist as in PotPlayer.
    QVERIFY(m_drawer->findChild<QToolButton *>(QStringLiteral("PlaylistAddButton")));
    triggerMenuAction("PlaylistAddMenu", QStringLiteral("Add Folder..."));
    QVERIFY(directoryMode);
    QTRY_COMPARE_WITH_TIMEOUT(mpvFiles(), m_expected, 10000);
    QTRY_COMPARE(viewFiles(), m_expected);
    QVERIFY(m_view->item(0)->text().endsWith(QLatin1String("episode 1.mkv")));

    // Adding again appends instead of replacing.
    m_window->playlist()->addFolder(m_show + QStringLiteral("/Extras"));
    QTRY_COMPARE(mpvFiles(), m_expected + QStringList{m_expected.last()});
}

void PlaylistTest::shiftOrder()
{
    using PlaylistOps::Shift;
    QList<int> rows;
    QCOMPARE(PlaylistOps::shiftOrder(5, {2, 4}, Shift::Top, &rows), (QList<int>{2, 4, 0, 1, 3}));
    QCOMPARE(rows, (QList<int>{0, 1}));
    QCOMPARE(PlaylistOps::shiftOrder(5, {0, 2}, Shift::Bottom, &rows), (QList<int>{1, 3, 4, 0, 2}));
    QCOMPARE(rows, (QList<int>{3, 4}));
    // A row already at the top stays; the one below it still moves up.
    QCOMPARE(PlaylistOps::shiftOrder(5, {0, 3}, Shift::Up, &rows), (QList<int>{0, 1, 3, 2, 4}));
    QCOMPARE(rows, (QList<int>{0, 2}));
    QCOMPARE(PlaylistOps::shiftOrder(5, {0, 1}, Shift::Up, &rows), (QList<int>{0, 1, 2, 3, 4}));
    QCOMPARE(PlaylistOps::shiftOrder(5, {1, 2}, Shift::Down, &rows), (QList<int>{0, 3, 1, 2, 4}));
    QCOMPARE(rows, (QList<int>{2, 3}));
    QCOMPARE(PlaylistOps::shiftOrder(5, {4}, Shift::Down, &rows), (QList<int>{0, 1, 2, 3, 4}));
    QCOMPARE(rows, QList<int>{4});
}

void PlaylistTest::moveButtons()
{
    load(m_expected);
    const auto clickMove = [this](const char *name) {
        auto *button = m_drawer->findChild<QToolButton *>(QString::fromLatin1(name));
        QVERIFY2(button, name);
        QTest::mouseClick(button, Qt::LeftButton);
    };
    m_view->clearSelection();
    m_view->item(3)->setSelected(true);
    clickMove("PlaylistMoveUpButton");
    QStringList expected = m_expected;
    expected.move(3, 2);
    QTRY_COMPARE(mpvFiles(), expected);
    QTRY_COMPARE(viewFiles(), expected);
    // The moved entry stays selected, so the button can be pressed again.
    QTRY_COMPARE(m_view->selectedItems().size(), 1);
    QCOMPARE(m_view->row(m_view->selectedItems().first()), 2);
    clickMove("PlaylistMoveTopButton");
    expected.move(2, 0);
    QTRY_COMPARE(mpvFiles(), expected);
    QTRY_COMPARE(viewFiles(), expected);
    QTRY_COMPARE(m_view->row(m_view->selectedItems().value(0)), 0);
    clickMove("PlaylistMoveBottomButton");
    expected.move(0, expected.size() - 1);
    QTRY_COMPARE(mpvFiles(), expected);
    QTRY_COMPARE(viewFiles(), expected);
    clickMove("PlaylistMoveDownButton"); // already at the bottom
    QTest::qWait(200);
    QCOMPARE(mpvFiles(), expected);
}

void PlaylistTest::playingEntryUpdatesInPlace()
{
    // Only the playing entry changes: the rows are updated, not rebuilt.
    load(m_expected);
    QListWidgetItem *first = m_view->item(0);
    QVERIFY(first->font().bold());
    m_mpv->command({QStringLiteral("playlist-play-index"), QStringLiteral("2")});
    QTRY_VERIFY(m_view->item(2)->font().bold());
    QCOMPARE(m_view->item(0), first);
    QVERIFY(!first->font().bold());
    QCOMPARE(viewFiles(), m_expected);
}

void PlaylistTest::sortByName()
{
    load(m_expected, 2); // "Episode 10" plays
    triggerMenuAction("PlaylistSortMenu", QStringLiteral("By Name (Z to A)"));
    QStringList expected = m_expected;
    std::sort(expected.begin(), expected.end(), [](const QString &a, const QString &b) {
        return MediaFiles::naturalLess(QFileInfo(b).fileName(), QFileInfo(a).fileName());
    });
    QCOMPARE(QFileInfo(expected.first()).fileName(), QStringLiteral("track.wav"));
    QTRY_COMPARE(mpvFiles(), expected);
    QTRY_COMPARE(viewFiles(), expected);
    // The playing entry was moved, not restarted.
    QCOMPARE(prop("path").toString(), m_expected[2]);
    QTRY_COMPARE(prop("playlist-pos").toInt(), int(expected.indexOf(m_expected[2])));
    QVERIFY(!m_mpv->isIdle());

    triggerMenuAction("PlaylistSortMenu", QStringLiteral("By Name (A to Z)"));
    std::reverse(expected.begin(), expected.end());
    QTRY_COMPARE(mpvFiles(), expected);
}

void PlaylistTest::sortByDuration()
{
    load(m_expected);
    triggerMenuAction("PlaylistSortMenu", QStringLiteral("By Duration (Shortest First)"));
    QStringList expected = m_expected;
    std::sort(expected.begin(), expected.end(),
              [this](const QString &a, const QString &b) { return m_seconds[a] < m_seconds[b]; });
    QTRY_COMPARE_WITH_TIMEOUT(mpvFiles(), expected, 15000);

    // The drawer shows the durations next to the names.
    QTRY_VERIFY(!m_view->item(0)->data(Qt::UserRole + 1).toString().isEmpty());
    QCOMPARE(m_view->item(0)->data(Qt::UserRole + 1).toString(), QStringLiteral("0:01"));

    triggerMenuAction("PlaylistSortMenu", QStringLiteral("By Duration (Longest First)"));
    std::reverse(expected.begin(), expected.end());
    QTRY_COMPARE(mpvFiles(), expected);
}

void PlaylistTest::sortByPathAndSize()
{
    QStringList shuffled = m_expected;
    std::reverse(shuffled.begin(), shuffled.end());
    load(shuffled);

    triggerMenuAction("PlaylistSortMenu", QStringLiteral("By File Path"));
    QStringList byPath = m_expected;
    std::sort(byPath.begin(), byPath.end(), MediaFiles::naturalLess);
    QTRY_COMPARE(mpvFiles(), byPath);

    triggerMenuAction("PlaylistSortMenu", QStringLiteral("By File Size (Largest First)"));
    QStringList bySize = m_expected;
    std::stable_sort(bySize.begin(), bySize.end(),
                     [](const QString &a, const QString &b) { return QFileInfo(a).size() > QFileInfo(b).size(); });
    QTRY_COMPARE(mpvFiles(), bySize);
}

void PlaylistTest::reverseOrder()
{
    load(m_expected);
    triggerMenuAction("PlaylistSortMenu", QStringLiteral("Reverse Order"));
    QStringList expected = m_expected;
    std::reverse(expected.begin(), expected.end());
    QTRY_COMPARE(mpvFiles(), expected);
}

void PlaylistTest::shuffle()
{
    load(m_expected);
    auto *button = m_drawer->findChild<QToolButton *>(QStringLiteral("PlaylistShuffleButton"));
    QVERIFY(button);
    // A shuffle may keep the order by chance; it won't keep it every time.
    bool changed = false;
    for (int attempt = 0; attempt < 20 && !changed; ++attempt) {
        const QStringList before = mpvFiles();
        QTest::mouseClick(button, Qt::LeftButton);
        QTest::qWait(50);
        QTRY_COMPARE(viewFiles(), mpvFiles());
        changed = mpvFiles() != before;
    }
    QVERIFY(changed);
    QStringList sorted = mpvFiles();
    sorted.sort();
    QStringList expected = m_expected;
    expected.sort();
    QCOMPARE(sorted, expected);
}

void PlaylistTest::filterKeepsPlaylist()
{
    load(m_expected);
    auto *filter = m_drawer->findChild<QLineEdit *>(QStringLiteral("PlaylistFilter"));
    auto *count = m_drawer->findChild<QLabel *>(QStringLiteral("PlaylistCount"));
    QVERIFY(filter && count);

    filter->setFocus();
    QTest::keyClicks(filter, QStringLiteral("episode"));
    QCOMPARE(filter->text(), QStringLiteral("episode")); // typing doesn't trigger player hotkeys
    int visible = 0;
    for (int row = 0; row < m_view->count(); ++row)
        visible += m_view->isRowHidden(row) ? 0 : 1;
    QCOMPARE(visible, 3);
    QCOMPARE(count->text(), QStringLiteral("(3/5)"));
    QCOMPARE(mpvFiles(), m_expected);

    // Words match anywhere in the name or path, in any order.
    m_drawer->setFilterText(QStringLiteral("extras BONUS"));
    QCOMPARE(count->text(), QStringLiteral("(1/5)"));
    QVERIFY(!m_view->isRowHidden(4));

    // Select All + Del only removes what the filter shows.
    m_drawer->setFilterText(QStringLiteral("episode"));
    m_view->setFocus();
    m_view->selectAll();
    QTest::keyClick(m_view, Qt::Key_Delete);
    QTRY_COMPARE(mpvFiles(), (QStringList{m_expected[3], m_expected[4]}));
    QTRY_COMPARE(count->text(), QStringLiteral("(0/2)"));

    // The filter survives playlist changes, and Esc clears it.
    filter->setFocus();
    QTest::keyClick(filter, Qt::Key_Escape);
    QCOMPARE(filter->text(), QString());
    QCOMPARE(count->text(), QStringLiteral("(2)"));
    QVERIFY(!m_view->isRowHidden(0) && !m_view->isRowHidden(1));
}

void PlaylistTest::filteredMove()
{
    load(m_expected);
    m_drawer->setFilterText(QStringLiteral("Episode 10"));
    // Rows keep their playlist indexes while others are hidden, so moves stay in sync with mpv.
    auto *view = qobject_cast<PlaylistView *>(m_view);
    QVERIFY(view);
    Q_EMIT view->moveRequested(2, 0);
    const QStringList expected{m_expected[2], m_expected[0], m_expected[1], m_expected[3], m_expected[4]};
    QTRY_COMPARE(mpvFiles(), expected);
    QTRY_COMPARE(viewFiles(), expected);
    QVERIFY(!m_view->isRowHidden(0));
    QVERIFY(m_view->isRowHidden(1));
}

void PlaylistTest::removeMissingAndDuplicates()
{
    const QString gone = m_dir.filePath(QStringLiteral("gone.mkv"));
    QVERIFY(QFile::copy(m_expected[0], gone));
    const QString ep1 = m_expected[0];
    const QString ep2 = m_expected[1];
    load({ep1, gone, ep1, ep2, ep1}, 2);
    QVERIFY(QFile::remove(gone));

    triggerMenuAction("PlaylistMoreMenu", QStringLiteral("Remove Missing/Inaccessible Files"));
    QTRY_COMPARE(mpvFiles(), (QStringList{ep1, ep1, ep2, ep1}));

    // Of the duplicates, the playing one stays, and keeps playing.
    triggerMenuAction("PlaylistMoreMenu", QStringLiteral("Remove Duplicates"));
    QTRY_COMPARE(mpvFiles(), (QStringList{ep1, ep2}));
    QTRY_COMPARE(prop("playlist-pos").toInt(), 0);
    QVERIFY(!m_mpv->isIdle());
}

void PlaylistTest::saveAndOpenPlaylist()
{
    load(m_expected);
    // Wait for the durations, which are written as #EXTINF lengths.
    QTRY_VERIFY_WITH_TIMEOUT(!m_view->item(4)->data(Qt::UserRole + 1).toString().isEmpty(), 15000);

    const QString path = m_dir.filePath(QStringLiteral("saved.m3u8"));
    QVERIFY(m_window->playlist()->savePlaylist(path));
    QFile file(path);
    QVERIFY(file.open(QIODevice::ReadOnly | QIODevice::Text));
    const QStringList lines = QString::fromUtf8(file.readAll()).split(QLatin1Char('\n'), Qt::SkipEmptyParts);
    QCOMPARE(lines.size(), 1 + 2 * m_expected.size());
    QCOMPARE(lines[0], QStringLiteral("#EXTM3U"));
    QCOMPARE(lines[1], QStringLiteral("#EXTINF:2,episode 1"));
    QCOMPARE(lines[2], m_expected[0]);
    QCOMPARE(lines[9], QStringLiteral("#EXTINF:5,Bonus 1"));

    m_mpv->command({QStringLiteral("stop")}); // without keep-playlist: clears it
    QTRY_COMPARE(mpvFiles(), QStringList());
    m_mpv->loadPlaylist(path);
    QTRY_COMPARE_WITH_TIMEOUT(mpvFiles(), m_expected, 10000);
}

void PlaylistTest::readPlaylistEntries()
{
    const QString m3u = m_dir.filePath(QStringLiteral("titled.m3u"));
    QFile file(m3u);
    QVERIFY(file.open(QIODevice::WriteOnly));
    file.write("#EXTM3U\n"
               "# a comment\n"
               "#EXTINF:2,The First, with a comma\n"
               "show/episode 1.mkv\n"
               "\n"
               "#EXTVLCOPT:network-caching=1000\n"
               "https://radio.example/live.mp3\n"
               "#EXTINF:-1,Stream\n"
               "file://" + m_expected[1].toUtf8().replace(" ", "%20") + "\n");
    file.close();
    const QList<PlaylistOps::Entry> entries = PlaylistOps::readPlaylistEntries(m3u);
    QCOMPARE(entries.size(), 3);
    QCOMPARE(entries[0].filename, m_expected[0]); // relative to the playlist
    QCOMPARE(entries[0].title, QStringLiteral("The First, with a comma"));
    QCOMPARE(entries[1].filename, QStringLiteral("https://radio.example/live.mp3"));
    QVERIFY(entries[1].title.isEmpty()); // titles belong to the next entry only
    QCOMPARE(entries[2].filename, m_expected[1]);
    QCOMPARE(entries[2].title, QStringLiteral("Stream"));

    const QString pls = m_dir.filePath(QStringLiteral("titled.pls"));
    QFile plsFile(pls);
    QVERIFY(plsFile.open(QIODevice::WriteOnly));
    plsFile.write("[playlist]\nFile2=https://b.example/2\nTitle2=Second\nFile1=show/track.wav\nNumberOfEntries=2\n");
    plsFile.close();
    const QList<PlaylistOps::Entry> plsEntries = PlaylistOps::readPlaylistEntries(pls);
    QCOMPARE(plsEntries.size(), 2);
    QCOMPARE(plsEntries[0].filename, m_expected[3]);
    QCOMPARE(plsEntries[1].title, QStringLiteral("Second"));
    QCOMPARE(PlaylistOps::readPlaylist(pls), QStringList({m_expected[3], QStringLiteral("https://b.example/2")}));
}

void PlaylistTest::openPlaylistKeepsTitlesAndSkipsMissing()
{
    const QString path = m_dir.filePath(QStringLiteral("mixed.m3u8"));
    QFile file(path);
    QVERIFY(file.open(QIODevice::WriteOnly));
    file.write("#EXTM3U\n"
               "#EXTINF:2,Pilot\n" + m_expected[0].toUtf8() + "\n"
               "#EXTINF:3,Gone\n" + m_dir.filePath(QStringLiteral("moved away.mkv")).toUtf8() + "\n"
               "#EXTINF:3,Second\n" + m_expected[1].toUtf8() + "\n");
    file.close();

    m_mpv->setMpvProperty(QStringLiteral("pause"), QStringLiteral("yes"));
    QSignalSpy messages(m_window->playlist(), &PlaylistController::message);
    // Through the menu's dialog, like a user.
    whenDialogOpens<QFileDialog>([&](QFileDialog *dialog) {
        QVERIFY(dialog->acceptMode() == QFileDialog::AcceptOpen);
        chooseInDialog(dialog, path);
        static_cast<QDialog *>(dialog)->accept();
    });
    m_window->openPlaylistDialog();
    QTRY_COMPARE_WITH_TIMEOUT(mpvFiles(), QStringList({m_expected[0], m_expected[1]}), 10000);
    const QVariantList playlist = prop("playlist").toList();
    QCOMPARE(playlist[0].toMap().value(QStringLiteral("title")).toString(), QStringLiteral("Pilot"));
    QCOMPARE(playlist[1].toMap().value(QStringLiteral("title")).toString(), QStringLiteral("Second"));
    QTRY_VERIFY(m_view->count() == 2 && m_view->item(1)->text().contains(QLatin1String("Second")));
    QVERIFY(!messages.isEmpty());
    QCOMPARE(messages.last().at(0).toString(), QStringLiteral("Opened Playlist"));
    QCOMPARE(messages.last().at(1).toString(), QStringLiteral("2 items from mixed.m3u8 · 1 missing skipped"));
    // The temporary copy given to mpv is cleaned up.
    QTRY_VERIFY(QDir(QDir::tempPath()).entryList({QStringLiteral("top-player-*")}, QDir::Dirs).isEmpty()
                || QDir(QDir::tempPath() + QLatin1Char('/') + QDir(QDir::tempPath()).entryList({QStringLiteral("top-player-*")}, QDir::Dirs).first())
                       .entryList({QStringLiteral("*.m3u8")}).isEmpty());
}

void PlaylistTest::openPlaylistFeedback()
{
    load({m_expected[2]});
    QSignalSpy messages(m_window->playlist(), &PlaylistController::message);
    const auto lastMessage = [&messages] {
        return messages.isEmpty() ? QString() : messages.last().at(0).toString() + QLatin1Char('|') + messages.last().at(1).toString();
    };

    const QString empty = m_dir.filePath(QStringLiteral("empty.m3u"));
    QFile file(empty);
    QVERIFY(file.open(QIODevice::WriteOnly));
    file.write("#EXTM3U\n# nothing here\n");
    file.close();
    m_window->playlist()->openPlaylist(empty);
    QTRY_COMPARE(lastMessage(), QStringLiteral("Playlist is empty|empty.m3u"));

    const QString gone = m_dir.filePath(QStringLiteral("gone.m3u"));
    QFile goneFile(gone);
    QVERIFY(goneFile.open(QIODevice::WriteOnly));
    goneFile.write("/nonexistent/a.mkv\n/nonexistent/b.mkv\n");
    goneFile.close();
    m_window->playlist()->openPlaylist(gone);
    QTRY_COMPARE(lastMessage(), QStringLiteral("Playlist files not found|gone.m3u"));

    m_window->playlist()->openPlaylist(m_dir.filePath(QStringLiteral("no such list.m3u")));
    QTRY_COMPARE(lastMessage(), QStringLiteral("Could not open playlist|no such list.m3u"));

    // None of these touched the playlist.
    QCOMPARE(mpvFiles(), QStringList{m_expected[2]});
}

void PlaylistTest::folderFeedback()
{
    m_mpv->setMpvProperty(QStringLiteral("pause"), QStringLiteral("yes"));
    QSignalSpy messages(m_window->playlist(), &PlaylistController::message);
    const auto lastMessage = [&messages] {
        return messages.isEmpty() ? QString() : messages.last().at(0).toString() + QLatin1Char('|') + messages.last().at(1).toString();
    };
    m_window->playlist()->openFolder(m_show);
    QTRY_COMPARE_WITH_TIMEOUT(mpvFiles(), m_expected, 10000);
    QTRY_COMPARE(lastMessage(), QStringLiteral("Opened Folder|5 items from show"));
    m_window->playlist()->addFolder(m_show + QStringLiteral("/Extras/"));
    QTRY_COMPARE(mpvFiles().size(), 6);
    QTRY_COMPARE(lastMessage(), QStringLiteral("Added to Playlist|Added 1 item from Extras"));
    const QString empty = m_dir.filePath(QStringLiteral("empty-folder"));
    QVERIFY(QDir().mkpath(empty));
    m_window->playlist()->addFolder(empty);
    QTRY_COMPARE(lastMessage(), QStringLiteral("No media files in|empty-folder"));
    // A folder that is gone isn't queued as a file.
    m_window->playlist()->addFolder(m_dir.filePath(QStringLiteral("gone-folder")));
    QTRY_COMPARE(lastMessage(), QStringLiteral("Folder not found|gone-folder"));
    m_window->playlist()->openFolder(m_dir.filePath(QStringLiteral("gone-folder")));
    QTRY_COMPARE(lastMessage(), QStringLiteral("Folder not found|gone-folder"));
    QTest::qWait(200);
    QCOMPARE(mpvFiles().size(), 6);
}

void PlaylistTest::openFolderReplacesPlayingPlaylist()
{
    // A playlist is playing; opening a folder must replace it, not add to it.
    const QString list = m_dir.filePath(QStringLiteral("replace-me.m3u8"));
    QFile file(list);
    QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
    file.write(("#EXTM3U\n" + m_expected[3] + "\n" + m_expected[4] + "\n").toUtf8());
    file.close();
    m_window->playlist()->openPlaylist(list);
    QTRY_COMPARE_WITH_TIMEOUT(mpvFiles(), (QStringList{m_expected[3], m_expected[4]}), 10000);
    QTRY_VERIFY_WITH_TIMEOUT(prop("time-pos").isValid(), 10000);

    m_window->playlist()->openFolder(m_show + QStringLiteral("/Extras"));
    QTRY_COMPARE_WITH_TIMEOUT(mpvFiles(), QStringList{m_expected[4]}, 10000);
    m_window->playlist()->openFolder(m_show);
    QTRY_COMPARE_WITH_TIMEOUT(mpvFiles(), m_expected, 10000);
    QTRY_COMPARE_WITH_TIMEOUT(prop("path").toString(), m_expected[0], 10000);
    QTest::qWait(300);
    QCOMPARE(mpvFiles(), m_expected);
    QTRY_COMPARE(viewFiles(), m_expected);
}

void PlaylistTest::staleOpenIsDropped()
{
    // Two opens in a row: whichever result arrives last, the newest open wins.
    m_window->playlist()->openFolder(m_show);
    m_window->playlist()->openFolder(m_show + QStringLiteral("/Extras"));
    QTRY_COMPARE_WITH_TIMEOUT(mpvFiles(), QStringList{m_expected[4]}, 10000);
    QTest::qWait(500);
    QCOMPARE(mpvFiles(), QStringList{m_expected[4]});

    // An add still scanning when a new open replaces the list is dropped too.
    m_window->playlist()->addFolder(m_show);
    m_window->playlist()->openFolder(m_show + QStringLiteral("/Extras"));
    QTest::qWait(500);
    QCOMPARE(mpvFiles(), QStringList{m_expected[4]});
}

void PlaylistTest::openStartsPlayback()
{
    // A pause (or the pause keep-open leaves at the end of a file) must not
    // carry over to newly opened media.
    load(m_expected);
    QVERIFY(prop("pause").toBool());
    m_window->playlist()->openFolder(m_show + QStringLiteral("/Extras"));
    QTRY_COMPARE_WITH_TIMEOUT(mpvFiles(), QStringList{m_expected[4]}, 10000);
    QTRY_VERIFY_WITH_TIMEOUT(prop("time-pos").isValid(), 10000);
    QTRY_VERIFY(!prop("pause").toBool());

    m_mpv->setMpvProperty(QStringLiteral("pause"), QStringLiteral("yes"));
    QTRY_VERIFY(prop("pause").toBool());
    m_window->openFiles({m_expected[1]});
    QTRY_COMPARE_WITH_TIMEOUT(prop("path").toString(), m_expected[1], 10000);
    QTRY_VERIFY(!prop("pause").toBool());

    // Queueing into a stopped player starts it unpaused as well.
    m_mpv->setMpvProperty(QStringLiteral("pause"), QStringLiteral("yes"));
    m_mpv->command({QStringLiteral("playlist-clear")});
    m_mpv->command({QStringLiteral("stop")});
    QTRY_VERIFY(m_mpv->isIdle());
    m_mpv->insertFiles({m_expected[2]});
    QTRY_COMPARE_WITH_TIMEOUT(prop("path").toString(), m_expected[2], 10000);
    QTRY_VERIFY(!prop("pause").toBool());
}

void PlaylistTest::savePlaylistDialogAddsSuffix()
{
    load(m_expected);
    whenDialogOpens<QFileDialog>([&](QFileDialog *dialog) {
        QVERIFY(dialog->acceptMode() == QFileDialog::AcceptSave);
        dialog->selectNameFilter(dialog->nameFilters().value(1)); // plain .m3u
        chooseInDialog(dialog, m_dir.filePath(QStringLiteral("mylist")));
        static_cast<QDialog *>(dialog)->accept();
    });
    triggerMenuAction("PlaylistMoreMenu", QStringLiteral("Save Playlist..."));
    QTRY_VERIFY(QFileInfo::exists(m_dir.filePath(QStringLiteral("mylist.m3u"))));
}

void PlaylistTest::sessionRestore()
{
    QVERIFY(PlaylistSession::sessionFile().startsWith(qEnvironmentVariable("XDG_CONFIG_HOME")));
    QVERIFY(!m_window->startSession(true)); // nothing saved yet
    load(m_expected, 4);
    m_mpv->command({QStringLiteral("seek"), QStringLiteral("2.5"), QStringLiteral("absolute")});
    QTRY_VERIFY(std::abs(prop("time-pos").toDouble() - 2.5) < 0.3);
    m_window->close(); // saves the session
    QVERIFY(QFileInfo::exists(PlaylistSession::sessionFile()));
    const auto state = PlaylistSession::load();
    QVERIFY(state);
    QCOMPARE(state->entries.size(), m_expected.size());
    QCOMPARE(state->current, 4);
    QVERIFY(std::abs(state->position - 2.5) < 0.3);

    delete m_window;
    createWindow();
    QVERIFY(m_window->startSession(true));
    QTRY_COMPARE_WITH_TIMEOUT(mpvFiles(), m_expected, 10000);
    QTRY_COMPARE(viewFiles(), m_expected);
    // The last entry reopens paused where it was left.
    QTRY_COMPARE_WITH_TIMEOUT(prop("playlist-playing-pos").toInt(), 4, 10000);
    QTRY_VERIFY_WITH_TIMEOUT(std::abs(prop("time-pos").toDouble() - 2.5) < 0.3, 10000);
    QVERIFY(prop("pause").toBool());
    // Durations come from the saved session.
    QTRY_COMPARE(m_view->item(4)->data(Qt::UserRole + 1).toString(), QStringLiteral("0:05"));

    // The resume position only applies to that entry.
    m_mpv->command({QStringLiteral("playlist-play-index"), QStringLiteral("1")});
    QTRY_COMPARE(prop("path").toString(), m_expected[1]);
    QTRY_VERIFY(prop("time-pos").isValid());
    QVERIFY(prop("time-pos").toDouble() < 0.5);

    // Without resuming, the queue comes back stopped, and Play starts at the saved entry.
    m_window->close();
    delete m_window;
    PlaylistSession::setResumePlayback(false);
    createWindow();
    QVERIFY(m_window->startSession(true));
    QTRY_COMPARE_WITH_TIMEOUT(mpvFiles(), m_expected, 10000);
    QTest::qWait(300);
    QVERIFY(m_mpv->isIdle());
    m_mpv->play();
    QTRY_COMPARE_WITH_TIMEOUT(prop("playlist-pos").toInt(), 1, 10000);
    PlaylistSession::setResumePlayback(true);
}

void PlaylistTest::sessionDisabled()
{
    m_window->startSession(false);
    load(m_expected);
    m_window->close();
    QVERIFY(QFileInfo::exists(PlaylistSession::sessionFile()));

    // Turning "Remember Playlist" off forgets the saved queue and stops saving it.
    PlaylistSession::setRememberPlaylist(false);
    QVERIFY(!QFileInfo::exists(PlaylistSession::sessionFile()));
    m_window->close();
    QVERIFY(!QFileInfo::exists(PlaylistSession::sessionFile()));
    delete m_window;
    createWindow();
    QVERIFY(!m_window->startSession(true));
    PlaylistSession::setRememberPlaylist(true);
}

void PlaylistTest::largeFolderStaysResponsive()
{
    // Hard links to one clip make a big folder cheaply.
    constexpr int kFiles = 5000;
    const QString big = m_dir.filePath(QStringLiteral("big"));
    if (!QFileInfo::exists(big)) {
        QVERIFY(QDir().mkpath(big));
        for (int i = 0; i < kFiles; ++i) {
            const QString link = big + QStringLiteral("/clip %1.mkv").arg(i, 4, 10, QLatin1Char('0'));
            if (!QFile::link(m_expected.first(), link) && !QFile::copy(m_expected.first(), link))
                QFAIL("could not create the test folder");
        }
    }
    QSignalSpy videoSize(m_mpv, &MpvWidget::videoSizeKnown);
    load({m_expected[0], m_expected[1]});
    // Opening the clip resizes the window to the video; let that settle first.
    QTRY_VERIFY_WITH_TIMEOUT(!videoSize.isEmpty(), 10000);
    QTest::qWait(200);

    // Measure how long the event loop stalls while the folder is added.
    QElapsedTimer clock;
    qint64 lastTick = 0;
    qint64 longestStall = 0;
    QTimer ticker;
    ticker.setInterval(5);
    connect(&ticker, &QTimer::timeout, this, [&] {
        const qint64 now = clock.elapsed();
        longestStall = std::max(longestStall, now - lastTick);
        lastTick = now;
    });
    clock.start();
    ticker.start();

    m_window->playlist()->addFolder(big);
    const qint64 returned = clock.elapsed();
    QTRY_COMPARE_WITH_TIMEOUT(m_view->count(), kFiles + 2, 30000);
    QTRY_COMPARE_WITH_TIMEOUT(prop("playlist-count").toInt(), kFiles + 2, 30000);
    const qint64 total = clock.elapsed();
    ticker.stop();
    qInfo("addFolder returned after %lld ms; %d entries listed after %lld ms; longest stall %lld ms",
          returned, kFiles, total, longestStall);
    // Scanning used to block the window, and only about the first thousand
    // files reached mpv. A freeze would take seconds; this leaves room for
    // slow machines.
    QVERIFY2(returned < 50, qPrintable(QStringLiteral("addFolder blocked for %1 ms").arg(returned)));
    QVERIFY2(longestStall < 250, qPrintable(QStringLiteral("the window stalled for %1 ms").arg(longestStall)));

    QCOMPARE(prop("playlist-pos").toInt(), 0);
    QCOMPARE(mpvFiles().mid(2, 3), (QStringList{big + QStringLiteral("/clip 0000.mkv"), big + QStringLiteral("/clip 0001.mkv"),
                                                big + QStringLiteral("/clip 0002.mkv")}));
    QCOMPARE(viewFiles(), mpvFiles());

    // Thousands of playlist edits at once overflow mpv's queue of pending
    // replies; none of them may be lost.
    QStringList reversed = mpvFiles();
    std::reverse(reversed.begin(), reversed.end());
    clock.restart();
    m_window->playlist()->reverse();
    qInfo("reversing %d entries took %lld ms", kFiles + 2, clock.elapsed());
    QTRY_COMPARE_WITH_TIMEOUT(mpvFiles(), reversed, 30000);
    QTRY_COMPARE(viewFiles(), reversed);
}

int main(int argc, char *argv[])
{
    // Keep the saved session and settings away from the user's own.
    QTemporaryDir config;
    qputenv("XDG_CONFIG_HOME", config.path().toLocal8Bit());
    QApplication app(argc, argv);
    // Reopened files play from the start instead of asking to resume (tst_resume covers that).
    ResumeManager::setMode(ResumeManager::Mode::Never);
    // libmpv requires the C numeric locale; QApplication may have changed it.
    std::setlocale(LC_NUMERIC, "C");
    QApplication::setAttribute(Qt::AA_DontUseNativeDialogs);
    PlaylistTest test;
    QTEST_SET_MAIN_SOURCE_PATH
    return QTest::qExec(&test, argc, argv);
}

#include "tst_playlist.moc"
