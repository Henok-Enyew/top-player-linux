#include "OsdWidget.h"
#include "Theme.h"
#include "TimeFormat.h"

#include <QEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPropertyAnimation>

#include <algorithm>
#include <cmath>

namespace {

const QColor kTextColor(0xFF, 0xFF, 0xFF);
const QColor kOutlineColor(0, 0, 0, 200);

constexpr int kHoldMs = 1200;
constexpr int kFadeMs = 350;

} // namespace

OsdWidget::OsdWidget(QWidget *parent)
    : QWidget(parent)
    , m_fade(new QPropertyAnimation(this, "opacity", this))
{
    setAttribute(Qt::WA_TransparentForMouseEvents);
    setAttribute(Qt::WA_NoSystemBackground);
    setFocusPolicy(Qt::NoFocus);
    hide();

    parent->installEventFilter(this);
    setGeometry(parent->rect());

    m_holdTimer.setSingleShot(true);
    m_holdTimer.setInterval(kHoldMs);
    connect(&m_holdTimer, &QTimer::timeout, m_fade, [this] { m_fade->start(); });

    m_fade->setDuration(kFadeMs);
    m_fade->setStartValue(1.0);
    m_fade->setEndValue(0.0);
    connect(m_fade, &QPropertyAnimation::finished, this, &QWidget::hide);
}

void OsdWidget::showValue(const QString &label, const QString &value, qreal progress)
{
    QList<Segment> segments{{label, false}};
    if (!value.isEmpty())
        segments.append({QStringLiteral(" ") + value, true});
    showSegments(segments, progress);
}

QString OsdWidget::text() const
{
    QString result;
    for (const Segment &segment : m_segments)
        result += segment.text;
    return result;
}

void OsdWidget::showTime(double position, double duration)
{
    QList<Segment> segments{{formatTime(position), true}};
    qreal progress = -1;
    if (duration > 0) {
        segments.append({QStringLiteral(" / ") + formatTime(duration), false});
        progress = std::clamp(position / duration, 0.0, 1.0);
    }
    showSegments(segments, progress);
}

void OsdWidget::showSeek(double offset, double target, double duration)
{
    const QString jump = (offset < 0 ? QStringLiteral("-") : QStringLiteral("+")) + formatTime(std::abs(offset));
    QList<Segment> segments{{QStringLiteral("[%1]  ").arg(jump), false}, {formatTime(target), true}};
    qreal progress = -1;
    if (duration > 0) {
        segments.append({QStringLiteral(" / ") + formatTime(duration), false});
        progress = std::clamp(target / duration, 0.0, 1.0);
    }
    showSegments(segments, progress);
}

void OsdWidget::setOpacity(qreal opacity)
{
    m_opacity = opacity;
    update();
}

void OsdWidget::showSegments(const QList<Segment> &segments, qreal progress)
{
    m_segments = segments;
    m_progress = progress;
    m_fade->stop();
    m_opacity = 1.0;
    m_holdTimer.start();
    show();
    raise();
    update();
}

bool OsdWidget::eventFilter(QObject *watched, QEvent *event)
{
    if (watched == parent() && event->type() == QEvent::Resize)
        setGeometry(parentWidget()->rect());
    return QWidget::eventFilter(watched, event);
}

void OsdWidget::paintEvent(QPaintEvent *)
{
    if (m_segments.isEmpty() || m_opacity <= 0)
        return;

    QFont font = this->font();
    font.setBold(true);
    font.setPixelSize(std::clamp(height() / 20, 16, 42));
    const QFontMetricsF metrics(font);
    const qreal fontSize = font.pixelSize();
    const qreal margin = fontSize * 0.75;
    const qreal baseline = margin + metrics.ascent();

    // Lay out each segment as a path so it can be outlined like PotPlayer's OSD.
    QPainterPath plainPath;
    QPainterPath accentPath;
    qreal x = margin;
    for (const Segment &segment : std::as_const(m_segments)) {
        (segment.accent ? accentPath : plainPath).addText(x, baseline, font, segment.text);
        x += metrics.horizontalAdvance(segment.text);
    }
    const qreal textWidth = x - margin;

    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setOpacity(m_opacity);

    const QPen outline(kOutlineColor, std::max(2.0, fontSize / 7), Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
    painter.strokePath(plainPath, outline);
    painter.strokePath(accentPath, outline);
    painter.fillPath(plainPath, kTextColor);
    painter.fillPath(accentPath, Theme::Accent);

    if (m_progress >= 0) {
        const qreal barHeight = std::max(4.0, fontSize / 5);
        const QRectF track(margin, baseline + metrics.descent() + fontSize * 0.3,
                           std::max(textWidth, fontSize * 10), barHeight);
        painter.setPen(QPen(kOutlineColor, 1.5));
        painter.setBrush(QColor(255, 255, 255, 70));
        painter.drawRect(track);
        painter.setPen(Qt::NoPen);
        painter.setBrush(Theme::Accent);
        painter.drawRect(QRectF(track.topLeft(), QSizeF(track.width() * m_progress, track.height())));
    }
}
