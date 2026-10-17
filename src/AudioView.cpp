#include "AudioView.h"
#include "Theme.h"

#include <QEvent>
#include <QLinearGradient>
#include <QPainter>
#include <QPainterPath>
#include <QRadialGradient>

#include <algorithm>
#include <cmath>

namespace {

constexpr int kMargin = 24;
constexpr int kTextGap = 18;
constexpr int kMinCover = 64;
constexpr qreal kCornerRadius = 6;
// The shadow is drawn at 1/kShadowScale and scaled up smoothly, which blurs it.
constexpr int kShadowScale = 8;
constexpr int kShadowSpread = 28;
constexpr int kShadowOffset = 10;

QFont scaledFont(const QFont &base, qreal factor, bool bold)
{
    QFont font = base;
    font.setPointSizeF(base.pointSizeF() * factor);
    font.setBold(bold);
    return font;
}

// Average color of the image, darkened, for the glow behind the cover.
QColor tintOf(const QImage &image)
{
    if (image.isNull())
        return {};
    const QColor average = image.scaled(1, 1, Qt::IgnoreAspectRatio, Qt::SmoothTransformation).pixelColor(0, 0);
    return QColor::fromHsvF(std::max<float>(average.hsvHueF(), 0), std::min<float>(average.hsvSaturationF(), 0.6f),
                            std::min<float>(average.valueF(), 0.32f));
}

void drawNote(QPainter &p, const QRectF &rect, const QColor &color)
{
    // A beamed pair of eighth notes, drawn on a 20x20 grid.
    p.save();
    p.translate(rect.topLeft());
    p.scale(rect.width() / 20, rect.height() / 20);
    p.setPen(Qt::NoPen);
    p.setBrush(color);
    p.drawEllipse(QPointF(5.5, 15), 2.6, 2);
    p.drawEllipse(QPointF(14.5, 13), 2.6, 2);
    p.drawRect(QRectF(7.3, 4.5, 1.2, 10.5));
    p.drawRect(QRectF(16.3, 2.5, 1.2, 10.5));
    QPainterPath beam;
    beam.addPolygon(QPolygonF({{7.3, 4.5}, {17.5, 2.5}, {17.5, 5}, {7.3, 7}}));
    p.drawPath(beam);
    p.restore();
}

} // namespace

AudioView::AudioView(QWidget *parent)
    : QWidget(parent)
{
    setObjectName(QStringLiteral("AudioView"));
    // Clicks, double-clicks and drops go to the player underneath.
    setAttribute(Qt::WA_TransparentForMouseEvents);
    setFocusPolicy(Qt::NoFocus);
    hide();
    parent->installEventFilter(this);
    setGeometry(parent->rect());
}

bool AudioView::eventFilter(QObject *watched, QEvent *event)
{
    if (watched == parent() && event->type() == QEvent::Resize)
        setGeometry(parentWidget()->rect());
    return QWidget::eventFilter(watched, event);
}

void AudioView::setMode(Mode mode)
{
    if (mode == m_mode)
        return;
    m_mode = mode;
    // Over a visualization only the caption is drawn; the rest stays see-through.
    setAttribute(Qt::WA_NoSystemBackground, mode == Mode::Visualizer);
    update();
}

void AudioView::setArtwork(const QImage &image)
{
    m_artwork = image;
    m_tint = tintOf(image);
    m_scaledArtwork = {};
    m_backdrop = {};
    update();
}

void AudioView::setTrackInfo(const AudioArtwork::TrackInfo &info)
{
    m_info = info;
    update();
}

int AudioView::textBlockHeight() const
{
    const QFont base = font();
    int height = QFontMetrics(scaledFont(base, 1.5, true)).height();
    if (!m_info.artist.isEmpty())
        height += 4 + QFontMetrics(scaledFont(base, 1.1, false)).height();
    if (!m_info.album.isEmpty())
        height += 2 + QFontMetrics(base).height();
    return height;
}

AudioView::Layout AudioView::layout() const
{
    const QRect area = rect().adjusted(kMargin, kMargin, -kMargin, -kMargin);
    const int textHeight = textBlockHeight();
    Layout result;
    if (m_mode == Mode::Artwork) {
        // The cover is square for the placeholder, else the image's shape.
        QSize cover = m_artwork.isNull() ? QSize(1, 1) : m_artwork.size();
        const int maxHeight = area.height() - textHeight - kTextGap;
        cover.scale(std::min(area.width(), static_cast<int>(area.width() * 0.8)), std::min(maxHeight, 640),
                    Qt::KeepAspectRatio);
        if (cover.width() >= kMinCover && cover.height() >= kMinCover) {
            const int top = area.top() + (area.height() - cover.height() - kTextGap - textHeight) / 2;
            result.cover = QRect(QPoint(area.center().x() - cover.width() / 2 + 1, top), cover);
            result.text = QRect(area.left(), result.cover.bottom() + kTextGap, area.width(), textHeight);
            return result;
        }
    }
    // Too small for a cover, or no cover wanted: the text alone, centered.
    result.text = QRect(area.left(), area.center().y() - textHeight / 2, area.width(), textHeight);
    return result;
}

QRect AudioView::artworkRect() const
{
    return isVisible() ? layout().cover : QRect();
}

void AudioView::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    p.setRenderHint(QPainter::SmoothPixmapTransform);

    if (m_mode == Mode::Visualizer) {
        paintCaption(p);
        return;
    }
    paintBackground(p);
    const Layout l = layout();
    if (!l.cover.isEmpty())
        paintCover(p, l.cover);
    else if (m_mode == Mode::Canvas && height() > textBlockHeight() + 3 * kMargin + 48)
        drawNote(p, QRectF(width() / 2.0 - 20, l.text.top() - 64, 40, 40), Theme::mix(Theme::Raised, Theme::TextDim, 0.35));
    paintText(p, l.text, Qt::AlignHCenter);
}

const QPixmap &AudioView::backdrop()
{
    const QSize pixels = size() * devicePixelRatioF();
    if (pixels == m_backdropSize && !m_backdrop.isNull())
        return m_backdrop;
    m_backdropSize = pixels;
    // Shrunk to a few pixels and scaled back up smoothly: a strong, cheap blur.
    // Cropped to the view's shape first, so it fills it without stretching.
    const QSize crop = pixels.scaled(m_artwork.size(), Qt::KeepAspectRatio);
    const QImage cropped = m_artwork.copy(QRect(QPoint((m_artwork.width() - crop.width()) / 2,
                                                       (m_artwork.height() - crop.height()) / 2), crop));
    QImage soft = cropped.scaled(cropped.size().scaled(10, 10, Qt::KeepAspectRatioByExpanding),
                                 Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
    // Doubling in steps keeps the blocks of the tiny image from showing.
    while (soft.width() * 2 < pixels.width())
        soft = soft.scaled(soft.size() * 2, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
    m_backdrop = QPixmap::fromImage(soft.scaled(pixels, Qt::IgnoreAspectRatio, Qt::SmoothTransformation));
    m_backdrop.setDevicePixelRatio(devicePixelRatioF());
    return m_backdrop;
}

void AudioView::paintBackground(QPainter &p)
{
    const QColor background = Theme::Surface;
    p.fillRect(rect(), background);
    if (m_mode == Mode::Artwork && !m_artwork.isNull()) {
        // The cover's colors fill the view, blurred and dimmed under it.
        p.drawPixmap(0, 0, backdrop());
        p.fillRect(rect(), Theme::withAlpha(background, Theme::isLight() ? 0.62 : 0.58));
        QRadialGradient shade(rect().center(), std::max(width(), height()) * 0.75);
        shade.setColorAt(0.4, Theme::withAlpha(background, 0));
        shade.setColorAt(1, Theme::withAlpha(background, 0.75));
        p.fillRect(rect(), shade);
        return;
    }
    // A faint glow in the cover's color, or an ambient one in the accent's.
    const bool cover = m_mode == Mode::Artwork && m_tint.isValid();
    const qreal reach = std::max(width(), height());
    QRadialGradient glow(rect().center(), reach * 0.6);
    glow.setColorAt(0, cover ? m_tint : Theme::mix(background, Theme::Accent, 0.16));
    glow.setColorAt(1, background);
    p.fillRect(rect(), glow);
    if (!cover) {
        QRadialGradient second(QPointF(width() * 0.85, height() * 0.9), reach * 0.5);
        second.setColorAt(0, Theme::withAlpha(Theme::Accent2, 0.1));
        second.setColorAt(1, Theme::withAlpha(Theme::Accent2, 0));
        p.fillRect(rect(), second);
    }
}

const QPixmap &AudioView::shadow(const QSize &coverSize)
{
    if (coverSize == m_shadowSize)
        return m_shadow;
    m_shadowSize = coverSize;
    const QSize full = coverSize + QSize(2 * kShadowSpread, 2 * kShadowSpread);
    QImage small(full / kShadowScale + QSize(1, 1), QImage::Format_ARGB32_Premultiplied);
    small.fill(Qt::transparent);
    {
        QPainter sp(&small);
        sp.setRenderHint(QPainter::Antialiasing);
        sp.setPen(Qt::NoPen);
        sp.setBrush(QColor(0, 0, 0, 190));
        const qreal inset = static_cast<qreal>(kShadowSpread) / kShadowScale;
        sp.drawRoundedRect(QRectF(inset, inset, static_cast<qreal>(coverSize.width()) / kShadowScale,
                                  static_cast<qreal>(coverSize.height()) / kShadowScale), 1, 1);
    }
    m_shadow = QPixmap::fromImage(small.scaled(full, Qt::IgnoreAspectRatio, Qt::SmoothTransformation)
                                      .scaled(full, Qt::IgnoreAspectRatio, Qt::SmoothTransformation));
    return m_shadow;
}

void AudioView::paintCover(QPainter &p, const QRect &rect)
{
    p.drawPixmap(rect.topLeft() - QPoint(kShadowSpread, kShadowSpread - kShadowOffset), shadow(rect.size()));

    QPainterPath clip;
    clip.addRoundedRect(QRectF(rect), kCornerRadius, kCornerRadius);
    p.save();
    p.setClipPath(clip);
    if (m_artwork.isNull()) {
        p.fillRect(rect, Theme::Raised);
        const qreal note = rect.width() * 0.4;
        drawNote(p, QRectF(rect.center().x() - note / 2, rect.center().y() - note / 2, note, note),
                 Theme::mix(Theme::Raised, Theme::TextDim, 0.5));
    } else {
        const qreal dpr = devicePixelRatioF();
        const QSize pixels = rect.size() * dpr;
        if (m_scaledArtwork.size() != pixels) {
            m_scaledArtwork = QPixmap::fromImage(m_artwork.scaled(pixels, Qt::IgnoreAspectRatio, Qt::SmoothTransformation));
            m_scaledArtwork.setDevicePixelRatio(dpr);
        }
        p.drawPixmap(rect.topLeft(), m_scaledArtwork);
    }
    p.restore();
    // A hairline edge keeps dark covers from melting into the background.
    p.setPen(QPen(QColor(255, 255, 255, 28), 1));
    p.setBrush(Qt::NoBrush);
    p.drawRoundedRect(QRectF(rect).adjusted(0.5, 0.5, -0.5, -0.5), kCornerRadius, kCornerRadius);
}

void AudioView::paintText(QPainter &p, const QRect &rect, Qt::Alignment alignment)
{
    const QFont base = font();
    int y = rect.top();
    auto line = [&](const QString &text, const QFont &lineFont, const QColor &color, int gap) {
        if (text.isEmpty())
            return;
        y += gap;
        const QFontMetrics metrics(lineFont);
        p.setFont(lineFont);
        p.setPen(color);
        p.drawText(QRect(rect.left(), y, rect.width(), metrics.height()), alignment | Qt::AlignVCenter,
                   metrics.elidedText(text, Qt::ElideRight, rect.width()));
        y += metrics.height();
    };
    // Over a visualization the caption sits on a dark scrim, whatever the theme.
    const bool scrim = m_mode == Mode::Visualizer;
    line(m_info.title, scaledFont(base, 1.5, true), scrim ? QColor(Qt::white) : Theme::TextPrimary, 0);
    line(m_info.artist, scaledFont(base, 1.1, false), Theme::Accent, 4);
    line(m_info.album, base, scrim ? QColor(0xC8, 0xCC, 0xD8) : Theme::TextSecondary, 2);
}

void AudioView::paintCaption(QPainter &p)
{
    // A scrim at the bottom keeps the text readable over the visualization.
    const int textHeight = textBlockHeight();
    const int scrimHeight = textHeight + 2 * kMargin;
    QLinearGradient scrim(0, height() - scrimHeight, 0, height());
    scrim.setColorAt(0, QColor(0, 0, 0, 0));
    scrim.setColorAt(1, QColor(0, 0, 0, 170));
    p.fillRect(QRect(0, height() - scrimHeight, width(), scrimHeight), scrim);
    paintText(p, QRect(kMargin, height() - kMargin - textHeight, width() - 2 * kMargin, textHeight), Qt::AlignLeft);
}
