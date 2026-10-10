#pragma once

#include <QElapsedTimer>
#include <QHash>
#include <QImage>
#include <QSet>
#include <QSize>
#include <QStringList>
#include <QTimer>
#include <QVariant>
#include <QWidget>

#include <memory>
#include <optional>

class MpvGlSurface;
class QOpenGLWidget;
class QTemporaryDir;
class QThread;
struct mpv_handle;
struct mpv_render_context;

// The video surface: plays media through libmpv and draws the picture with
// libmpv's render API. The picture is drawn with OpenGL by a child surface
// that fills this widget, or, in the software (compatibility) video output,
// by the CPU into an image this widget paints. Widgets stacked over the video
// (lyrics, the OSD, ...) are children of this widget.
class MpvWidget : public QWidget
{
    Q_OBJECT

public:
    enum class VideoOutput {
        OpenGL,   // GPU rendering, hardware decoding: the default
        Software, // drawn by the CPU into a plain widget; for systems where
                  // OpenGL video stays black or the window stops updating
    };

    explicit MpvWidget(QWidget *parent = nullptr);
    ~MpvWidget() override;

    // The video output new players use: --video-output, else
    // $TOPPLAYER_VIDEO_OUTPUT ("opengl" or "software"), else the setting.
    static VideoOutput configuredVideoOutput();
    // The saved setting, used from the next start on.
    static void saveVideoOutput(VideoOutput output);
    // Overrides the setting for this run (the --video-output option);
    // nullopt goes back to the setting.
    static void overrideVideoOutput(std::optional<VideoOutput> output);
    static std::optional<VideoOutput> parseVideoOutput(const QString &name);
    VideoOutput videoOutput() const { return m_output; }
    // The last picture drawn, at its rendered size (for tests and screenshots).
    QImage grabFrame();

    // `subtitles` are added once the file has loaded.
    void loadFile(const QString &pathOrUrl, const QStringList &subtitles = {});
    // Replaces the playlist: plays the first file and queues the rest.
    void loadFiles(const QStringList &files, const QStringList &subtitles = {});
    // Replaces the playlist with `files` and plays entry `start`.
    void playFiles(const QStringList &files, int start);
    // Replaces the playlist with the entries of a playlist file (.m3u, .pls, ...).
    void loadPlaylist(const QString &path);
    // Replaces the playlist with `files`, titled with `titles` (same order;
    // empty ones keep mpv's own title), and plays the first.
    void loadTitledFiles(const QStringList &files, const QStringList &titles);
    // Replaces the playlist with `files` without starting playback; Play
    // starts at entry `current`. With `resumeAt` >= 0, entry `current` is
    // opened paused at `resumeAt` seconds instead.
    void restorePlaylist(const QStringList &files, int current, double resumeAt = -1);
    // Plays a stream, replacing the playlist, or with `append` adds it to the
    // playlist (starting playback if idle). `options` are mpv options that
    // apply to this entry only, e.g. {"referrer": "...", "force-media-title": "..."}.
    void loadStream(const QString &url, const QVariantMap &options, bool append = false);
    // Queues files at playlist index `row` (-1 appends). Starts playback if idle.
    void insertFiles(const QStringList &files, int row = -1);
    // Adds an external subtitle file to the current file and selects it.
    void addSubtitle(const QString &path);
    void adjustVolume(double delta);

    // Transport controls. Unlike the raw mpv commands these also work after
    // stop(): the playlist is kept, and playing resumes from the last entry.
    void play();
    void pause();
    void togglePause();
    // Stops playback and blanks the video surface, keeping the playlist.
    void stop();
    void playlistNext();
    void playlistPrev();
    // True while nothing is loaded (startup, after stop() or an empty playlist).
    bool isIdle() const;
    // True if the loaded file has audio but no video, apart from cover art.
    // Known from fileLoaded() on.
    bool isAudioOnly() const { return m_audioOnly; }
    // The entry that is playing, or played last before a stop; -1 if none.
    int lastPlaylistPos() const { return m_lastPlaylistPos; }

    // Changes whenever the playlist is replaced (loadFiles(), loadStream(), ...)
    // or a replacement is reserved. Work that finishes later, such as a folder
    // scan, compares it with the value it started with to tell whether the
    // user has opened something else in the meantime.
    quint64 replaceTicket() const { return m_replaceTicket; }
    // Reserves a replacement that is still being prepared: earlier tickets
    // become stale. Returns the new ticket.
    quint64 reserveReplace() { return ++m_replaceTicket; }
    // The next replacement carries out the reservation made with
    // reserveReplace(), keeping its ticket, so that adds made since stay valid.
    // Call right before the load.
    void continueReplace() { m_continueReplace = true; }

    // Runs an mpv command asynchronously, e.g. {"seek", "5", "relative"}.
    // Commands run in order; none is dropped, however many are sent at once.
    void command(const QStringList &args);
    // Runs an mpv command with named arguments, e.g. {"name": "loadfile", "url": ...},
    // in order with the other commands.
    void command(const QVariantMap &args);

    // Reads a property synchronously; maps and arrays become QVariantMap/QVariantList.
    QVariant mpvProperty(const QString &name) const;
    QString mpvPropertyString(const QString &name) const;
    // Sets a property asynchronously from its string form, e.g. ("speed", "1.5"),
    // in order with the commands sent before it.
    void setMpvProperty(const QString &name, const QString &value);

    // The OpenGL renderer the video is drawn with, e.g. "Mesa Intel(R) UHD
    // Graphics 620 (KBL GT2)"; empty until the widget is first shown, and
    // with the software video output.
    QString glRenderer() const { return m_glRenderer; }

    // Tracks of `type` ("video", "audio" or "sub") from mpv's track-list.
    QList<QVariantMap> tracks(const QString &type) const;
    // Human-readable track name, e.g. "#2: Commentary [jpn] (aac)".
    static QString trackLabel(const QVariantMap &track);

    // The codec in an mpv log message saying no decoder could be opened for
    // it (prefix "vd"), else empty.
    static QString missingDecoderCodec(const QString &prefix, const QString &text);

    static bool isSubtitleFile(const QString &path);
    // A QFileDialog name filter matching subtitle files.
    static QString subtitleFileFilter();

Q_SIGNALS:
    void titleChanged(const QString &title);
    // Emitted when an observed property changes after its initial value is known.
    // Use this for notifications such as the OSD.
    void propertyChanged(const QString &name, const QVariant &value);
    // Emitted for every report of an observed property, including the initial
    // one; the value is invalid while the property is unavailable. Use this for
    // widgets that mirror player state.
    void propertyUpdated(const QString &name, const QVariant &value);
    // Emitted as mpv starts opening a playlist entry.
    void fileStarted();
    // Emitted when an entry could not be played, e.g. an offline stream.
    // `path` is the entry as it was loaded; `error` is mpv's reason.
    void fileFailed(const QString &path, const QString &error);
    // Emitted once the entry's tracks are known (see isAudioOnly()).
    void fileLoaded();
    // Emitted once playback resumes after a user seek.
    void seeked();
    // Emitted once per file, when the video's display size is first known.
    // Not emitted for audio files, whose cover art or visualization is no
    // reason to resize the window.
    void videoSizeKnown(const QSize &size);
    // The OpenGL picture keeps failing to reach the screen while the window
    // is in front (the video stays black, the window only updates when the
    // pointer moves). The software video output avoids it. Emitted once.
    void renderStalled();
    // The video of the file can't be shown, e.g. its codec is missing from
    // the system's FFmpeg. `codec` is mpv's codec name, if known.
    void videoDecodeFailed(const QString &codec, const QString &message);

protected:
    void paintEvent(QPaintEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;

private Q_SLOTS:
    void processMpvEvents();
    void onRenderUpdate();
    void onFrameSwapped();

private:
    friend class MpvGlSurface;
    // OpenGL: called by the surface.
    void initializeGl();
    void paintGl();
    // Software: creates the render context and its thread.
    void initializeSoftware();
    // Software: renders a frame on the render thread, then paints it.
    void requestSoftwareFrame();
    void onSoftwareFrame(const QImage &frame);
    // Commands deferred until the render context existed.
    void runPendingLoads();
    // Counts a frame that took `ms` to reach the screen (OpenGL).
    void noteFrameLatency(qint64 ms);
    bool windowInFront() const;
    void repaintVideo();

    static void onMpvWakeup(void *ctx);
    static void onMpvRenderUpdate(void *ctx);
    struct QueuedCommand {
        QStringList args;
        QVariantMap named; // used instead of args if not empty
        quint64 reply = 0;
    };
    // Runs `commands` now, or once the render context exists.
    void runOrDefer(const QList<QStringList> &commands);
    void runOrDefer(const QVariantMap &command);
    // Plays playlist entry `index`, clamped to the playlist. Returns false if it is empty.
    bool playIndex(int index);
    // Commands that add `files` to the playlist like loadfile with `flag`
    // ("replace", "append" or "append-play"). Runs of several files are
    // written to a temporary playlist and added with a single loadlist, so a
    // big folder costs mpv one command and the UI one playlist update.
    QList<QStringList> queueCommands(const QStringList &files, const QString &flag);
    // Writes `files` to a new temporary M3U playlist; returns its path, or an
    // empty string on failure. It is deleted once mpv has read it.
    QString writeBatch(const QStringList &files, const QStringList &titles = {});
    // Sends queued commands while fewer than kMaxPendingReplies are unanswered.
    void sendQueuedCommands();
    // Bookkeeping for a command sequence that replaces the playlist: the new
    // ticket, and `commands` preceded by clearing the pause flag, so newly
    // opened media plays even if the last file was paused or ran out.
    QList<QStringList> replacing(QList<QStringList> commands);
    // Emits the latest time-pos now, if one is waiting.
    void flushTimePos();

    mpv_handle *m_mpv = nullptr;
    mpv_render_context *m_renderCtx = nullptr;
    VideoOutput m_output = VideoOutput::OpenGL;
    MpvGlSurface *m_surface = nullptr;
    // Software output: the render thread, an object living in it to queue
    // work on, the last frame, and whether a frame is being or should be drawn.
    QThread *m_renderThread = nullptr;
    QObject *m_renderWorker = nullptr;
    QImage m_frame;
    bool m_frameBusy = false;
    bool m_frameDirty = false;
    // OpenGL: when a frame was asked for that hasn't reached the screen yet,
    // slow frames counted, and whether renderStalled() went out.
    QElapsedTimer m_frameRequested;
    QTimer *m_stallTimer = nullptr;
    int m_slowFrames = 0;
    bool m_stallReported = false;
    // Codecs a decode failure was reported for.
    QSet<QString> m_reportedCodecs;
    // Files requested before the GL context existed; loading them earlier
    // would make mpv's video output fail to initialize.
    // Commands that start playback, deferred until the render context exists.
    QList<QueuedCommand> m_pendingLoads;
    QStringList m_pendingSubtitles;
    QString m_glRenderer;
    QSet<QString> m_initializedProperties;
    QSet<QString> m_stateProperties;
    bool m_fileLoaded = false;
    bool m_seeking = false;
    bool m_awaitingVideoSize = false;
    bool m_audioOnly = false;
    // The "start" option was set for a resumed entry and must not apply to later files.
    bool m_resetStart = false;
    // Mirrors idle-active for painting, which must not block on mpv.
    bool m_idle = true;
    // Playlist entry that played last; mpv forgets it on stop.
    int m_lastPlaylistPos = -1;
    // Temporary playlists from writeBatch(): not yet sent, and sent awaiting
    // their command reply (by reply id).
    std::unique_ptr<QTemporaryDir> m_batchDir;
    QSet<QString> m_batchFiles;
    QHash<quint64, QString> m_batchReplies;
    quint64 m_nextBatch = 1;
    // mpv refuses asynchronous commands while ~1000 replies are pending, so
    // long runs of commands (moving thousands of entries) wait here instead.
    QList<QueuedCommand> m_commandQueue;
    int m_pendingReplies = 0;
    // Reply id of the loadlist the queue waits for, or 0.
    quint64 m_awaitedBatch = 0;
    quint64 m_replaceTicket = 0;
    bool m_continueReplace = false;
    // time-pos changes with every frame; widgets hear of it at most every
    // kTimePosIntervalMs (and at once after a seek or a new file).
    QTimer *m_timePosTimer = nullptr;
    QVariant m_timePos;
    bool m_timePosPending = false;
};
