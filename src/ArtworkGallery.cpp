#include "ArtworkGallery.h"
#include "PlaylistSession.h"

#include <QCache>
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QLinearGradient>
#include <QMutex>
#include <QPainter>
#include <QPainterPath>
#include <QRadialGradient>
#include <QRandomGenerator>
#include <QSettings>
#include <QtMath>

#include <algorithm>
#include <cmath>

namespace {

// Everything is drawn on a 1000x1000 canvas, scaled to the requested size.
constexpr qreal kCanvas = 1000;

struct Palette {
    QColor background;
    QColor a, b, c, d;
};

QColor rgb(quint32 value)
{
    return QColor::fromRgb(value);
}

// Curated palettes; each style picks among the ones that suit it.
const Palette kPalettes[] = {
    {rgb(0x140F2E), rgb(0x7C5CFF), rgb(0xFF4FD8), rgb(0xFFB86B), rgb(0x5EEAD4)}, // 0 violet dream
    {rgb(0x1A0B2E), rgb(0xFF6B6B), rgb(0xFFB347), rgb(0xFF3CAC), rgb(0x784BA0)}, // 1 sunset
    {rgb(0x04121F), rgb(0x00D2FF), rgb(0x3A7BD5), rgb(0x00F5A0), rgb(0xA1C4FD)}, // 2 ocean
    {rgb(0x0E1A14), rgb(0xA3E635), rgb(0x34D399), rgb(0xFDE68A), rgb(0x0EA5E9)}, // 3 forest
    {rgb(0x120A0A), rgb(0xFF4D00), rgb(0xFFB000), rgb(0xFF006E), rgb(0xFB5607)}, // 4 ember
    {rgb(0x1B1033), rgb(0xF72585), rgb(0x7209B7), rgb(0x4CC9F0), rgb(0xFFD6FF)}, // 5 candy
    {rgb(0x0C0C0C), rgb(0xE9C46A), rgb(0xF4A261), rgb(0xE76F51), rgb(0xF1FAEE)}, // 6 gold
    {rgb(0x0B1320), rgb(0xCAF0F8), rgb(0x90E0EF), rgb(0x00B4D8), rgb(0x7C5CFF)}, // 7 arctic
    {rgb(0xF1E9DA), rgb(0xE63946), rgb(0x1D3557), rgb(0xF4A261), rgb(0x2A9D8F)}, // 8 bauhaus cream
    {rgb(0xFFF1E6), rgb(0xFF5D8F), rgb(0x3A86FF), rgb(0xFFBE0B), rgb(0x8338EC)}, // 9 memphis pastel
};

class Brush
{
public:
    explicit Brush(quint32 seed)
        : m_random(seed ? seed : 1)
    {
    }

    qreal real(qreal low, qreal high) { return low + m_random.generateDouble() * (high - low); }
    int integer(int low, int high) { return low + int(m_random.bounded(high - low + 1)); }
    bool chance(qreal p) { return m_random.generateDouble() < p; }
    template <typename T>
    const T &pick(std::initializer_list<T> list) { return *(list.begin() + m_random.bounded(int(list.size()))); }
    const Palette &palette(std::initializer_list<int> choices) { return kPalettes[pick(choices)]; }

private:
    QRandomGenerator m_random;
};

QColor alpha(QColor color, qreal a)
{
    color.setAlphaF(float(std::clamp(a, 0.0, 1.0)));
    return color;
}

QColor mix(const QColor &a, const QColor &b, qreal t)
{
    return QColor::fromRgbF(float(a.redF() + (b.redF() - a.redF()) * t), float(a.greenF() + (b.greenF() - a.greenF()) * t),
                            float(a.blueF() + (b.blueF() - a.blueF()) * t));
}

void glow(QPainter &p, const QPointF &center, qreal radius, const QColor &color, qreal strength = 1)
{
    QRadialGradient g(center, radius);
    g.setColorAt(0, alpha(color, 0.9 * strength));
    g.setColorAt(0.35, alpha(color, 0.35 * strength));
    g.setColorAt(1, alpha(color, 0));
    p.setPen(Qt::NoPen);
    p.setBrush(g);
    p.drawEllipse(center, radius, radius);
}

void vignette(QPainter &p, qreal strength = 0.55)
{
    QRadialGradient g(QPointF(500, 500), 720);
    g.setColorAt(0.55, QColor(0, 0, 0, 0));
    g.setColorAt(1, QColor(0, 0, 0, int(255 * strength)));
    p.fillRect(QRectF(0, 0, kCanvas, kCanvas), g);
}

void stars(QPainter &p, Brush &r, int count, qreal maxY = kCanvas)
{
    p.setPen(Qt::NoPen);
    for (int i = 0; i < count; ++i) {
        const qreal size = r.chance(0.08) ? r.real(2.2, 3.4) : r.real(0.8, 1.8);
        p.setBrush(QColor(255, 255, 255, r.integer(70, 230)));
        p.drawEllipse(QPointF(r.real(0, kCanvas), r.real(0, maxY)), size, size);
    }
}

// Film grain, from a tile made once; gives flat gradients some texture.
const QImage &grainTile()
{
    static const QImage tile = [] {
        QImage image(256, 256, QImage::Format_ARGB32_Premultiplied);
        QRandomGenerator random(7);
        for (int y = 0; y < image.height(); ++y) {
            auto *line = reinterpret_cast<QRgb *>(image.scanLine(y));
            for (int x = 0; x < image.width(); ++x) {
                const int v = int(random.bounded(256));
                const int a = 18;
                line[x] = qRgba(v * a / 255, v * a / 255, v * a / 255, a);
            }
        }
        return image;
    }();
    return tile;
}

void grain(QPainter &p, qreal opacity = 1)
{
    p.save();
    p.resetTransform();
    p.setOpacity(opacity);
    p.setCompositionMode(QPainter::CompositionMode_Overlay);
    p.fillRect(p.device() ? QRect(0, 0, p.device()->width(), p.device()->height()) : QRect(), QBrush(grainTile()));
    p.restore();
}

void fillBackground(QPainter &p, const QColor &top, const QColor &bottom)
{
    QLinearGradient g(0, 0, 0, kCanvas);
    g.setColorAt(0, top);
    g.setColorAt(1, bottom);
    p.fillRect(QRectF(0, 0, kCanvas, kCanvas), g);
}

// ---- The styles ----------------------------------------------------------

void aurora(QPainter &p, Brush &r)
{
    const Palette &pal = r.palette({0, 2, 3, 7});
    fillBackground(p, mix(pal.background, Qt::black, 0.4), mix(pal.background, pal.a, 0.15));
    stars(p, r, 160, 650);
    const QColor colors[] = {pal.d, pal.c, pal.a, pal.b};
    p.setCompositionMode(QPainter::CompositionMode_Plus);
    for (int band = 0; band < 4; ++band) {
        const qreal base = r.real(300, 560);
        const qreal phase = r.real(0, 6.28);
        const qreal amp = r.real(50, 110);
        const qreal ripple = r.real(25, 45);
        // Thin overlapping rays whose height flows smoothly: a curtain of light.
        for (int i = 0; i < 220; ++i) {
            const qreal x = i * kCanvas / 219;
            const qreal y = base + std::sin(x / 180 + phase) * amp + std::sin(x / 61 + phase * 2) * amp * 0.25;
            const qreal height = 230 + std::sin(x / ripple + phase) * 70 + std::sin(x / 13 + band) * 18;
            QLinearGradient g(x, y - height, x, y + 20);
            g.setColorAt(0, alpha(colors[band], 0));
            g.setColorAt(0.8, alpha(colors[band], 0.05));
            g.setColorAt(1, alpha(colors[band], 0));
            p.fillRect(QRectF(x - 6, y - height, 12, height + 20), g);
        }
    }
    p.setCompositionMode(QPainter::CompositionMode_SourceOver);
    // Mountains against the sky.
    for (int layer = 0; layer < 2; ++layer) {
        QPainterPath ridge(QPointF(0, kCanvas));
        qreal y = 780 + layer * 70;
        for (qreal x = 0; x <= kCanvas; x += 50) {
            y = std::clamp(y + r.real(-45, 45), 700.0 + layer * 60, 900.0);
            ridge.lineTo(x, y);
        }
        ridge.lineTo(kCanvas, kCanvas);
        p.setPen(Qt::NoPen);
        p.setBrush(mix(pal.background, Qt::black, layer ? 0.75 : 0.55));
        p.drawPath(ridge);
    }
    vignette(p, 0.4);
}

void synthwave(QPainter &p, Brush &r)
{
    const Palette &pal = r.palette({0, 1, 5});
    fillBackground(p, mix(pal.background, Qt::black, 0.3), mix(pal.d, pal.b, 0.4));
    stars(p, r, 90, 450);
    // Striped sun.
    const QPointF sun(500, r.real(420, 470));
    const qreal radius = r.real(200, 250);
    glow(p, sun, radius * 1.9, pal.b, 0.6);
    QLinearGradient sunFill(0, sun.y() - radius, 0, sun.y() + radius);
    sunFill.setColorAt(0, pal.c);
    sunFill.setColorAt(1, pal.b);
    QPainterPath disc;
    disc.addEllipse(sun, radius, radius);
    QPainterPath cuts;
    for (int i = 0; i < 7; ++i) {
        const qreal y = sun.y() + i * radius / 7 + 10;
        cuts.addRect(QRectF(0, y, kCanvas, 3 + i * 2.4));
    }
    p.setPen(Qt::NoPen);
    p.setBrush(sunFill);
    p.drawPath(disc.subtracted(cuts));
    // Horizon and the perspective grid.
    const qreal horizon = 640;
    QLinearGradient ground(0, horizon, 0, kCanvas);
    ground.setColorAt(0, mix(pal.background, pal.d, 0.3));
    ground.setColorAt(1, mix(pal.background, Qt::black, 0.5));
    p.fillRect(QRectF(0, horizon, kCanvas, kCanvas - horizon), ground);
    p.setPen(QPen(alpha(pal.b, 0.85), 2.2));
    for (int i = -12; i <= 12; ++i)
        p.drawLine(QPointF(500 + i * 18, horizon), QPointF(500 + i * 140, kCanvas));
    for (int i = 0; i < 10; ++i) {
        const qreal t = std::pow(i / 9.0, 2.1);
        const qreal y = horizon + t * (kCanvas - horizon);
        p.setPen(QPen(alpha(pal.b, 0.35 + 0.6 * t), 1.5 + t * 2));
        p.drawLine(QPointF(0, y), QPointF(kCanvas, y));
    }
    p.setPen(QPen(alpha(pal.c, 0.9), 3));
    p.drawLine(QPointF(0, horizon), QPointF(kCanvas, horizon));
    vignette(p, 0.35);
}

void vinyl(QPainter &p, Brush &r)
{
    const Palette &pal = r.palette({0, 1, 4, 6, 5});
    fillBackground(p, mix(pal.background, pal.a, 0.18), pal.background);
    const QPointF center(r.real(470, 540), r.real(470, 540));
    const qreal radius = 400;
    glow(p, center, radius * 1.3, pal.a, 0.35);
    p.setPen(Qt::NoPen);
    p.setBrush(QColor(12, 12, 14));
    p.drawEllipse(center, radius, radius);
    // Grooves.
    for (qreal ring = radius - 12; ring > 140; ring -= r.real(4, 7)) {
        p.setPen(QPen(QColor(255, 255, 255, r.integer(6, 20)), 1));
        p.setBrush(Qt::NoBrush);
        p.drawEllipse(center, ring, ring);
    }
    // Sheen across the record.
    QConicalGradient sheen(center, r.real(20, 70));
    sheen.setColorAt(0, QColor(255, 255, 255, 0));
    sheen.setColorAt(0.08, QColor(255, 255, 255, 38));
    sheen.setColorAt(0.16, QColor(255, 255, 255, 0));
    sheen.setColorAt(0.5, QColor(255, 255, 255, 0));
    sheen.setColorAt(0.58, QColor(255, 255, 255, 30));
    sheen.setColorAt(0.66, QColor(255, 255, 255, 0));
    sheen.setColorAt(1, QColor(255, 255, 255, 0));
    p.setPen(Qt::NoPen);
    p.setBrush(sheen);
    p.drawEllipse(center, radius, radius);
    // Label.
    QLinearGradient label(center.x() - 130, center.y() - 130, center.x() + 130, center.y() + 130);
    label.setColorAt(0, pal.a);
    label.setColorAt(1, pal.b);
    p.setBrush(label);
    p.drawEllipse(center, 135, 135);
    p.setPen(QPen(alpha(pal.c, 0.9), 6));
    p.setBrush(Qt::NoBrush);
    p.drawEllipse(center, 96, 96);
    p.setPen(Qt::NoPen);
    p.setBrush(pal.background);
    p.drawEllipse(center, 11, 11);
    vignette(p, 0.45);
}

void nebula(QPainter &p, Brush &r)
{
    const Palette &pal = r.palette({0, 2, 5, 7});
    p.fillRect(QRectF(0, 0, kCanvas, kCanvas), mix(pal.background, Qt::black, 0.5));
    p.setCompositionMode(QPainter::CompositionMode_Plus);
    const QColor colors[] = {pal.a, pal.b, pal.d, pal.c};
    for (int i = 0; i < 26; ++i)
        glow(p, QPointF(r.real(150, 850), r.real(150, 850)), r.real(120, 360), colors[i % 4], r.real(0.12, 0.3));
    p.setCompositionMode(QPainter::CompositionMode_SourceOver);
    stars(p, r, 260);
    for (int i = 0; i < 4; ++i) {
        const QPointF star(r.real(100, 900), r.real(100, 900));
        glow(p, star, r.real(30, 60), Qt::white, 0.7);
        p.setPen(QPen(QColor(255, 255, 255, 140), 1.2));
        p.drawLine(star - QPointF(50, 0), star + QPointF(50, 0));
        p.drawLine(star - QPointF(0, 50), star + QPointF(0, 50));
    }
    vignette(p, 0.5);
}

void liquid(QPainter &p, Brush &r)
{
    const Palette &pal = r.palette({0, 1, 2, 5, 3, 7});
    p.fillRect(QRectF(0, 0, kCanvas, kCanvas), pal.a);
    // A gradient mesh made of big soft blobs.
    const QColor colors[] = {pal.b, pal.c, pal.d, pal.background, pal.a};
    for (int i = 0; i < 9; ++i) {
        const QPointF center(r.real(-100, 1100), r.real(-100, 1100));
        const qreal radius = r.real(380, 640);
        QRadialGradient g(center, radius);
        g.setColorAt(0, alpha(colors[i % 5], 0.95));
        g.setColorAt(1, alpha(colors[i % 5], 0));
        p.fillRect(QRectF(0, 0, kCanvas, kCanvas), g);
    }
    // Soft glassy ribbons on top.
    for (int i = 0; i < 3; ++i) {
        QPainterPath ribbon;
        const qreal y = r.real(150, 850);
        ribbon.moveTo(-50, y);
        ribbon.cubicTo(r.real(200, 400), y + r.real(-300, 300), r.real(600, 800), y + r.real(-300, 300), 1050, y + r.real(-200, 200));
        p.setPen(QPen(QColor(255, 255, 255, 34), r.real(30, 90), Qt::SolidLine, Qt::RoundCap));
        p.setBrush(Qt::NoBrush);
        p.drawPath(ribbon);
    }
}

void bauhaus(QPainter &p, Brush &r)
{
    const Palette &pal = kPalettes[8];
    p.fillRect(QRectF(0, 0, kCanvas, kCanvas), pal.background);
    const QColor colors[] = {pal.a, pal.b, pal.c, pal.d};
    p.setPen(Qt::NoPen);
    // A 4x4 grid of tiles, each a quarter or half circle, a circle or a block.
    const qreal cell = kCanvas / 4;
    for (int y = 0; y < 4; ++y) {
        for (int x = 0; x < 4; ++x) {
            const QRectF tile(x * cell, y * cell, cell, cell);
            const QColor color = colors[r.integer(0, 3)];
            if (r.chance(0.25)) {
                p.fillRect(tile, colors[r.integer(0, 3)]);
            }
            p.setBrush(color);
            switch (r.integer(0, 4)) {
            case 0:
                p.drawPie(QRectF(tile.topLeft() - QPointF(cell, cell) * (r.integer(0, 1)), QSizeF(cell * 2, cell * 2)),
                          r.integer(0, 3) * 90 * 16, 90 * 16);
                break;
            case 1:
                p.drawEllipse(tile.adjusted(18, 18, -18, -18));
                break;
            case 2:
                p.drawChord(tile.adjusted(0, 0, 0, cell), 0, 180 * 16);
                break;
            case 3:
                p.drawRect(tile.adjusted(0, cell / 2, 0, 0));
                break;
            default:
                p.setBrush(pal.background);
                p.drawEllipse(tile.center(), cell * 0.18, cell * 0.18);
                break;
            }
        }
    }
    grain(p, 1);
}

void waves(QPainter &p, Brush &r)
{
    const Palette &pal = r.palette({0, 2, 5, 1, 7});
    fillBackground(p, pal.background, mix(pal.background, Qt::black, 0.5));
    const int lines = 46;
    const qreal f1 = r.real(0.006, 0.012), f2 = r.real(0.015, 0.03);
    const qreal phase = r.real(0, 6.28);
    for (int i = 0; i < lines; ++i) {
        const qreal t = i / qreal(lines - 1);
        const qreal center = 260 + t * 480;
        QPainterPath path(QPointF(0, center));
        for (qreal x = 0; x <= kCanvas; x += 8) {
            // Louder in the middle, like a sound wave.
            const qreal envelope = std::sin(x / kCanvas * M_PI);
            const qreal y = center + envelope * (std::sin(x * f1 + phase + t * 3) * 90 + std::sin(x * f2 + t * 6) * 30);
            path.lineTo(x, y);
        }
        p.setPen(QPen(alpha(mix(pal.a, pal.b, t), 0.55), 1.6));
        p.setBrush(Qt::NoBrush);
        p.drawPath(path);
    }
    vignette(p, 0.45);
}

void skyline(QPainter &p, Brush &r)
{
    const Palette &pal = r.palette({0, 1, 2, 4, 5});
    fillBackground(p, mix(pal.background, Qt::black, 0.4), mix(pal.background, pal.a, 0.25));
    const qreal floor = 720;
    const int bars = r.integer(20, 30);
    const qreal width = kCanvas / bars;
    for (int i = 0; i < bars; ++i) {
        const qreal t = i / qreal(bars - 1);
        const qreal envelope = 0.35 + 0.65 * std::sin(t * M_PI);
        const qreal height = r.real(80, 520) * envelope;
        const QRectF bar(i * width + 4, floor - height, width - 8, height);
        QLinearGradient g(0, bar.top(), 0, floor);
        g.setColorAt(0, mix(pal.a, pal.b, t));
        g.setColorAt(1, alpha(mix(pal.a, pal.b, t), 0.5));
        p.setPen(Qt::NoPen);
        p.setBrush(g);
        p.drawRoundedRect(bar, 6, 6);
        // Peak caps and the reflection on the floor.
        p.setBrush(alpha(pal.c, 0.95));
        p.drawRoundedRect(QRectF(bar.left(), bar.top() - 18, bar.width(), 7), 3, 3);
        QLinearGradient mirror(0, floor, 0, floor + height * 0.6);
        mirror.setColorAt(0, alpha(mix(pal.a, pal.b, t), 0.3));
        mirror.setColorAt(1, alpha(mix(pal.a, pal.b, t), 0));
        p.setBrush(mirror);
        p.drawRect(QRectF(bar.left(), floor + 6, bar.width(), height * 0.6));
    }
    vignette(p, 0.4);
}

void topographic(QPainter &p, Brush &r)
{
    const Palette &pal = r.palette({0, 3, 6, 7, 2});
    QLinearGradient ground(0, 0, kCanvas, kCanvas);
    ground.setColorAt(0, mix(pal.background, pal.a, 0.22));
    ground.setColorAt(1, mix(pal.background, pal.b, 0.18));
    p.fillRect(QRectF(0, 0, kCanvas, kCanvas), ground);
    const int peaks = r.integer(1, 2);
    for (int k = 0; k < peaks; ++k) {
        const QPointF center(r.real(250, 750), r.real(250, 750));
        const qreal wobble = r.real(0.05, 0.11);
        const int seedA = r.integer(2, 5), seedB = r.integer(3, 7);
        const qreal phase = r.real(0, 6.28);
        for (int ring = 1; ring < 22; ++ring) {
            const qreal radius = ring * 34 + r.real(-3, 3);
            QPainterPath path;
            for (int i = 0; i <= 120; ++i) {
                const qreal angle = i / 120.0 * 2 * M_PI;
                const qreal rr = radius * (1 + wobble * std::sin(angle * seedA + phase + ring * 0.2)
                                           + wobble * 0.5 * std::cos(angle * seedB - phase));
                const QPointF point = center + QPointF(std::cos(angle) * rr, std::sin(angle) * rr);
                i ? path.lineTo(point) : path.moveTo(point);
            }
            const bool major = ring % 5 == 0;
            p.setPen(QPen(alpha(major ? pal.a : pal.d, major ? 0.8 : 0.3), major ? 2.4 : 1.2));
            p.setBrush(Qt::NoBrush);
            p.drawPath(path);
        }
    }
    grain(p, 0.8);
    vignette(p, 0.35);
}

void bokeh(QPainter &p, Brush &r)
{
    const Palette &pal = r.palette({1, 4, 6, 5, 0});
    fillBackground(p, mix(pal.background, Qt::black, 0.3), pal.background);
    const QColor colors[] = {pal.a, pal.b, pal.c, pal.d};
    p.setCompositionMode(QPainter::CompositionMode_Screen);
    for (int i = 0; i < 70; ++i) {
        const QPointF c(r.real(0, kCanvas), r.real(0, kCanvas));
        const qreal radius = r.chance(0.2) ? r.real(70, 140) : r.real(15, 60);
        const QColor color = colors[i % 4];
        QRadialGradient g(c, radius);
        g.setColorAt(0, alpha(color, 0.18));
        g.setColorAt(0.85, alpha(color, 0.32));
        g.setColorAt(1, alpha(color, 0));
        p.setPen(Qt::NoPen);
        p.setBrush(g);
        p.drawEllipse(c, radius, radius);
    }
    p.setCompositionMode(QPainter::CompositionMode_SourceOver);
    vignette(p, 0.5);
}

void cassette(QPainter &p, Brush &r)
{
    const Palette &pal = r.palette({1, 5, 9, 0, 4});
    const bool light = pal.background.lightnessF() > 0.5;
    p.fillRect(QRectF(0, 0, kCanvas, kCanvas), pal.background);
    // Diagonal stripes behind.
    p.save();
    p.translate(500, 500);
    p.rotate(-20);
    const QColor stripes[] = {pal.a, pal.c, pal.d};
    for (int i = 0; i < 3; ++i)
        p.fillRect(QRectF(-800, -260 + i * 90, 1600, 70), alpha(stripes[i], 0.85));
    p.restore();
    // The tape.
    const QRectF body(150, 250, 700, 460);
    p.setPen(Qt::NoPen);
    p.setBrush(QColor(0, 0, 0, 90));
    p.drawRoundedRect(body.translated(16, 22), 36, 36);
    p.setBrush(light ? QColor(0x22, 0x22, 0x2A) : QColor(0xEE, 0xEA, 0xE2));
    p.drawRoundedRect(body, 36, 36);
    QLinearGradient label(body.left(), 0, body.right(), 0);
    label.setColorAt(0, pal.a);
    label.setColorAt(1, pal.b);
    p.setBrush(label);
    p.drawRoundedRect(QRectF(body.left() + 50, body.top() + 40, body.width() - 100, 250), 18, 18);
    p.setBrush(QColor(20, 20, 24));
    p.drawRoundedRect(QRectF(body.left() + 160, body.top() + 140, body.width() - 320, 110), 55, 55);
    for (qreal x : {body.left() + 215, body.right() - 215}) {
        p.setBrush(QColor(0xF5, 0xF5, 0xF5));
        p.drawEllipse(QPointF(x, body.top() + 195), 40, 40);
        p.setBrush(QColor(20, 20, 24));
        for (int k = 0; k < 6; ++k) {
            const qreal a = k * M_PI / 3;
            p.drawEllipse(QPointF(x + std::cos(a) * 24, body.top() + 195 + std::sin(a) * 24), 6, 6);
        }
    }
    p.setBrush(light ? QColor(0x33, 0x33, 0x3D) : QColor(0xD5, 0xD0, 0xC6));
    QPainterPath trapezoid;
    trapezoid.addPolygon(QPolygonF({{body.left() + 170, body.bottom()}, {body.left() + 210, body.bottom() - 90},
                                    {body.right() - 210, body.bottom() - 90}, {body.right() - 170, body.bottom()}}));
    p.drawPath(trapezoid);
    grain(p, 1);
}

void oceanDusk(QPainter &p, Brush &r)
{
    const Palette &pal = r.palette({1, 2, 7, 0});
    fillBackground(p, mix(pal.background, pal.d, 0.3), mix(pal.b, pal.a, 0.5));
    const QPointF moon(r.real(300, 700), r.real(220, 330));
    glow(p, moon, 260, pal.c, 0.45);
    p.setPen(Qt::NoPen);
    p.setBrush(mix(pal.c, Qt::white, 0.6));
    p.drawEllipse(moon, 70, 70);
    for (int layer = 0; layer < 7; ++layer) {
        const qreal t = layer / 6.0;
        const qreal base = 540 + layer * 70;
        QPainterPath wave(QPointF(0, kCanvas));
        const qreal phase = r.real(0, 6.28);
        for (qreal x = 0; x <= kCanvas; x += 10)
            wave.lineTo(x, base + std::sin(x / r.real(70, 110) + phase) * (10 + layer * 3));
        wave.lineTo(kCanvas, kCanvas);
        p.setBrush(mix(mix(pal.background, pal.a, 0.35), Qt::black, 0.15 + t * 0.6));
        p.drawPath(wave);
    }
    // The moon's path on the water.
    for (int i = 0; i < 26; ++i) {
        const qreal y = r.real(560, 960);
        const qreal w = r.real(20, 90) * (1 - (y - 560) / 600);
        p.setBrush(alpha(mix(pal.c, Qt::white, 0.5), r.real(0.25, 0.6)));
        p.drawRoundedRect(QRectF(moon.x() - w / 2 + r.real(-30, 30), y, w, 3), 1.5, 1.5);
    }
    vignette(p, 0.35);
}

void neonRings(QPainter &p, Brush &r)
{
    const Palette &pal = r.palette({0, 5, 2, 1});
    p.fillRect(QRectF(0, 0, kCanvas, kCanvas), mix(pal.background, Qt::black, 0.55));
    const QPointF center(500, 500);
    const QColor colors[] = {pal.a, pal.b, pal.d, pal.c};
    p.setCompositionMode(QPainter::CompositionMode_Plus);
    for (int i = 0; i < 9; ++i) {
        const qreal radius = 90 + i * 42;
        const int start = r.integer(0, 359) * 16;
        const int span = r.integer(90, 300) * 16;
        const QColor color = colors[i % 4];
        const QRectF box(center.x() - radius, center.y() - radius, radius * 2, radius * 2);
        for (int g = 3; g >= 0; --g) {
            p.setPen(QPen(alpha(color, g ? 0.08 : 0.95), 6 + g * 9, Qt::SolidLine, Qt::RoundCap));
            p.drawArc(box, start, span);
        }
    }
    p.setCompositionMode(QPainter::CompositionMode_SourceOver);
    glow(p, center, 120, pal.a, 0.5);
    vignette(p, 0.4);
}

void paperCut(QPainter &p, Brush &r)
{
    const Palette &pal = r.palette({1, 3, 6, 0, 2});
    fillBackground(p, mix(pal.c, Qt::white, 0.35), mix(pal.c, pal.a, 0.4));
    glow(p, QPointF(r.real(250, 750), 300), 140, Qt::white, 0.5);
    p.setPen(Qt::NoPen);
    p.setBrush(QColor(255, 255, 255, 230));
    p.drawEllipse(QPointF(r.real(250, 750), r.real(220, 320)), 60, 60);
    for (int layer = 0; layer < 6; ++layer) {
        const qreal t = layer / 5.0;
        const qreal base = 420 + layer * 95;
        QPainterPath hill(QPointF(-20, kCanvas));
        const qreal phase = r.real(0, 6.28);
        const qreal freq = r.real(0.004, 0.009);
        for (qreal x = -20; x <= kCanvas + 20; x += 12)
            hill.lineTo(x, base + std::sin(x * freq + phase) * r.real(38, 42) + std::sin(x * freq * 2.7 + phase) * 18);
        hill.lineTo(kCanvas + 20, kCanvas);
        // Each layer casts a soft shadow on the one behind.
        p.setBrush(QColor(0, 0, 0, 50));
        p.drawPath(hill.translated(0, -10));
        p.setBrush(mix(mix(pal.a, pal.b, t), pal.background, t * 0.75));
        p.drawPath(hill);
    }
    grain(p, 0.9);
}

void halftone(QPainter &p, Brush &r)
{
    const Palette &pal = r.palette({5, 1, 0, 4, 9});
    const bool light = pal.background.lightnessF() > 0.5;
    p.fillRect(QRectF(0, 0, kCanvas, kCanvas), pal.background);
    const QPointF focus(r.real(200, 800), r.real(200, 800));
    const qreal step = 26;
    p.setPen(Qt::NoPen);
    for (qreal y = step / 2; y < kCanvas; y += step) {
        for (qreal x = step / 2 + (int(y / step) % 2) * step / 2; x < kCanvas; x += step) {
            const qreal d = std::hypot(x - focus.x(), y - focus.y()) / 800;
            const qreal size = std::max(0.0, (1 - d) * step * 0.55);
            p.setBrush(mix(pal.a, pal.b, std::clamp(d * 1.3, 0.0, 1.0)));
            p.drawEllipse(QPointF(x, y), size, size);
        }
    }
    // A bold offset disc for the pop-art look.
    p.setBrush(alpha(pal.c, 0.9));
    const QPointF disc(1000 - focus.x(), 1000 - focus.y());
    p.drawEllipse(disc, 150, 150);
    p.setBrush(light ? QColor(0x1D, 0x1D, 0x24) : pal.background);
    p.drawEllipse(disc + QPointF(26, 26), 150, 150);
    p.setBrush(alpha(pal.c, 0.95));
    p.drawEllipse(disc, 138, 138);
    grain(p, 0.8);
}

void prism(QPainter &p, Brush &r)
{
    const Palette &pal = r.palette({0, 7, 2, 5});
    fillBackground(p, mix(pal.background, Qt::black, 0.5), pal.background);
    const QPointF apex(500, 260), left(300, 640), right(700, 640);
    const qreal entry = r.real(330, 470);
    // The white beam in, the spectrum out.
    p.setPen(QPen(QColor(255, 255, 255, 220), 6));
    p.drawLine(QPointF(0, entry + 40), QPointF(400, entry + 30));
    const QColor spectrum[] = {QColor(0xFF, 0x3B, 0x3B), QColor(0xFF, 0x9F, 0x1C), QColor(0xFF, 0xE1, 0x4D),
                               QColor(0x3D, 0xDC, 0x84), QColor(0x2F, 0x80, 0xED), QColor(0x9B, 0x51, 0xE0)};
    p.setCompositionMode(QPainter::CompositionMode_Plus);
    for (int i = 0; i < 6; ++i) {
        QPainterPath ray;
        const qreal y0 = 470 + i * 6;
        const qreal y1 = r.real(560, 600) + i * 70;
        ray.moveTo(600, y0);
        ray.lineTo(kCanvas + 10, y1);
        ray.lineTo(kCanvas + 10, y1 + 62);
        ray.lineTo(600, y0 + 6);
        p.setPen(Qt::NoPen);
        p.setBrush(alpha(spectrum[i], 0.75));
        p.drawPath(ray);
    }
    p.setCompositionMode(QPainter::CompositionMode_SourceOver);
    QLinearGradient glass(left, right);
    glass.setColorAt(0, QColor(255, 255, 255, 40));
    glass.setColorAt(1, QColor(255, 255, 255, 12));
    QPainterPath triangle;
    triangle.addPolygon(QPolygonF({apex, right, left, apex}));
    p.setBrush(glass);
    p.setPen(QPen(QColor(255, 255, 255, 200), 3, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    p.drawPath(triangle);
    glow(p, QPointF(600, 480), 90, Qt::white, 0.35);
    stars(p, r, 60);
    vignette(p, 0.45);
}

void memphis(QPainter &p, Brush &r)
{
    const Palette &pal = kPalettes[9];
    p.fillRect(QRectF(0, 0, kCanvas, kCanvas), pal.background);
    const QColor colors[] = {pal.a, pal.b, pal.c, pal.d};
    const QColor ink(0x1D, 0x1D, 0x24);
    for (int i = 0; i < 20; ++i) {
        const QPointF at(r.real(60, 940), r.real(60, 940));
        const QColor color = colors[i % 4];
        p.save();
        p.translate(at);
        p.rotate(r.real(0, 360));
        p.scale(1.7, 1.7);
        switch (i % 5) {
        case 0: { // squiggle
            QPainterPath squiggle(QPointF(-70, 0));
            for (int k = 0; k < 4; ++k)
                squiggle.quadTo(QPointF(-52 + k * 36, k % 2 ? 26 : -26), QPointF(-34 + k * 36, 0));
            p.setPen(QPen(color, 10, Qt::SolidLine, Qt::RoundCap));
            p.setBrush(Qt::NoBrush);
            p.drawPath(squiggle);
            break;
        }
        case 1: // triangle
            p.setPen(QPen(ink, 4));
            p.setBrush(color);
            p.drawPolygon(QPolygonF({{0, -40}, {38, 30}, {-38, 30}}));
            break;
        case 2: // dotted block
            p.setPen(Qt::NoPen);
            p.setBrush(ink);
            for (int y = 0; y < 4; ++y)
                for (int x = 0; x < 4; ++x)
                    p.drawEllipse(QPointF(x * 16 - 24, y * 16 - 24), 3.5, 3.5);
            break;
        case 3: // ring
            p.setPen(QPen(color, 12));
            p.setBrush(Qt::NoBrush);
            p.drawEllipse(QPointF(), 34, 34);
            break;
        default: // pill
            p.setPen(QPen(ink, 4));
            p.setBrush(color);
            p.drawRoundedRect(QRectF(-60, -16, 120, 32), 16, 16);
            break;
        }
        p.restore();
    }
    grain(p, 0.8);
}

void orbit(QPainter &p, Brush &r)
{
    const Palette &pal = r.palette({0, 2, 4, 7, 5});
    p.fillRect(QRectF(0, 0, kCanvas, kCanvas), mix(pal.background, Qt::black, 0.45));
    stars(p, r, 220);
    const QPointF sun(r.real(380, 620), r.real(380, 620));
    glow(p, sun, 260, pal.c, 0.7);
    QRadialGradient core(sun - QPointF(20, 20), 90);
    core.setColorAt(0, mix(pal.c, Qt::white, 0.5));
    core.setColorAt(1, pal.b);
    p.setPen(Qt::NoPen);
    p.setBrush(core);
    p.drawEllipse(sun, 80, 80);
    const QColor colors[] = {pal.a, pal.d, pal.b, pal.c};
    for (int i = 0; i < 5; ++i) {
        const qreal radius = 150 + i * 75;
        p.setPen(QPen(QColor(255, 255, 255, 34), 1.4));
        p.setBrush(Qt::NoBrush);
        p.drawEllipse(sun, radius, radius * 0.92);
        const qreal angle = r.real(0, 2 * M_PI);
        const QPointF planet = sun + QPointF(std::cos(angle) * radius, std::sin(angle) * radius * 0.92);
        const qreal size = r.real(10, 32);
        QRadialGradient shade(planet - QPointF(size / 3, size / 3), size * 1.4);
        shade.setColorAt(0, mix(colors[i % 4], Qt::white, 0.35));
        shade.setColorAt(1, mix(colors[i % 4], Qt::black, 0.5));
        p.setPen(Qt::NoPen);
        p.setBrush(shade);
        p.drawEllipse(planet, size, size);
        if (i == 3) {
            p.setPen(QPen(alpha(colors[i % 4], 0.8), 3));
            p.setBrush(Qt::NoBrush);
            p.drawEllipse(planet, size * 1.9, size * 0.55);
        }
    }
    vignette(p, 0.4);
}

using Painter = void (*)(QPainter &, Brush &);

struct Entry {
    const char *id;
    const char *name;
    Painter paint;
};

const Entry kStyles[] = {
    {"aurora", QT_TRANSLATE_NOOP("ArtworkGallery", "Aurora"), aurora},
    {"synthwave", QT_TRANSLATE_NOOP("ArtworkGallery", "Synthwave Sunset"), synthwave},
    {"vinyl", QT_TRANSLATE_NOOP("ArtworkGallery", "Vinyl"), vinyl},
    {"nebula", QT_TRANSLATE_NOOP("ArtworkGallery", "Nebula"), nebula},
    {"liquid", QT_TRANSLATE_NOOP("ArtworkGallery", "Liquid Gradient"), liquid},
    {"bauhaus", QT_TRANSLATE_NOOP("ArtworkGallery", "Bauhaus"), bauhaus},
    {"waves", QT_TRANSLATE_NOOP("ArtworkGallery", "Sound Waves"), waves},
    {"skyline", QT_TRANSLATE_NOOP("ArtworkGallery", "Equalizer Skyline"), skyline},
    {"topographic", QT_TRANSLATE_NOOP("ArtworkGallery", "Topographic"), topographic},
    {"bokeh", QT_TRANSLATE_NOOP("ArtworkGallery", "Bokeh Night"), bokeh},
    {"cassette", QT_TRANSLATE_NOOP("ArtworkGallery", "Mixtape"), cassette},
    {"ocean", QT_TRANSLATE_NOOP("ArtworkGallery", "Ocean Dusk"), oceanDusk},
    {"rings", QT_TRANSLATE_NOOP("ArtworkGallery", "Neon Rings"), neonRings},
    {"papercut", QT_TRANSLATE_NOOP("ArtworkGallery", "Paper Hills"), paperCut},
    {"halftone", QT_TRANSLATE_NOOP("ArtworkGallery", "Halftone Pop"), halftone},
    {"prism", QT_TRANSLATE_NOOP("ArtworkGallery", "Prism"), prism},
    {"memphis", QT_TRANSLATE_NOOP("ArtworkGallery", "Memphis"), memphis},
    {"orbit", QT_TRANSLATE_NOOP("ArtworkGallery", "Orbit"), orbit},
};

const Entry &entryFor(const QString &id)
{
    for (const Entry &entry : kStyles) {
        if (id == QLatin1String(entry.id))
            return entry;
    }
    return kStyles[0];
}

QString settingsFile()
{
    return PlaylistSession::configDir() + QStringLiteral("/settings.ini");
}

} // namespace

namespace ArtworkGallery {

QList<Style> styles()
{
    QList<Style> result;
    for (const Entry &entry : kStyles)
        result.append({QString::fromLatin1(entry.id), QCoreApplication::translate("ArtworkGallery", entry.name)});
    return result;
}

bool isStyle(const QString &id)
{
    return std::any_of(std::begin(kStyles), std::end(kStyles), [&](const Entry &e) { return id == QLatin1String(e.id); });
}

quint32 seedFor(const QString &trackPath)
{
    const QByteArray hash = QCryptographicHash::hash(trackPath.toUtf8(), QCryptographicHash::Sha1);
    return quint32(uchar(hash[0])) << 24 | quint32(uchar(hash[1])) << 16 | quint32(uchar(hash[2])) << 8 | uchar(hash[3]);
}

QString styleFor(const QString &trackPath)
{
    const quint32 seed = seedFor(trackPath);
    return QString::fromLatin1(kStyles[(seed >> 8) % std::size(kStyles)].id);
}

QImage render(const QString &id, int size, quint32 seed)
{
    // Song changes ask for the same few pictures again and again.
    static QMutex mutex;
    static QCache<QString, QImage> cache(3 * 1024 * 1024 * 4 / 1024); // in KiB: three 1024px images
    const QString key = QStringLiteral("%1/%2/%3").arg(id).arg(size).arg(seed);
    {
        QMutexLocker lock(&mutex);
        if (const QImage *cached = cache.object(key))
            return *cached;
    }
    QImage image(size, size, QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::black);
    {
        QPainter p(&image);
        p.setRenderHint(QPainter::Antialiasing);
        p.scale(size / kCanvas, size / kCanvas);
        Brush brush(seed ^ qHash(id));
        entryFor(id).paint(p, brush);
    }
    QMutexLocker lock(&mutex);
    cache.insert(key, new QImage(image), int(image.sizeInBytes() / 1024));
    return image;
}

QString autoChoice()
{
    return QStringLiteral("auto");
}

QString noneChoice()
{
    return QStringLiteral("none");
}

QString choice()
{
    const QString value = QSettings(settingsFile(), QSettings::IniFormat)
                              .value(QStringLiteral("audio/builtinArtwork"), autoChoice())
                              .toString();
    return value == noneChoice() || isStyle(value) ? value : autoChoice();
}

void setChoice(const QString &value)
{
    QSettings(settingsFile(), QSettings::IniFormat).setValue(QStringLiteral("audio/builtinArtwork"), value);
}

QImage artworkFor(const QString &trackPath, int size)
{
    const QString current = choice();
    if (current == noneChoice())
        return {};
    const QString style = current == autoChoice() ? styleFor(trackPath) : current;
    return render(style, size, seedFor(trackPath));
}

} // namespace ArtworkGallery
