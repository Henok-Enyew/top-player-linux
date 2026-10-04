// The library of folders and playlists in the playlist drawer, and resizing
// and expanding the drawer. Drives the real window and checks the result on
// mpv's playlist. Needs a display (run under xvfb-run) and ffmpeg, which
// generates the test clips.

#include "ResumeManager.h"
#include "LibraryPanel.h"
#include "MainWindow.h"
#include "MediaFiles.h"
#include "MediaLibrary.h"
#include "MpvWidget.h"
#include "PlaylistController.h"
#include "PlaylistDrawer.h"
#include "PlaylistOps.h"
#include "PlaylistSession.h"
#include "TestClip.h"

#include <QApplication>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QInputDialog>
#include <QMessageBox>
#include <QSettings>
#include <QTemporaryDir>
#include <QSignalSpy>
#include <QTest>
#include <QTimer>
#include <QToolButton>

#include <clocale>
#include <functional>

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

bool writeText(const QString &path, const QString &text)
{
    QFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write(text.toUtf8()) == text.toUtf8().size();
}

} // namespace

class LibraryTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void initTestCase();
    void init();
    void cleanup();

    void readPlaylist();
    void libraryPersistence();
    void folderTree();
    void playFolder();
    void playFileContinuesFolder();
    void addFolderButton();
    void saveQueueToLibrary();
    void playPlaylistEntry();
    void deleteOwnedPlaylist();
    void dragToResize();
    void expandButton();
    void sectionsAreClickable();

private:
    QVariant prop(const char *name) const { return m_mpv->mpvProperty(QString::fromLatin1(name)); }
    QStringList mpvFiles() const;
    // Waits for mpv to play `files`, from entry `current`.
    void waitForPlaylist(const QStringList &files, int current);
    void createWindow();

    QTemporaryDir m_dir;
    QString m_show;
    QStringList m_expected; // media files of m_show, in natural folder order
    MainWindow *m_window = nullptr;
    MpvWidget *m_mpv = nullptr;
    PlaylistDrawer *m_drawer = nullptr;
    LibraryPanel *m_panel = nullptr;
    MediaLibrary *m_library = nullptr;
};

void LibraryTest::initTestCase()
{
    QVERIFY(m_dir.isValid());
    m_show = m_dir.filePath(QStringLiteral("show"));
    QVERIFY(QDir().mkpath(m_show + QStringLiteral("/Extras")));
    for (const QString &name : {QStringLiteral("Episode 1.mkv"), QStringLiteral("Episode 2.mkv"),
                                QStringLiteral("Episode 10.mkv"), QStringLiteral("Extras/Bonus.mkv")}) {
        const QString path = m_show + QLatin1Char('/') + name;
        if (!makeTestClip(path, 30))
            QSKIP("ffmpeg is needed to generate the test clips");
        m_expected.append(path);
    }
    QVERIFY(writeText(m_show + QStringLiteral("/notes.txt"), QStringLiteral("not media")));
}

void LibraryTest::createWindow()
{
    m_window = new MainWindow;
    m_window->resize(900, 500);
    m_window->show();
    QVERIFY(QTest::qWaitForWindowExposed(m_window));
    m_mpv = m_window->findChild<MpvWidget *>();
    m_drawer = m_window->findChild<PlaylistDrawer *>();
    QVERIFY(m_mpv && m_drawer);
    m_panel = m_drawer->libraryPanel();
    m_library = m_window->playlist()->library();
    QVERIFY(m_panel && m_library);
    m_drawer->setExpanded(true, false);
    m_drawer->setCurrentPage(PlaylistDrawer::LibraryPage);
    // Short clips would otherwise play through mid-test.
    m_mpv->setMpvProperty(QStringLiteral("pause"), QStringLiteral("yes"));
}

void LibraryTest::init()
{
    createWindow();
}

void LibraryTest::cleanup()
{
    delete m_window;
    m_window = nullptr;
    QFile::remove(MediaLibrary::defaultFile());
    QDir(MediaLibrary::playlistsDir()).removeRecursively();
    QFile::remove(PlaylistSession::sessionFile());
    QSettings(PlaylistSession::configDir() + QStringLiteral("/settings.ini"), QSettings::IniFormat).clear();
}

QStringList LibraryTest::mpvFiles() const
{
    QStringList files;
    for (const QVariant &entry : prop("playlist").toList())
        files.append(entry.toMap().value(QStringLiteral("filename")).toString());
    return files;
}

void LibraryTest::waitForPlaylist(const QStringList &files, int current)
{
    QTRY_COMPARE_WITH_TIMEOUT(mpvFiles(), files, 10000);
    QTRY_VERIFY_WITH_TIMEOUT(prop("playlist-playing-pos").toInt() == current && prop("time-pos").isValid(), 10000);
    QCOMPARE(prop("path").toString(), files[current]);
}

void LibraryTest::readPlaylist()
{
    const QString m3u = m_dir.filePath(QStringLiteral("list.m3u8"));
    QVERIFY(writeText(m3u, QStringLiteral("#EXTM3U\n#EXTINF:30,One\nshow/Episode 1.mkv\n\n"
                                          "file://%1\nhttps://example.com/live.m3u8\n")
                               .arg(m_expected[1])));
    QCOMPARE(PlaylistOps::readPlaylist(m3u),
             QStringList({m_expected[0], m_expected[1], QStringLiteral("https://example.com/live.m3u8")}));

    const QString pls = m_dir.filePath(QStringLiteral("list.pls"));
    QVERIFY(writeText(pls, QStringLiteral("[playlist]\nFile2=%1\nTitle1=x\nFile1=show/Episode 10.mkv\nNumberOfEntries=2\n")
                               .arg(m_expected[1])));
    QCOMPARE(PlaylistOps::readPlaylist(pls), QStringList({m_expected[2], m_expected[1]}));
    QVERIFY(PlaylistOps::readPlaylist(m_dir.filePath(QStringLiteral("missing.m3u"))).isEmpty());
}

void LibraryTest::libraryPersistence()
{
    QVERIFY(MediaLibrary::defaultFile().startsWith(qEnvironmentVariable("XDG_CONFIG_HOME")));
    const QString file = m_dir.filePath(QStringLiteral("library.json"));
    const QString playlist = m_dir.filePath(QStringLiteral("list.m3u8"));
    {
        MediaLibrary library(file);
        QVERIFY(library.add(MediaLibrary::Kind::Folder, m_show + QStringLiteral("/")));
        QVERIFY(!library.add(MediaLibrary::Kind::Folder, m_show)); // the same folder
        QVERIFY(library.add(MediaLibrary::Kind::Folder, m_show + QStringLiteral("/Extras"), QStringLiteral("Bonus")));
        QVERIFY(library.add(MediaLibrary::Kind::Playlist, playlist));
        QVERIFY(library.rename(MediaLibrary::Kind::Playlist, playlist, QStringLiteral("Favourites")));
        QVERIFY(!library.rename(MediaLibrary::Kind::Playlist, playlist, QStringLiteral("  ")));
    }
    MediaLibrary library(file);
    const QList<MediaLibrary::Item> folders = library.items(MediaLibrary::Kind::Folder);
    QCOMPARE(folders.size(), 2);
    QCOMPARE(folders[0].path, m_show);
    QCOMPARE(folders[0].name, QStringLiteral("show"));
    QCOMPARE(folders[1].name, QStringLiteral("Bonus"));
    const QList<MediaLibrary::Item> playlists = library.items(MediaLibrary::Kind::Playlist);
    QCOMPARE(playlists.size(), 1);
    QCOMPARE(playlists[0].name, QStringLiteral("Favourites"));

    // Removing a referenced playlist leaves its file alone.
    QVERIFY(writeText(playlist, QStringLiteral("#EXTM3U\n")));
    QVERIFY(library.remove(MediaLibrary::Kind::Playlist, playlist));
    QVERIFY(QFileInfo::exists(playlist));
    QVERIFY(!library.remove(MediaLibrary::Kind::Playlist, playlist));
    QCOMPARE(MediaLibrary(file).items(MediaLibrary::Kind::Playlist).size(), 0);

    // New playlist files get names of their own.
    const QString first = MediaLibrary::newPlaylistPath(QStringLiteral("a/b"));
    QCOMPARE(QFileInfo(first).fileName(), QStringLiteral("a_b.m3u8"));
    QVERIFY(MediaLibrary::ownsPlaylist(first));
    QVERIFY(QDir().mkpath(MediaLibrary::playlistsDir()));
    QVERIFY(writeText(first, QStringLiteral("#EXTM3U\n")));
    QCOMPARE(QFileInfo(MediaLibrary::newPlaylistPath(QStringLiteral("a/b"))).fileName(), QStringLiteral("a_b (2).m3u8"));
}

void LibraryTest::folderTree()
{
    QVERIFY(m_library->add(MediaLibrary::Kind::Folder, m_show));
    QTreeWidgetItem *folder = m_panel->findItem(LibraryPanel::EntryType::Folder, m_show);
    QVERIFY(folder);
    QCOMPARE(folder->text(0), QStringLiteral("show"));
    m_panel->expandItem(folder);
    // Subfolders first, then media files in natural order; no other files.
    QStringList children;
    for (int i = 0; i < folder->childCount(); ++i)
        children.append(folder->child(i)->text(0));
    QCOMPARE(children, QStringList({QStringLiteral("Extras"), QStringLiteral("Episode 1.mkv"),
                                    QStringLiteral("Episode 2.mkv"), QStringLiteral("Episode 10.mkv")}));

    // Expanded folders stay open when the library changes.
    QVERIFY(m_library->add(MediaLibrary::Kind::Folder, m_show + QStringLiteral("/Extras")));
    folder = m_panel->findItem(LibraryPanel::EntryType::Folder, m_show);
    QVERIFY(folder && folder->isExpanded());
    QCOMPARE(folder->childCount(), 4);
}

void LibraryTest::playFolder()
{
    QVERIFY(m_library->add(MediaLibrary::Kind::Folder, m_show));
    QTreeWidgetItem *folder = m_panel->findItem(LibraryPanel::EntryType::Folder, m_show);
    QVERIFY(folder);
    Q_EMIT m_panel->view()->itemActivated(folder, 0);
    waitForPlaylist(m_expected, 0);
}

void LibraryTest::playFileContinuesFolder()
{
    QVERIFY(m_library->add(MediaLibrary::Kind::Folder, m_show));
    m_panel->expandItem(m_panel->findItem(LibraryPanel::EntryType::Folder, m_show));
    QTreeWidgetItem *episode = m_panel->findItem(LibraryPanel::EntryType::File, m_expected[1]);
    QVERIFY(episode);
    Q_EMIT m_panel->view()->itemActivated(episode, 0);
    waitForPlaylist(m_expected, 1);
}

void LibraryTest::addFolderButton()
{
    whenDialogOpens<QFileDialog>([&](QFileDialog *dialog) {
        dialog->setDirectory(m_dir.path());
        chooseInDialog(dialog, m_show);
        static_cast<QDialog *>(dialog)->accept();
    });
    auto *button = m_panel->findChild<QToolButton *>(QStringLiteral("LibraryAddFolderButton"));
    QVERIFY(button);
    button->click();
    QTRY_COMPARE(m_library->items(MediaLibrary::Kind::Folder).size(), 1);
    QCOMPARE(m_library->items(MediaLibrary::Kind::Folder).first().path, m_show);
    QVERIFY(m_panel->findItem(LibraryPanel::EntryType::Folder, m_show));

    // The library is there after a restart.
    delete m_window;
    createWindow();
    QVERIFY(m_panel->findItem(LibraryPanel::EntryType::Folder, m_show));
}

void LibraryTest::saveQueueToLibrary()
{
    const QStringList queue{m_expected[2], m_expected[0]};
    m_window->openFiles(queue);
    waitForPlaylist(queue, 0);

    whenDialogOpens<QInputDialog>([](QInputDialog *dialog) {
        QCOMPARE(dialog->textValue(), QStringLiteral("show")); // the queue's folder
        dialog->setTextValue(QStringLiteral("Watch Later"));
        dialog->accept();
    });
    m_panel->findChild<QToolButton *>(QStringLiteral("LibrarySaveButton"))->click();
    QTRY_COMPARE(m_library->items(MediaLibrary::Kind::Playlist).size(), 1);
    const MediaLibrary::Item saved = m_library->items(MediaLibrary::Kind::Playlist).first();
    QCOMPARE(saved.name, QStringLiteral("Watch Later"));
    QVERIFY(MediaLibrary::ownsPlaylist(saved.path));
    QCOMPARE(PlaylistOps::readPlaylist(saved.path), queue);

    // Playing it replaces the queue.
    m_window->openFiles({m_expected[3]});
    waitForPlaylist({m_expected[3]}, 0);
    QTreeWidgetItem *item = m_panel->findItem(LibraryPanel::EntryType::Playlist, saved.path);
    QVERIFY(item);
    QCOMPARE(item->text(0), QStringLiteral("Watch Later"));
    Q_EMIT m_panel->view()->itemActivated(item, 0);
    waitForPlaylist(queue, 0);
}

void LibraryTest::playPlaylistEntry()
{
    const QString list = m_dir.filePath(QStringLiteral("entries.m3u"));
    QVERIFY(writeText(list, QStringLiteral("show/Episode 2.mkv\nshow/Episode 10.mkv\nshow/Extras/Bonus.mkv\n")));
    QVERIFY(m_library->add(MediaLibrary::Kind::Playlist, list));
    QTreeWidgetItem *playlist = m_panel->findItem(LibraryPanel::EntryType::Playlist, list);
    QVERIFY(playlist);
    m_panel->expandItem(playlist);
    QCOMPARE(playlist->childCount(), 3);
    QCOMPARE(playlist->child(1)->text(0), QStringLiteral("Episode 10.mkv"));
    Q_EMIT m_panel->view()->itemActivated(playlist->child(1), 0);
    waitForPlaylist({m_expected[1], m_expected[2], m_expected[3]}, 1);

    // Queueing appends its entries.
    m_window->playlist()->queueFromLibrary(LibraryPanel::EntryType::Playlist, list);
    QTRY_COMPARE(mpvFiles().size(), 6);
}

void LibraryTest::deleteOwnedPlaylist()
{
    m_window->openFiles({m_expected[0]});
    waitForPlaylist({m_expected[0]}, 0);
    const QString path = m_window->playlist()->saveQueueToLibrary(QStringLiteral("Mine"));
    QVERIFY(QFileInfo::exists(path));
    QTreeWidgetItem *item = m_panel->findItem(LibraryPanel::EntryType::Playlist, path);
    QVERIFY(item);
    m_panel->view()->setCurrentItem(item);

    // Declining keeps it; confirming deletes the file too.
    whenDialogOpens<QMessageBox>([](QMessageBox *box) { box->button(QMessageBox::No)->click(); });
    QTest::keyClick(m_panel->view(), Qt::Key_Delete);
    QVERIFY(QFileInfo::exists(path));
    QCOMPARE(m_library->items(MediaLibrary::Kind::Playlist).size(), 1);
    whenDialogOpens<QMessageBox>([](QMessageBox *box) { box->button(QMessageBox::Yes)->click(); });
    m_panel->findChild<QToolButton *>(QStringLiteral("LibraryRemoveButton"))->click();
    QCOMPARE(m_library->items(MediaLibrary::Kind::Playlist).size(), 0);
    QVERIFY(!QFileInfo::exists(path));
}

void LibraryTest::dragToResize()
{
    QWidget *handle = m_drawer->findChild<QWidget *>(QStringLiteral("PlaylistResizeHandle"));
    QVERIFY(handle);
    const int before = m_drawer->width();
    QCOMPARE(before, m_drawer->preferredWidth());

    // Drag the left edge 100 px to the left.
    const QPoint start(2, handle->height() / 2);
    QTest::mousePress(handle, Qt::LeftButton, {}, start);
    QMouseEvent move(QEvent::MouseMove, QPointF(start - QPoint(100, 0)), handle->mapToGlobal(QPointF(start - QPoint(100, 0))),
                     Qt::NoButton, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(handle, &move);
    QTest::mouseRelease(handle, Qt::LeftButton, {}, start - QPoint(100, 0));
    QCOMPARE(m_drawer->width(), before + 100);
    QCOMPARE(m_drawer->preferredWidth(), before + 100);

    // Remembered on the next start.
    delete m_window;
    createWindow();
    QCOMPARE(m_drawer->width(), before + 100);

    // Never narrower than the minimum, never over the whole video.
    m_drawer->setPreferredWidth(10);
    QCOMPARE(m_drawer->width(), 200);
    m_drawer->setPreferredWidth(5000);
    QCOMPARE(m_drawer->width(), m_drawer->maximumDrawerWidth());
    QVERIFY(m_mpv->width() >= 150);
}

void LibraryTest::sectionsAreClickable()
{
    QTreeWidget *view = m_panel->view();
    QTRY_VERIFY(view->isVisible());
    QTreeWidgetItem *folders = view->topLevelItem(0);
    QTreeWidgetItem *playlists = view->topLevelItem(1);
    QVERIFY(folders && playlists);
    QVERIFY(folders->flags() & Qt::ItemIsEnabled);
    QVERIFY(folders->flags() & Qt::ItemIsSelectable);
    QVERIFY(folders->isExpanded());

    // An empty section offers its action; clicking it runs it. (Without the
    // controller, which would open its modal file dialogs.)
    disconnect(m_panel, &LibraryPanel::addFolderRequested, nullptr, nullptr);
    disconnect(m_panel, &LibraryPanel::saveQueueRequested, nullptr, nullptr);
    QSignalSpy addFolder(m_panel, &LibraryPanel::addFolderRequested);
    QSignalSpy saveQueue(m_panel, &LibraryPanel::saveQueueRequested);
    QCOMPARE(folders->childCount(), 1);
    const auto clickItem = [view](QTreeWidgetItem *item) {
        view->scrollToItem(item);
        QTest::mouseClick(view->viewport(), Qt::LeftButton, Qt::NoModifier, view->visualItemRect(item).center());
    };
    clickItem(folders->child(0));
    QCOMPARE(addFolder.count(), 1);
    clickItem(playlists->child(0));
    QCOMPARE(saveQueue.count(), 1);

    // Clicking a section header folds and unfolds it, and that survives a rebuild.
    clickItem(folders);
    QVERIFY(!folders->isExpanded());
    QVERIFY(m_library->add(MediaLibrary::Kind::Folder, m_show));
    folders = view->topLevelItem(0);
    QVERIFY(!folders->isExpanded());
    QCOMPARE(folders->text(0), QStringLiteral("Folders (1)"));
    clickItem(folders);
    QVERIFY(folders->isExpanded());

    // A stored folder opens with a click and plays with a double click.
    QTreeWidgetItem *folder = m_panel->findItem(LibraryPanel::EntryType::Folder, m_show);
    QVERIFY(folder && !folder->isExpanded());
    clickItem(folder);
    QVERIFY(folder->isExpanded());
    QCOMPARE(folder->child(0)->text(0), QStringLiteral("Extras"));
}

void LibraryTest::expandButton()
{
    auto *button = m_drawer->findChild<QToolButton *>(QStringLiteral("PlaylistExpandButton"));
    QVERIFY(button);
    const int normal = m_drawer->width();
    button->click();
    QVERIFY(m_drawer->isWide());
    QTRY_COMPARE(m_drawer->width(), m_drawer->maximumDrawerWidth());
    QVERIFY(m_drawer->width() > normal);

    // Stays wide as the window grows.
    m_window->resize(1100, 500);
    QTRY_COMPARE(m_drawer->width(), m_drawer->maximumDrawerWidth());

    // Hiding and showing the drawer keeps it wide; the button restores it.
    m_drawer->setExpanded(false, false);
    m_drawer->setExpanded(true, false);
    QCOMPARE(m_drawer->width(), m_drawer->maximumDrawerWidth());
    button->click();
    QVERIFY(!m_drawer->isWide());
    QTRY_COMPARE(m_drawer->width(), normal);

    // Double-clicking the edge toggles it too.
    QWidget *handle = m_drawer->findChild<QWidget *>(QStringLiteral("PlaylistResizeHandle"));
    QTest::mouseDClick(handle, Qt::LeftButton);
    QVERIFY(m_drawer->isWide());
}

int main(int argc, char *argv[])
{
    // Keep the library and settings away from the user's own.
    QTemporaryDir config;
    qputenv("XDG_CONFIG_HOME", config.path().toLocal8Bit());
    QApplication app(argc, argv);
    // Reopened files play from the start instead of asking to resume (tst_resume covers that).
    ResumeManager::setMode(ResumeManager::Mode::Never);
    // libmpv requires the C numeric locale; QApplication may have changed it.
    std::setlocale(LC_NUMERIC, "C");
    QApplication::setAttribute(Qt::AA_DontUseNativeDialogs);
    LibraryTest test;
    QTEST_SET_MAIN_SOURCE_PATH
    return QTest::qExec(&test, argc, argv);
}

#include "tst_library.moc"
