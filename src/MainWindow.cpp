#include "MainWindow.h"
#include "AboutDialog.h"
#include "AudioControlDialog.h"
#include "AudioController.h"
#include "AudioEffects.h"
#include "ControlBar.h"
#include "EmptyStateWidget.h"
#include "LiveStreamDialog.h"
#include "LyricsController.h"
#include "LyricsView.h"
#include "MediaCutterDialog.h"
#include "MediaDownloaderDialog.h"
#include "MediaFiles.h"
#include "MpvWidget.h"
#ifdef TOPPLAYER_HAVE_DBUS
#include "MprisService.h"
#endif
#include "OsdWidget.h"
#include "PlayerMenu.h"
#include "PlaylistController.h"
#include "PlaylistDrawer.h"
#include "ResumeManager.h"
#include "SeekBar.h"
#include "SubtitleDownloadDialog.h"
#include "ThumbnailGenerator.h"
#include "ThumbnailPopup.h"
#include "TitleBar.h"
#include "Theme.h"
#include "WindowPin.h"
#include "Icons.h"
#include "PlaylistSession.h"

#include <QAbstractItemView>
#include <QApplication>
#include <QClipboard>
#include <QCursor>
#include <QCloseEvent>
#include <QContextMenuEvent>
#include <QDesktopServices>
#include <QCheckBox>
#include <QMessageBox>
#include <QPainter>
#include <QPointer>
#include <QPushButton>
#include <QProcess>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QFileDialog>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QKeyEvent>
#include <QMimeData>
#include <QMouseEvent>
#include <QScreen>
#include <QSettings>
#include <QStyle>
#include <QToolButton>
#include <QStandardPaths>
#include <QTreeView>
#include <QVBoxLayout>
#include <QWheelEvent>
#include <QWindow>

#include <algorithm>
#include <cmath>
#include <functional>
#include <utility>

namespace {

constexpr double kVolumeStep = 5.0;
// How close to the edge a press resizes the window; wider in the mini player.
constexpr int kResizeMargin = 8;
constexpr int kMiniResizeMargin = 12;
constexpr int kGripSize = 22;
// Largest share of the screen's available area an automatic resize may use.
constexpr qreal kMaxScreenFraction = 0.9;
// Fullscreen controls and cursor hide after this long without mouse movement.
constexpr int kIdleHideMs = 2000;
// Distance between the seekbar and the thumbnail popup above it.
constexpr int kPopupGap = 6;
// Swipe seeking: a drag across the whole video moves this far through the
// file (or the whole file, if shorter), like VLC's seek gesture.
constexpr double kSeekDragSpanSeconds = 180.0;
// Seeks sent while dragging are spaced out by this much.
constexpr int kSeekDragIntervalMs = 60;
// A horizontal wheel notch (120 units) or touchpad swipe of the same size seeks this far.
constexpr double kWheelSeekSeconds = 5.0;
// The normal window's smallest size, and the mini player's.
constexpr QSize kMinimumSize(420, 260);
constexpr QSize kMiniMinimumSize(200, 112);
constexpr int kMiniDefaultWidth = 400;
constexpr int kMiniScreenMargin = 24;
// The window manager needs the mini player mapped before it can be pinned.
constexpr int kPinDelayMs = 150;

QString settingsFile()
{
    return PlaylistSession::configDir() + QStringLiteral("/settings.ini");
}

Qt::CursorShape cursorFor(Qt::Edges edges)
{
    const bool horizontal = edges & (Qt::LeftEdge | Qt::RightEdge);
    const bool vertical = edges & (Qt::TopEdge | Qt::BottomEdge);
    if (horizontal && vertical) {
        // Top-left and bottom-right share one diagonal arrow; the other corners the other.
        const bool falling = bool(edges & Qt::LeftEdge) == bool(edges & Qt::TopEdge);
        return falling ? Qt::SizeFDiagCursor : Qt::SizeBDiagCursor;
    }
    return horizontal ? Qt::SizeHorCursor : Qt::SizeVerCursor;
}

// The mini player's resize handle: three diagonal lines in a corner. A press
// on it resizes the window from that corner.
class ResizeGrip : public QWidget
{
public:
    ResizeGrip(QWidget *parent, std::function<void(Qt::Edges, const QPoint &)> onPress)
        : QWidget(parent)
        , m_onPress(std::move(onPress))
    {
        setObjectName(QStringLiteral("MiniResizeGrip"));
        setFixedSize(kGripSize, kGripSize);
        setToolTip(QObject::tr("Drag to resize"));
        setCorner(Qt::LeftEdge | Qt::TopEdge);
    }

    Qt::Edges corner() const { return m_corner; }
    void setCorner(Qt::Edges corner)
    {
        m_corner = corner;
        setCursor(cursorFor(corner));
        update();
    }

protected:
    void mousePressEvent(QMouseEvent *event) override
    {
        if (event->button() != Qt::LeftButton) {
            event->ignore();
            return;
        }
        m_onPress(m_corner, event->globalPosition().toPoint());
        event->accept();
    }

    void paintEvent(QPaintEvent *) override
    {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        p.setPen(Qt::NoPen);
        p.setBrush(QColor(18, 19, 22, 200));
        p.drawRoundedRect(QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5), 5, 5);
        // Lines across the corner the grip sits in.
        const bool left = m_corner & Qt::LeftEdge;
        const bool top = m_corner & Qt::TopEdge;
        p.setPen(QPen(Theme::Accent, 1.6, Qt::SolidLine, Qt::RoundCap));
        const qreal s = width();
        for (const qreal d : {5.0, 9.5, 14.0}) {
            const QPointF a(left ? 4 : s - 4, top ? 4 + d : s - 4 - d);
            const QPointF b(left ? 4 + d : s - 4 - d, top ? 4 : s - 4);
            p.drawLine(a, b);
        }
    }

private:
    std::function<void(Qt::Edges, const QPoint &)> m_onPress;
    Qt::Edges m_corner;
};

// Keys that a focused list keeps for its own navigation instead of letting the
// player's shortcuts (volume, fullscreen) take them. A tree also keeps Left and
// Right, which open and close its folders, instead of seeking.
bool isListNavigationKey(const QKeyEvent *event, bool tree)
{
    if (event->modifiers() & ~(Qt::ShiftModifier | Qt::KeypadModifier))
        return false;
    switch (event->key()) {
    case Qt::Key_Left:
    case Qt::Key_Right:
        return tree;
    case Qt::Key_Up:
    case Qt::Key_Down:
    case Qt::Key_Home:
    case Qt::Key_End:
    case Qt::Key_Return:
    case Qt::Key_Enter:
        return true;
    default:
        return false;
    }
}

QString formatDelay(double seconds)
{
    const long long ms = std::llround(seconds * 1000);
    return QStringLiteral("%1%2 ms").arg(ms > 0 ? QStringLiteral("+") : QString()).arg(ms);
}

} // namespace

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent)
    , m_mpv(new MpvWidget(this))
{
    setWindowFlag(Qt::FramelessWindowHint);
    setAcceptDrops(true);
    setMinimumSize(kMinimumSize);
    // Clicking the video takes keyboard focus back from the playlist, so the
    // arrow keys control the player again.
    m_mpv->setFocusPolicy(Qt::ClickFocus);

    m_root = new QWidget(this);
    m_root->setObjectName(QStringLiteral("RootWidget"));
    m_titleBar = new TitleBar(this);
    m_controlBar = new ControlBar(m_mpv, m_root);
    m_drawer = new PlaylistDrawer(m_root);

    auto *body = new QHBoxLayout;
    body->setContentsMargins(0, 0, 0, 0);
    body->setSpacing(0);
    body->addWidget(m_mpv, 1);
    body->addWidget(m_drawer);

    m_rootLayout = new QVBoxLayout(m_root);
    m_rootLayout->setContentsMargins(0, 0, 0, 0);
    m_rootLayout->setSpacing(0);
    m_rootLayout->addWidget(m_titleBar);
    m_rootLayout->addLayout(body, 1);
    m_rootLayout->addWidget(m_controlBar);
    setCentralWidget(m_root);

    // Stacked over the video in creation order: the audio view, the lyrics,
    // then the start screen, the OSD and the resume prompt on top.
    m_audio = new AudioController(m_mpv, this);
    m_lyrics = new LyricsController(m_mpv, m_audio, this);
    m_audioEffects = new AudioEffectsController(m_mpv, this);
    m_emptyState = new EmptyStateWidget(m_mpv);
    m_osd = new OsdWidget(m_mpv);
    m_resume = new ResumeManager(m_mpv, m_mpv);
    m_menu = new PlayerMenu(m_mpv, this);
    m_titleBar->setTitle(QApplication::applicationDisplayName());

    const QString pictures = QStandardPaths::writableLocation(QStandardPaths::PicturesLocation);
    if (!pictures.isEmpty())
        m_mpv->setMpvProperty(QStringLiteral("screenshot-directory"), pictures);

    connect(m_mpv, &MpvWidget::titleChanged, this, [this](const QString &title) {
        setWindowTitle(title);
        m_titleBar->setTitle(title.isEmpty() ? QApplication::applicationDisplayName() : title);
    });
    connect(m_mpv, &MpvWidget::propertyChanged, this, &MainWindow::showPropertyOsd);
    connect(m_mpv, &MpvWidget::propertyUpdated, this, &MainWindow::onStateUpdated);
    connect(m_mpv, &MpvWidget::seeked, this, &MainWindow::showSeekOsd);
    // Like PotPlayer, open each file at 100% of its video resolution.
    connect(m_mpv, &MpvWidget::videoSizeKnown, this, [this](const QSize &size) {
        if (!isFullScreen() && !isMaximized() && !m_mini)
            resizeToVideo(size, 1.0);
    });
    connect(m_audio, &AudioController::message, m_osd,
            [this](const QString &label, const QString &value) { m_osd->showValue(label, value); });
    connect(m_lyrics, &LyricsController::message, m_osd,
            [this](const QString &label, const QString &value) { m_osd->showValue(label, value); });
    connect(m_resume, &ResumeManager::message, m_osd,
            [this](const QString &label, const QString &value) { m_osd->showValue(label, value); });
    connect(m_menu, &PlayerMenu::osdRequested, m_osd,
            [this](const QString &label, const QString &value) { m_osd->showValue(label, value); });
    connect(m_controlBar, &ControlBar::openRequested, this, &MainWindow::openFileDialog);
    connect(m_emptyState, &EmptyStateWidget::openFileRequested, this, &MainWindow::openFileDialog);
    connect(m_emptyState, &EmptyStateWidget::openFolderRequested, this, &MainWindow::openFolderDialog);
    connect(m_emptyState, &EmptyStateWidget::openUrlRequested, this, &MainWindow::openUrlDialog);
    connect(m_emptyState, &EmptyStateWidget::openPlaylistRequested, this, &MainWindow::openPlaylistDialog);
    connect(m_emptyState, &EmptyStateWidget::urlsDropped, this, &MainWindow::openUrls);
    connect(m_mpv, &MpvWidget::fileStarted, m_emptyState, [this] { m_emptyState->setActive(false); });
    connect(m_mpv, &MpvWidget::fileFailed, this, &MainWindow::onFileFailed);
    connect(m_mpv, &MpvWidget::renderStalled, this, &MainWindow::offerSoftwareVideo);
    connect(m_mpv, &MpvWidget::videoDecodeFailed, this, &MainWindow::onVideoDecodeFailed);
    connect(m_controlBar, &ControlBar::fullScreenRequested, this, &MainWindow::toggleFullScreen);
    connect(m_titleBar, &TitleBar::fullScreenRequested, this, &MainWindow::toggleFullScreen);
    connect(m_titleBar, &TitleBar::pinToggled, this, [this](bool onTop) {
        setAlwaysOnTop(onTop);
        m_osd->showValue(tr("Always on Top"), onTop ? tr("On") : tr("Off"));
    });
    connect(m_controlBar, &ControlBar::playlistToggled, this, &MainWindow::setPlaylistVisible);
    connect(m_controlBar, &ControlBar::message, m_osd,
            [this](const QString &label, const QString &value) { m_osd->showValue(label, value); });

    setupPlaylist();
    setupThumbnails();
#ifdef TOPPLAYER_HAVE_DBUS
    // The desktop hands the keyboard's media keys to players over MPRIS.
    auto *mpris = new MprisService(m_mpv, this, this);
    connect(mpris, &MprisService::openRequested, this, &MainWindow::openFile);
    m_mpris = mpris;
#endif

    m_clickTimer.setSingleShot(true);
    connect(&m_clickTimer, &QTimer::timeout, this, [this] {
        if (!m_mpv->isIdle())
            m_mpv->togglePause();
    });

    m_seekDragTimer.setSingleShot(true);
    m_seekDragTimer.setInterval(kSeekDragIntervalMs);
    connect(&m_seekDragTimer, &QTimer::timeout, this, [this] {
        // Keyframe seeks while dragging: quick, and the exact one follows on release.
        if (m_seekDrag)
            m_mpv->command({QStringLiteral("seek"), QString::number(m_seekDrag->target, 'f', 3),
                            QStringLiteral("absolute+keyframes")});
    });

    // The mini player's way back to the full window, in its top-right corner.
    m_miniRestoreButton = new QToolButton(m_root);
    m_miniRestoreButton->setObjectName(QStringLiteral("MiniRestoreButton"));
    m_miniRestoreButton->setIcon(skinIcon(IconType::Maximize));
    m_miniRestoreButton->setIconSize(QSize(16, 16));
    m_miniRestoreButton->setToolTip(tr("Back to Full Window (Esc)"));
    m_miniRestoreButton->setFocusPolicy(Qt::NoFocus);
    m_miniRestoreButton->hide();
    connect(m_miniRestoreButton, &QToolButton::clicked, this, [this] { setMiniPlayer(false); });
    m_resizeGrip = new ResizeGrip(m_root, [this](Qt::Edges edges, const QPoint &globalPos) { beginResize(edges, globalPos); });
    m_resizeGrip->hide();
    connect(m_titleBar, &TitleBar::miniPlayerRequested, this, [this] { setMiniPlayer(true); });

    m_idleTimer.setSingleShot(true);
    m_idleTimer.setInterval(kIdleHideMs);
    connect(&m_idleTimer, &QTimer::timeout, this, [this] {
        if (!m_controlsOverlaid)
            return;
        if (!m_controlBar->underMouse() && !m_miniRestoreButton->underMouse() && !m_resizeGrip->underMouse()
            && !m_manualResize)
            setOverlayControlsVisible(false);
        // The mini player keeps the pointer: it is a window among others.
        if (isFullScreen())
            m_mpv->setCursor(Qt::BlankCursor);
    });
    // Mouse moves over child widgets (the video, the bars) drive the fullscreen chrome.
    // The title bar and the drawer too, for the resize cursor along the edges.
    for (QWidget *widget : {static_cast<QWidget *>(m_mpv), m_root, static_cast<QWidget *>(m_controlBar),
                            static_cast<QWidget *>(m_titleBar), static_cast<QWidget *>(m_drawer)})
        widget->setMouseTracking(true);
    qApp->installEventFilter(this);
}

void MainWindow::setupPlaylist()
{
    m_playlist = new PlaylistController(m_mpv, m_drawer, this);
    connect(m_playlist, &PlaylistController::message, m_osd,
            [this](const QString &label, const QString &value) { m_osd->showValue(label, value); });
    connect(m_drawer, &PlaylistDrawer::openPlaylistRequested, this, &MainWindow::openPlaylistDialog);
    connect(m_drawer, &PlaylistDrawer::openFolderRequested, this, &MainWindow::openFolderDialog);
    connect(m_drawer, &PlaylistDrawer::expandedChanged, m_controlBar, &ControlBar::setPlaylistChecked);
    connect(m_drawer, &PlaylistDrawer::expandedChanged, this, [this](bool expanded) {
        // Don't leave the keyboard on a list that is going away.
        if (!expanded && m_drawer->isAncestorOf(QApplication::focusWidget()))
            m_mpv->setFocus();
    });
}

void MainWindow::setupThumbnails()
{
    m_thumbnails = new ThumbnailGenerator(this);
    m_thumbnailPopup = new ThumbnailPopup(m_root);

    SeekBar *seekBar = m_controlBar->seekBar();
    connect(seekBar, &SeekBar::hovered, this, [this, seekBar](double seconds, int x) {
        m_hoverSecond = static_cast<int>(seconds);
        m_thumbnailPopup->setTime(seconds);
        if (m_thumbnails->isAvailable())
            m_thumbnails->request(seconds);
        else
            m_thumbnailPopup->clearImage();
        m_popupAnchor = seekBar->mapTo(m_root, QPoint(x, -kPopupGap));
        m_thumbnailPopup->showAt(m_popupAnchor);
    });
    connect(seekBar, &SeekBar::hoverEnded, this, [this] {
        m_hoverSecond = -1;
        m_thumbnailPopup->hide();
    });
    // An audio file's "video" is its cover or a visualization: no previews.
    connect(m_mpv, &MpvWidget::fileLoaded, this, [this] {
        if (m_mpv->isAudioOnly())
            m_thumbnails->setFile({});
    });
    connect(m_thumbnails, &ThumbnailGenerator::thumbnailReady, this, [this](int second, const QImage &image) {
        // Keep showing the previous frame until the one under the pointer arrives.
        if (m_hoverSecond < 0 || !m_thumbnailPopup->isVisible() || second != m_hoverSecond)
            return;
        m_thumbnailPopup->setImage(image);
        m_thumbnailPopup->showAt(m_popupAnchor);
    });
}

void MainWindow::onStateUpdated(const QString &name, const QVariant &value)
{
    if (name == QLatin1String("idle-active")) {
        // Nothing loaded (startup, stop, or the playlist ran out or was cleared).
        // The report can arrive after the next file already started, so check again.
        if (value.toBool() && m_mpv->isIdle())
            m_emptyState->setActive(true);
    } else if (name == QLatin1String("playlist")) {
        m_playlist->setPlaylist(value.toList());
    } else if (name == QLatin1String("path")) {
        // Previews come from a second decoder, so only local files are worth it.
        const QString path = value.toString();
        m_thumbnails->setFile(QFileInfo(path).isFile() ? path : QString());
        // In/Out points belong to the file they were set in.
        if (m_clipIn >= 0 || m_clipOut >= 0)
            clearClipRange();
    }
}

void MainWindow::openFile(const QString &pathOrUrl)
{
    openFiles({pathOrUrl});
}

void MainWindow::openFiles(const QStringList &files)
{
    m_mpv->loadFiles(files);
}

void MainWindow::openFileDialog()
{
    const QStringList files = QFileDialog::getOpenFileNames(this, tr("Open Files"), {}, MediaFiles::mediaFileFilter());
    if (!files.isEmpty())
        openFiles(files);
}

void MainWindow::openFolderDialog()
{
    const QString folder = QFileDialog::getExistingDirectory(this, tr("Open Folder"));
    if (!folder.isEmpty())
        m_playlist->openFolder(folder);
}

void MainWindow::openUrlDialog()
{
    QInputDialog dialog(this);
    dialog.setWindowTitle(tr("Open URL / Stream"));
    dialog.setLabelText(tr("Video or audio URL (http, https, rtsp, rtmp, ...):"));
    dialog.setOkButtonText(tr("Open"));
    dialog.resize(520, dialog.sizeHint().height());
    // Offer a URL that is already on the clipboard.
    const QString clipboard = QGuiApplication::clipboard()->text().trimmed();
    if (clipboard.contains(QLatin1String("://")) && !clipboard.contains(QLatin1Char('\n')))
        dialog.setTextValue(clipboard);
    if (dialog.exec() != QDialog::Accepted)
        return;

    const QString text = dialog.textValue().trimmed();
    if (text.isEmpty())
        return;
    // Accept "example.com/video.mp4" as well as full URLs, which mpv takes as typed.
    const bool hasScheme = text.contains(QLatin1String("://"));
    const QUrl url = hasScheme ? QUrl(text) : QUrl::fromUserInput(text);
    if (!url.isValid() || url.scheme().isEmpty()) {
        m_osd->showValue(tr("Invalid URL"), text);
        return;
    }
    if (url.isLocalFile())
        openFile(url.toLocalFile());
    else
        openFile(hasScheme ? text : url.toString());
}

void MainWindow::openPlaylistDialog()
{
    m_playlist->openPlaylistDialog();
}

void MainWindow::savePlaylistDialog()
{
    m_playlist->savePlaylistDialog();
}

void MainWindow::openAudioControlDialog()
{
    if (!m_audioControl)
        m_audioControl = new AudioControlDialog(m_audioEffects, this);
    m_audioControl->show();
    m_audioControl->raise();
    m_audioControl->activateWindow();
}

void MainWindow::openLiveStreamDialog()
{
    if (!m_liveStreams) {
        m_liveStreams = new LiveStreamDialog(this);
        connect(m_liveStreams, &LiveStreamDialog::playRequested, this, &MainWindow::playStream);
        connect(m_liveStreams, &LiveStreamDialog::queueRequested, this, &MainWindow::queueStream);
    }
    m_liveStreams->show();
    m_liveStreams->raise();
    m_liveStreams->activateWindow();
}

void MainWindow::playStream(const StreamCatalog::Station &station, bool radio)
{
    m_streamRadio = radio;
    m_streamFallbacks = m_liveStreams ? m_liveStreams->alternatives(station) : QList<StreamCatalog::Station>();
    startStream(station);
    m_osd->showValue(radio ? tr("Streaming Radio:") : tr("Streaming:"), station.name);
}

void MainWindow::startStream(const StreamCatalog::Station &station)
{
    m_stream = station;
    // Radio streams are audio-only, so AudioController shows the audio view
    // with the chosen visualization as soon as the stream's tracks are known.
    m_mpv->loadStream(station.url, StreamCatalog::playbackOptions(station));
}

void MainWindow::queueStream(const StreamCatalog::Station &station)
{
    m_mpv->loadStream(station.url, StreamCatalog::playbackOptions(station), true);
    m_osd->showValue(tr("Added to Playlist"), station.name);
}

void MainWindow::onFileFailed(const QString &path, const QString & /*error*/)
{
    if (m_liveStreams)
        m_liveStreams->markUnavailable(path);
    if (path.isEmpty() || path != m_stream.url)
        return;
    const QString name = m_stream.name;
    m_stream = {};
    if (!m_streamFallbacks.isEmpty()) {
        startStream(m_streamFallbacks.takeFirst());
        m_osd->showValue(tr("Trying another source:"), name);
        return;
    }
    m_osd->showValue(m_streamRadio ? tr("Station unavailable:") : tr("Channel unavailable:"),
                     tr("%1 (offline, or not available in your region)").arg(name));
}

bool MainWindow::startSession(bool restore, bool handoff)
{
    return m_playlist->startSession(restore, handoff);
}

void MainWindow::openUrls(const QList<QUrl> &urls)
{
    // Subtitle files are added to the playing video, or to a video dropped with them.
    QStringList media;
    QStringList subtitles;
    // An image dropped on a playing song becomes its cover.
    if (urls.size() == 1 && urls.first().isLocalFile() && m_audio->isActive()
        && AudioArtwork::isImageFile(urls.first().toLocalFile())) {
        if (!m_audio->setCustomArtwork(urls.first().toLocalFile()))
            m_osd->showValue(tr("Not an image"), QFileInfo(urls.first().toLocalFile()).fileName());
        return;
    }
    // A lyrics file dropped on a song is loaded as its lyrics.
    if (urls.size() == 1 && urls.first().isLocalFile() && !m_mpv->isIdle()
        && urls.first().toLocalFile().endsWith(QLatin1String(".lrc"), Qt::CaseInsensitive)) {
        m_lyrics->loadFile(urls.first().toLocalFile());
        return;
    }
    for (const QUrl &url : urls) {
        if (!url.isLocalFile()) {
            media.append(url.toString());
            continue;
        }
        const QString path = url.toLocalFile();
        if (QFileInfo(path).isDir())
            media.append(path); // expanded below, off the GUI thread
        else if (MpvWidget::isSubtitleFile(path))
            subtitles.append(path);
        else
            media.append(path);
    }
    if (!media.isEmpty()) {
        m_playlist->openEntries(media, subtitles);
    } else if (!subtitles.isEmpty()) {
        for (const QString &subtitle : std::as_const(subtitles))
            loadSubtitle(subtitle);
    } else {
        m_osd->showValue(tr("No media files found"));
    }
}

bool MainWindow::isPlaylistVisible() const
{
    return m_drawer->isExpanded();
}

void MainWindow::setPlaylistVisible(bool visible)
{
    // The drawer sits beside the video in the layout, in fullscreen too.
    m_drawer->setExpanded(visible);
    if (visible)
        m_drawer->raise();
}

void MainWindow::loadSubtitle(const QString &path)
{
    m_mpv->addSubtitle(path);
    m_osd->showValue(tr("Subtitle Loaded"), QFileInfo(path).fileName());
}

void MainWindow::openSubtitleDownloadDialog()
{
    const QString path = m_mpv->isIdle() ? QString() : m_mpv->mpvPropertyString(QStringLiteral("path"));
    if (path.isEmpty()) {
        m_osd->showValue(tr("Open a video to download subtitles for"));
        return;
    }
    auto *dialog = new SubtitleDownloadDialog(path, this);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    connect(dialog, &SubtitleDownloadDialog::subtitleDownloaded, this, [this, path](const QString &file, const QString &label) {
        // The video may have changed while the dialog was open.
        if (m_mpv->mpvPropertyString(QStringLiteral("path")) != path)
            return;
        m_mpv->addSubtitle(file);
        // A subtitle the user just asked for should show even if subtitles were hidden.
        m_mpv->setMpvProperty(QStringLiteral("sub-visibility"), QStringLiteral("yes"));
        m_osd->showValue(tr("Subtitles loaded:"), label);
    });
    dialog->open();
}

void MainWindow::showAbout()
{
    if (m_about) {
        m_about->raise();
        m_about->activateWindow();
        return;
    }
    m_about = new AboutDialog(m_mpv, this);
    m_about->setAttribute(Qt::WA_DeleteOnClose);
    m_about->open();
}

void MainWindow::openSubtitleSettingsDialog()
{
    SubtitleSettingsDialog dialog(this);
    dialog.exec();
}

void MainWindow::openMediaDownloaderDialog()
{
    auto *dialog = new MediaDownloaderDialog(this);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    connect(dialog, &MediaDownloaderDialog::downloaded, this, [this](const QString &path, bool play) {
        if (play)
            openFile(path);
        else
            m_mpv->insertFiles({path});
        m_osd->showValue(tr("Downloaded:"), QFileInfo(path).fileName());
    });
    connect(dialog, &MediaDownloaderDialog::downloadedMany, this, [this](const QStringList &paths, bool play) {
        if (play)
            openFiles(paths);
        else
            m_mpv->insertFiles(paths);
        m_osd->showValue(tr("Downloaded:"), tr("%n song(s)", nullptr, int(paths.size())));
    });
    connect(dialog, &MediaDownloaderDialog::tracksStreamRequested, this,
            [this](const QStringList &urls, const QStringList &titles, const QString &format) {
                // Each entry is a page (or a YouTube search for a Spotify song)
                // that mpv resolves through yt-dlp.
                m_mpv->setMpvProperty(QStringLiteral("ytdl-format"), format);
                m_mpv->loadTitledFiles(urls, titles);
                m_osd->showValue(tr("Streaming"), titles.size() == 1 ? titles.first() : tr("%n entries", nullptr, int(titles.size())));
            });
    connect(dialog, &MediaDownloaderDialog::streamRequested, this, [this](const QString &url, const QString &format) {
        // mpv resolves the page through yt-dlp, picking streams with this format.
        m_mpv->setMpvProperty(QStringLiteral("ytdl-format"), format);
        openFile(url);
        m_osd->showValue(tr("Streaming"), url);
    });
    dialog->open();
}

void MainWindow::setClipIn()
{
    if (m_mpv->isIdle()) {
        m_osd->showValue(tr("Nothing is playing"));
        return;
    }
    const double position = m_mpv->mpvProperty(QStringLiteral("time-pos")).toDouble();
    m_clipIn = std::max(0.0, position);
    // An Out-point before the new In-point no longer makes a range.
    if (m_clipOut >= 0 && m_clipOut <= m_clipIn)
        m_clipOut = -1;
    updateClipRange();
    m_osd->showValue(tr("In-Point (A)"), MediaCutter::formatTimestamp(m_clipIn));
}

void MainWindow::setClipOut()
{
    if (m_mpv->isIdle()) {
        m_osd->showValue(tr("Nothing is playing"));
        return;
    }
    const double position = m_mpv->mpvProperty(QStringLiteral("time-pos")).toDouble();
    m_clipOut = std::max(0.0, position);
    if (m_clipIn >= 0 && m_clipIn >= m_clipOut)
        m_clipIn = -1;
    updateClipRange();
    m_osd->showValue(tr("Out-Point (B)"), MediaCutter::formatTimestamp(m_clipOut));
}

void MainWindow::clearClipRange()
{
    const bool hadRange = m_clipIn >= 0 || m_clipOut >= 0;
    m_clipIn = -1;
    m_clipOut = -1;
    updateClipRange();
    if (hadRange && !m_mpv->isIdle())
        m_osd->showValue(tr("In/Out Points Cleared"));
}

void MainWindow::updateClipRange()
{
    m_controlBar->seekBar()->setClipRange(m_clipIn, m_clipOut);
}

void MainWindow::openMediaCutterDialog()
{
    const QString path = m_mpv->isIdle() ? QString() : MediaFiles::localPath(m_mpv->mpvPropertyString(QStringLiteral("path")));
    if (path.isEmpty() || !QFileInfo(path).isFile()) {
        m_osd->showValue(m_mpv->isIdle() ? tr("Open a file to cut") : tr("Only local files can be cut"));
        return;
    }
    MediaCutterDialog::Setup setup;
    setup.input = path;
    setup.title = m_mpv->mpvPropertyString(QStringLiteral("media-title"));
    setup.duration = m_mpv->mpvProperty(QStringLiteral("duration")).toDouble();
    // Without both points, the range runs from the start or to the end.
    setup.start = m_clipIn >= 0 ? m_clipIn : 0;
    setup.end = m_clipOut >= 0 ? m_clipOut : setup.duration;
    QPointer<MpvWidget> mpv = m_mpv;
    setup.currentTime = [mpv] { return mpv ? mpv->mpvProperty(QStringLiteral("time-pos")).toDouble() : 0.0; };

    auto *dialog = new MediaCutterDialog(setup, this);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    connect(dialog, &MediaCutterDialog::exported, this, &MainWindow::onClipExported);
    dialog->open();
}

void MainWindow::onClipExported(const QString &path)
{
    m_osd->showValue(tr("Clip saved:"), QFileInfo(path).fileName());
    auto *box = new QMessageBox(QMessageBox::Information, tr("Clip Saved"),
                                tr("Saved %1").arg(QFileInfo(path).fileName()), QMessageBox::Close, this);
    box->setObjectName(QStringLiteral("ClipSavedMessage"));
    box->setInformativeText(QDir::toNativeSeparators(QFileInfo(path).absolutePath()));
    box->setAttribute(Qt::WA_DeleteOnClose);
    QPushButton *open = box->addButton(tr("Open in Player"), QMessageBox::AcceptRole);
    open->setObjectName(QStringLiteral("ClipOpenButton"));
    QPushButton *show = box->addButton(tr("Show in File Manager"), QMessageBox::ActionRole);
    show->setObjectName(QStringLiteral("ClipShowButton"));
    connect(box, &QMessageBox::buttonClicked, this, [this, open, show, path](QAbstractButton *button) {
        if (button == open)
            openFile(path);
        else if (button == show)
            showInFileManager(path);
    });
    box->open();
}

void MainWindow::showInFileManager(const QString &path)
{
    const QUrl folder = QUrl::fromLocalFile(QFileInfo(path).absolutePath());
    // The freedesktop FileManager1 interface opens the folder with the file
    // selected (Nautilus, Dolphin, Nemo, Thunar, ...); else just open the folder.
    const QString dbusSend = QStandardPaths::findExecutable(QStringLiteral("dbus-send"));
    if (dbusSend.isEmpty()) {
        QDesktopServices::openUrl(folder);
        return;
    }
    auto *process = new QProcess;
    QObject::connect(process, &QProcess::finished, process, [process, folder](int exitCode, QProcess::ExitStatus status) {
        if (status != QProcess::NormalExit || exitCode != 0)
            QDesktopServices::openUrl(folder);
        process->deleteLater();
    });
    QObject::connect(process, &QProcess::errorOccurred, process, [process, folder](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart) {
            QDesktopServices::openUrl(folder);
            process->deleteLater();
        }
    });
    process->start(dbusSend, {QStringLiteral("--session"), QStringLiteral("--print-reply"),
                              QStringLiteral("--dest=org.freedesktop.FileManager1"), QStringLiteral("--type=method_call"),
                              QStringLiteral("/org/freedesktop/FileManager1"), QStringLiteral("org.freedesktop.FileManager1.ShowItems"),
                              QStringLiteral("array:string:") + QUrl::fromLocalFile(path).toString(QUrl::FullyEncoded),
                              QStringLiteral("string:")});
}

void MainWindow::toggleFullScreen()
{
    if (isFullScreen()) {
        exitFullScreen();
        return;
    }
    if (m_mini)
        setMiniPlayer(false);
    m_maximizedBeforeFullScreen = isMaximized();
    m_geometryBeforeFullScreen = m_maximizedBeforeFullScreen ? normalGeometry() : geometry();
    showFullScreen();
}

void MainWindow::exitFullScreen()
{
    if (!isFullScreen())
        return;
    if (m_maximizedBeforeFullScreen) {
        showMaximized();
        return;
    }
    showNormal();
    if (m_geometryBeforeFullScreen.isValid())
        setGeometry(m_geometryBeforeFullScreen);
}

void MainWindow::setAlwaysOnTop(bool onTop)
{
    // Changing window flags hides the window, so restore its geometry and show it again.
    m_titleBar->setPinned(onTop);
    if (windowFlags().testFlag(Qt::WindowStaysOnTopHint) == onTop)
        return;
    const QRect geometry = this->geometry();
    setWindowFlag(Qt::WindowStaysOnTopHint, onTop);
    setGeometry(geometry);
    show();
}

void MainWindow::scaleToVideo(qreal scale)
{
    const int videoWidth = m_mpv->mpvProperty(QStringLiteral("dwidth")).toInt();
    const int videoHeight = m_mpv->mpvProperty(QStringLiteral("dheight")).toInt();
    if (videoWidth <= 0 || videoHeight <= 0)
        return;

    if (m_mini)
        setMiniPlayer(false);
    if (isFullScreen() || isMaximized())
        showNormal();
    if (resizeToVideo(QSize(videoWidth, videoHeight), scale))
        m_osd->showValue(tr("Window Size"), QStringLiteral("%1%").arg(qRound(scale * 100)));
}

bool MainWindow::resizeToVideo(const QSize &videoSize, qreal scale)
{
    QScreen *screen = this->screen();
    if (!screen || videoSize.isEmpty())
        return false;

    // Video pixels map 1:1 to device pixels at 100%.
    const qreal dpr = devicePixelRatioF();
    QSizeF target(videoSize.width() * scale / dpr, videoSize.height() * scale / dpr);
    const QRect available = screen->availableGeometry();
    // The title bar, control bar and playlist drawer surround the video.
    const QSize chrome = size() - m_mpv->size();
    const QSizeF limit = QSizeF(available.size()) * kMaxScreenFraction - QSizeF(chrome);
    if (target.width() > limit.width() || target.height() > limit.height())
        target.scale(limit, Qt::KeepAspectRatio);
    const QSize size = (target.toSize() + chrome).expandedTo(minimumSize());

    QRect frame(QPoint(), size);
    frame.moveCenter(geometry().center());
    // Keep the whole window on the screen it is on.
    frame.moveLeft(std::clamp(frame.left(), available.left(), std::max(available.left(), available.right() - size.width() + 1)));
    frame.moveTop(std::clamp(frame.top(), available.top(), std::max(available.top(), available.bottom() - size.height() + 1)));
    setGeometry(frame);
    return true;
}

void MainWindow::showPropertyOsd(const QString &name, const QVariant &value)
{
    if (name == QLatin1String("volume")) {
        const double volume = value.toDouble();
        m_osd->showValue(tr("Volume"), QStringLiteral("%1%").arg(qRound(volume)), std::min(volume, 100.0) / 100.0);
    } else if (name == QLatin1String("mute")) {
        m_osd->showValue(tr("Mute"), value.toBool() ? tr("On") : tr("Off"));
    } else if (name == QLatin1String("speed")) {
        m_osd->showValue(tr("Speed"), QStringLiteral("%1x").arg(value.toDouble(), 0, 'f', 2));
    } else if (name == QLatin1String("pause")) {
        m_osd->showValue(value.toBool() ? tr("Pause") : tr("Play"));
    } else if (name == QLatin1String("audio-delay")) {
        m_osd->showValue(tr("Audio Delay"), formatDelay(value.toDouble()));
    } else if (name == QLatin1String("sub-delay")) {
        m_osd->showValue(tr("Subtitle Delay"), formatDelay(value.toDouble()));
    } else if (name == QLatin1String("sub-pos")) {
        m_osd->showValue(tr("Subtitle Position"), QStringLiteral("%1%").arg(qRound(value.toDouble())));
    } else if (name == QLatin1String("sub-scale")) {
        m_osd->showValue(tr("Subtitle Size"), QStringLiteral("%1%").arg(qRound(value.toDouble() * 100)));
    }
}

void MainWindow::showSeekOsd()
{
    m_osd->showTime(m_mpv->mpvProperty(QStringLiteral("time-pos")).toDouble(),
                    m_mpv->mpvProperty(QStringLiteral("duration")).toDouble());
}

void MainWindow::changeEvent(QEvent *event)
{
    QMainWindow::changeEvent(event);
    if (event->type() == QEvent::WindowStateChange) {
        clearEdgeCursor();
        updateChrome();
    }
}

void MainWindow::closeEvent(QCloseEvent *event)
{
    m_playlist->saveSession(m_restarting);
    m_resume->saveNow();
    m_controlBar->saveState();
    if (m_mini)
        saveMiniPlayerGeometry();
    QMainWindow::closeEvent(event);
}

void MainWindow::updateChrome()
{
    // Fullscreen and the mini player show the picture only; the controls
    // float over it when the pointer comes near.
    const bool immersive = isFullScreen() || m_mini;
    // A drag anywhere on the mini player moves it, over the lyrics too.
    m_lyrics->view()->setTakesPresses(!m_mini);
    if (immersive == m_wasImmersive)
        return;
    m_wasImmersive = immersive;

    m_titleBar->setVisible(!immersive);
    setControlsOverlaid(immersive);
    // The playlist drawer stays as it is: only the user opens or closes it.
    if (immersive) {
        setOverlayControlsVisible(false);
        m_idleTimer.start();
    } else {
        m_idleTimer.stop();
        m_mpv->unsetCursor();
    }
}

void MainWindow::setControlsOverlaid(bool overlaid)
{
    if (overlaid == m_controlsOverlaid)
        return;
    m_controlsOverlaid = overlaid;
    // Over the video the bar no longer takes room from it, so showing and
    // hiding it doesn't move the picture.
    if (overlaid)
        m_rootLayout->removeWidget(m_controlBar);
    else
        m_rootLayout->addWidget(m_controlBar);
    m_controlBar->setProperty("overlay", overlaid);
    m_controlBar->style()->unpolish(m_controlBar);
    m_controlBar->style()->polish(m_controlBar);
    if (overlaid) {
        placeOverlays();
    } else {
        m_miniRestoreButton->hide();
        m_resizeGrip->hide();
        m_controlBar->show();
    }
}

void MainWindow::setOverlayControlsVisible(bool visible)
{
    if (!m_controlsOverlaid)
        return;
    if (visible) {
        placeOverlays();
        m_controlBar->show();
        m_controlBar->raise();
        if (m_mini) {
            m_miniRestoreButton->show();
            m_miniRestoreButton->raise();
            m_resizeGrip->show();
            m_resizeGrip->raise();
        }
    } else {
        m_controlBar->hide();
        m_miniRestoreButton->hide();
        m_resizeGrip->hide();
    }
}

void MainWindow::placeOverlays()
{
    if (!m_controlsOverlaid)
        return;
    // Along the bottom of the video, beside an open playlist drawer.
    const QRect video = m_mpv->geometry();
    const int height = m_controlBar->sizeHint().height();
    m_controlBar->setGeometry(video.left(), video.bottom() + 1 - height, video.width(), height);
    // The resize grip takes the corner facing the middle of the screen, so
    // the mini player grows into the screen rather than off it; the way back
    // to the full window takes a top corner the grip leaves free.
    QScreen *screen = this->screen();
    const QPoint middle = screen ? screen->availableGeometry().center() : geometry().center();
    const bool gripLeft = geometry().center().x() > middle.x();
    const bool gripTop = geometry().center().y() > middle.y();
    auto *grip = static_cast<ResizeGrip *>(m_resizeGrip);
    grip->setCorner((gripLeft ? Qt::LeftEdge : Qt::RightEdge) | (gripTop ? Qt::TopEdge : Qt::BottomEdge));
    const int gripY = gripTop ? video.top() + 4 : video.bottom() - height - kGripSize - 4;
    grip->move(gripLeft ? video.left() + 4 : video.right() - kGripSize - 3, gripY);
    const QSize button = m_miniRestoreButton->sizeHint();
    const bool buttonLeft = gripTop && !gripLeft;
    m_miniRestoreButton->setGeometry(buttonLeft ? video.left() + 6 : video.right() - button.width() - 6, video.top() + 6,
                                     button.width(), button.height());
}

bool MainWindow::beginResize(Qt::Edges edges, const QPoint &globalPos)
{
    if (!edges || isFullScreen() || isMaximized() || !windowHandle())
        return false;
    m_videoPress.reset();
    m_clickTimer.stop();
    // The window manager resizes best (snapping, size hints); where it can't,
    // follow the pointer ourselves.
    if (windowHandle()->startSystemResize(edges))
        return true;
    m_manualResize = ManualResize{edges, globalPos, geometry()};
    grabMouse(cursorFor(edges));
    return true;
}

bool MainWindow::beginMove(const QPoint &globalPos)
{
    if (isFullScreen() || !windowHandle())
        return false;
    m_videoPress.reset();
    m_clickTimer.stop();
    if (windowHandle()->startSystemMove())
        return true;
    // No window manager support: move the window along with the pointer.
    m_manualResize = ManualResize{{}, globalPos, geometry()};
    grabMouse(Qt::ClosedHandCursor);
    return true;
}

void MainWindow::updateManualResize(const QPoint &globalPos)
{
    const ManualResize &resize = *m_manualResize;
    const QPoint delta = globalPos - resize.origin;
    if (!resize.edges) {
        if (pos() != resize.geometry.topLeft() + delta)
            move(resize.geometry.topLeft() + delta);
        return;
    }
    const QSize min = minimumSize().expandedTo(minimumSizeHint().boundedTo(minimumSize()));
    const QSize max = maximumSize();
    QRect g = resize.geometry;
    if (resize.edges & Qt::LeftEdge)
        g.setLeft(std::clamp(g.left() + delta.x(), g.right() + 1 - max.width(), g.right() + 1 - min.width()));
    if (resize.edges & Qt::RightEdge)
        g.setRight(std::clamp(g.right() + delta.x(), g.left() - 1 + min.width(), g.left() - 1 + max.width()));
    if (resize.edges & Qt::TopEdge)
        g.setTop(std::clamp(g.top() + delta.y(), g.bottom() + 1 - max.height(), g.bottom() + 1 - min.height()));
    if (resize.edges & Qt::BottomEdge)
        g.setBottom(std::clamp(g.bottom() + delta.y(), g.top() - 1 + min.height(), g.top() - 1 + max.height()));
    if (g != geometry())
        setGeometry(g);
}

void MainWindow::endManualResize()
{
    if (!std::exchange(m_manualResize, std::nullopt))
        return;
    releaseMouse();
    saveMiniPlayerGeometry();
}

void MainWindow::updateEdgeCursor(const QPoint &globalPos)
{
    const Qt::Edges edges = edgesAt(mapFromGlobal(globalPos));
    if (!edges) {
        clearEdgeCursor();
        return;
    }
    const Qt::CursorShape shape = cursorFor(edges);
    if (m_edgeCursor == shape)
        return;
    if (m_edgeCursor)
        QGuiApplication::changeOverrideCursor(shape);
    else
        QGuiApplication::setOverrideCursor(shape);
    m_edgeCursor = shape;
}

void MainWindow::clearEdgeCursor()
{
    if (std::exchange(m_edgeCursor, std::nullopt))
        QGuiApplication::restoreOverrideCursor();
}

QRect MainWindow::miniPlayerGeometry() const
{
    const QRect saved = QSettings(settingsFile(), QSettings::IniFormat).value(QStringLiteral("miniPlayer/geometry")).toRect();
    if (saved.isValid() && saved.width() >= kMiniMinimumSize.width() && saved.height() >= kMiniMinimumSize.height()) {
        // Only where a screen still is (monitors come and go).
        for (QScreen *screen : QGuiApplication::screens()) {
            if (screen->availableGeometry().intersected(saved).width() >= kMiniMinimumSize.width() / 2)
                return saved;
        }
    }
    // The video's shape, or a short strip for audio and lyrics.
    const int videoWidth = m_mpv->mpvProperty(QStringLiteral("dwidth")).toInt();
    const int videoHeight = m_mpv->mpvProperty(QStringLiteral("dheight")).toInt();
    int height = kMiniDefaultWidth * 9 / 16;
    if (!m_mpv->isIdle() && !m_mpv->isAudioOnly() && videoWidth > 0 && videoHeight > 0)
        height = std::clamp(kMiniDefaultWidth * videoHeight / videoWidth, kMiniMinimumSize.height(), kMiniDefaultWidth);
    QScreen *screen = this->screen() ? this->screen() : QGuiApplication::primaryScreen();
    const QRect available = screen ? screen->availableGeometry() : QRect(0, 0, 1280, 720);
    return QRect(available.right() - kMiniScreenMargin - kMiniDefaultWidth + 1,
                 available.bottom() - kMiniScreenMargin - height + 1, kMiniDefaultWidth, height);
}

void MainWindow::saveMiniPlayerGeometry()
{
    if (m_mini)
        QSettings(settingsFile(), QSettings::IniFormat).setValue(QStringLiteral("miniPlayer/geometry"), geometry());
}

void MainWindow::setMiniPlayer(bool on)
{
    if (on == m_mini)
        return;
    if (on) {
        if (isFullScreen())
            exitFullScreen();
        m_maximizedBeforeMini = isMaximized();
        m_geometryBeforeMini = m_maximizedBeforeMini ? normalGeometry() : geometry();
        m_onTopBeforeMini = windowFlags().testFlag(Qt::WindowStaysOnTopHint);
        m_drawerBeforeMini = m_drawer->isExpanded();
        m_mini = true;
        m_drawer->setExpanded(false);
        if (isMaximized())
            showNormal();
        setMinimumSize(kMiniMinimumSize);
        setAlwaysOnTop(true);
        setGeometry(miniPlayerGeometry());
        updateChrome();
        QPointer<MainWindow> self = this;
        QTimer::singleShot(kPinDelayMs, this, [self] {
            if (self && self->m_mini) {
                WindowPin::setKeepAbove(self->windowHandle(), true);
                WindowPin::setOnAllWorkspaces(self->windowHandle(), true);
            }
        });
        m_osd->showValue(tr("Mini Player"), WindowPin::isSupported() ? tr("On top, on every workspace") : tr("On top"));
        // Wayland ignores both: offer the mode where they work.
        if (canUseX11Mode() && !isX11Mode())
            QTimer::singleShot(kPinDelayMs * 2, this, &MainWindow::offerX11Mode);
        return;
    }

    saveMiniPlayerGeometry();
    WindowPin::setOnAllWorkspaces(windowHandle(), false);
    if (!m_onTopBeforeMini)
        WindowPin::setKeepAbove(windowHandle(), false);
    m_mini = false;
    setMinimumSize(kMinimumSize);
    setAlwaysOnTop(m_onTopBeforeMini);
    updateChrome();
    if (m_maximizedBeforeMini) {
        showMaximized();
    } else if (m_geometryBeforeMini.isValid()) {
        setGeometry(m_geometryBeforeMini);
    }
    if (m_drawerBeforeMini)
        setPlaylistVisible(true);
}

bool MainWindow::canUseX11Mode()
{
    // A Wayland session with XWayland running ($DISPLAY set).
    return !qEnvironmentVariableIsEmpty("WAYLAND_DISPLAY") && !qEnvironmentVariableIsEmpty("DISPLAY");
}

bool MainWindow::isX11Mode()
{
    return QGuiApplication::platformName() == QLatin1String("xcb") && !qEnvironmentVariableIsEmpty("WAYLAND_DISPLAY");
}

void MainWindow::offerX11Mode()
{
    if (!m_mini || m_askedAboutX11 || isX11Mode()
        || QSettings(settingsFile(), QSettings::IniFormat).value(QStringLiteral("window/askX11Mode"), true).toBool() == false)
        return;
    m_askedAboutX11 = true;
    auto *box = new QMessageBox(QMessageBox::Question, tr("Keep the Mini Player on Top?"),
                                tr("Your desktop runs on Wayland, which doesn't let apps keep a window above "
                                   "other apps or on every workspace, so the mini player can end up behind them."),
                                QMessageBox::NoButton, this);
    box->setObjectName(QStringLiteral("X11ModeQuestion"));
    box->setInformativeText(tr("Top Player can restart in X11 mode (through XWayland), where the mini player stays "
                               "on top of everything and on every workspace. The song keeps playing from where it is.\n\n"
                               "You can switch back any time: Window \u203a Keep Mini Player Above Other Apps."));
    QPushButton *restart = box->addButton(tr("Restart in X11 Mode"), QMessageBox::AcceptRole);
    box->addButton(tr("Not Now"), QMessageBox::RejectRole);
    auto *dontAsk = new QCheckBox(tr("Don't ask again"), box);
    box->setCheckBox(dontAsk);
    box->setDefaultButton(restart);
    box->setAttribute(Qt::WA_DeleteOnClose);
    connect(box, &QMessageBox::buttonClicked, this, [this, box, restart, dontAsk](QAbstractButton *button) {
        if (dontAsk->isChecked())
            QSettings(settingsFile(), QSettings::IniFormat).setValue(QStringLiteral("window/askX11Mode"), false);
        if (button == restart)
            QTimer::singleShot(0, this, [this] { restartInX11Mode(true); });
        Q_UNUSED(box);
    });
    box->open();
}

void MainWindow::restartInX11Mode(bool x11)
{
    PlaylistSession::setX11Mode(x11);
    restartPlayer(x11);
}

void MainWindow::setVideoOutput(MpvWidget::VideoOutput output)
{
    MpvWidget::saveVideoOutput(output);
    if (output != m_mpv->videoOutput())
        restartPlayer();
}

void MainWindow::offerSoftwareVideo()
{
    if (m_mpv->videoOutput() == MpvWidget::VideoOutput::Software)
        return;
    auto *box = new QMessageBox(QMessageBox::Warning, tr("Video Isn't Showing Properly"),
                                tr("The video picture isn't reaching the screen: it stays black, or the window "
                                   "only updates while the pointer moves over it. This happens with some graphics "
                                   "drivers and desktops."),
                                QMessageBox::NoButton, this);
    box->setObjectName(QStringLiteral("SoftwareVideoQuestion"));
    box->setInformativeText(tr("Top Player can restart with the software video output, which draws the picture "
                               "without OpenGL. It uses more CPU, but works everywhere. Playback continues "
                               "from where it is.\n\nYou can switch back any time: Video \u203a Video Output."));
    QPushButton *restart = box->addButton(tr("Use Software Video Output"), QMessageBox::AcceptRole);
    box->addButton(tr("Keep OpenGL"), QMessageBox::RejectRole);
    box->setDefaultButton(restart);
    box->setAttribute(Qt::WA_DeleteOnClose);
    connect(box, &QMessageBox::buttonClicked, this, [this, restart](QAbstractButton *button) {
        if (button == restart)
            QTimer::singleShot(0, this, [this] { setVideoOutput(MpvWidget::VideoOutput::Software); });
    });
    box->open();
}

void MainWindow::onVideoDecodeFailed(const QString &codec, const QString &message)
{
    Q_UNUSED(message);
    const QString name = codec.toUpper();
    m_osd->showValue(tr("Can't decode the video"), tr("%1 codec missing").arg(name));
    auto *box = new QMessageBox(QMessageBox::Warning, tr("Video Codec Missing"),
                                tr("This video uses the %1 codec, which the FFmpeg installed on this system can't "
                                   "decode, so only the sound plays.").arg(name),
                                QMessageBox::Ok, this);
    box->setObjectName(QStringLiteral("CodecMissingMessage"));
    box->setTextInteractionFlags(Qt::TextSelectableByMouse);
    box->setInformativeText(tr("Some distributions ship FFmpeg without patented codecs (H.264, HEVC, ...). "
                               "Install the full FFmpeg to play them:\n\n"
                               "Fedora: enable RPM Fusion, then\n"
                               "    sudo dnf swap ffmpeg-free ffmpeg --allowerasing\n"
                               "openSUSE: install ffmpeg from Packman\n\n"
                               "The Flatpak and AppImage builds bring their own codecs."));
    box->setAttribute(Qt::WA_DeleteOnClose);
    box->open();
}

void MainWindow::restartPlayer(std::optional<bool> x11)
{
    const bool playing = !m_mpv->isIdle() && !m_mpv->mpvProperty(QStringLiteral("pause")).toBool();
    const bool mini = m_mini;
    QStringList args{QStringLiteral("--handoff")};
    if (playing)
        args << QStringLiteral("--play");
    if (mini)
        args << QStringLiteral("--mini");
    // An AppImage runs from a mount that goes away with this process.
    const QString appImage = qEnvironmentVariable("APPIMAGE");
    QProcess process;
    process.setProgram(appImage.isEmpty() ? QCoreApplication::applicationFilePath() : appImage);
    process.setArguments(args);
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    // Falls back to Wayland if the X11 platform plugin is missing.
    if (x11)
        env.insert(QStringLiteral("QT_QPA_PLATFORM"), *x11 ? QStringLiteral("xcb;wayland") : QStringLiteral("wayland;xcb"));
    process.setProcessEnvironment(env);

    // Hand the queue and position over, then make way for the new instance.
    m_restarting = true;
    close();
    if (!process.startDetached()) {
        m_restarting = false;
        show();
        m_osd->showValue(tr("Could not restart Top Player"));
        return;
    }
    QCoreApplication::quit();
}

void MainWindow::setMiniPlayerWidth(int width)
{
    if (!m_mini)
        setMiniPlayer(true);
    const QRect current = geometry();
    const double aspect = current.width() > 0 ? double(current.height()) / current.width() : 9.0 / 16.0;
    QSize size(width, static_cast<int>(std::lround(width * aspect)));
    size = size.expandedTo(kMiniMinimumSize);
    QRect frame(QPoint(), size);
    // Grow from the corner the window sits in, so it stays tucked away.
    QScreen *screen = this->screen();
    const QRect available = screen ? screen->availableGeometry() : current;
    const bool right = current.center().x() > available.center().x();
    const bool bottom = current.center().y() > available.center().y();
    frame.moveLeft(right ? current.right() - size.width() + 1 : current.left());
    frame.moveTop(bottom ? current.bottom() - size.height() + 1 : current.top());
    frame.moveLeft(std::clamp(frame.left(), available.left(), std::max(available.left(), available.right() - size.width() + 1)));
    frame.moveTop(std::clamp(frame.top(), available.top(), std::max(available.top(), available.bottom() - size.height() + 1)));
    setGeometry(frame);
    saveMiniPlayerGeometry();
}

bool MainWindow::eventFilter(QObject *watched, QEvent *event)
{
    // This sees every event of the application; let the rest through at once.
    const QEvent::Type type = event->type();
    if ((type == QEvent::Resize || type == QEvent::Move) && (watched == m_mpv || watched == m_root)) {
        placeOverlays();
        return QMainWindow::eventFilter(watched, event);
    }
    // Resizing from the edges, whatever widget is under the pointer there
    // (the floating controls, the lyrics, the playlist...).
    if ((type == QEvent::MouseButtonPress || type == QEvent::MouseButtonDblClick || type == QEvent::MouseButtonRelease
         || type == QEvent::MouseMove)
        && watched->isWidgetType() && static_cast<QWidget *>(watched)->window() == this) {
        auto *mouse = static_cast<QMouseEvent *>(event);
        const QPoint globalPos = mouse->globalPosition().toPoint();
        if (m_manualResize) {
            if (type == QEvent::MouseMove)
                updateManualResize(globalPos);
            else if (type == QEvent::MouseButtonRelease && mouse->button() == Qt::LeftButton)
                endManualResize();
            return true;
        }
        if (type == QEvent::MouseButtonPress && mouse->button() == Qt::LeftButton) {
            if (beginResize(edgesAt(mapFromGlobal(globalPos)), globalPos))
                return true;
        } else if (type == QEvent::MouseButtonDblClick && edgesAt(mapFromGlobal(globalPos))) {
            // The second click of a double click on an edge is not fullscreen.
            return true;
        } else if (type == QEvent::MouseMove && mouse->buttons() == Qt::NoButton) {
            updateEdgeCursor(globalPos);
        }
    }
    if (type == QEvent::Leave && watched == this)
        clearEdgeCursor();
    else if (type == QEvent::Enter && watched->isWidgetType() && static_cast<QWidget *>(watched)->window() == this)
        updateEdgeCursor(QCursor::pos());
    if (type != QEvent::ShortcutOverride && type != QEvent::KeyPress && type != QEvent::MouseMove)
        return QMainWindow::eventFilter(watched, event);
    // A shortcut fires unless the focused widget claims the key first. Let a
    // focused list (the playlist) keep its navigation keys, so Up/Down move the
    // selection instead of changing the volume.
    if (event->type() == QEvent::ShortcutOverride && qobject_cast<QAbstractItemView *>(watched)
        && static_cast<QWidget *>(watched)->window() == this
        && isListNavigationKey(static_cast<QKeyEvent *>(event), qobject_cast<QTreeView *>(watched))) {
        event->accept();
        return true;
    }
    // Esc cancels a swipe seek, returning to where it started.
    if (event->type() == QEvent::KeyPress && m_seekDrag && static_cast<QKeyEvent *>(event)->key() == Qt::Key_Escape) {
        endSeekDrag(true);
        return true;
    }
    // Esc always leaves fullscreen or the mini player, whichever widget has the keyboard.
    if (event->type() == QEvent::KeyPress && m_mini && !isFullScreen() && watched->isWidgetType()
        && static_cast<QWidget *>(watched)->window() == this
        && static_cast<QKeyEvent *>(event)->key() == Qt::Key_Escape) {
        setMiniPlayer(false);
        return true;
    }
    if (event->type() == QEvent::KeyPress && isFullScreen() && watched->isWidgetType()
        && static_cast<QWidget *>(watched)->window() == this
        && static_cast<QKeyEvent *>(event)->key() == Qt::Key_Escape) {
        exitFullScreen();
        return true;
    }
    if (event->type() == QEvent::MouseMove && watched->isWidgetType()
        && static_cast<QWidget *>(watched)->window() == this) {
        onMouseActivity(static_cast<QMouseEvent *>(event)->globalPosition().toPoint());
    }
    return QMainWindow::eventFilter(watched, event);
}

void MainWindow::onMouseActivity(const QPoint &globalPos)
{
    if (!m_controlsOverlaid)
        return;
    m_mpv->unsetCursor();
    const QPoint pos = mapFromGlobal(globalPos);
    if (m_mini) {
        // The mini player is small: anywhere over it brings the controls.
        if (rect().contains(pos))
            setOverlayControlsVisible(true);
    } else {
        // Reveal the controls when the pointer nears the bottom edge.
        const int revealHeight = m_controlBar->sizeHint().height() * 2;
        if (pos.y() >= height() - revealHeight)
            setOverlayControlsVisible(true);
    }
    m_idleTimer.start();
}

void MainWindow::keyPressEvent(QKeyEvent *event)
{
    // Everything else, the media keys included, is a QAction shortcut
    // registered by PlayerMenu.
    switch (event->key()) {
    case Qt::Key_Enter:
        toggleFullScreen();
        break;
    case Qt::Key_Escape:
        if (m_mini && !isFullScreen())
            setMiniPlayer(false);
        else
            exitFullScreen();
        break;
    default:
        QMainWindow::keyPressEvent(event);
        return;
    }
    event->accept();
}

void MainWindow::wheelEvent(QWheelEvent *event)
{
    // The playlist scrolls itself; at its ends the leftover wheel events land here.
    if (m_drawer->isVisible() && m_drawer->rect().contains(m_drawer->mapFromGlobal(event->globalPosition().toPoint()))) {
        event->ignore();
        return;
    }
    // One standard wheel notch is 120 units; scale to support high-resolution wheels.
    const QPoint angle = event->angleDelta();
    if (std::abs(angle.x()) > std::abs(angle.y())) {
        // Sideways (a touchpad swipe or a tilting wheel) seeks: scrolling right
        // goes forward, as with mpv's WHEEL_RIGHT. Qt reports right as negative.
        const int delta = -angle.x();
        if (!m_mpv->isIdle()) {
            m_wheelSeekRemainder += delta;
            const double seconds = kWheelSeekSeconds * m_wheelSeekRemainder / 120.0;
            // Touchpads report small steps: seek once a second's worth has built up.
            if (std::abs(seconds) >= 1.0) {
                m_wheelSeekRemainder = 0;
                m_mpv->command({QStringLiteral("seek"), QString::number(seconds, 'f', 3), QStringLiteral("relative")});
            }
        }
    } else if (angle.y() != 0) {
        m_mpv->adjustVolume(kVolumeStep * angle.y() / 120.0);
    }
    event->accept();
}

void MainWindow::mousePressEvent(QMouseEvent *event)
{
    // Another button during a swipe seek cancels it.
    if (m_seekDrag && event->button() != Qt::LeftButton) {
        endSeekDrag(true);
        event->accept();
        return;
    }
    if (event->button() != Qt::LeftButton) {
        QMainWindow::mousePressEvent(event);
        return;
    }
    const QPoint globalPos = event->globalPosition().toPoint();
    const bool canMove = !isFullScreen() && windowHandle();

    // Without a frame, let the compositor move the window for us; the
    // edges are handled in eventFilter(), before any child sees the press.
    if (isOverVideo(globalPos)) {
        // Wait for the release (a click: pause) or for the pointer to move (a drag: move the window).
        m_videoPress = globalPos;
    } else if (canMove) {
        beginMove(globalPos);
    } else {
        QMainWindow::mousePressEvent(event);
        return;
    }
    event->accept();
}

void MainWindow::mouseMoveEvent(QMouseEvent *event)
{
    const QPoint globalPos = event->globalPosition().toPoint();
    if (m_seekDrag && (event->buttons() & Qt::LeftButton)) {
        updateSeekDrag(globalPos);
        event->accept();
        return;
    }
    if (m_videoPress && (event->buttons() & Qt::LeftButton)
        && (globalPos - *m_videoPress).manhattanLength() >= QApplication::startDragDistance()) {
        const QPoint origin = *std::exchange(m_videoPress, std::nullopt);
        const QPoint delta = globalPos - origin;
        // Sideways scrubs through the file; otherwise the drag moves the
        // window. The mini player is a window to place: any drag moves it
        // (the touchpad's sideways swipe still seeks).
        if (!m_mini && std::abs(delta.x()) > std::abs(delta.y()) && beginSeekDrag(origin))
            updateSeekDrag(globalPos);
        else
            beginMove(origin);
        event->accept();
        return;
    }
    QMainWindow::mouseMoveEvent(event);
}

bool MainWindow::beginSeekDrag(const QPoint &origin)
{
    if (m_mpv->isIdle())
        return false;
    const double duration = m_mpv->mpvProperty(QStringLiteral("duration")).toDouble();
    if (duration <= 0 || !m_mpv->mpvProperty(QStringLiteral("seekable")).toBool())
        return false;
    const double position = m_mpv->mpvProperty(QStringLiteral("time-pos")).toDouble();
    m_seekDrag = SeekDrag{origin, position, position, duration};
    m_clickTimer.stop();
    m_mpv->setCursor(Qt::SizeHorCursor);
    return true;
}

void MainWindow::updateSeekDrag(const QPoint &globalPos)
{
    const int width = std::max(1, m_mpv->width());
    const double span = std::min(m_seekDrag->duration, kSeekDragSpanSeconds);
    const double offset = (globalPos.x() - m_seekDrag->origin.x()) * span / width;
    const double target = std::clamp(m_seekDrag->startTime + offset, 0.0, m_seekDrag->duration);
    m_seekDrag->target = target;
    m_osd->showSeek(target - m_seekDrag->startTime, target, m_seekDrag->duration);
    m_controlBar->seekBar()->setPosition(target);
    if (!m_seekDragTimer.isActive())
        m_seekDragTimer.start();
}

void MainWindow::endSeekDrag(bool cancel)
{
    const std::optional<SeekDrag> drag = std::exchange(m_seekDrag, std::nullopt);
    if (!drag)
        return;
    m_seekDragTimer.stop();
    m_mpv->unsetCursor();
    const double target = cancel ? drag->startTime : drag->target;
    m_mpv->command({QStringLiteral("seek"), QString::number(target, 'f', 3), QStringLiteral("absolute+exact")});
    if (cancel)
        m_osd->showTime(target, drag->duration);
}

void MainWindow::mouseReleaseEvent(QMouseEvent *event)
{
    if (event->button() == Qt::LeftButton && m_seekDrag) {
        endSeekDrag(false);
        event->accept();
        return;
    }
    if (event->button() == Qt::LeftButton && std::exchange(m_videoPress, std::nullopt)) {
        // Toggling now would make every double click pause and resume playback.
        m_clickTimer.start(QApplication::doubleClickInterval());
        event->accept();
        return;
    }
    QMainWindow::mouseReleaseEvent(event);
}

void MainWindow::mouseDoubleClickEvent(QMouseEvent *event)
{
    // Only the video area toggles fullscreen; the title bar maximizes instead.
    if (event->button() == Qt::LeftButton && isOverVideo(event->globalPosition().toPoint())) {
        m_clickTimer.stop();
        m_videoPress.reset();
        // The mini player opens back up into the full window.
        if (m_mini)
            setMiniPlayer(false);
        else
            toggleFullScreen();
        event->accept();
        return;
    }
    QMainWindow::mouseDoubleClickEvent(event);
}

bool MainWindow::isOverVideo(const QPoint &globalPos) const
{
    // The floating controls cover part of the video; clicks between their
    // buttons are not clicks on the picture.
    if (m_controlsOverlaid && m_controlBar->isVisible()
        && m_controlBar->rect().contains(m_controlBar->mapFromGlobal(globalPos)))
        return false;
    return m_mpv->isVisible() && m_mpv->rect().contains(m_mpv->mapFromGlobal(globalPos));
}

void MainWindow::contextMenuEvent(QContextMenuEvent *event)
{
    m_menu->popup(event->globalPos());
    event->accept();
}

void MainWindow::dragEnterEvent(QDragEnterEvent *event)
{
    if (event->mimeData()->hasUrls())
        event->acceptProposedAction();
}

void MainWindow::dropEvent(QDropEvent *event)
{
    const QList<QUrl> urls = event->mimeData()->urls();
    if (urls.isEmpty())
        return;
    openUrls(urls);
    event->acceptProposedAction();
}

Qt::Edges MainWindow::edgesAt(const QPoint &pos) const
{
    Qt::Edges edges;
    if (isFullScreen() || isMaximized() || !rect().contains(pos))
        return edges;
    const int margin = m_mini ? kMiniResizeMargin : kResizeMargin;
    // Corners are easier to catch than the edges between them.
    const int corner = margin * 2;
    const bool nearLeft = pos.x() < margin, nearRight = pos.x() >= width() - margin;
    const bool nearTop = pos.y() < margin, nearBottom = pos.y() >= height() - margin;
    const bool cornerX = pos.x() < corner || pos.x() >= width() - corner;
    const bool cornerY = pos.y() < corner || pos.y() >= height() - corner;
    if (nearLeft || (cornerX && pos.x() < corner && (nearTop || nearBottom)))
        edges |= Qt::LeftEdge;
    if (nearRight || (cornerX && pos.x() >= width() - corner && (nearTop || nearBottom)))
        edges |= Qt::RightEdge;
    if (nearTop || (cornerY && pos.y() < corner && (nearLeft || nearRight)))
        edges |= Qt::TopEdge;
    if (nearBottom || (cornerY && pos.y() >= height() - corner && (nearLeft || nearRight)))
        edges |= Qt::BottomEdge;
    return edges;
}
