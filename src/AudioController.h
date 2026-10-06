#pragma once

#include "AudioArtwork.h"

#include <QImage>
#include <QObject>

class ArtworkExtractor;
class AudioView;
class MpvWidget;
class QWidget;

// Presents audio files: detects them, finds their cover art (custom, then
// embedded, then a cover file in the folder) and shows it, or has mpv draw a
// visualization with a lavfi-complex graph. Audio track switching goes
// through here, because the graph decides which audio track plays.
class AudioController : public QObject
{
    Q_OBJECT

public:
    // Where the cover comes from, in order of preference.
    enum class ArtworkSource { None, Custom, Embedded, Folder, Default };
    // What is on screen for the current file.
    enum class Display { None, Artwork, Waveform, Spectrum, Canvas };

    AudioController(MpvWidget *mpv, QWidget *dialogParent);

    AudioView *view() const { return m_view; }
    // True while an audio-only file is loaded.
    bool isActive() const { return m_active; }
    Display display() const { return m_display; }
    QImage artwork() const { return m_artwork; }
    ArtworkSource artworkSource() const { return m_source; }

    AudioArtwork::Visualization visualization() const { return m_visualization; }
    // Switches the presentation, for this and later audio files.
    void setVisualization(AudioArtwork::Visualization mode);

    void setCustomArtworkDialog();
    // Assigns `imagePath` as the current track's cover. Returns false if it
    // is not a readable image or no audio file is loaded.
    bool setCustomArtwork(const QString &imagePath);
    void clearCustomArtwork();
    bool hasCustomArtwork() const { return m_source == ArtworkSource::Custom; }

    // Audio -> Default Artwork: the image for songs without a cover of their own.
    void setDefaultArtworkDialog();
    bool setDefaultArtwork(const QString &imagePath);
    void clearDefaultArtwork();

    // The selected audio track ("1", "2", ... or "no"), and selecting one.
    // While a visualization runs, mpv's "aid" is not used, so the graph is
    // rebuilt for the new track instead.
    QString audioTrack() const;
    void selectAudioTrack(const QString &id);

Q_SIGNALS:
    void message(const QString &label, const QString &value = QString());
    // The cover for the current file changed (null when it has none).
    void artworkChanged(const QImage &image);

private:
    void onFileLoaded();
    void onArtworkReady(const QString &path, const QImage &image);
    void updateTrackInfo();
    // Falls back to the default artwork when the track has no cover.
    void useDefaultArtwork();
    // Shows the current state and sets up the mpv graph it needs.
    void apply();
    void setGraph(const QString &filter);

    MpvWidget *m_mpv;
    QWidget *m_dialogParent;
    AudioView *m_view;
    ArtworkExtractor *m_extractor;
    AudioArtwork::Visualization m_visualization;
    Display m_display = Display::None;
    bool m_active = false;
    QString m_path;
    QImage m_artwork;
    ArtworkSource m_source = ArtworkSource::None;
    bool m_extracting = false;
    // A custom cover the user picked for this track is shown even while a
    // visualization is selected, until they pick a visualization again.
    bool m_customWins = false;
    // Audio track fed to the visualization graph.
    QString m_audioId;
    // The visualization filter in the current graph, empty if none.
    QString m_graphFilter;
};
