#include "ResumeManager.h"
#include "MediaFiles.h"
#include "MpvWidget.h"
#include "PlaylistSession.h"
#include "ResumePrompt.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QFileInfo>
#include <QSettings>

#include <cmath>

namespace {

// Positions are saved this often while playing, so a crash loses little.
constexpr double kSaveInterval = 5.0;
// Only this many files are remembered; the oldest are dropped.
constexpr int kMaxEntries = 400;

QString resumeFile()
{
    return PlaylistSession::configDir() + QStringLiteral("/resume.ini");
}

QString settingsFile()
{
    return PlaylistSession::configDir() + QStringLiteral("/settings.ini");
}

// Local files by absolute path (however they were opened), others as given.
QString mediaKey(const QString &media)
{
    const QString local = MediaFiles::localPath(media);
    const QString id = local.isEmpty() ? media : QFileInfo(local).absoluteFilePath();
    return QStringLiteral("positions/")
           + QString::fromLatin1(QCryptographicHash::hash(id.toUtf8(), QCryptographicHash::Sha1).toHex());
}

} // namespace

ResumeManager::ResumeManager(MpvWidget *mpv, QWidget *overlayParent)
    : QObject(overlayParent)
    , m_mpv(mpv)
    , m_prompt(new ResumePrompt(overlayParent))
{
    connect(m_mpv, &MpvWidget::fileStarted, this, &ResumeManager::onFileStarted);
    connect(m_mpv, &MpvWidget::fileLoaded, this, &ResumeManager::onFileLoaded);
    connect(m_mpv, &MpvWidget::propertyUpdated, this, [this](const QString &name, const QVariant &value) {
        if (name == QLatin1String("duration")) {
            if (value.isValid())
                m_duration = value.toDouble();
        } else if (name == QLatin1String("time-pos")) {
            if (m_asking || m_media.isEmpty() || !value.isValid() || m_audioOnly)
                return;
            m_position = value.toDouble();
            if (std::abs(m_position - m_lastSaved) >= kSaveInterval) {
                m_lastSaved = m_position;
                remember(m_media, m_position, m_duration);
            }
        } else if (name == QLatin1String("idle-active") && value.toBool() && m_mpv->isIdle()) {
            // Stopped, or the playlist ran out.
            saveNow();
            m_media.clear();
            m_asking = false;
            m_prompt->dismiss();
        }
    });
    connect(m_prompt, &ResumePrompt::resumeChosen, this, &ResumeManager::resume);
    connect(m_prompt, &ResumePrompt::startOverChosen, this, &ResumeManager::startOver);
}

ResumeManager::Mode ResumeManager::mode()
{
    const QString value = QSettings(settingsFile(), QSettings::IniFormat).value(QStringLiteral("resume/mode")).toString();
    if (value == QLatin1String("always"))
        return Mode::Always;
    if (value == QLatin1String("never"))
        return Mode::Never;
    return Mode::Ask;
}

void ResumeManager::setMode(Mode mode)
{
    const char *value = mode == Mode::Always ? "always" : mode == Mode::Never ? "never" : "ask";
    QSettings(settingsFile(), QSettings::IniFormat).setValue(QStringLiteral("resume/mode"), QLatin1String(value));
}

std::optional<ResumeManager::Entry> ResumeManager::lookup(const QString &media)
{
    if (media.isEmpty())
        return std::nullopt;
    const QVariantList value = QSettings(resumeFile(), QSettings::IniFormat).value(mediaKey(media)).toList();
    if (value.size() < 2)
        return std::nullopt;
    Entry entry{value[0].toDouble(), value[1].toDouble()};
    if (entry.position < kMinMargin)
        return std::nullopt;
    return entry;
}

void ResumeManager::remember(const QString &media, double position, double duration)
{
    // Live streams have no length to come back to.
    if (media.isEmpty() || duration < 3 * kMinMargin)
        return;
    QSettings settings(resumeFile(), QSettings::IniFormat);
    const QString key = mediaKey(media);
    if (position < kMinMargin || duration - position < kMinMargin) {
        settings.remove(key);
        return;
    }
    settings.setValue(key, QVariantList{position, duration, QDateTime::currentSecsSinceEpoch()});

    settings.beginGroup(QStringLiteral("positions"));
    const QStringList keys = settings.childKeys();
    if (keys.size() > kMaxEntries) {
        QList<QPair<qint64, QString>> byAge;
        for (const QString &k : keys)
            byAge.append({settings.value(k).toList().value(2).toLongLong(), k});
        std::sort(byAge.begin(), byAge.end());
        for (int i = 0; i < byAge.size() - kMaxEntries; ++i)
            settings.remove(byAge[i].second);
    }
    settings.endGroup();
}

void ResumeManager::forget(const QString &media)
{
    QSettings(resumeFile(), QSettings::IniFormat).remove(mediaKey(media));
}

void ResumeManager::saveNow()
{
    if (!m_asking && !m_audioOnly && !m_media.isEmpty() && m_position >= 0)
        remember(m_media, m_position, m_duration);
}

QString ResumeManager::currentEntry() const
{
    for (const QVariant &entry : m_mpv->mpvProperty(QStringLiteral("playlist")).toList()) {
        const QVariantMap map = entry.toMap();
        if (map.value(QStringLiteral("current")).toBool() || map.value(QStringLiteral("playing")).toBool())
            return map.value(QStringLiteral("filename")).toString();
    }
    return {};
}

void ResumeManager::onFileStarted()
{
    // The file before this one ended or was replaced: keep where it was left.
    saveNow();
    m_media.clear();
    m_position = -1;
    m_lastSaved = -1;
    m_duration = 0;
    m_audioOnly = false;
    m_pending.reset();
    if (m_asking) {
        m_asking = false;
        m_prompt->dismiss();
    }
    if (mode() == Mode::Never)
        return;
    // A restored session already opens the file where it was left.
    const QString start = m_mpv->mpvPropertyString(QStringLiteral("start"));
    if (!start.isEmpty() && start != QLatin1String("none"))
        return;
    m_pendingMedia = currentEntry();
    m_pending = lookup(m_pendingMedia);
    if (!m_pending)
        return;
    // Hold the file at its start until the user has answered.
    m_wasPaused = m_mpv->mpvProperty(QStringLiteral("pause")).toBool();
    if (mode() == Mode::Ask)
        m_mpv->setMpvProperty(QStringLiteral("pause"), QStringLiteral("yes"));
}

void ResumeManager::onFileLoaded()
{
    m_media = m_mpv->mpvPropertyString(QStringLiteral("path"));
    const QVariant duration = m_mpv->mpvProperty(QStringLiteral("duration"));
    if (duration.isValid())
        m_duration = duration.toDouble();
    // Songs always play from the start: only videos are resumed. Whether the
    // file is a song is known only now, so let it play if it was held.
    m_audioOnly = m_mpv->isAudioOnly();
    if (m_audioOnly) {
        if (m_pending) {
            m_pending.reset();
            forget(m_media);
            if (mode() == Mode::Ask)
                m_mpv->setMpvProperty(QStringLiteral("pause"), m_wasPaused ? QStringLiteral("yes") : QStringLiteral("no"));
        }
        return;
    }
    if (!m_pending)
        return;
    if (mode() == Mode::Always) {
        resume();
        return;
    }
    m_asking = true;
    QString title = m_mpv->mpvPropertyString(QStringLiteral("media-title"));
    if (title.isEmpty())
        title = QFileInfo(m_media).fileName();
    m_prompt->ask(title, m_pending->position, m_duration > 0 ? m_duration : m_pending->duration, m_mpv->isAudioOnly());
}

void ResumeManager::resume()
{
    m_asking = false;
    if (!m_pending)
        return;
    const double position = m_pending->position;
    m_pending.reset();
    m_position = position;
    m_mpv->command({QStringLiteral("seek"), QString::number(position, 'f', 3), QStringLiteral("absolute")});
    m_mpv->setMpvProperty(QStringLiteral("pause"), m_wasPaused ? QStringLiteral("yes") : QStringLiteral("no"));
    const long long s = std::llround(position);
    Q_EMIT message(tr("Resumed at"), QStringLiteral("%1:%2:%3")
                                         .arg(s / 3600, 2, 10, QLatin1Char('0'))
                                         .arg((s / 60) % 60, 2, 10, QLatin1Char('0'))
                                         .arg(s % 60, 2, 10, QLatin1Char('0')));
}

void ResumeManager::startOver()
{
    m_asking = false;
    m_pending.reset();
    m_position = 0;
    forget(m_media);
    m_mpv->setMpvProperty(QStringLiteral("pause"), m_wasPaused ? QStringLiteral("yes") : QStringLiteral("no"));
    Q_EMIT message(tr("Playing from the start"));
}
