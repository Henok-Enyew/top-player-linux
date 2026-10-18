#include "SeekBar.h"

#include <QLinearGradient>
#include <QMouseEvent>
#include <QPainter>

#include <algorithm>
#include <cmath>

namespace {

constexpr qreal kSidePadding = 6;
constexpr qreal kGrooveHeight = 4;
constexpr qreal kGrooveHoverHeight = 6;
constexpr qreal kHandleRadius = 6;

} // namespace

SeekBar::SeekBar(QWidget *parent)
    : QWidget(parent)
{
    setMouseTracking(true);
    setCursor(Qt::PointingHandCursor);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
}

void SeekBar::setDuration(double seconds)
{
    m_duration = std::max(0.0, seconds);
    setEnabled(m_duration > 0);
    update();
}

void SeekBar::setPosition(double seconds)
{
    const qreal oldX = xFor(m_position);
    m_position = seconds;
    // On a long file most position updates move the bar by less than a
    // device pixel; skip repainting for those.
    if (!m_dragging && std::abs(xFor(seconds) - oldX) * devicePixelRatioF() >= 0.5)
        update();
}

void SeekBar::setChapters(const QList<double> &times)
{
    m_chapters = times;
    update();
}

void SeekBar::setClipRange(double in, double out)
{
    m_clipIn = in;
    m_clipOut = out;
    update();
}

QSize SeekBar::sizeHint() const
{
    return {200, 18};
}

QRectF SeekBar::grooveRect() const
{
    const qreal h = (m_hovering || m_dragging) ? kGrooveHoverHeight : kGrooveHeight;
    return {kSidePadding, (height() - h) / 2, width() - 2 * kSidePadding, h};
}

double SeekBar::timeAt(qreal x) const
{
    const QRectF groove = grooveRect();
    if (groove.width() <= 0)
        return 0;
    return std::clamp((x - groove.left()) / groove.width(), 0.0, 1.0) * m_duration;
}

qreal SeekBar::xFor(double seconds) const
{
    const QRectF groove = grooveRect();
    const double fraction = m_duration > 0 ? std::clamp(seconds / m_duration, 0.0, 1.0) : 0.0;
    return groove.left() + groove.width() * fraction;
}

void SeekBar::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    p.setPen(Qt::NoPen);

    const QRectF groove = grooveRect();
    p.setBrush(m_grooveColor);
    p.drawRoundedRect(groove, groove.height() / 2, groove.height() / 2);

    if (m_duration <= 0)
        return;

    const double shown = m_dragging ? m_dragPosition : m_position;
    const qreal progressX = xFor(shown);

    if (m_hovering && !m_dragging && m_hoverX > progressX) {
        p.setBrush(m_hoverColor);
        p.drawRect(QRectF(QPointF(progressX, groove.top()), QPointF(std::min(m_hoverX, groove.right()), groove.bottom())));
    }

    const QRectF progress(groove.topLeft(), QPointF(progressX, groove.bottom()));
    if (m_glowColor.alpha() > 0 && progress.width() > 0) {
        // Two widening, fading layers make a cheap halo.
        for (const qreal spread : {3.0, 1.5}) {
            QColor halo = m_glowColor;
            halo.setAlphaF(halo.alphaF() * (spread > 2 ? 0.35f : 0.6f));
            p.setBrush(halo);
            const QRectF band = progress.adjusted(-spread, -spread, spread, spread);
            p.drawRoundedRect(band, band.height() / 2, band.height() / 2);
        }
    }
    // The gradient spans the whole groove, so its colors stay put as the progress grows.
    QLinearGradient fill(groove.topLeft(), groove.topRight());
    fill.setColorAt(0, m_progressColor);
    fill.setColorAt(1, m_progressColor2.isValid() ? m_progressColor2 : m_progressColor);
    p.setBrush(fill);
    p.drawRoundedRect(progress, groove.height() / 2, groove.height() / 2);

    // Chapter boundaries are cut into the groove as thin gaps.
    p.setBrush(m_chapterColor);
    for (double chapter : std::as_const(m_chapters)) {
        if (chapter > 0 && chapter < m_duration)
            p.drawRect(QRectF(xFor(chapter) - 1, groove.top(), 2, groove.height()));
    }

    // The cutter's range: a band over the groove between brackets at A and B.
    const bool hasIn = m_clipIn >= 0;
    const bool hasOut = m_clipOut >= 0;
    if (hasIn || hasOut) {
        const qreal inX = hasIn ? xFor(m_clipIn) : groove.left();
        const qreal outX = hasOut ? xFor(m_clipOut) : groove.right();
        QColor band = m_clipColor;
        band.setAlpha(170);
        p.setBrush(band);
        if (outX > inX)
            p.drawRect(QRectF(QPointF(inX, groove.top()), QPointF(outX, groove.bottom())));
        const qreal top = std::max(0.0, groove.center().y() - 7);
        const qreal bottom = std::min<qreal>(height(), groove.center().y() + 7);
        p.setBrush(m_clipColor);
        auto bracket = [&](qreal x, qreal direction) {
            // A 2 px post with feet pointing into the range: "[" or "]".
            p.drawRect(QRectF(x - 1, top, 2, bottom - top));
            const qreal footX = direction > 0 ? x : x - 4;
            p.drawRect(QRectF(footX, top, 4, 2));
            p.drawRect(QRectF(footX, bottom - 2, 4, 2));
        };
        if (hasIn)
            bracket(inX, 1);
        if (hasOut)
            bracket(outX, -1);
    }

    if (m_hovering || m_dragging) {
        p.setBrush(m_handleColor);
        p.drawEllipse(QPointF(progressX, groove.center().y()), kHandleRadius, kHandleRadius);
    }
}

void SeekBar::mousePressEvent(QMouseEvent *event)
{
    if (event->button() != Qt::LeftButton || m_duration <= 0) {
        QWidget::mousePressEvent(event);
        return;
    }
    m_dragging = true;
    m_dragPosition = timeAt(event->position().x());
    Q_EMIT seekRequested(m_dragPosition, false);
    update();
}

void SeekBar::mouseMoveEvent(QMouseEvent *event)
{
    m_hovering = true;
    m_hoverX = event->position().x();
    const double time = timeAt(m_hoverX);
    if (m_dragging && time != m_dragPosition) {
        m_dragPosition = time;
        Q_EMIT seekRequested(time, false);
    }
    if (m_duration > 0)
        Q_EMIT hovered(time, qRound(std::clamp(m_hoverX, grooveRect().left(), grooveRect().right())));
    update();
}

void SeekBar::mouseReleaseEvent(QMouseEvent *event)
{
    if (event->button() != Qt::LeftButton || !m_dragging) {
        QWidget::mouseReleaseEvent(event);
        return;
    }
    m_dragging = false;
    m_dragPosition = timeAt(event->position().x());
    m_position = m_dragPosition;
    Q_EMIT seekRequested(m_dragPosition, true);
    update();
}

void SeekBar::leaveEvent(QEvent *)
{
    m_hovering = false;
    Q_EMIT hoverEnded();
    update();
}
