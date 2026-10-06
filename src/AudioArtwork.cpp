#include "AudioArtwork.h"
#include "PlaylistSession.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QImageReader>
#include <QObject>
#include <QSettings>

namespace {

// In order of preference.
const char *const kCoverNames[] = {"cover", "folder", "front", "album", "albumart"};
const char *const kCoverSuffixes[] = {"jpg", "jpeg", "png", "webp", "bmp"};

QString settingsFile()
{
    return PlaylistSession::configDir() + QStringLiteral("/settings.ini");
}

QString artworkFile()
{
    return PlaylistSession::configDir() + QStringLiteral("/artwork.ini");
}

// Paths contain '/', which QSettings reads as groups, so tracks are keyed by a hash.
QString trackKey(const QString &trackPath)
{
    const QByteArray path = QFileInfo(trackPath).absoluteFilePath().toUtf8();
    return QStringLiteral("custom/") + QString::fromLatin1(QCryptographicHash::hash(path, QCryptographicHash::Sha1).toHex());
}

QString metadataValue(const QVariantMap &metadata, const QString &key)
{
    for (auto it = metadata.cbegin(); it != metadata.cend(); ++it) {
        if (it.key().compare(key, Qt::CaseInsensitive) == 0)
            return it.value().toString().trimmed();
    }
    return {};
}

} // namespace

namespace AudioArtwork {

Visualization visualization()
{
    const QString value = QSettings(settingsFile(), QSettings::IniFormat)
                              .value(QStringLiteral("audio/visualization")).toString();
    if (value == QLatin1String("waveform"))
        return Visualization::Waveform;
    if (value == QLatin1String("spectrum"))
        return Visualization::Spectrum;
    if (value == QLatin1String("off"))
        return Visualization::Off;
    return Visualization::AlbumArt;
}

void setVisualization(Visualization mode)
{
    QString value;
    switch (mode) {
    case Visualization::AlbumArt:
        value = QStringLiteral("albumart");
        break;
    case Visualization::Waveform:
        value = QStringLiteral("waveform");
        break;
    case Visualization::Spectrum:
        value = QStringLiteral("spectrum");
        break;
    case Visualization::Off:
        value = QStringLiteral("off");
        break;
    }
    QSettings(settingsFile(), QSettings::IniFormat).setValue(QStringLiteral("audio/visualization"), value);
}

QString folderCover(const QString &trackPath)
{
    const QDir dir = QFileInfo(trackPath).absoluteDir();
    QHash<QString, QString> files; // lower-case name -> actual name
    for (const QString &name : dir.entryList(QDir::Files | QDir::Readable))
        files.insert(name.toLower(), name);
    for (const char *base : kCoverNames) {
        for (const char *suffix : kCoverSuffixes) {
            const QString name = QStringLiteral("%1.%2").arg(QLatin1String(base), QLatin1String(suffix));
            if (const auto it = files.constFind(name); it != files.cend())
                return dir.filePath(it.value());
        }
    }
    return {};
}

QString customArtwork(const QString &trackPath)
{
    const QString image = QSettings(artworkFile(), QSettings::IniFormat).value(trackKey(trackPath)).toString();
    return !image.isEmpty() && QFileInfo(image).isFile() ? image : QString();
}

void setCustomArtwork(const QString &trackPath, const QString &imagePath)
{
    QSettings(artworkFile(), QSettings::IniFormat)
        .setValue(trackKey(trackPath), QFileInfo(imagePath).absoluteFilePath());
}

void clearCustomArtwork(const QString &trackPath)
{
    QSettings(artworkFile(), QSettings::IniFormat).remove(trackKey(trackPath));
}

QString defaultArtwork()
{
    const QString image = QSettings(settingsFile(), QSettings::IniFormat).value(QStringLiteral("audio/defaultArtwork")).toString();
    return !image.isEmpty() && QFileInfo(image).isFile() ? image : QString();
}

bool setDefaultArtwork(const QString &imagePath)
{
    QImageReader reader(imagePath);
    if (!reader.canRead())
        return false;
    const QString previous = defaultArtwork();
    QDir().mkpath(PlaylistSession::configDir());
    // A new name each time, so a cached copy of the old one is never shown.
    const QString suffix = QFileInfo(imagePath).suffix().toLower();
    const QString copy = PlaylistSession::configDir() + QStringLiteral("/default-artwork-%1.%2")
                                                            .arg(QDateTime::currentMSecsSinceEpoch())
                                                            .arg(suffix.isEmpty() ? QStringLiteral("img") : suffix);
    if (!QFile::copy(imagePath, copy))
        return false;
    QSettings(settingsFile(), QSettings::IniFormat).setValue(QStringLiteral("audio/defaultArtwork"), copy);
    if (!previous.isEmpty() && previous.startsWith(PlaylistSession::configDir()))
        QFile::remove(previous);
    return true;
}

void clearDefaultArtwork()
{
    const QString previous = defaultArtwork();
    QSettings(settingsFile(), QSettings::IniFormat).remove(QStringLiteral("audio/defaultArtwork"));
    if (!previous.isEmpty() && previous.startsWith(PlaylistSession::configDir()))
        QFile::remove(previous);
}

bool isImageFile(const QString &path)
{
    const QByteArray suffix = QFileInfo(path).suffix().toLower().toLatin1();
    return !suffix.isEmpty() && QImageReader::supportedImageFormats().contains(suffix) && QFileInfo(path).isFile();
}

QString imageFileFilter()
{
    QStringList patterns;
    for (const QByteArray &format : QImageReader::supportedImageFormats())
        patterns.append(QStringLiteral("*.") + QString::fromLatin1(format));
    return QObject::tr("Images (%1);;All Files (*)").arg(patterns.join(QLatin1Char(' ')));
}

TrackInfo trackInfo(const QVariantMap &metadata, const QString &fallbackTitle)
{
    TrackInfo info;
    info.title = metadataValue(metadata, QStringLiteral("title"));
    // Internet radio streams put the playing song in icy-title.
    if (info.title.isEmpty())
        info.title = metadataValue(metadata, QStringLiteral("icy-title"));
    if (info.title.isEmpty())
        info.title = fallbackTitle;
    info.artist = metadataValue(metadata, QStringLiteral("artist"));
    if (info.artist.isEmpty())
        info.artist = metadataValue(metadata, QStringLiteral("album_artist"));
    info.album = metadataValue(metadata, QStringLiteral("album"));
    return info;
}

} // namespace AudioArtwork
