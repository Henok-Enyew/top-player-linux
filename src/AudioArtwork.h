#pragma once

#include <QImage>
#include <QString>
#include <QVariantMap>

// Cover art lookup for audio files, track metadata, and the audio display
// settings stored in ~/.config/top-player.
namespace AudioArtwork {

// What the video area shows while an audio file plays.
enum class Visualization {
    AlbumArt, // the cover; a spectrum when there is none
    Waveform,
    Spectrum,
    Off,      // a dark canvas with the track's metadata
};

Visualization visualization();
void setVisualization(Visualization mode);

// A cover image in the track's folder (cover.jpg, folder.jpg, album.png,
// front.jpg, ..., in any letter case), or an empty string.
QString folderCover(const QString &trackPath);

// The image the user assigned to a track, or an empty string. Assigned
// images that no longer exist are ignored.
QString customArtwork(const QString &trackPath);
void setCustomArtwork(const QString &trackPath, const QString &imagePath);
void clearCustomArtwork(const QString &trackPath);
// True for a file Qt can read as an image (by its suffix).
bool isImageFile(const QString &path);
// QFileDialog name filter for images Qt can read.
QString imageFileFilter();

struct TrackInfo {
    QString title;
    QString artist;
    QString album;
};

// Title, artist and album from mpv's "metadata" property (whose keys vary in
// case between formats). The title falls back to `fallbackTitle`.
TrackInfo trackInfo(const QVariantMap &metadata, const QString &fallbackTitle);

} // namespace AudioArtwork
