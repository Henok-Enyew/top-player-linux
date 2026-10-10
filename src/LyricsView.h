#pragma once

#include "Lyrics.h"

#include <QColor>
#include <QImage>
#include <QPixmap>
#include <QTimer>
#include <QVariantAnimation>
#include <QWidget>

#include <optional>

// How the lyrics look: font, size, spacing, alignment and colors. Kept in
// settings.ini ([lyricsStyle]); Lyrics -> Lyrics Appearance... edits it.
struct LyricsStyle {
    QString family;              // empty: the skin's font
    double scale = 1.0;          // text size, kMinScale..kMaxScale
    double spacing = 0.5;        // gap between lines, in line heights
    Qt::Alignment align = Qt::AlignHCenter;
    QColor highlight = QColor(0xFF, 0xFF, 0xFF); // the line being sung
    bool glow = true;            // a soft accent glow behind it
    bool bold = true;            // and bold
    int dim = 70;                // % darkening behind lyrics over a video

    static constexpr double kMinScale = 0.6;
    static constexpr double kMaxScale = 2.5;

    static LyricsStyle load();
    void save() const;
    bool operator==(const LyricsStyle &other) const;
    bool operator!=(const LyricsStyle &other) const { return !(*this == other); }
};

// Lyrics over the video surface, karaoke style: the line being sung sits in
// the middle, large, bold and bright, and the lines around it fade out with
// their distance from it. Moving to the next line scrolls smoothly. Plain
// (unsynced) lyrics scroll along with the playback position instead.
//
// The lyrics can be browsed: dragging a line or turning the wheel over them
// scrolls freely, and they glide back to the sung line a few seconds after the
// last touch (or at once with "Back to current line"). Clicking a line that
// has a time seeks there; lines without one (plain lyrics) only scroll.
// Ctrl+wheel changes the text size. In a small window fewer lines show, so
// the sung one always stays readable.
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
    const LyricsStyle &lyricsStyle() const { return m_style; }
    void setLyricsStyle(const LyricsStyle &style);

    int activeLine() const { return m_active; }
    // Where line `index` is drawn now, in widget coordinates.
    QRect lineRect(int index) const;
    // The line drawn at `pos`, or -1.
    int lineAt(const QPoint &pos) const;
    // The playback position that shows line `index`, or -1 if it has no time.
    double seekTime(int index) const;
    // True while the user scrolls through the lyrics instead of following playback.
    bool isBrowsing() const { return m_browsing; }
    // Scrolls by `pixels` (positive: further down the song) and browses.
    void browseBy(double pixels);
    // Ends browsing and glides back to the sung line.
    void followPlayback();
    // How many lines are drawn around the sung one (-1: as many as fit).
    int visibleRadius() const;
    // True when the window is so small that the title strip is left out.
    bool isCompact() const;
    // Off: presses on the lyrics are left to the window (the mini player,
    // which a drag anywhere moves). The wheel still browses.
    void setTakesPresses(bool takes) { m_takesPresses = takes; }

    // Browsing ends this long after the last scroll or pointer movement.
    static constexpr int kBrowseHoldMs = 4000;

Q_SIGNALS:
    // A line with a time was clicked: play from `seconds`.
    void seekRequested(double seconds);
    // For the OSD, e.g. ("Lyrics Size", "120%").
    void message(const QString &label, const QString &value);
    // Ctrl+wheel changed the text size; the new style is saved already.
    void lyricsStyleChanged(const LyricsStyle &style);

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;
    void paintEvent(QPaintEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;
    void hideEvent(QHideEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void mouseDoubleClickEvent(QMouseEvent *event) override;
    void wheelEvent(QWheelEvent *event) override;
    void leaveEvent(QEvent *event) override;

private:
    void relayout();
    void scrollTo(double y, bool animate, int durationMs = -1);
    double targetScroll() const;
    QFont lineFont(bool active) const;
    int headingHeight() const;
    int textWidth() const;
    int textLeft() const;
    // The text column, where presses scroll and click lines.
    QRect columnRect() const;
    // "Back to current line", shown while browsing.
    QRect backButtonRect() const;
    void startBrowsing();
    void setHovered(int index);
    void updateCursor(const QPoint &pos);
    double clampScroll(double y) const;

    Lyrics::Document m_doc;
    LyricsStyle m_style;
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
    int m_gap = 0;
    double m_scroll = 0; // content y shown at the middle of the view
    QVariantAnimation m_scrollAnimation;
    // 0..1, how far the active line has grown into its highlighted look.
    double m_emphasis = 1;
    QVariantAnimation m_emphasisAnimation;

    bool m_browsing = false;
    bool m_takesPresses = true;
    QTimer m_browseTimer;
    int m_hovered = -1;
    bool m_backHovered = false;
    // A left press on the lyrics: where, and the scroll then. Becomes a drag
    // once it moves far enough, else a click on release.
    struct Press {
        QPoint pos;
        double scroll = 0;
        bool dragging = false;
        bool onBackButton = false;
    };
    std::optional<Press> m_press;
};
