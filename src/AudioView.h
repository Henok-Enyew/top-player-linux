#pragma once

#include "AudioArtwork.h"

#include <QImage>
#include <QPixmap>
#include <QWidget>

// Shown over the video surface while an audio file plays: the cover art with
// a soft drop shadow and the track's title, artist and album; or, while mpv
// draws a visualization underneath, just a caption with the metadata.
class AudioView : public QWidget
{
    Q_OBJECT

public:
    enum class Mode {
        Artwork,    // the cover (a placeholder while it is null) above the metadata
        Canvas,     // a dark canvas with the metadata
        Visualizer, // transparent, with the metadata in a caption at the bottom
    };

    explicit AudioView(QWidget *parent);

    void setMode(Mode mode);
    Mode mode() const { return m_mode; }
    void setArtwork(const QImage &image);
    QImage artwork() const { return m_artwork; }
    void setTrackInfo(const AudioArtwork::TrackInfo &info);
    AudioArtwork::TrackInfo trackInfo() const { return m_info; }

    // Where the cover is drawn, in widget coordinates (empty if not shown).
    QRect artworkRect() const;

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;
    void paintEvent(QPaintEvent *event) override;

private:
    struct Layout {
        QRect cover;
        QRect text;
    };
    Layout layout() const;
    void paintBackground(QPainter &p);
    void paintCover(QPainter &p, const QRect &rect);
    void paintText(QPainter &p, const QRect &rect, Qt::Alignment alignment);
    void paintCaption(QPainter &p);
    int textBlockHeight() const;
    const QPixmap &shadow(const QSize &coverSize);
    // The cover blurred to fill the view, made for its current size.
    const QPixmap &backdrop();

    Mode m_mode = Mode::Artwork;
    QImage m_artwork;
    QColor m_tint;
    AudioArtwork::TrackInfo m_info;
    QPixmap m_shadow;
    QSize m_shadowSize;
    QPixmap m_scaledArtwork;
    QPixmap m_backdrop;
    QSize m_backdropSize;
};
