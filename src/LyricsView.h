#pragma once

#include "Lyrics.h"

#include <QImage>
#include <QPixmap>
#include <QVariantAnimation>
#include <QWidget>

// Lyrics over the video surface, karaoke style: the line being sung sits in
// the middle, large, bold and bright, and the lines around it fade out with
// their distance from it. Moving to the next line scrolls smoothly. Plain
// (unsynced) lyrics scroll along with the playback position instead.
class LyricsView : public QWidget
{
    Q_OBJECT

public:
    explicit LyricsView(QWidget *parent);

    void setDocument(const Lyrics::Document &doc);
    const Lyrics::Document &document() const { return m_doc; }
    // A hint shown when there are no lyrics, e.g. how to get some.
    void setPlaceholder(const QString &text);
    // The playback position and the file's length (for plain lyrics).
    void setPosition(double seconds, double duration);
    // Over a video the view is a translucent scrim; for audio it is opaque,
    // tinted with the cover's colors.
    void setOpaque(bool opaque);
    void setArtwork(const QImage &image);
    void setHeading(const QString &title, const QString &artist);

    int activeLine() const { return m_active; }
    // Where line `index` is drawn now, in widget coordinates.
    QRect lineRect(int index) const;

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;
    void paintEvent(QPaintEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;

private:
    void relayout();
    void scrollTo(double y, bool animate);
    double targetScroll() const;
    QFont lineFont(bool active) const;

    Lyrics::Document m_doc;
    QString m_placeholder;
    QString m_title;
    QString m_artist;
    QImage m_artwork;
    QPixmap m_cover;
    QColor m_tint;
    bool m_opaque = true;
    int m_active = -1;
    double m_position = 0;
    double m_duration = 0;
    // Each line's top and height in content coordinates, laid out with the
    // active font so lines keep their place when they light up.
    QList<QPair<int, int>> m_layout;
    int m_contentHeight = 0;
    double m_scroll = 0; // content y shown at the middle of the view
    QVariantAnimation m_scrollAnimation;
    // 0..1, how far the active line has grown into its highlighted look.
    double m_emphasis = 1;
    QVariantAnimation m_emphasisAnimation;
};
