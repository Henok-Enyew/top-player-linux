#include "MpvWidget.h"
#include "MpvHelpers.h"

#include <QByteArray>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QMetaObject>
#include <QOpenGLContext>
#include <QOpenGLFunctions>
#include <QPalette>
#include <QTemporaryDir>

#include <mpv/client.h>
#include <mpv/render_gl.h>

#include <algorithm>
#include <stdexcept>
#include <utility>
#include <vector>

namespace {

void *getProcAddress(void *, const char *name)
{
    QOpenGLContext *ctx = QOpenGLContext::currentContext();
    if (!ctx)
        return nullptr;
    return reinterpret_cast<void *>(ctx->getProcAddress(QByteArray(name)));
}

QVariant nodeToVariant(const mpv_node *node)
{
    switch (node->format) {
    case MPV_FORMAT_STRING:
        return QString::fromUtf8(node->u.string);
    case MPV_FORMAT_FLAG:
        return node->u.flag != 0;
    case MPV_FORMAT_INT64:
        return static_cast<qlonglong>(node->u.int64);
    case MPV_FORMAT_DOUBLE:
        return node->u.double_;
    case MPV_FORMAT_NODE_ARRAY: {
        QVariantList list;
        for (int i = 0; i < node->u.list->num; ++i)
            list.append(nodeToVariant(&node->u.list->values[i]));
        return list;
    }
    case MPV_FORMAT_NODE_MAP: {
        QVariantMap map;
        for (int i = 0; i < node->u.list->num; ++i)
            map.insert(QString::fromUtf8(node->u.list->keys[i]), nodeToVariant(&node->u.list->values[i]));
        return map;
    }
    default:
        return {};
    }
}

// Player state mirrored by widgets through propertyUpdated() only.
constexpr const char *kStateProperties[] = {
    "time-pos", "duration", "playlist", "chapter-list", "path", "idle-active", "playlist-pos", "metadata",
    "shuffle", "loop-file", "loop-playlist", "video-unscaled", "video-aspect-override",
};

// Properties whose changes are also forwarded through propertyChanged().
constexpr const char *kObservedProperties[] = {
    "volume", "mute", "speed", "pause", "audio-delay", "sub-delay", "sub-scale", "sub-pos",
};

// Whether `entry` can go into a temporary M3U playlist and come back out
// unchanged. Streams are loaded one by one: mpv trusts entries of a playlist
// file less than ones it is given directly.
bool isBatchable(const QString &entry)
{
    if (entry.isEmpty() || entry.contains(QLatin1Char('\n')) || entry.contains(QLatin1Char('\r'))
        || entry.trimmed() != entry || entry.startsWith(QLatin1Char('#')))
        return false;
    return !entry.contains(QLatin1String("://")) || entry.startsWith(QLatin1String("file://"));
}

// How often widgets mirroring the playback position are updated: smooth
// enough for the seekbar, and a fraction of the per-frame reports.
constexpr int kTimePosIntervalMs = 100;

// Well below the ~1000 unanswered requests mpv accepts per client.
constexpr int kMaxPendingReplies = 256;

const QStringList kSubtitleExtensions{
    QStringLiteral("srt"), QStringLiteral("ass"), QStringLiteral("ssa"), QStringLiteral("vtt"),
    QStringLiteral("sub"), QStringLiteral("idx"), QStringLiteral("sup"), QStringLiteral("smi"),
};

} // namespace

MpvWidget::MpvWidget(QWidget *parent)
    : QOpenGLWidget(parent)
{
    m_mpv = mpv_create();
    if (!m_mpv)
        throw std::runtime_error("could not create mpv context");

    // Rendering happens through the render API, and all input is handled by Qt.
    mpv_set_option_string(m_mpv, "vo", "libmpv");
    mpv_set_option_string(m_mpv, "hwdec", "auto-safe");
    mpv_set_option_string(m_mpv, "keep-open", "yes");
    mpv_set_option_string(m_mpv, "input-default-bindings", "no");
    mpv_set_option_string(m_mpv, "input-vo-keyboard", "no");
    // The OSD is drawn by Qt; mpv only renders subtitles.
    mpv_set_option_string(m_mpv, "osd-level", "0");
    mpv_set_option_string(m_mpv, "osd-bar", "no");
    // Like PotPlayer, pick up subtitles next to the video or in a subtitle folder.
    mpv_set_option_string(m_mpv, "sub-auto", "fuzzy");
    mpv_set_option_string(m_mpv, "sub-file-paths", "sub:subs:subtitles:Subs:Subtitles");
    // Decode with as many threads as there are cores, and keep enough of the
    // stream around that seeking back and forth is served from memory.
    mpv_set_option_string(m_mpv, "vd-lavc-threads", "0");
    mpv_set_option_string(m_mpv, "demuxer-max-bytes", "150MiB");
    mpv_set_option_string(m_mpv, "demuxer-max-back-bytes", "75MiB");
    mpv_set_option_string(m_mpv, "terminal", "yes");
    mpv_set_option_string(m_mpv, "msg-level", "all=warn");

    if (mpv_initialize(m_mpv) < 0)
        throw std::runtime_error("could not initialize mpv context");

    mpv_observe_property(m_mpv, 0, "media-title", MPV_FORMAT_STRING);
    for (const char *name : kObservedProperties)
        mpv_observe_property(m_mpv, 0, name, MPV_FORMAT_NODE);
    for (const char *name : kStateProperties)
        mpv_observe_property(m_mpv, 0, name, MPV_FORMAT_NODE);
    for (const char *name : kStateProperties)
        m_stateProperties.insert(QString::fromLatin1(name));
    mpv_set_wakeup_callback(m_mpv, &MpvWidget::onMpvWakeup, this);

    m_timePosTimer = new QTimer(this);
    m_timePosTimer->setSingleShot(true);
    m_timePosTimer->setInterval(kTimePosIntervalMs);
    connect(m_timePosTimer, &QTimer::timeout, this, &MpvWidget::flushTimePos);
}

MpvWidget::~MpvWidget()
{
    mpv_set_wakeup_callback(m_mpv, nullptr, nullptr);
    makeCurrent();
    if (m_renderCtx)
        mpv_render_context_free(m_renderCtx);
    doneCurrent();
    mpv_terminate_destroy(m_mpv);
}

void MpvWidget::loadFile(const QString &pathOrUrl, const QStringList &subtitles)
{
    loadFiles({pathOrUrl}, subtitles);
}

void MpvWidget::loadFiles(const QStringList &files, const QStringList &subtitles)
{
    if (files.isEmpty())
        return;
    m_pendingSubtitles = subtitles;
    const QList<QStringList> commands = replacing(queueCommands(files, QStringLiteral("replace")));

    if (!m_renderCtx) {
        m_pendingLoads.clear();
        for (const QStringList &cmd : commands)
            m_pendingLoads.append({cmd, {}});
        return;
    }
    for (const QStringList &cmd : std::as_const(commands))
        command(cmd);
}

void MpvWidget::loadStream(const QString &url, const QVariantMap &options, bool append)
{
    QVariantMap cmd{
        {QStringLiteral("name"), QStringLiteral("loadfile")},
        {QStringLiteral("url"), url},
        {QStringLiteral("flags"), append ? QStringLiteral("append-play") : QStringLiteral("replace")},
    };
    if (!options.isEmpty())
        cmd.insert(QStringLiteral("options"), options);
    if (append) {
        runOrDefer(cmd);
        return;
    }
    m_pendingSubtitles.clear();
    if (!m_renderCtx)
        m_pendingLoads.clear();
    runOrDefer(replacing({}));
    runOrDefer(cmd);
}

void MpvWidget::playFiles(const QStringList &files, int start)
{
    if (start <= 0) {
        loadFiles(files);
        return;
    }
    m_pendingSubtitles.clear();
    // Queue everything first, so the entries before `start` never begin playing.
    QList<QStringList> commands{{QStringLiteral("stop")}};
    commands.append(queueCommands(files, QStringLiteral("append")));
    start = std::min(start, static_cast<int>(files.size()) - 1);
    commands.append({QStringLiteral("playlist-play-index"), QString::number(start)});
    runOrDefer(replacing(commands));
}

void MpvWidget::loadPlaylist(const QString &path)
{
    m_pendingSubtitles.clear();
    const QList<QStringList> commands = replacing({{QStringLiteral("loadlist"), path, QStringLiteral("replace")}});
    if (!m_renderCtx)
        m_pendingLoads.clear();
    runOrDefer(commands);
}

void MpvWidget::loadTitledFiles(const QStringList &files, const QStringList &titles)
{
    if (files.isEmpty())
        return;
    // One temporary playlist carries the titles, as the original file did.
    const QString list = writeBatch(files, titles);
    if (list.isEmpty()) {
        loadFiles(files);
        return;
    }
    m_pendingSubtitles.clear();
    if (!m_renderCtx)
        m_pendingLoads.clear();
    runOrDefer(replacing({{QStringLiteral("loadlist"), list, QStringLiteral("replace")}}));
}

void MpvWidget::restorePlaylist(const QStringList &files, int current, double resumeAt)
{
    if (files.isEmpty())
        return;
    m_pendingSubtitles.clear();
    // "append" queues without starting playback, unlike "append-play".
    QList<QStringList> commands{{QStringLiteral("stop")}};
    commands.append(queueCommands(files, QStringLiteral("append")));
    current = std::clamp(current, -1, static_cast<int>(files.size()) - 1);
    m_lastPlaylistPos = current;
    // Restored paused on purpose (see resumeAt); only the ticket changes.
    if (!std::exchange(m_continueReplace, false))
        ++m_replaceTicket;
    if (resumeAt >= 0 && current >= 0) {
        // The "start" option is global; it is reset once this file has loaded.
        m_resetStart = true;
        commands.append({QStringLiteral("set"), QStringLiteral("start"), QString::number(resumeAt, 'f', 3)});
        commands.append({QStringLiteral("set"), QStringLiteral("pause"), QStringLiteral("yes")});
        commands.append({QStringLiteral("playlist-play-index"), QString::number(current)});
    }
    runOrDefer(commands);
}

void MpvWidget::runOrDefer(const QList<QStringList> &commands)
{
    if (!m_renderCtx) {
        for (const QStringList &cmd : commands)
            m_pendingLoads.append({cmd, {}});
        return;
    }
    for (const QStringList &cmd : commands)
        command(cmd);
}

void MpvWidget::runOrDefer(const QVariantMap &cmd)
{
    if (!m_renderCtx)
        m_pendingLoads.append({{}, cmd});
    else
        command(cmd);
}

void MpvWidget::insertFiles(const QStringList &files, int row)
{
    if (files.isEmpty())
        return;
    QList<QStringList> commands = queueCommands(files, QStringLiteral("append-play"));
    // append-play starts an idle player: like an open, that should not be paused.
    if (!m_renderCtx || isIdle())
        commands.prepend({QStringLiteral("set"), QStringLiteral("pause"), QStringLiteral("no")});
    if (!m_renderCtx) {
        runOrDefer(commands);
        return;
    }
    // Commands run in order, so the appended entries are at count + i when they are moved.
    // (The count misses entries still waiting in m_commandQueue, which only
    // holds anything after hundreds of commands in a row.)
    const int count = mpvProperty(QStringLiteral("playlist-count")).toInt();
    for (const QStringList &cmd : commands)
        command(cmd);
    if (row >= 0 && row < count) {
        for (qsizetype i = 0; i < files.size(); ++i)
            command({QStringLiteral("playlist-move"), QString::number(count + i), QString::number(row + i)});
    }
}

QList<QStringList> MpvWidget::queueCommands(const QStringList &files, const QString &flag)
{
    QList<QStringList> commands;
    QString nextFlag = flag;
    const auto add = [&](const QString &name, const QString &target) {
        commands.append({name, target, nextFlag});
        // Only the first command replaces the playlist; the rest add to it.
        if (nextFlag == QLatin1String("replace"))
            nextFlag = QStringLiteral("append");
    };
    QStringList batch;
    const auto flush = [&] {
        const QString list = batch.size() > 1 ? writeBatch(batch) : QString();
        if (!list.isEmpty()) {
            add(QStringLiteral("loadlist"), list);
        } else {
            for (const QString &file : std::as_const(batch))
                add(QStringLiteral("loadfile"), file);
        }
        batch.clear();
    };
    for (const QString &file : files) {
        if (isBatchable(file)) {
            batch.append(file);
        } else {
            flush();
            add(QStringLiteral("loadfile"), file);
        }
    }
    flush();
    return commands;
}

QList<QStringList> MpvWidget::replacing(QList<QStringList> commands)
{
    if (!std::exchange(m_continueReplace, false))
        ++m_replaceTicket;
    commands.prepend({QStringLiteral("set"), QStringLiteral("pause"), QStringLiteral("no")});
    return commands;
}

QString MpvWidget::writeBatch(const QStringList &files, const QStringList &titles)
{
    if (!m_batchDir)
        m_batchDir = std::make_unique<QTemporaryDir>(QDir::tempPath() + QStringLiteral("/top-player-XXXXXX"));
    if (!m_batchDir->isValid())
        return {};
    const QString path = m_batchDir->filePath(QStringLiteral("queue-%1.m3u8").arg(m_nextBatch++));
    QByteArray data("#EXTM3U\n");
    for (qsizetype i = 0; i < files.size(); ++i) {
        const QString &file = files[i];
        QString title = titles.value(i);
        if (!title.isEmpty()) {
            // A line break would end the #EXTINF line early.
            title.replace(QLatin1Char('\n'), QLatin1Char(' ')).remove(QLatin1Char('\r'));
            data += "#EXTINF:-1," + title.toUtf8() + '\n';
        }
        // Relative entries would be resolved against the playlist's folder.
        const bool url = file.contains(QLatin1String("://"));
        data += (url || QDir::isAbsolutePath(file) ? file : QFileInfo(file).absoluteFilePath()).toUtf8();
        data += '\n';
    }
    QFile out(path);
    if (!out.open(QIODevice::WriteOnly | QIODevice::Truncate) || out.write(data) != data.size()) {
        out.remove();
        return {};
    }
    m_batchFiles.insert(path);
    return path;
}

void MpvWidget::addSubtitle(const QString &path)
{
    command({QStringLiteral("sub-add"), path, QStringLiteral("select")});
}

void MpvWidget::adjustVolume(double delta)
{
    command({QStringLiteral("add"), QStringLiteral("volume"), QString::number(delta)});
}

bool MpvWidget::isIdle() const
{
    // Asked live: the observed value can lag behind a just-started file.
    return mpvProperty(QStringLiteral("idle-active")).toBool();
}

void MpvWidget::play()
{
    if (isIdle()) {
        // The pause flag outlives stop; clear it so the entry doesn't start paused.
        setMpvProperty(QStringLiteral("pause"), QStringLiteral("no"));
        playIndex(std::max(m_lastPlaylistPos, 0));
        return;
    }
    // With keep-open, mpv pauses on the last frame; playing again starts over.
    if (mpvProperty(QStringLiteral("eof-reached")).toBool())
        command({QStringLiteral("seek"), QStringLiteral("0"), QStringLiteral("absolute")});
    setMpvProperty(QStringLiteral("pause"), QStringLiteral("no"));
}

void MpvWidget::pause()
{
    // Pausing while idle would make the next file start paused.
    if (!isIdle())
        setMpvProperty(QStringLiteral("pause"), QStringLiteral("yes"));
}

void MpvWidget::togglePause()
{
    if (isIdle() || mpvProperty(QStringLiteral("pause")).toBool())
        play();
    else
        pause();
}

void MpvWidget::stop()
{
    // The playlist-pos notification may still be queued; remember the live entry.
    const int pos = mpvProperty(QStringLiteral("playlist-pos")).toInt();
    if (pos >= 0)
        m_lastPlaylistPos = pos;
    command({QStringLiteral("stop"), QStringLiteral("keep-playlist")});
}

void MpvWidget::playlistNext()
{
    // mpv has no current entry to step from once stopped.
    if (isIdle()) {
        playIndex(m_lastPlaylistPos + 1);
        return;
    }
    // Skipping to another song means wanting to hear it, even from pause
    // (unless there is nothing to skip to).
    bool resume = false;
    if (mpvProperty(QStringLiteral("pause")).toBool()) {
        const int pos = mpvProperty(QStringLiteral("playlist-pos")).toInt();
        const int count = mpvProperty(QStringLiteral("playlist-count")).toInt();
        const QString loop = mpvPropertyString(QStringLiteral("loop-playlist"));
        resume = pos + 1 < count || (count > 1 && !loop.isEmpty() && loop != QLatin1String("no"));
    }
    command({QStringLiteral("playlist-next")});
    if (resume)
        setMpvProperty(QStringLiteral("pause"), QStringLiteral("no"));
}

void MpvWidget::playlistPrev()
{
    if (isIdle()) {
        playIndex(std::max(m_lastPlaylistPos - 1, 0));
        return;
    }
    bool resume = false;
    if (mpvProperty(QStringLiteral("pause")).toBool()) {
        const int pos = mpvProperty(QStringLiteral("playlist-pos")).toInt();
        const int count = mpvProperty(QStringLiteral("playlist-count")).toInt();
        const QString loop = mpvPropertyString(QStringLiteral("loop-playlist"));
        resume = pos > 0 || (count > 1 && !loop.isEmpty() && loop != QLatin1String("no"));
    }
    command({QStringLiteral("playlist-prev")});
    if (resume)
        setMpvProperty(QStringLiteral("pause"), QStringLiteral("no"));
}

bool MpvWidget::playIndex(int index)
{
    const int count = mpvProperty(QStringLiteral("playlist-count")).toInt();
    if (count <= 0)
        return false;
    command({QStringLiteral("playlist-play-index"), QString::number(std::clamp(index, 0, count - 1))});
    return true;
}

void MpvWidget::command(const QStringList &args)
{
    quint64 reply = 0;
    if (args.size() > 1 && args.first() == QLatin1String("loadlist") && m_batchFiles.remove(args[1])) {
        reply = m_nextBatch++;
        m_batchReplies.insert(reply, args[1]);
    }
    m_commandQueue.append({args, {}, reply});
    sendQueuedCommands();
}

void MpvWidget::command(const QVariantMap &args)
{
    m_commandQueue.append({{}, args, 0});
    sendQueuedCommands();
}

void MpvWidget::sendQueuedCommands()
{
    // mpv reads a playlist file in a thread of its own, so commands after a
    // loadlist (playing an entry, moving the new ones) wait for it to finish.
    while (!m_commandQueue.isEmpty() && m_pendingReplies < kMaxPendingReplies && !m_awaitedBatch) {
        const QueuedCommand next = m_commandQueue.takeFirst();
        const int result = next.named.isEmpty() ? mpvCommandAsync(m_mpv, next.args, next.reply)
                                                : mpvCommandNodeAsync(m_mpv, next.named, next.reply);
        if (result >= 0) {
            ++m_pendingReplies;
            m_awaitedBatch = next.reply;
        } else if (next.reply) {
            // An invalid command; there will be no reply.
            QFile::remove(m_batchReplies.take(next.reply));
        }
    }
}

QVariant MpvWidget::mpvProperty(const QString &name) const
{
    mpv_node node;
    if (mpv_get_property(m_mpv, name.toUtf8().constData(), MPV_FORMAT_NODE, &node) < 0)
        return {};
    QVariant value = nodeToVariant(&node);
    mpv_free_node_contents(&node);
    return value;
}

QString MpvWidget::mpvPropertyString(const QString &name) const
{
    char *value = mpv_get_property_string(m_mpv, name.toUtf8().constData());
    if (!value)
        return {};
    QString result = QString::fromUtf8(value);
    mpv_free(value);
    return result;
}

void MpvWidget::setMpvProperty(const QString &name, const QString &value)
{
    // The set command parses the value like setting the property as a string.
    command({QStringLiteral("set"), name, value});
}

QList<QVariantMap> MpvWidget::tracks(const QString &type) const
{
    QList<QVariantMap> result;
    for (const QVariant &entry : mpvProperty(QStringLiteral("track-list")).toList()) {
        QVariantMap track = entry.toMap();
        if (track.value(QStringLiteral("type")).toString() == type)
            result.append(std::move(track));
    }
    return result;
}

QString MpvWidget::trackLabel(const QVariantMap &track)
{
    QString label = QStringLiteral("#%1").arg(track.value(QStringLiteral("id")).toLongLong());
    const QString title = track.value(QStringLiteral("title")).toString();
    const QString lang = track.value(QStringLiteral("lang")).toString();
    const QString codec = track.value(QStringLiteral("codec")).toString();
    if (!title.isEmpty())
        label += QStringLiteral(": ") + title;
    if (!lang.isEmpty())
        label += QStringLiteral(" [%1]").arg(lang);
    if (!codec.isEmpty())
        label += QStringLiteral(" (%1)").arg(codec);
    if (track.value(QStringLiteral("external")).toBool())
        label += QStringLiteral(" - external");
    return label;
}

bool MpvWidget::isSubtitleFile(const QString &path)
{
    return kSubtitleExtensions.contains(QFileInfo(path).suffix().toLower());
}

QString MpvWidget::subtitleFileFilter()
{
    QStringList patterns;
    for (const QString &ext : kSubtitleExtensions)
        patterns.append(QStringLiteral("*.") + ext);
    return tr("Subtitles (%1);;All Files (*)").arg(patterns.join(QLatin1Char(' ')));
}

void MpvWidget::initializeGL()
{
    mpv_opengl_init_params glInit{&getProcAddress, nullptr};
    std::vector<mpv_render_param> params{
        {MPV_RENDER_PARAM_API_TYPE, const_cast<char *>(MPV_RENDER_API_TYPE_OPENGL)},
        {MPV_RENDER_PARAM_OPENGL_INIT_PARAMS, &glInit},
    };

    // Hardware decoding interop needs the native display connection.
#if QT_CONFIG(xcb)
    if (auto *x11 = qGuiApp->nativeInterface<QNativeInterface::QX11Application>())
        params.push_back({MPV_RENDER_PARAM_X11_DISPLAY, x11->display()});
#endif
#if QT_VERSION >= QT_VERSION_CHECK(6, 5, 0) && QT_CONFIG(wayland)
    if (auto *wayland = qGuiApp->nativeInterface<QNativeInterface::QWaylandApplication>())
        params.push_back({MPV_RENDER_PARAM_WL_DISPLAY, wayland->display()});
#endif
    params.push_back({MPV_RENDER_PARAM_INVALID, nullptr});

    if (mpv_render_context_create(&m_renderCtx, m_mpv, params.data()) < 0)
        throw std::runtime_error("failed to initialize mpv GL context");

    mpv_render_context_set_update_callback(m_renderCtx, &MpvWidget::onMpvRenderUpdate, this);
    connect(this, &QOpenGLWidget::frameSwapped, this, &MpvWidget::onFrameSwapped, Qt::UniqueConnection);

    m_glRenderer = QString::fromLatin1(reinterpret_cast<const char *>(context()->functions()->glGetString(GL_RENDERER)));

    for (const QueuedCommand &cmd : std::exchange(m_pendingLoads, {})) {
        if (cmd.named.isEmpty())
            command(cmd.args);
        else
            command(cmd.named);
    }
}

void MpvWidget::paintGL()
{
    if (!m_renderCtx)
        return;

    QOpenGLFunctions *gl = context()->functions();
    if (m_idle) {
        // Nothing is loaded: show the skin's background instead of the last frame.
        const QColor background = palette().color(QPalette::Window);
        gl->glClearColor(background.redF(), background.greenF(), background.blueF(), 1);
        gl->glClear(GL_COLOR_BUFFER_BIT);
        return;
    }

    const qreal dpr = devicePixelRatioF();
    mpv_opengl_fbo fbo{
        static_cast<int>(defaultFramebufferObject()),
        static_cast<int>(width() * dpr),
        static_cast<int>(height() * dpr),
        0,
    };
    int flipY = 1;
    mpv_render_param params[]{
        {MPV_RENDER_PARAM_OPENGL_FBO, &fbo},
        {MPV_RENDER_PARAM_FLIP_Y, &flipY},
        {MPV_RENDER_PARAM_INVALID, nullptr},
    };
    mpv_render_context_render(m_renderCtx, params);

    // mpv may leave the framebuffer's alpha channel at 0 under the video. Qt
    // blends this widget's texture when composing the window (e.g. with
    // overlays on top), which would then drop the video and keep only
    // subtitles, so force the alpha channel to opaque.
    gl->glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_TRUE);
    gl->glClearColor(0, 0, 0, 1);
    gl->glClear(GL_COLOR_BUFFER_BIT);
    gl->glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
}

void MpvWidget::onFrameSwapped()
{
    // Lets mpv time its frames against the real presentation.
    if (m_renderCtx && !m_idle)
        mpv_render_context_report_swap(m_renderCtx);
}

void MpvWidget::processMpvEvents()
{
    while (m_mpv) {
        mpv_event *event = mpv_wait_event(m_mpv, 0);
        if (event->event_id == MPV_EVENT_NONE)
            break;

        switch (event->event_id) {
        case MPV_EVENT_PROPERTY_CHANGE: {
            auto *prop = static_cast<mpv_event_property *>(event->data);
            const QString name = QString::fromUtf8(prop->name);
            if (name == QLatin1String("media-title")) {
                QString title;
                if (prop->format == MPV_FORMAT_STRING && prop->data)
                    title = QString::fromUtf8(*static_cast<char **>(prop->data));
                Q_EMIT titleChanged(title);
            } else {
                const QVariant value = prop->format == MPV_FORMAT_NODE
                    ? nodeToVariant(static_cast<mpv_node *>(prop->data))
                    : QVariant();
                if (name == QLatin1String("idle-active")) {
                    m_idle = value.toBool();
                    update();
                } else if (name == QLatin1String("playlist-pos") && value.toInt() >= 0) {
                    m_lastPlaylistPos = value.toInt();
                } else if (name == QLatin1String("time-pos")) {
                    m_timePos = value;
                    m_timePosPending = true;
                    // The first report after a pause goes out at once; the
                    // rest at most every kTimePosIntervalMs.
                    if (!m_timePosTimer->isActive()) {
                        flushTimePos();
                        m_timePosTimer->start();
                    }
                    break;
                }
                Q_EMIT propertyUpdated(name, value);
                if (!m_stateProperties.contains(name)) {
                    // mpv reports every observed property once on startup; that is not a change.
                    if (!m_initializedProperties.contains(name))
                        m_initializedProperties.insert(name);
                    else if (value.isValid())
                        Q_EMIT propertyChanged(name, value);
                }
            }
            break;
        }
        case MPV_EVENT_COMMAND_REPLY:
            --m_pendingReplies;
            // mpv has read the temporary playlist.
            if (const QString batch = m_batchReplies.take(event->reply_userdata); !batch.isEmpty())
                QFile::remove(batch);
            if (event->reply_userdata == m_awaitedBatch)
                m_awaitedBatch = 0;
            sendQueuedCommands();
            break;
        case MPV_EVENT_START_FILE:
            flushTimePos();
            m_fileLoaded = false;
            m_audioOnly = false;
            m_seeking = false;
            m_awaitingVideoSize = true;
            Q_EMIT fileStarted();
            break;
        case MPV_EVENT_END_FILE: {
            const auto *end = static_cast<const mpv_event_end_file *>(event->data);
            if (end->reason != MPV_END_FILE_REASON_ERROR)
                break;
            // The entry is still in the playlist; "path" was never set for it.
            QString path;
            for (const QVariant &entry : mpvProperty(QStringLiteral("playlist")).toList()) {
                const QVariantMap map = entry.toMap();
                if (map.value(QStringLiteral("id")).toLongLong() == end->playlist_entry_id) {
                    path = map.value(QStringLiteral("filename")).toString();
                    break;
                }
            }
            Q_EMIT fileFailed(path, QString::fromUtf8(mpv_error_string(end->error)));
            break;
        }
        case MPV_EVENT_FILE_LOADED: {
            m_fileLoaded = true;
            // Cover art (embedded or a cover file next to it) shows up as an
            // "albumart" video track; it doesn't make a file a video.
            bool hasAudio = false;
            bool hasVideo = false;
            for (const QVariant &entry : mpvProperty(QStringLiteral("track-list")).toList()) {
                const QVariantMap track = entry.toMap();
                const QString type = track.value(QStringLiteral("type")).toString();
                if (type == QLatin1String("audio"))
                    hasAudio = true;
                else if (type == QLatin1String("video") && !track.value(QStringLiteral("albumart")).toBool())
                    hasVideo = true;
            }
            m_audioOnly = hasAudio && !hasVideo;
            if (std::exchange(m_resetStart, false))
                setMpvProperty(QStringLiteral("start"), QStringLiteral("none"));
            for (const QString &subtitle : std::exchange(m_pendingSubtitles, {}))
                addSubtitle(subtitle);
            Q_EMIT fileLoaded();
            break;
        }
        case MPV_EVENT_SEEK:
            m_seeking = m_fileLoaded;
            break;
        case MPV_EVENT_VIDEO_RECONFIG:
            if (m_awaitingVideoSize && !m_audioOnly) {
                const QSize size(mpvProperty(QStringLiteral("dwidth")).toInt(),
                                 mpvProperty(QStringLiteral("dheight")).toInt());
                if (!size.isEmpty()) {
                    m_awaitingVideoSize = false;
                    Q_EMIT videoSizeKnown(size);
                }
            }
            break;
        case MPV_EVENT_PLAYBACK_RESTART:
            // Show where a seek landed without waiting for the next tick.
            flushTimePos();
            if (m_seeking) {
                m_seeking = false;
                Q_EMIT seeked();
            }
            break;
        default:
            break;
        }
    }
}

void MpvWidget::flushTimePos()
{
    if (!std::exchange(m_timePosPending, false))
        return;
    Q_EMIT propertyUpdated(QStringLiteral("time-pos"), m_timePos);
}

void MpvWidget::onRenderUpdate()
{
    update();
}

void MpvWidget::onMpvWakeup(void *ctx)
{
    // Called from an mpv thread: hop back onto the GUI thread.
    QMetaObject::invokeMethod(static_cast<MpvWidget *>(ctx), "processMpvEvents", Qt::QueuedConnection);
}

void MpvWidget::onMpvRenderUpdate(void *ctx)
{
    QMetaObject::invokeMethod(static_cast<MpvWidget *>(ctx), "onRenderUpdate", Qt::QueuedConnection);
}
