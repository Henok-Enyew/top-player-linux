#pragma once

#include "StreamCatalog.h"

#include <QMainWindow>
#include <QPointer>
#include <QTimer>
#include <QUrl>

#include <optional>

class AboutDialog;
class AudioControlDialog;
class AudioController;
class AudioEffectsController;
class ControlBar;
class EmptyStateWidget;
class LiveStreamDialog;
class LyricsController;
class MpvWidget;
class OsdWidget;
class PlayerMenu;
class PlaylistController;
class PlaylistDrawer;
class ResumeManager;
class ThumbnailGenerator;
class ThumbnailPopup;
class TitleBar;
class QToolButton;
class QVBoxLayout;

// Borderless top-level window: skin title bar, video surface with the
// playlist drawer beside it, and the control bar underneath.
class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    explicit MainWindow(QWidget *parent = nullptr);

    void openFile(const QString &pathOrUrl);
    // Plays the first file and queues the rest.
    void openFiles(const QStringList &files);
    // Opens dropped or pasted URLs: folders are expanded, subtitle files are
    // added to the video, everything else replaces the playlist.
    void openUrls(const QList<QUrl> &urls);
    void openFileDialog();
    void openFolderDialog();
    void openUrlDialog();
    void openPlaylistDialog();
    void savePlaylistDialog();
    // The Live TV & Radio browser; created on first use.
    void openLiveStreamDialog();
    LiveStreamDialog *liveStreamDialog() const { return m_liveStreams; }
    // Plays a live stream, titled with the station's name and sent with the
    // HTTP headers it needs. If it can't be played, the channel's other
    // streams in the Live TV list are tried in turn.
    void playStream(const StreamCatalog::Station &station, bool radio);
    void queueStream(const StreamCatalog::Station &station);
    // Starts saving the queue for the next run and, if `restore`, reopens the
    // last one (as configured). Returns true if a queue was restored.
    bool startSession(bool restore);
    PlaylistController *playlist() const { return m_playlist; }
    AudioController *audio() const { return m_audio; }
    LyricsController *lyrics() const { return m_lyrics; }
    ResumeManager *resume() const { return m_resume; }
    AudioEffectsController *audioEffects() const { return m_audioEffects; }
    // Audio -> Audio Control & Equalizer; created on first use.
    void openAudioControlDialog();
    AudioControlDialog *audioControlDialog() const { return m_audioControl; }
    void loadSubtitle(const QString &path);
    // Opens the OpenSubtitles search for the playing file.
    void openSubtitleDownloadDialog();
    void openSubtitleSettingsDialog();
    // Help -> About Top Player (F1).
    void showAbout();

    // Download from URL... (yt-dlp).
    void openMediaDownloaderDialog();

    // The cutter's In (A) and Out (B) points: set to the playback position,
    // shown on the seekbar, and cleared when another file opens. -1 if unset.
    void setClipIn();
    void setClipOut();
    void clearClipRange();
    double clipIn() const { return m_clipIn; }
    double clipOut() const { return m_clipOut; }
    // Tools -> Cut / Extract Media...
    void openMediaCutterDialog();
    // Opens the file manager at `path`, selecting it where supported.
    static void showInFileManager(const QString &path);
    void toggleFullScreen();
    // Leaves fullscreen for the maximized or normal geometry the window had before.
    void exitFullScreen();
    void setAlwaysOnTop(bool onTop);
    // The mini player (pop-out): a small, borderless window that stays on top
    // and, on X11, on every workspace. The controls float over the picture
    // and fade out; lyrics shrink to the lines that fit. Its size and place
    // are remembered. Esc, a double click or the corner button bring the
    // full window back.
    void setMiniPlayer(bool on);
    void toggleMiniPlayer() { setMiniPlayer(!m_mini); }
    bool isMiniPlayer() const { return m_mini; }
    // Resizes the mini player to `width`, keeping the video's shape.
    void setMiniPlayerWidth(int width);
    // True while the control bar floats over the video (fullscreen, mini player).
    bool areControlsOverlaid() const { return m_controlsOverlaid; }
    // Resizes the window from `edges` as the pointer moves from `globalPos`:
    // through the window manager, or by itself where that isn't available.
    // Returns false if the window can't be resized now (fullscreen, maximized).
    bool beginResize(Qt::Edges edges, const QPoint &globalPos);
    bool isResizing() const { return m_manualResize.has_value(); }
    // The window edges within reach of `pos` (window coordinates), if it can be resized.
    Qt::Edges edgesAt(const QPoint &pos) const;
    // Resizes the window so the video shows at `scale` times its display size.
    void scaleToVideo(qreal scale);

    bool isPlaylistVisible() const;
    void setPlaylistVisible(bool visible);

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;
    void changeEvent(QEvent *event) override;
    void closeEvent(QCloseEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;
    void wheelEvent(QWheelEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void mouseDoubleClickEvent(QMouseEvent *event) override;
    void contextMenuEvent(QContextMenuEvent *event) override;
    void dragEnterEvent(QDragEnterEvent *event) override;
    void dropEvent(QDropEvent *event) override;

private:
    void startStream(const StreamCatalog::Station &station);
    void onFileFailed(const QString &path, const QString &error);
    void setupPlaylist();
    void setupThumbnails();
    void onStateUpdated(const QString &name, const QVariant &value);
    void showPropertyOsd(const QString &name, const QVariant &value);
    void showSeekOsd();
    void updateClipRange();
    void onClipExported(const QString &path);
    // Resizes the window so the video area is `scale` times `videoSize`, shrunk
    // to fit the screen, keeping the window centered. Returns false if skipped.
    bool resizeToVideo(const QSize &videoSize, qreal scale);
    // Shows or hides the title bar, control bar and drawer for fullscreen
    // and the mini player.
    void updateChrome();
    // Takes the control bar out of the layout to float over the video, or puts it back.
    void setControlsOverlaid(bool overlaid);
    // Places the floating control bar and the mini player's buttons.
    void placeOverlays();
    // Shows or hides the floating controls (and the mini player's buttons).
    void setOverlayControlsVisible(bool visible);
    // Where the mini player opens: where it was last, else the screen's bottom right.
    QRect miniPlayerGeometry() const;
    void saveMiniPlayerGeometry();
    // In fullscreen, reveals the control bar near the bottom edge and hides
    // it and the cursor again after a moment without mouse movement.
    void onMouseActivity(const QPoint &globalPos);
    bool isOverVideo(const QPoint &globalPos) const;
    // Swipe seeking: starts a drag at `globalPos`, follows it, and ends it
    // (seeking there, or back to where it started if `cancel`).
    bool beginSeekDrag(const QPoint &origin);
    void updateSeekDrag(const QPoint &globalPos);
    void endSeekDrag(bool cancel);

    MpvWidget *m_mpv = nullptr;
    EmptyStateWidget *m_emptyState = nullptr;
    OsdWidget *m_osd = nullptr;
    PlayerMenu *m_menu = nullptr;
    QObject *m_mpris = nullptr; // MprisService, when built with D-Bus
    TitleBar *m_titleBar = nullptr;
    ControlBar *m_controlBar = nullptr;
    PlaylistDrawer *m_drawer = nullptr;
    PlaylistController *m_playlist = nullptr;
    AudioController *m_audio = nullptr;
    LyricsController *m_lyrics = nullptr;
    ResumeManager *m_resume = nullptr;
    ThumbnailGenerator *m_thumbnails = nullptr;
    ThumbnailPopup *m_thumbnailPopup = nullptr;
    LiveStreamDialog *m_liveStreams = nullptr;
    AudioEffectsController *m_audioEffects = nullptr;
    AudioControlDialog *m_audioControl = nullptr;
    // The live stream played last, and its channel's untried other streams.
    StreamCatalog::Station m_stream;
    bool m_streamRadio = false;
    QList<StreamCatalog::Station> m_streamFallbacks;
    QWidget *m_root = nullptr;
    QPointer<AboutDialog> m_about;
    QTimer m_idleTimer;
    // A click on the video pauses once it is clear that no double click follows.
    QTimer m_clickTimer;
    // A left press on the video that may still become a click or a window drag.
    std::optional<QPoint> m_videoPress;
    // A horizontal drag on the video scrubs through the file (as in VLC):
    // where it started, the time then, the time it points at now, and the
    // timer that spaces out the seeks sent while dragging.
    struct SeekDrag {
        QPoint origin;
        double startTime = 0;
        double target = 0;
        double duration = 0;
    };
    std::optional<SeekDrag> m_seekDrag;
    QTimer m_seekDragTimer;
    // Horizontal wheel (touchpad swipe, tilt wheel) units not yet turned into a seek.
    int m_wheelSeekRemainder = 0;
    QVBoxLayout *m_rootLayout = nullptr;
    bool m_controlsOverlaid = false;
    bool m_wasImmersive = false;
    // The mini player, and the window it was opened from.
    bool m_mini = false;
    QRect m_geometryBeforeMini;
    bool m_maximizedBeforeMini = false;
    bool m_onTopBeforeMini = false;
    bool m_drawerBeforeMini = false;
    QToolButton *m_miniRestoreButton = nullptr;
    // The mini player's resize handle, in the corner facing the middle of the screen.
    QWidget *m_resizeGrip = nullptr;
    // A resize the window does itself (when the window manager can't): the
    // edges dragged, where the drag started and the geometry then.
    struct ManualResize {
        Qt::Edges edges;
        QPoint origin;
        QRect geometry;
    };
    std::optional<ManualResize> m_manualResize;
    // The resize cursor shown while the pointer is over an edge.
    std::optional<Qt::CursorShape> m_edgeCursor;
    void updateEdgeCursor(const QPoint &globalPos);
    void clearEdgeCursor();
    void updateManualResize(const QPoint &globalPos);
    void endManualResize();
    QRect m_geometryBeforeFullScreen;
    bool m_maximizedBeforeFullScreen = false;
    int m_hoverSecond = -1;
    QPoint m_popupAnchor;
    double m_clipIn = -1;
    double m_clipOut = -1;
};
