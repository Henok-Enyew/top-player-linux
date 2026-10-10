#include "Icons.h"
#include "Theme.h"

#include <QLinearGradient>
#include <QPainter>
#include <QPainterPath>
#include <QPixmap>
#include <QRadialGradient>

namespace {

const QColor kNormalColor(0xD8, 0xDB, 0xE4);
const QColor kActiveColor = Theme::AccentHover;
const QColor kDisabledColor = Theme::TextDim;

constexpr int kLogicalSize = 20;
constexpr qreal kScale = 2.0; // rendered at 2x so icons stay sharp on HiDPI screens

QPainterPath polygon(std::initializer_list<QPointF> points)
{
    QPainterPath path;
    path.addPolygon(QPolygonF(points));
    path.closeSubpath();
    return path;
}

// A rounded loop with arrowheads, for the repeat modes.
void drawRepeatLoop(QPainter &p)
{
    const QColor color = p.pen().color();
    p.drawPolyline(QPolygonF({{4, 11}, {4, 7.5}, {5.5, 6}, {14, 6}}));
    p.drawPolyline(QPolygonF({{16, 9}, {16, 12.5}, {14.5, 14}, {6, 14}}));
    p.fillPath(polygon({{13.5, 3.5}, {17, 6}, {13.5, 8.5}}), color);
    p.fillPath(polygon({{6.5, 11.5}, {3, 14}, {6.5, 16.5}}), color);
}

// A screen frame, for the aspect modes.
void drawFrame(QPainter &p, const QRectF &rect)
{
    p.drawRoundedRect(rect, 1.5, 1.5);
}

void drawSpeaker(QPainter &p)
{
    p.fillPath(polygon({{3, 7.5}, {6, 7.5}, {10, 4}, {10, 16}, {6, 12.5}, {3, 12.5}}), p.pen().color());
}

// Draws the icon on a 20x20 grid using the painter's pen color.
void drawIcon(QPainter &p, IconType type)
{
    const QColor color = p.pen().color();
    switch (type) {
    case IconType::Open:
        p.fillPath(polygon({{4, 11}, {10, 4.5}, {16, 11}}), color);
        p.fillRect(QRectF(4, 13, 12, 2.5), color);
        break;
    case IconType::Play:
        p.fillPath(polygon({{6, 4}, {16, 10}, {6, 16}}), color);
        break;
    case IconType::Pause:
        p.fillRect(QRectF(5.5, 4, 3, 12), color);
        p.fillRect(QRectF(11.5, 4, 3, 12), color);
        break;
    case IconType::Stop:
        p.fillRect(QRectF(5, 5, 10, 10), color);
        break;
    case IconType::Previous:
        p.fillRect(QRectF(4, 5, 2, 10), color);
        p.fillPath(polygon({{16, 5}, {7, 10}, {16, 15}}), color);
        break;
    case IconType::Next:
        p.fillRect(QRectF(14, 5, 2, 10), color);
        p.fillPath(polygon({{4, 5}, {13, 10}, {4, 15}}), color);
        break;
    case IconType::Playlist:
        p.drawLine(QPointF(3, 5), QPointF(17, 5));
        p.drawLine(QPointF(3, 10), QPointF(11, 10));
        p.drawLine(QPointF(3, 15), QPointF(11, 15));
        p.fillPath(polygon({{13.5, 9}, {18, 12.5}, {13.5, 16}}), color);
        break;
    case IconType::Volume:
        drawSpeaker(p);
        p.drawArc(QRectF(8, 7, 5, 6), -60 * 16, 120 * 16);
        p.drawArc(QRectF(8, 4.5, 8.5, 11), -60 * 16, 120 * 16);
        break;
    case IconType::Muted:
        drawSpeaker(p);
        p.drawLine(QPointF(12.5, 8), QPointF(16.5, 12));
        p.drawLine(QPointF(16.5, 8), QPointF(12.5, 12));
        break;
    case IconType::Fullscreen:
        p.drawPolyline(QPolygonF({{3.5, 7.5}, {3.5, 3.5}, {7.5, 3.5}}));
        p.drawPolyline(QPolygonF({{12.5, 3.5}, {16.5, 3.5}, {16.5, 7.5}}));
        p.drawPolyline(QPolygonF({{16.5, 12.5}, {16.5, 16.5}, {12.5, 16.5}}));
        p.drawPolyline(QPolygonF({{7.5, 16.5}, {3.5, 16.5}, {3.5, 12.5}}));
        break;
    // The window controls: thin, centered glyphs, as on current desktops.
    case IconType::Minimize:
        p.setPen(QPen(color, 1.3, Qt::SolidLine, Qt::RoundCap));
        p.drawLine(QPointF(5.5, 10.5), QPointF(14.5, 10.5));
        break;
    case IconType::Maximize:
        p.setPen(QPen(color, 1.3, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        p.drawRoundedRect(QRectF(5.5, 5.5, 9, 9), 2, 2);
        break;
    case IconType::Restore: {
        p.setPen(QPen(color, 1.3, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        p.drawRoundedRect(QRectF(5, 7.5, 7.5, 7.5), 1.8, 1.8);
        QPainterPath back;
        back.moveTo(7.5, 5.5);
        back.lineTo(13, 5.5);
        back.quadTo(15, 5.5, 15, 7.5);
        back.lineTo(15, 13);
        p.drawPath(back);
        break;
    }
    case IconType::Close:
        p.setPen(QPen(color, 1.3, Qt::SolidLine, Qt::RoundCap));
        p.drawLine(QPointF(6, 6), QPointF(14, 14));
        p.drawLine(QPointF(14, 6), QPointF(6, 14));
        break;
    case IconType::Pin:
        // A pushpin, tilted: head, body and needle.
        p.setPen(QPen(color, 1.3, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        p.drawPath(polygon({{11, 3.5}, {16.5, 9}, {14.5, 9.8}, {12, 12.3}, {11.6, 15}, {5, 8.4}, {7.7, 8}, {10.2, 5.5}}));
        p.drawLine(QPointF(8.3, 11.7), QPointF(4, 16));
        break;
    case IconType::MiniPlayer:
        // Picture in picture: a screen with a small window in its corner.
        p.setPen(QPen(color, 1.3, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        p.drawRoundedRect(QRectF(3, 4.5, 14, 11), 2, 2);
        p.fillPath([] {
            QPainterPath path;
            path.addRoundedRect(QRectF(9.5, 9.5, 5.5, 4), 1, 1);
            return path;
        }(), color);
        break;
    case IconType::MoveTop:
        p.drawLine(QPointF(4.5, 4), QPointF(15.5, 4));
        p.drawPolyline(QPolygonF({{5.5, 12}, {10, 7.5}, {14.5, 12}}));
        p.drawLine(QPointF(10, 7.5), QPointF(10, 16.5));
        break;
    case IconType::MoveUp:
        p.drawPolyline(QPolygonF({{5, 10}, {10, 5}, {15, 10}}));
        p.drawLine(QPointF(10, 5), QPointF(10, 16));
        break;
    case IconType::MoveDown:
        p.drawPolyline(QPolygonF({{5, 10}, {10, 15}, {15, 10}}));
        p.drawLine(QPointF(10, 4), QPointF(10, 15));
        break;
    case IconType::MoveBottom:
        p.drawLine(QPointF(4.5, 16), QPointF(15.5, 16));
        p.drawPolyline(QPolygonF({{5.5, 8}, {10, 12.5}, {14.5, 8}}));
        p.drawLine(QPointF(10, 3.5), QPointF(10, 12.5));
        break;
    case IconType::Add:
        p.drawLine(QPointF(10, 4), QPointF(10, 16));
        p.drawLine(QPointF(4, 10), QPointF(16, 10));
        break;
    case IconType::Remove:
        p.drawLine(QPointF(4, 10), QPointF(16, 10));
        break;
    case IconType::Clear:
        p.drawLine(QPointF(4, 6), QPointF(16, 6));
        p.drawLine(QPointF(8, 6), QPointF(8.5, 3.5));
        p.drawLine(QPointF(8.5, 3.5), QPointF(11.5, 3.5));
        p.drawLine(QPointF(11.5, 3.5), QPointF(12, 6));
        p.drawPolyline(QPolygonF({{5.5, 6}, {6.5, 16.5}, {13.5, 16.5}, {14.5, 6}}));
        break;
    case IconType::Folder:
        p.drawPath(polygon({{2.5, 4.5}, {8, 4.5}, {9.5, 6.5}, {17.5, 6.5}, {17.5, 15.5}, {2.5, 15.5}}));
        p.drawLine(QPointF(2.5, 8.5), QPointF(17.5, 8.5));
        break;
    case IconType::Download:
        // An arrow down onto a tray.
        p.drawLine(QPointF(10, 3), QPointF(10, 12.5));
        p.drawPolyline(QPolygonF({{6, 9}, {10, 13}, {14, 9}}));
        p.drawPolyline(QPolygonF({{4, 13}, {4, 16.5}, {16, 16.5}, {16, 13}}));
        break;
    case IconType::Url:
        // A globe: outline, meridian and two parallels.
        p.drawEllipse(QPointF(10, 10), 7, 7);
        p.drawEllipse(QPointF(10, 10), 3, 7);
        p.drawLine(QPointF(3, 10), QPointF(17, 10));
        p.drawLine(QPointF(4.5, 6.5), QPointF(15.5, 6.5));
        p.drawLine(QPointF(4.5, 13.5), QPointF(15.5, 13.5));
        break;
    case IconType::Shuffle:
        // Two crossing paths with arrowheads on the right.
        p.drawPolyline(QPolygonF({{3, 6}, {7, 6}, {12.5, 14}, {15.5, 14}}));
        p.drawPolyline(QPolygonF({{3, 14}, {7, 14}, {12.5, 6}, {15.5, 6}}));
        p.fillPath(polygon({{15, 3.5}, {18.5, 6}, {15, 8.5}}), color);
        p.fillPath(polygon({{15, 11.5}, {18.5, 14}, {15, 16.5}}), color);
        break;
    case IconType::Sort:
        // Bars getting shorter, beside a downward arrow.
        p.drawLine(QPointF(3, 5), QPointF(11, 5));
        p.drawLine(QPointF(3, 10), QPointF(9, 10));
        p.drawLine(QPointF(3, 15), QPointF(7, 15));
        p.drawLine(QPointF(14.5, 4), QPointF(14.5, 14));
        p.fillPath(polygon({{11.5, 12.5}, {14.5, 17}, {17.5, 12.5}}), color);
        break;
    case IconType::More:
        p.setPen(Qt::NoPen);
        p.setBrush(color);
        for (qreal y : {4.5, 10.0, 15.5})
            p.drawEllipse(QPointF(10, y), 1.6, 1.6);
        break;
    case IconType::Search:
        p.drawEllipse(QPointF(8.5, 8.5), 4.5, 4.5);
        p.drawLine(QPointF(12, 12), QPointF(16.5, 16.5));
        break;
    case IconType::Expand:
        // Double chevron pointing left: the drawer grows into the video.
        p.drawPolyline(QPolygonF({{10, 5}, {5, 10}, {10, 15}}));
        p.drawPolyline(QPolygonF({{15, 5}, {10, 10}, {15, 15}}));
        break;
    case IconType::Collapse:
        p.drawPolyline(QPolygonF({{5, 5}, {10, 10}, {5, 15}}));
        p.drawPolyline(QPolygonF({{10, 5}, {15, 10}, {10, 15}}));
        break;
    case IconType::Repeat:
        drawRepeatLoop(p);
        break;
    case IconType::RepeatOne: {
        drawRepeatLoop(p);
        // A small "1" inside the loop.
        QPen thin = p.pen();
        thin.setWidthF(1.3);
        p.setPen(thin);
        p.drawPolyline(QPolygonF({{9, 8.8}, {10.3, 7.8}, {10.3, 12.2}}));
        break;
    }
    case IconType::AspectFit:
        // Arrows pushing out to the corners of the frame.
        drawFrame(p, QRectF(2.5, 4.5, 15, 11));
        p.drawPolyline(QPolygonF({{5.5, 9}, {5.5, 7.5}, {7, 7.5}}));
        p.drawPolyline(QPolygonF({{13, 7.5}, {14.5, 7.5}, {14.5, 9}}));
        p.drawPolyline(QPolygonF({{14.5, 11}, {14.5, 12.5}, {13, 12.5}}));
        p.drawPolyline(QPolygonF({{7, 12.5}, {5.5, 12.5}, {5.5, 11}}));
        break;
    case IconType::AspectWide:
        // A 16:9 frame between letterbox bars.
        drawFrame(p, QRectF(2, 5.5, 16, 9));
        p.fillRect(QRectF(4.5, 8, 11, 4), color);
        break;
    case IconType::AspectOriginal:
        // Pixels at 1:1, a small picture centered in the frame.
        drawFrame(p, QRectF(2.5, 4.5, 15, 11));
        p.fillRect(QRectF(7.5, 8, 5, 4), color);
        break;
    }
}

QPixmap renderIcon(IconType type, const QColor &color)
{
    QPixmap pixmap(QSize(kLogicalSize, kLogicalSize) * kScale);
    pixmap.setDevicePixelRatio(kScale);
    pixmap.fill(Qt::transparent);
    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setPen(QPen(color, 1.6, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    painter.setBrush(Qt::NoBrush);
    drawIcon(painter, type);
    return pixmap;
}

} // namespace

QIcon skinIcon(IconType type)
{
    return skinIcon(type, kActiveColor);
}

QIcon skinIcon(IconType type, const QColor &hoverColor)
{
    QIcon icon;
    icon.addPixmap(renderIcon(type, kNormalColor), QIcon::Normal);
    icon.addPixmap(renderIcon(type, hoverColor), QIcon::Active);
    icon.addPixmap(renderIcon(type, kDisabledColor), QIcon::Disabled);
    const QPixmap on = renderIcon(type, Theme::Accent);
    icon.addPixmap(on, QIcon::Normal, QIcon::On);
    icon.addPixmap(on, QIcon::Active, QIcon::On);
    return icon;
}

QPixmap appLogo(int size, qreal devicePixelRatio)
{
    QPixmap pixmap(QSize(size, size) * devicePixelRatio);
    pixmap.setDevicePixelRatio(devicePixelRatio);
    pixmap.fill(Qt::transparent);
    QPainter p(&pixmap);
    p.setRenderHint(QPainter::Antialiasing);
    // Drawn on the 256x256 grid of packaging/linux/org.github.topplayer.svg.
    p.scale(size / 256.0, size / 256.0);

    // Obsidian tile.
    QLinearGradient background(0, 16, 0, 240);
    background.setColorAt(0, QColor(0x1E, 0x21, 0x29));
    background.setColorAt(1, QColor(0x0E, 0x0F, 0x12));
    p.setPen(QPen(Theme::Border, 1));
    p.setBrush(background);
    p.drawRoundedRect(QRectF(16.5, 16.5, 223, 223), 47.5, 47.5);

    // Neon glow behind the play triangle.
    QRadialGradient glow(QPointF(124, 128), 96);
    glow.setColorAt(0, QColor(0x00, 0xD2, 0xFF, 90));
    glow.setColorAt(1, QColor(0x00, 0xD2, 0xFF, 0));
    p.setPen(Qt::NoPen);
    p.setBrush(glow);
    p.drawEllipse(QPointF(124, 128), 96, 96);

    // Play triangle with rounded corners.
    QLinearGradient cyan(84, 64, 196, 192);
    cyan.setColorAt(0, Theme::AccentHover);
    cyan.setColorAt(1, Theme::AccentDeep);
    p.setPen(QPen(QBrush(cyan), 16, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    p.setBrush(cyan);
    p.drawPath(polygon({{84, 64}, {196, 128}, {84, 192}}));

    // A geometric "T" cut out of the triangle, showing the tile through it.
    QPainterPath letter;
    letter.addRoundedRect(QRectF(93, 100, 60, 18), 3, 3);
    letter.addRoundedRect(QRectF(114, 108, 18, 56), 3, 3);
    p.setPen(Qt::NoPen);
    p.setBrush(background);
    p.drawPath(letter.simplified());
    return pixmap;
}
