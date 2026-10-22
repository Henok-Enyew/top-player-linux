#pragma once

#include <QDBusAbstractAdaptor>
#include <QDBusObjectPath>
#include <QObject>
#include <QStringList>
#include <QVariantMap>

class MpvWidget;
class QWidget;

// MPRIS 2 (org.mpris.MediaPlayer2) on the session bus, so the desktop's media
// keys, its sound applet and lock screen, and tools like playerctl can control
// playback. On Linux the keyboard's Play/Pause, Next, Previous and Stop keys
// reach players this way, whichever window has the focus.
class MprisService : public QObject
{
    Q_OBJECT

public:
    MprisService(MpvWidget *mpv, QWidget *window, QObject *parent = nullptr);
    ~MprisService() override;

    // Whether the service got its name on the session bus.
    bool isRegistered() const { return m_registered; }
    static QString serviceName();
    // The name this player has on the bus: serviceName(), or while another
    // window holds that, serviceName() + ".instance<pid>". Empty if none.
    QString registeredName() const;

    MpvWidget *mpv() const { return m_mpv; }
    QWidget *window() const { return m_window; }

    // Player state as MPRIS reports it.
    QString playbackStatus() const;
    QVariantMap metadata() const;
    qlonglong positionUs() const;
    bool canPlay() const;
    bool canGoNext() const;
    bool canGoPrevious() const;
    bool canSeek() const;

    // Sends org.freedesktop.DBus.Properties.PropertiesChanged for the player.
    void notifyPlayerChanged(const QStringList &names);

Q_SIGNALS:
    // Relayed by the player adaptor as MPRIS's Seeked(x) signal.
    void seeked(qlonglong positionUs);
    // OpenUri: a file or URL to play.
    void openRequested(const QString &pathOrUrl);

private Q_SLOTS:
    // org.freedesktop.DBus.NameOwnerChanged: another window of the player
    // left the bus.
    void onNameOwnerChanged(const QString &name, const QString &oldOwner, const QString &newOwner);

private:
    void onPropertyUpdated(const QString &name);
    // Registers serviceName(), or if another window holds it, an instance name.
    bool takeName();

    MpvWidget *m_mpv;
    QWidget *m_window;
    QString m_name;
    bool m_registered = false;
};

// org.mpris.MediaPlayer2
class MprisRootAdaptor : public QDBusAbstractAdaptor
{
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.mpris.MediaPlayer2")
    Q_PROPERTY(bool CanQuit READ canQuit)
    Q_PROPERTY(bool CanRaise READ canRaise)
    Q_PROPERTY(bool CanSetFullscreen READ canSetFullscreen)
    Q_PROPERTY(bool Fullscreen READ fullscreen)
    Q_PROPERTY(bool HasTrackList READ hasTrackList)
    Q_PROPERTY(QString Identity READ identity)
    Q_PROPERTY(QString DesktopEntry READ desktopEntry)
    Q_PROPERTY(QStringList SupportedUriSchemes READ supportedUriSchemes)
    Q_PROPERTY(QStringList SupportedMimeTypes READ supportedMimeTypes)

public:
    explicit MprisRootAdaptor(MprisService *service);

    bool canQuit() const { return true; }
    bool canRaise() const { return true; }
    bool canSetFullscreen() const { return false; }
    bool fullscreen() const;
    bool hasTrackList() const { return false; }
    QString identity() const;
    QString desktopEntry() const;
    QStringList supportedUriSchemes() const;
    QStringList supportedMimeTypes() const;

public Q_SLOTS:
    void Raise();
    void Quit();

private:
    MprisService *m_service;
};

// org.mpris.MediaPlayer2.Player
class MprisPlayerAdaptor : public QDBusAbstractAdaptor
{
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.mpris.MediaPlayer2.Player")
    Q_PROPERTY(QString PlaybackStatus READ playbackStatus)
    Q_PROPERTY(double Rate READ rate WRITE setRate)
    Q_PROPERTY(QVariantMap Metadata READ metadata)
    Q_PROPERTY(double Volume READ volume WRITE setVolume)
    Q_PROPERTY(qlonglong Position READ position)
    Q_PROPERTY(double MinimumRate READ minimumRate)
    Q_PROPERTY(double MaximumRate READ maximumRate)
    Q_PROPERTY(bool CanGoNext READ canGoNext)
    Q_PROPERTY(bool CanGoPrevious READ canGoPrevious)
    Q_PROPERTY(bool CanPlay READ canPlay)
    Q_PROPERTY(bool CanPause READ canPause)
    Q_PROPERTY(bool CanSeek READ canSeek)
    Q_PROPERTY(bool CanControl READ canControl)

public:
    explicit MprisPlayerAdaptor(MprisService *service);

    QString playbackStatus() const { return m_service->playbackStatus(); }
    double rate() const;
    void setRate(double rate);
    QVariantMap metadata() const { return m_service->metadata(); }
    double volume() const;
    void setVolume(double volume);
    qlonglong position() const { return m_service->positionUs(); }
    double minimumRate() const { return 0.25; }
    double maximumRate() const { return 4.0; }
    bool canGoNext() const { return m_service->canGoNext(); }
    bool canGoPrevious() const { return m_service->canGoPrevious(); }
    bool canPlay() const { return m_service->canPlay(); }
    bool canPause() const { return m_service->canPlay(); }
    bool canSeek() const { return m_service->canSeek(); }
    bool canControl() const { return true; }

public Q_SLOTS:
    void Next();
    void Previous();
    void Pause();
    void PlayPause();
    void Stop();
    void Play();
    // `offset` and `position` are in microseconds.
    void Seek(qlonglong offset);
    void SetPosition(const QDBusObjectPath &trackId, qlonglong position);
    void OpenUri(const QString &uri);

Q_SIGNALS:
    void Seeked(qlonglong Position);

private:
    MprisService *m_service;
};
