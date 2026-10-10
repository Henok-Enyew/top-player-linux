// The start screen shown while nothing is loaded: when it shows, its layout
// at different window sizes, and its open actions and drop zone. Needs a
// display (run under xvfb-run) and ffmpeg, which generates the test clip.
//
// Set TOPPLAYER_SCREENSHOTS=<dir> to save a screenshot of each layout case.

#include "EmptyStateWidget.h"
#include "MainWindow.h"
#include "MediaFiles.h"
#include "MpvWidget.h"
#include "TestClip.h"
#include "Theme.h"

#include <QApplication>
#include <QDir>
#include <QDragEnterEvent>
#include <QFile>
#include <QFileDialog>
#include <QInputDialog>
#include <QLabel>
#include <QMimeData>
#include <QPushButton>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>

#include <clocale>
#include <functional>

namespace {

const char *const kButtons[] = {"OpenFileButton", "OpenFolderButton", "OpenUrlButton", "OpenPlaylistButton"};

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

} // namespace

class EmptyStateTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void initTestCase();
    void init();
    void cleanup();

    void shownOnStartup();
    void layout_data();
    void layout();
    void hiddenWhilePlaying();
    void mediaFilesInFolder();
    void openFileButton();
    void openFolderButton();
    void openUrlButton();
    void openPlaylistButton();
    void dropOnEmptyState();

private:
    QVariant prop(const char *name) const { return m_mpv->mpvProperty(QString::fromLatin1(name)); }
    QPushButton *button(const char *name) const;
    QStringList playlistFiles() const;
    // A folder holding "Episode 2", "Episode 10", a text file and a subfolder.
    QString makeFolder();

    QTemporaryDir m_dir;
    QString m_clip;
    MainWindow *m_window = nullptr;
    MpvWidget *m_mpv = nullptr;
    EmptyStateWidget *m_empty = nullptr;
};

void EmptyStateTest::initTestCase()
{
    QVERIFY(m_dir.isValid());
    m_clip = m_dir.filePath(QStringLiteral("clip.mkv"));
    if (!makeTestClip(m_clip))
        QSKIP("ffmpeg is needed to generate the test clip");
}

void EmptyStateTest::init()
{
    m_window = new MainWindow;
    m_window->resize(960, 540);
    m_window->show();
    m_window->activateWindow();
    QVERIFY(QTest::qWaitForWindowExposed(m_window));
    m_mpv = m_window->findChild<MpvWidget *>();
    m_empty = m_window->findChild<EmptyStateWidget *>();
    QVERIFY(m_mpv);
    QVERIFY(m_empty);
}

void EmptyStateTest::cleanup()
{
    // Leave no dialog behind if a test failed while one was open.
    while (QWidget *modal = QApplication::activeModalWidget())
        modal->close();
    delete m_window;
    m_window = nullptr;
}

QPushButton *EmptyStateTest::button(const char *name) const
{
    return m_empty->findChild<QPushButton *>(QString::fromLatin1(name));
}

QStringList EmptyStateTest::playlistFiles() const
{
    QStringList files;
    for (const QVariant &entry : prop("playlist").toList())
        files.append(entry.toMap().value(QStringLiteral("filename")).toString());
    return files;
}

QString EmptyStateTest::makeFolder()
{
    const QString folder = m_dir.filePath(QStringLiteral("Show"));
    QDir(folder).removeRecursively();
    QDir().mkpath(folder + QStringLiteral("/Season 2"));
    for (const QString &name : {QStringLiteral("Episode 10.mkv"), QStringLiteral("Episode 2.mkv"),
                                QStringLiteral("Season 2/Episode 1.mkv")})
        QFile::copy(m_clip, folder + QLatin1Char('/') + name);
    QFile notes(folder + QStringLiteral("/notes.txt"));
    notes.open(QIODevice::WriteOnly);
    return folder;
}

void EmptyStateTest::shownOnStartup()
{
    QVERIFY(m_empty->isVisible());
    QCOMPARE(m_empty->geometry(), m_mpv->rect());
    auto *title = m_empty->findChild<QLabel *>(QStringLiteral("EmptyStateTitle"));
    QVERIFY(title);
    QCOMPARE(title->accessibleName(), QStringLiteral("Top Player"));
    auto *hint = m_empty->findChild<QLabel *>(QStringLiteral("EmptyStateHint"));
    QVERIFY(hint && hint->isVisible());
    QCOMPARE(hint->text(), QStringLiteral("or drag and drop files and folders here"));
    for (const char *name : kButtons) {
        QVERIFY2(button(name), name);
        QVERIFY2(button(name)->isVisible(), name);
    }
}

void EmptyStateTest::layout_data()
{
    QTest::addColumn<QSize>("windowSize");
    QTest::addColumn<bool>("playlist");
    QTest::addColumn<bool>("logo");

    QTest::newRow("default") << QSize(960, 540) << false << true;
    QTest::newRow("large") << QSize(1500, 900) << false << true;
    QTest::newRow("minimum") << QSize(420, 260) << false << false;
    QTest::newRow("short-wide") << QSize(900, 260) << false << false;
    QTest::newRow("narrow-with-playlist") << QSize(520, 600) << true << true;
    QTest::newRow("minimum-with-playlist") << QSize(420, 260) << true << false;
}

void EmptyStateTest::layout()
{
    QFETCH(QSize, windowSize);
    QFETCH(bool, playlist);
    QFETCH(bool, logo);

    m_window->setPlaylistVisible(playlist);
    m_window->resize(windowSize);
    // Let the window and the drawer animation settle.
    QTest::qWait(400);
    QTRY_COMPARE(m_empty->geometry(), m_mpv->rect());

    // Every action stays reachable inside the video area.
    const QRect area = m_empty->rect();
    for (const char *name : kButtons) {
        QPushButton *b = button(name);
        QVERIFY2(b->isVisible(), name);
        const QRect rect(b->mapTo(m_empty, QPoint()), b->size());
        QVERIFY2(area.contains(rect), qPrintable(QStringLiteral("%1 at %2,%3 %4x%5 outside %6x%7")
            .arg(QLatin1String(name)).arg(rect.x()).arg(rect.y()).arg(rect.width()).arg(rect.height())
            .arg(area.width()).arg(area.height())));
    }
    // The content is centered.
    auto *content = m_empty->findChild<QWidget *>(QStringLiteral("EmptyStateContent"));
    QVERIFY(content);
    QVERIFY(qAbs(content->geometry().center().x() - area.center().x()) <= 1);
    QVERIFY(qAbs(content->geometry().center().y() - area.center().y()) <= 1);
    auto *logoLabel = m_empty->findChild<QLabel *>(QStringLiteral("EmptyStateLogo"));
    QCOMPARE(logoLabel->isVisible(), logo);

    const QString screenshots = qEnvironmentVariable("TOPPLAYER_SCREENSHOTS");
    if (!screenshots.isEmpty()) {
        QDir().mkpath(screenshots);
        m_window->grab().save(QStringLiteral("%1/empty-state-%2.png").arg(screenshots, QLatin1String(QTest::currentDataTag())));
    }
}

void EmptyStateTest::hiddenWhilePlaying()
{
    m_window->openFiles({m_clip});
    QTRY_VERIFY_WITH_TIMEOUT(!m_empty->isVisible(), 10000);
    QTRY_VERIFY(prop("time-pos").isValid());

    m_mpv->stop();
    QTRY_VERIFY(m_empty->isVisible());
    QTRY_VERIFY(m_empty->isActive());

    // Back again when playback restarts, and when the playlist is emptied.
    m_mpv->play();
    QTRY_VERIFY(!m_empty->isVisible());
    m_mpv->command({QStringLiteral("playlist-clear")});
    m_mpv->command({QStringLiteral("playlist-remove"), QStringLiteral("current")});
    QTRY_VERIFY(m_empty->isVisible());
}

void EmptyStateTest::mediaFilesInFolder()
{
    const QString folder = makeFolder();
    QCOMPARE(MediaFiles::mediaFilesInFolder(folder),
             QStringList({folder + QStringLiteral("/Episode 2.mkv"), folder + QStringLiteral("/Episode 10.mkv"),
                          folder + QStringLiteral("/Season 2/Episode 1.mkv")}));
    QVERIFY(MediaFiles::isMediaFile(QStringLiteral("a.MKV")));
    QVERIFY(MediaFiles::isMediaFile(QStringLiteral("a.flac")));
    QVERIFY(!MediaFiles::isMediaFile(QStringLiteral("a.srt")));
    QVERIFY(MediaFiles::isPlaylistFile(QStringLiteral("a.m3u8")));
    // The common formats, and the file dialog's patterns in both cases
    // (GTK's file chooser matches them case-sensitively).
    for (const char *name : {"a.mkv", "a.mp4", "a.webm", "a.avi", "a.mov", "a.m2ts", "a.mts", "a.wmv", "a.flv",
                             "a.mpg", "a.vob", "a.ogv", "a.3gp", "a.mxf", "a.hevc", "a.mp3", "a.m4a", "a.opus",
                             "a.wav", "a.ogg", "a.aac", "a.m4b"})
        QVERIFY2(MediaFiles::isMediaFile(QString::fromLatin1(name)), name);
    const QString filter = MediaFiles::mediaFileFilter();
    QVERIFY(filter.contains(QStringLiteral("*.mkv")));
    QVERIFY(filter.contains(QStringLiteral("*.MKV")));
    QVERIFY(filter.contains(QStringLiteral("*.MP4")));
}

void EmptyStateTest::openFileButton()
{
    QStringList filters;
    whenDialogOpens<QFileDialog>([&](QFileDialog *dialog) {
        filters = dialog->nameFilters();
        chooseInDialog(dialog, m_clip);
        static_cast<QDialog *>(dialog)->accept(); // public in QDialog, protected in QFileDialog
    });
    QTest::mouseClick(button("OpenFileButton"), Qt::LeftButton);
    QVERIFY(!filters.isEmpty());
    QVERIFY(filters.first().startsWith(QStringLiteral("Media Files")));
    QVERIFY(filters.first().contains(QStringLiteral("*.mkv")));
    QVERIFY(filters.first().contains(QStringLiteral("*.mp3")));
    QTRY_COMPARE_WITH_TIMEOUT(playlistFiles(), QStringList{m_clip}, 10000);
    QTRY_VERIFY(!m_empty->isVisible());
}

void EmptyStateTest::openFolderButton()
{
    const QString folder = makeFolder();
    bool directoryMode = false;
    whenDialogOpens<QFileDialog>([&](QFileDialog *dialog) {
        directoryMode = dialog->fileMode() == QFileDialog::Directory;
        dialog->setDirectory(m_dir.path());
        chooseInDialog(dialog, folder);
        static_cast<QDialog *>(dialog)->accept();
    });
    QTest::mouseClick(button("OpenFolderButton"), Qt::LeftButton);
    QVERIFY(directoryMode);
    QTRY_COMPARE_WITH_TIMEOUT(playlistFiles(), MediaFiles::mediaFilesInFolder(folder), 10000);
    QCOMPARE(playlistFiles().size(), 3);
    QTRY_VERIFY(!m_empty->isVisible());
}

void EmptyStateTest::openUrlButton()
{
    const QString url = QUrl::fromLocalFile(m_clip).toString();
    whenDialogOpens<QInputDialog>([&](QInputDialog *dialog) {
        dialog->setTextValue(url);
        dialog->accept();
    });
    QTest::mouseClick(button("OpenUrlButton"), Qt::LeftButton);
    QTRY_COMPARE_WITH_TIMEOUT(playlistFiles(), QStringList{m_clip}, 10000);
    QTRY_VERIFY(!m_empty->isVisible());
}

void EmptyStateTest::openPlaylistButton()
{
    const QString playlist = m_dir.filePath(QStringLiteral("list.m3u8"));
    QFile file(playlist);
    QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Text));
    file.write(QStringLiteral("#EXTM3U\n%1\n%1\n").arg(m_clip).toUtf8());
    file.close();

    QStringList filters;
    whenDialogOpens<QFileDialog>([&](QFileDialog *dialog) {
        filters = dialog->nameFilters();
        chooseInDialog(dialog, playlist);
        static_cast<QDialog *>(dialog)->accept();
    });
    QTest::mouseClick(button("OpenPlaylistButton"), Qt::LeftButton);
    QVERIFY(!filters.isEmpty());
    QVERIFY(filters.first().contains(QStringLiteral("*.m3u8")));
    QTRY_COMPARE_WITH_TIMEOUT(prop("playlist-count").toInt(), 2, 10000);
    QTRY_VERIFY(!m_empty->isVisible());
}

void EmptyStateTest::dropOnEmptyState()
{
    const QString folder = makeFolder();
    QMimeData mime;
    mime.setUrls({QUrl::fromLocalFile(folder)});
    const QPoint pos = m_empty->rect().center();
    auto *hint = m_empty->findChild<QLabel *>(QStringLiteral("EmptyStateHint"));

    QDragEnterEvent enter(pos, Qt::CopyAction, &mime, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(m_empty, &enter);
    QVERIFY(enter.isAccepted());
    QVERIFY(hint->property("dropTarget").toBool());

    QDropEvent drop(QPointF(pos), Qt::CopyAction, &mime, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(m_empty, &drop);
    QVERIFY(drop.isAccepted());
    QVERIFY(!hint->property("dropTarget").toBool());
    QTRY_COMPARE_WITH_TIMEOUT(playlistFiles(), MediaFiles::mediaFilesInFolder(folder), 10000);
    QTRY_VERIFY(!m_empty->isVisible());
}

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);
    // libmpv requires the C numeric locale; QApplication may have changed it.
    std::setlocale(LC_NUMERIC, "C");
    // Qt's own dialogs can be driven from the test; native ones cannot.
    QApplication::setAttribute(Qt::AA_DontUseNativeDialogs);
    applyDarkSkin(app);
    EmptyStateTest test;
    QTEST_SET_MAIN_SOURCE_PATH
    return QTest::qExec(&test, argc, argv);
}

#include "tst_emptystate.moc"
