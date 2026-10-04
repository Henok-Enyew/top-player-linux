#include "LyricsView.h"
#include "Theme.h"

#include <QEasingCurve>
#include <QEvent>
#include <QLinearGradient>
#include <QPainter>
#include <QPainterPath>
#include <QRadialGradient>

#include <algorithm>
#include <cmath>

namespace {

constexpr int kScrollMs = 420;
constexpr int kEmphasisMs = 260;
constexpr int kHeadingHeight = 64;
constexpr qreal kLineScale = 1.45;
constexpr qreal kActiveScale = 1.85;
constexpr int kMaxTextWidth = 900;

const QColor kActiveColor = Theme::TextPrimary;
const QColor kUpcomingColor(0xC8, 0xCC, 0xD8);
const QColor kPastColor(0x8A, 0x8F, 0x9E);

QColor withAlpha(QColor color, qreal alpha)
{
    color.setAlphaF(std::clamp<float>(static_cast<float>(alpha), 0.f, 1.f));
    return color;
}

} // namespace

LyricsView::LyricsView(QWidget *parent)
    : QWidget(parent)
{
    setObjectName(QStringLiteral("LyricsView"));
    // Clicks, double-clicks and drops go to the player underneath.
    setAttribute(Qt::WA_TransparentForMouseEvents);
    setFocusPolicy(Qt::NoFocus);
    hide();
    parent->installEventFilter(this);
    setGeometry(parent->rect());

    m_scrollAnimation.setDuration(kScrollMs);
    m_scrollAnimation.setEasingCurve(QEasingCurve::OutCubic);
    connect(&m_scrollAnimation, &QVariantAnimation::valueChanged, this, [this](const QVariant &value) {
        m_scroll = value.toDouble();
        update();
    });
    m_emphasisAnimation.setDuration(kEmphasisMs);
    m_emphasisAnimation.setStartValue(0.0);
    m_emphasisAnimation.setEndValue(1.0);
    m_emphasisAnimation.setEasingCurve(QEasingCurve::OutCubic);
    connect(&m_emphasisAnimation, &QVariantAnimation::valueChanged, this, [this](const QVariant &value) {
        m_emphasis = value.toDouble();
        update();
    });
}

bool LyricsView::eventFilter(QObject *watched, QEvent *event)
{
    if (watched == parent() && event->type() == QEvent::Resize)
        setGeometry(parentWidget()->rect());
    return QWidget::eventFilter(watched, event);
}

void LyricsView::setDocument(const Lyrics::Document &doc)
{
    m_doc = doc;
    m_active = -1;
    relayout();
    scrollTo(targetScroll(), false);
    setPosition(m_position, m_duration);
    update();
}

void LyricsView::setPlaceholder(const QString &text)
{
    m_placeholder = text;
    update();
}

void LyricsView::setOpaque(bool opaque)
{
    m_opaque = opaque;
    setAttribute(Qt::WA_NoSystemBackground, !opaque);
    update();
}

void LyricsView::setArtwork(const QImage &image)
{
    m_artwork = image;
    m_cover = {};
    m_tint = {};
    if (!image.isNull()) {
        const QColor average = image.scaled(1, 1, Qt::IgnoreAspectRatio, Qt::SmoothTransformation).pixelColor(0, 0);
        m_tint = QColor::fromHsvF(std::max<float>(average.hsvHueF(), 0), std::min<float>(average.hsvSaturationF(), 0.65f),
                                  std::min<float>(average.valueF(), 0.38f));
    }
    update();
}

void LyricsView::setHeading(const QString &title, const QString &artist)
{
    m_title = title;
    m_artist = artist;
    update();
}

void LyricsView::setPosition(double seconds, double duration)
{
    m_position = seconds;
    m_duration = duration;
    if (m_doc.isSynced()) {
        const int active = Lyrics::activeLine(m_doc, seconds);
        if (active != m_active) {
            m_active = active;
            m_emphasisAnimation.stop();
            m_emphasis = 0;
            m_emphasisAnimation.start();
            scrollTo(targetScroll(), isVisible());
        }
    } else if (!m_doc.isEmpty()) {
        const double target = targetScroll();
        // Plain lyrics drift with the playback; jumps (seeks) animate.
        scrollTo(target, std::abs(target - m_scroll) > height() / 4.0 && isVisible());
    }
}

QFont LyricsView::lineFont(bool active) const
{
    QFont f = font();
    f.setPointSizeF(font().pointSizeF() * (active ? kActiveScale : kLineScale));
    f.setBold(active);
    return f;
}

void LyricsView::relayout()
{
    m_layout.clear();
    const int width = std::min(kMaxTextWidth, static_cast<int>(this->width() * 0.84));
    const QFontMetrics metrics(lineFont(true));
    const int gap = metrics.height() / 2;
    int y = 0;
    for (const Lyrics::Line &line : std::as_const(m_doc.lines)) {
        const int h = line.text.trimmed().isEmpty()
                          ? metrics.height() / 2
                          : metrics.boundingRect(QRect(0, 0, std::max(width, 50), 100000),
                                                 Qt::AlignHCenter | Qt::TextWordWrap, line.text).height();
        m_layout.append({y, h});
        y += h + gap;
    }
    m_contentHeight = std::max(0, y - gap);
}

double LyricsView::targetScroll() const
{
    if (m_layout.isEmpty())
        return 0;
    if (m_doc.isSynced()) {
        // Before the first line, the first one waits in the middle.
        const int index = std::max(0, m_active);
        return m_layout[index].first + m_layout[index].second / 2.0;
    }
    const double progress = m_duration > 0 ? std::clamp(m_position / m_duration, 0.0, 1.0) : 0;
    return progress * m_contentHeight;
}

void LyricsView::scrollTo(double y, bool animate)
{
    m_scrollAnimation.stop();
    if (!animate) {
        m_scroll = y;
        update();
        return;
    }
    m_scrollAnimation.setStartValue(m_scroll);
    m_scrollAnimation.setEndValue(y);
    m_scrollAnimation.start();
}

QRect LyricsView::lineRect(int index) const
{
    if (index < 0 || index >= m_layout.size())
        return {};
    const int width = std::min(kMaxTextWidth, static_cast<int>(this->width() * 0.84));
    const int centerY = (height() - kHeadingHeight) / 2;
    const int top = static_cast<int>(std::lround(centerY - m_scroll + m_layout[index].first));
    return QRect((this->width() - width) / 2, top, width, m_layout[index].second);
}

void LyricsView::resizeEvent(QResizeEvent *event)
{
    QWidget::resizeEvent(event);
    relayout();
    scrollTo(targetScroll(), false);
}

void LyricsView::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    p.setRenderHint(QPainter::TextAntialiasing);
    p.setRenderHint(QPainter::SmoothPixmapTransform);

    // Background: the cover's colors (blurred) for audio, a scrim over video.
    if (m_opaque) {
        p.fillRect(rect(), Theme::Surface);
        if (!m_artwork.isNull()) {
            // Scaling down to a few pixels and back up blurs it heavily.
            const QImage tiny = m_artwork.scaled(12, 12, Qt::KeepAspectRatioByExpanding, Qt::SmoothTransformation);
            p.setOpacity(0.35);
            p.drawImage(rect(), tiny.scaled(size(), Qt::KeepAspectRatioByExpanding, Qt::SmoothTransformation));
            p.setOpacity(1);
        }
        QRadialGradient glow(rect().center(), std::max(width(), height()) * 0.7);
        glow.setColorAt(0, withAlpha(m_tint.isValid() ? m_tint : QColor(0x1E, 0x24, 0x30), 0.55));
        glow.setColorAt(1, withAlpha(Theme::Surface, 0.92));
        p.fillRect(rect(), glow);
    } else {
        p.fillRect(rect(), QColor(0, 0, 0, 175));
    }

    const int width = std::min(kMaxTextWidth, static_cast<int>(this->width() * 0.84));
    const int left = (this->width() - width) / 2;
    // The heading takes the bottom strip; the OSD uses the top-left corner.
    const double centerY = (height() - kHeadingHeight) / 2.0;

    if (m_doc.isEmpty()) {
        QFont f = font();
        f.setPointSizeF(font().pointSizeF() * 1.2);
        p.setFont(f);
        p.setPen(Theme::TextSecondary);
        p.drawText(QRect(left, 0, width, height() - kHeadingHeight), Qt::AlignCenter | Qt::TextWordWrap, m_placeholder);
    } else {
        const bool synced = m_doc.isSynced();
        const QFont normal = lineFont(false);
        const QFont active = lineFont(true);
        const double halfHeight = std::max(1.0, (height() - kHeadingHeight) / 2.0);
        for (int i = 0; i < m_doc.lines.size(); ++i) {
            const Lyrics::Line &line = m_doc.lines[i];
            if (line.text.trimmed().isEmpty())
                continue;
            const QRect r = lineRect(i);
            if (r.bottom() < 0 || r.top() > height() - kHeadingHeight + 30)
                continue;
            // Lines fade out toward the top and bottom edges.
            const double fromCenter = std::abs(r.center().y() - centerY) / halfHeight;
            const double edge = std::clamp(1.0 - std::pow(fromCenter, 2.2) * 0.85, 0.0, 1.0);

            const bool isActive = synced && i == m_active;
            QColor color;
            double alpha;
            if (isActive) {
                color = kActiveColor;
                alpha = 1.0;
            } else if (synced) {
                const int distance = m_active < 0 ? i + 1 : std::abs(i - m_active);
                color = i < m_active ? kPastColor : kUpcomingColor;
                alpha = std::max(0.18, 0.62 - 0.11 * (distance - 1));
            } else {
                color = kUpcomingColor;
                alpha = 0.85;
            }

            QFont f = isActive ? active : normal;
            if (isActive) {
                // Grow into the highlighted size.
                f.setPointSizeF(normal.pointSizeF() + (active.pointSizeF() - normal.pointSizeF()) * m_emphasis);
                // A soft accent glow behind the sung line.
                p.setFont(f);
                p.setPen(withAlpha(Theme::Accent, 0.22 * m_emphasis));
                for (const QPoint &offset : {QPoint(0, 2), QPoint(1, 1), QPoint(-1, 1)})
                    p.drawText(r.translated(offset), Qt::AlignHCenter | Qt::AlignVCenter | Qt::TextWordWrap, line.text);
            }
            p.setFont(f);
            p.setPen(withAlpha(color, alpha * edge));
            p.drawText(r, Qt::AlignHCenter | Qt::AlignVCenter | Qt::TextWordWrap, line.text);
        }
    }

    // Heading along the bottom: a small cover, the title and the artist, over a fade.
    const int stripTop = height() - kHeadingHeight;
    QLinearGradient fade(0, stripTop - 30, 0, height());
    fade.setColorAt(0, QColor(0, 0, 0, 0));
    fade.setColorAt(1, m_opaque ? withAlpha(Theme::Surface, 0.95) : QColor(0, 0, 0, 150));
    p.fillRect(QRect(0, stripTop - 30, this->width(), kHeadingHeight + 30), fade);
    int textLeft = 18;
    if (!m_artwork.isNull()) {
        const QRect coverRect(16, stripTop + 12, 40, 40);
        if (m_cover.isNull() || m_cover.size() != coverRect.size() * devicePixelRatioF()) {
            m_cover = QPixmap::fromImage(m_artwork.scaled(coverRect.size() * devicePixelRatioF(),
                                                          Qt::KeepAspectRatioByExpanding, Qt::SmoothTransformation));
            m_cover.setDevicePixelRatio(devicePixelRatioF());
        }
        QPainterPath clip;
        clip.addRoundedRect(QRectF(coverRect), 4, 4);
        p.save();
        p.setClipPath(clip);
        p.drawPixmap(coverRect.topLeft(), m_cover);
        p.restore();
        textLeft = coverRect.right() + 12;
    }
    QFont titleFont = font();
    titleFont.setBold(true);
    titleFont.setPointSizeF(font().pointSizeF() * 1.05);
    p.setFont(titleFont);
    p.setPen(Theme::TextPrimary);
    const int textWidth = this->width() - textLeft - 16;
    p.drawText(QRect(textLeft, stripTop + 12, textWidth, 22), Qt::AlignLeft | Qt::AlignVCenter,
               QFontMetrics(titleFont).elidedText(m_title, Qt::ElideRight, textWidth));
    p.setFont(font());
    p.setPen(Theme::Accent);
    p.drawText(QRect(textLeft, stripTop + 32, textWidth, 20), Qt::AlignLeft | Qt::AlignVCenter,
               QFontMetrics(font()).elidedText(m_artist, Qt::ElideRight, textWidth));
}
