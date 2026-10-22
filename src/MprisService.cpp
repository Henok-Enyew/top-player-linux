#include "MprisService.h"
#include "MpvWidget.h"

#include <QApplication>
#include <QDBusConnection>
#include <QDBusMessage>
#include <QFileInfo>
#include <QUrl>
#include <QWidget>

#include <algorithm>
#include <cmath>
#include <utility>

namespace {

const QString kObjectPath = QStringLiteral("/org/mpris/MediaPlayer2");
const QString kPlayerInterface = QStringLiteral("org.mpris.MediaPlayer2.Player");
constexpr double kMicro = 1e6;

QDBusObjectPath trackId(int index)
{
    return QDBusObjectPath(QStringLiteral("/org/github/topplayer/track/%1").arg(std::max(index, 0)));
}

} // namespace

MprisService::MprisService(MpvWidget *mpv, QWidget *window, QObject *parent)
    : QObject(parent)
    , m_mpv(mpv)
    , m_window(window)
{
    // The adaptors are children of this object and exported along with it.
    new MprisRootAdaptor(this);
    auto *player = new MprisPlayerAdaptor(this);
    connect(this, &MprisService::seeked, player, &MprisPlayerAdaptor::Seeked);

    connect(m_mpv, &MpvWidget::propertyUpdated, this, &MprisService::onPropertyUpdated);
    connect(m_mpv, &MpvWidget::titleChanged, this, [this] { notifyPlayerChanged({QStringLiteral("Metadata")}); });
    connect(m_mpv, &MpvWidget::seeked, this, [this] { Q_EMIT seeked(positionUs()); });

    QDBusConnection bus = QDBusConnection::sessionBus();
    if (!bus.isConnected())
        return;
    // A second instance takes a name of its own, as the specification suggests.
    if (!takeName())
        return;
    m_registered = bus.registerObject(kObjectPath, this, QDBusConnection::ExportAdaptors);
    // The desktop sends the media keys to one player, usually the one that
    // showed up last. When another window of the player closes, this one
    // shows up again (or takes over the plain name), so the keys keep
    // reaching a window that is still open.
    bus.connect(QStringLiteral("org.freedesktop.DBus"), QStringLiteral("/org/freedesktop/DBus"),
                QStringLiteral("org.freedesktop.DBus"), QStringLiteral("NameOwnerChanged"), this,
                SLOT(onNameOwnerChanged(QString,QString,QString)));
}

MprisService::~MprisService()
{
    if (!m_registered)
        return;
    QDBusConnection bus = QDBusConnection::sessionBus();
    bus.unregisterObject(kObjectPath);
    // Freed at once, so another window can take it over before this process ends.
    bus.unregisterService(m_name);
}

bool MprisService::takeName()
{
    QDBusConnection bus = QDBusConnection::sessionBus();
    m_name = serviceName();
    if (bus.registerService(m_name))
        return true;
    m_name += QStringLiteral(".instance%1").arg(QApplication::applicationPid());
    if (bus.registerService(m_name))
        return true;
    m_name.clear();
    return false;
}

QString MprisService::registeredName() const
{
    return m_registered ? m_name : QString();
}

void MprisService::onNameOwnerChanged(const QString &name, const QString & /*oldOwner*/, const QString &newOwner)
{
    if (!m_registered || !newOwner.isEmpty() || name == m_name
        || (name != serviceName() && !name.startsWith(serviceName() + QLatin1Char('.'))))
        return;
    QDBusConnection bus = QDBusConnection::sessionBus();
    if (name == serviceName()) {
        // The window with the plain name closed: take it over.
        if (bus.registerService(serviceName()))
            bus.unregisterService(std::exchange(m_name, serviceName()));
        return;
    }
    // Another window closed; it may have been the one getting the keys.
    // Appear anew under the plain name.
    if (m_name != serviceName())
        return;
    bus.unregisterService(m_name);
    if (!takeName())
        m_registered = false;
}

QString MprisService::serviceName()
{
    return QStringLiteral("org.mpris.MediaPlayer2.top_player");
}

QString MprisService::playbackStatus() const
{
    if (m_mpv->isIdle())
        return QStringLiteral("Stopped");
    return m_mpv->mpvProperty(QStringLiteral("pause")).toBool() ? QStringLiteral("Paused") : QStringLiteral("Playing");
}

QVariantMap MprisService::metadata() const
{
    if (m_mpv->isIdle())
        return {{QStringLiteral("mpris:trackid"), QVariant::fromValue(QDBusObjectPath(QStringLiteral("/org/mpris/MediaPlayer2/TrackList/NoTrack")))}};
    QVariantMap map;
    map.insert(QStringLiteral("mpris:trackid"), QVariant::fromValue(trackId(m_mpv->mpvProperty(QStringLiteral("playlist-pos")).toInt())));
    const double duration = m_mpv->mpvProperty(QStringLiteral("duration")).toDouble();
    if (duration > 0)
        map.insert(QStringLiteral("mpris:length"), static_cast<qlonglong>(std::llround(duration * kMicro)));
    const QString title = m_mpv->mpvPropertyString(QStringLiteral("media-title"));
    if (!title.isEmpty())
        map.insert(QStringLiteral("xesam:title"), title);
    const QString path = m_mpv->mpvPropertyString(QStringLiteral("path"));
    if (!path.isEmpty()) {
        const bool url = path.contains(QLatin1String("://"));
        map.insert(QStringLiteral("xesam:url"), url ? path : QUrl::fromLocalFile(QFileInfo(path).absoluteFilePath()).toString());
    }
    const QVariantMap tags = m_mpv->mpvProperty(QStringLiteral("metadata")).toMap();
    for (auto it = tags.cbegin(); it != tags.cend(); ++it) {
        const QString key = it.key().toLower();
        if (key == QLatin1String("artist"))
            map.insert(QStringLiteral("xesam:artist"), QStringList{it.value().toString()});
        else if (key == QLatin1String("album"))
            map.insert(QStringLiteral("xesam:album"), it.value().toString());
    }
    return map;
}

qlonglong MprisService::positionUs() const
{
    return static_cast<qlonglong>(std::llround(std::max(0.0, m_mpv->mpvProperty(QStringLiteral("time-pos")).toDouble()) * kMicro));
}

bool MprisService::canPlay() const
{
    return m_mpv->mpvProperty(QStringLiteral("playlist-count")).toInt() > 0;
}

bool MprisService::canGoNext() const
{
    const int count = m_mpv->mpvProperty(QStringLiteral("playlist-count")).toInt();
    return count > 1 || (count > 0 && m_mpv->isIdle());
}

bool MprisService::canGoPrevious() const
{
    return canGoNext();
}

bool MprisService::canSeek() const
{
    return !m_mpv->isIdle() && m_mpv->mpvProperty(QStringLiteral("seekable")).toBool();
}

void MprisService::notifyPlayerChanged(const QStringList &names)
{
    if (!m_registered)
        return;
    QVariantMap changed;
    for (const QString &name : names) {
        if (name == QLatin1String("PlaybackStatus"))
            changed.insert(name, playbackStatus());
        else if (name == QLatin1String("Metadata"))
            changed.insert(name, metadata());
        else if (name == QLatin1String("Volume"))
            changed.insert(name, m_mpv->mpvProperty(QStringLiteral("volume")).toDouble() / 100.0);
        else if (name == QLatin1String("Rate"))
            changed.insert(name, m_mpv->mpvProperty(QStringLiteral("speed")).toDouble());
        else if (name == QLatin1String("CanGoNext"))
            changed.insert(name, canGoNext());
        else if (name == QLatin1String("CanGoPrevious"))
            changed.insert(name, canGoPrevious());
        else if (name == QLatin1String("CanPlay") || name == QLatin1String("CanPause"))
            changed.insert(name, canPlay());
        else if (name == QLatin1String("CanSeek"))
            changed.insert(name, canSeek());
    }
    QDBusMessage message = QDBusMessage::createSignal(kObjectPath, QStringLiteral("org.freedesktop.DBus.Properties"),
                                                      QStringLiteral("PropertiesChanged"));
    message << kPlayerInterface << changed << QStringList();
    QDBusConnection::sessionBus().send(message);
}

void MprisService::onPropertyUpdated(const QString &name)
{
    if (name == QLatin1String("pause") || name == QLatin1String("idle-active")) {
        notifyPlayerChanged({QStringLiteral("PlaybackStatus"), QStringLiteral("CanPlay"), QStringLiteral("CanPause"),
                             QStringLiteral("CanSeek"), QStringLiteral("CanGoNext"), QStringLiteral("CanGoPrevious")});
    } else if (name == QLatin1String("duration") || name == QLatin1String("playlist-pos") || name == QLatin1String("metadata")) {
        notifyPlayerChanged({QStringLiteral("Metadata"), QStringLiteral("CanGoNext"), QStringLiteral("CanGoPrevious")});
    } else if (name == QLatin1String("volume")) {
        notifyPlayerChanged({QStringLiteral("Volume")});
    } else if (name == QLatin1String("speed")) {
        notifyPlayerChanged({QStringLiteral("Rate")});
    }
}

MprisRootAdaptor::MprisRootAdaptor(MprisService *service)
    : QDBusAbstractAdaptor(service)
    , m_service(service)
{
}

bool MprisRootAdaptor::fullscreen() const
{
    return m_service->window()->isFullScreen();
}

QString MprisRootAdaptor::identity() const
{
    return QApplication::applicationDisplayName();
}

QString MprisRootAdaptor::desktopEntry() const
{
    return QGuiApplication::desktopFileName();
}

QStringList MprisRootAdaptor::supportedUriSchemes() const
{
    return {QStringLiteral("file"), QStringLiteral("http"), QStringLiteral("https"), QStringLiteral("rtsp"),
            QStringLiteral("rtmp"), QStringLiteral("mms")};
}

QStringList MprisRootAdaptor::supportedMimeTypes() const
{
    return {QStringLiteral("video/mp4"), QStringLiteral("video/x-matroska"), QStringLiteral("video/webm"),
            QStringLiteral("video/x-msvideo"), QStringLiteral("video/quicktime"), QStringLiteral("audio/mpeg"),
            QStringLiteral("audio/flac"), QStringLiteral("audio/ogg"), QStringLiteral("audio/x-wav"),
            QStringLiteral("audio/mp4"), QStringLiteral("audio/x-mpegurl")};
}

void MprisRootAdaptor::Raise()
{
    QWidget *window = m_service->window();
    if (window->isMinimized())
        window->showNormal();
    window->raise();
    window->activateWindow();
}

void MprisRootAdaptor::Quit()
{
    m_service->window()->close();
}

MprisPlayerAdaptor::MprisPlayerAdaptor(MprisService *service)
    : QDBusAbstractAdaptor(service)
    , m_service(service)
{
}

double MprisPlayerAdaptor::rate() const
{
    return m_service->mpv()->mpvProperty(QStringLiteral("speed")).toDouble();
}

void MprisPlayerAdaptor::setRate(double rate)
{
    if (rate <= 0) {
        Pause();
        return;
    }
    m_service->mpv()->setMpvProperty(QStringLiteral("speed"), QString::number(std::clamp(rate, minimumRate(), maximumRate())));
}

double MprisPlayerAdaptor::volume() const
{
    return m_service->mpv()->mpvProperty(QStringLiteral("volume")).toDouble() / 100.0;
}

void MprisPlayerAdaptor::setVolume(double volume)
{
    const double max = m_service->mpv()->mpvProperty(QStringLiteral("volume-max")).toDouble();
    m_service->mpv()->setMpvProperty(QStringLiteral("volume"), QString::number(std::clamp(volume * 100.0, 0.0, max > 0 ? max : 100.0)));
}

void MprisPlayerAdaptor::Next()
{
    m_service->mpv()->playlistNext();
}

void MprisPlayerAdaptor::Previous()
{
    m_service->mpv()->playlistPrev();
}

void MprisPlayerAdaptor::Pause()
{
    m_service->mpv()->pause();
}

void MprisPlayerAdaptor::PlayPause()
{
    m_service->mpv()->togglePause();
}

void MprisPlayerAdaptor::Stop()
{
    m_service->mpv()->stop();
}

void MprisPlayerAdaptor::Play()
{
    m_service->mpv()->play();
}

void MprisPlayerAdaptor::Seek(qlonglong offset)
{
    if (!m_service->canSeek())
        return;
    m_service->mpv()->command({QStringLiteral("seek"), QString::number(offset / kMicro, 'f', 3), QStringLiteral("relative")});
}

void MprisPlayerAdaptor::SetPosition(const QDBusObjectPath &track, qlonglong position)
{
    // The position is ignored unless it is for the entry that is playing.
    if (!m_service->canSeek() || track != trackId(m_service->mpv()->mpvProperty(QStringLiteral("playlist-pos")).toInt()))
        return;
    m_service->mpv()->command({QStringLiteral("seek"), QString::number(position / kMicro, 'f', 3), QStringLiteral("absolute")});
}

void MprisPlayerAdaptor::OpenUri(const QString &uri)
{
    const QUrl url(uri);
    Q_EMIT m_service->openRequested(url.isLocalFile() ? url.toLocalFile() : uri);
}
