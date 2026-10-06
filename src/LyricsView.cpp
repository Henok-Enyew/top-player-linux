#include "LyricsView.h"
#include "PlaylistSession.h"
#include "Theme.h"

#include <QApplication>
#include <QEasingCurve>
#include <QEvent>
#include <QLinearGradient>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QRadialGradient>
#include <QSettings>
#include <QWheelEvent>

#include <algorithm>
#include <cmath>

namespace {

constexpr int kScrollMs = 420;
constexpr int kWheelScrollMs = 160;
constexpr int kEmphasisMs = 260;
constexpr int kHeadingHeight = 64;
constexpr qreal kLineScale = 1.45;
constexpr qreal kActiveScale = 1.85;
constexpr int kMaxTextWidth = 900;
// Below this height (or width) the title strip makes room for the lyrics.
constexpr int kCompactHeight = 280;
constexpr int kCompactWidth = 300;
// With room for this many lines or more, every line that fits is drawn.
constexpr int kAllLinesFrom = 7;

const QColor kUpcomingColor(0xC8, 0xCC, 0xD8);
const QColor kPastColor(0x8A, 0x8F, 0x9E);

QColor withAlpha(QColor color, qreal alpha)
{
    color.setAlphaF(std::clamp<float>(static_cast<float>(alpha), 0.f, 1.f));
    return color;
}

QString settingsFile()
{
    return PlaylistSession::configDir() + QStringLiteral("/settings.ini");
}

QString shortTime(double seconds)
{
    const long long s = std::llround(std::max(0.0, seconds));
    if (s >= 3600)
        return QStringLiteral("%1:%2:%3").arg(s / 3600).arg((s / 60) % 60, 2, 10, QLatin1Char('0')).arg(s % 60, 2, 10, QLatin1Char('0'));
    return QStringLiteral("%1:%2").arg(s / 60).arg(s % 60, 2, 10, QLatin1Char('0'));
}

} // namespace

LyricsStyle LyricsStyle::load()
{
    const QSettings settings(settingsFile(), QSettings::IniFormat);
    LyricsStyle style;
    style.family = settings.value(QStringLiteral("lyricsStyle/family")).toString();
    style.scale = std::clamp(settings.value(QStringLiteral("lyricsStyle/scale"), style.scale).toDouble(), kMinScale, kMaxScale);
    style.spacing = std::clamp(settings.value(QStringLiteral("lyricsStyle/spacing"), style.spacing).toDouble(), 0.1, 2.0);
    const QString align = settings.value(QStringLiteral("lyricsStyle/align")).toString();
    style.align = align == QLatin1String("left") ? Qt::AlignLeft : align == QLatin1String("right") ? Qt::AlignRight : Qt::AlignHCenter;
    const QColor highlight(settings.value(QStringLiteral("lyricsStyle/highlight")).toString());
    if (highlight.isValid())
        style.highlight = highlight;
    style.glow = settings.value(QStringLiteral("lyricsStyle/glow"), style.glow).toBool();
    style.bold = settings.value(QStringLiteral("lyricsStyle/bold"), style.bold).toBool();
    style.dim = std::clamp(settings.value(QStringLiteral("lyricsStyle/dim"), style.dim).toInt(), 0, 100);
    return style;
}

void LyricsStyle::save() const
{
    QSettings settings(settingsFile(), QSettings::IniFormat);
    settings.setValue(QStringLiteral("lyricsStyle/family"), family);
    settings.setValue(QStringLiteral("lyricsStyle/scale"), scale);
    settings.setValue(QStringLiteral("lyricsStyle/spacing"), spacing);
    settings.setValue(QStringLiteral("lyricsStyle/align"), align == Qt::AlignLeft ? QStringLiteral("left")
                                                          : align == Qt::AlignRight ? QStringLiteral("right")
                                                                                     : QStringLiteral("center"));
    settings.setValue(QStringLiteral("lyricsStyle/highlight"), highlight.name(QColor::HexRgb));
    settings.setValue(QStringLiteral("lyricsStyle/glow"), glow);
    settings.setValue(QStringLiteral("lyricsStyle/bold"), bold);
    settings.setValue(QStringLiteral("lyricsStyle/dim"), dim);
}

bool LyricsStyle::operator==(const LyricsStyle &other) const
{
    return family == other.family && std::abs(scale - other.scale) < 1e-6 && std::abs(spacing - other.spacing) < 1e-6
           && align == other.align && highlight == other.highlight && glow == other.glow && bold == other.bold
           && dim == other.dim;
}

LyricsView::LyricsView(QWidget *parent)
    : QWidget(parent)
    , m_style(LyricsStyle::load())
{
    setObjectName(QStringLiteral("LyricsView"));
    // Lines take clicks and drags; other presses fall through to the player
    // (click to pause, double click for fullscreen, dragging the window).
    setMouseTracking(true);
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

    m_browseTimer.setSingleShot(true);
    m_browseTimer.setInterval(kBrowseHoldMs);
    connect(&m_browseTimer, &QTimer::timeout, this, [this] {
        // Still holding the lyrics: wait for the release.
        if (m_press)
            m_browseTimer.start();
        else
            followPlayback();
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
    m_browsing = false;
    m_browseTimer.stop();
    m_hovered = -1;
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

void LyricsView::setLyricsStyle(const LyricsStyle &style)
{
    m_style = style;
    m_style.scale = std::clamp(m_style.scale, LyricsStyle::kMinScale, LyricsStyle::kMaxScale);
    relayout();
    if (!m_browsing)
        scrollTo(targetScroll(), false);
    else
        m_scroll = clampScroll(m_scroll);
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
            // While browsing, the sung line lights up wherever it is.
            if (!m_browsing)
                scrollTo(targetScroll(), isVisible());
            update();
        }
    } else if (!m_doc.isEmpty() && !m_browsing) {
        const double target = targetScroll();
        // Plain lyrics drift with the playback; jumps (seeks) animate.
        scrollTo(target, std::abs(target - m_scroll) > height() / 4.0 && isVisible());
    }
}

bool LyricsView::isCompact() const
{
    return height() < kCompactHeight || width() < kCompactWidth;
}

int LyricsView::headingHeight() const
{
    return isCompact() ? 0 : kHeadingHeight;
}

int LyricsView::textWidth() const
{
    const double share = isCompact() ? 0.92 : 0.84;
    const int max = static_cast<int>(kMaxTextWidth * std::max(1.0, m_style.scale));
    return std::max(50, std::min(max, static_cast<int>(width() * share)));
}

int LyricsView::textLeft() const
{
    return (width() - textWidth()) / 2;
}

QRect LyricsView::columnRect() const
{
    return QRect(textLeft() - 24, 0, textWidth() + 48, height() - headingHeight());
}

QFont LyricsView::lineFont(bool active) const
{
    QFont f = font();
    if (!m_style.family.isEmpty())
        f.setFamily(m_style.family);
    // Small windows (the mini player) shrink the text so lines still fit.
    const double fit = std::clamp(std::min(height() / 440.0, width() / 640.0), 0.62, 1.0);
    const double base = font().pointSizeF() > 0 ? font().pointSizeF() : 9.0;
    f.setPointSizeF(std::max(6.0, base * (active ? kActiveScale : kLineScale) * m_style.scale * fit));
    f.setBold(active && m_style.bold);
    return f;
}

void LyricsView::relayout()
{
    m_layout.clear();
    const int width = textWidth();
    const QFontMetrics metrics(lineFont(true));
    m_gap = static_cast<int>(std::lround(metrics.height() * m_style.spacing));
    int y = 0;
    for (const Lyrics::Line &line : std::as_const(m_doc.lines)) {
        const int h = line.text.trimmed().isEmpty()
                          ? metrics.height() / 2
                          : metrics.boundingRect(QRect(0, 0, width, 100000),
                                                 Qt::AlignHCenter | Qt::TextWordWrap, line.text).height();
        m_layout.append({y, h});
        y += h + m_gap;
    }
    m_contentHeight = std::max(0, y - m_gap);
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

double LyricsView::clampScroll(double y) const
{
    return std::clamp(y, 0.0, double(std::max(0, m_contentHeight)));
}

void LyricsView::scrollTo(double y, bool animate, int durationMs)
{
    m_scrollAnimation.stop();
    if (!animate) {
        m_scroll = y;
        update();
        return;
    }
    m_scrollAnimation.setDuration(durationMs > 0 ? durationMs : kScrollMs);
    m_scrollAnimation.setStartValue(m_scroll);
    m_scrollAnimation.setEndValue(y);
    m_scrollAnimation.start();
}

int LyricsView::visibleRadius() const
{
    if (m_browsing || !m_doc.isSynced() || m_layout.isEmpty())
        return -1;
    const int available = height() - headingHeight();
    const int perLine = static_cast<int>(QFontMetrics(lineFont(true)).height() * 1.15) + m_gap;
    const int fit = perLine > 0 ? available / perLine : kAllLinesFrom;
    if (fit >= kAllLinesFrom)
        return -1;
    return std::max(0, (fit - 1) / 2);
}

QRect LyricsView::lineRect(int index) const
{
    if (index < 0 || index >= m_layout.size())
        return {};
    const int centerY = (height() - headingHeight()) / 2;
    const int top = static_cast<int>(std::lround(centerY - m_scroll + m_layout[index].first));
    return QRect(textLeft(), top, textWidth(), m_layout[index].second);
}

int LyricsView::lineAt(const QPoint &pos) const
{
    if (!columnRect().contains(pos))
        return -1;
    const int radius = visibleRadius();
    const int center = std::max(0, m_active);
    for (int i = 0; i < m_doc.lines.size(); ++i) {
        if (m_doc.lines[i].text.trimmed().isEmpty())
            continue;
        if (radius >= 0 && std::abs(i - center) > radius)
            continue;
        const QRect r = lineRect(i);
        if (pos.y() >= r.top() - m_gap / 2 && pos.y() <= r.bottom() + m_gap / 2 + 1)
            return i;
    }
    return -1;
}

double LyricsView::seekTime(int index) const
{
    if (index < 0 || index >= m_doc.lines.size() || m_doc.lines[index].start < 0)
        return -1;
    // activeLine() applies the offset to the playback position; undo it, and
    // land a hair inside the line so it lights up at once.
    return std::max(0.0, m_doc.lines[index].start - m_doc.offset + 0.01);
}

QRect LyricsView::backButtonRect() const
{
    if (!m_browsing || m_doc.isEmpty())
        return {};
    QFont f = font();
    f.setBold(true);
    const QFontMetrics metrics(f);
    const QString text = m_doc.isSynced() ? tr("Back to current line") : tr("Follow playback");
    const int w = metrics.horizontalAdvance(text) + 48;
    const int h = metrics.height() + 14;
    const int bottom = height() - headingHeight() - (isCompact() ? 8 : 16);
    return QRect((width() - w) / 2, bottom - h, w, h);
}

void LyricsView::startBrowsing()
{
    if (!m_browsing) {
        m_browsing = true;
        update();
    }
    m_browseTimer.start();
}

void LyricsView::browseBy(double pixels)
{
    if (m_doc.isEmpty())
        return;
    startBrowsing();
    const double from = m_scrollAnimation.state() == QAbstractAnimation::Running ? m_scrollAnimation.endValue().toDouble()
                                                                                  : m_scroll;
    scrollTo(clampScroll(from + pixels), isVisible(), kWheelScrollMs);
}

void LyricsView::followPlayback()
{
    m_browseTimer.stop();
    if (!m_browsing)
        return;
    m_browsing = false;
    scrollTo(targetScroll(), isVisible());
    update();
}

void LyricsView::setHovered(int index)
{
    if (index == m_hovered)
        return;
    m_hovered = index;
    update();
}

void LyricsView::updateCursor(const QPoint &pos)
{
    if (m_press && m_press->dragging)
        setCursor(Qt::ClosedHandCursor);
    else if (backButtonRect().contains(pos) || seekTime(lineAt(pos)) >= 0)
        setCursor(Qt::PointingHandCursor);
    else
        unsetCursor();
}

void LyricsView::mousePressEvent(QMouseEvent *event)
{
    const QPoint pos = event->position().toPoint();
    if (event->button() != Qt::LeftButton || m_doc.isEmpty()) {
        event->ignore();
        return;
    }
    if (backButtonRect().contains(pos)) {
        m_press = Press{pos, m_scroll, false, true};
        event->accept();
        return;
    }
    // Only presses on the lyrics themselves (or anywhere in the column while
    // browsing) are ours; the rest is the player's.
    if (!columnRect().contains(pos) || (!m_browsing && lineAt(pos) < 0)) {
        event->ignore();
        return;
    }
    m_scrollAnimation.stop();
    m_press = Press{pos, m_scroll, false, false};
    event->accept();
}

void LyricsView::mouseMoveEvent(QMouseEvent *event)
{
    const QPoint pos = event->position().toPoint();
    if (m_press && !m_press->onBackButton && (event->buttons() & Qt::LeftButton)) {
        const int dy = pos.y() - m_press->pos.y();
        if (!m_press->dragging && std::abs(dy) >= QApplication::startDragDistance()) {
            m_press->dragging = true;
            setHovered(-1);
            startBrowsing();
        }
        if (m_press->dragging) {
            m_scroll = clampScroll(m_press->scroll - dy);
            m_browseTimer.start();
            update();
        }
        updateCursor(pos);
        event->accept();
        return;
    }
    const bool overBack = backButtonRect().contains(pos);
    if (overBack != m_backHovered) {
        m_backHovered = overBack;
        update();
    }
    setHovered(overBack ? -1 : lineAt(pos));
    updateCursor(pos);
    // Looking around keeps the lyrics where they were scrolled to.
    if (m_browsing && columnRect().contains(pos))
        m_browseTimer.start();
    event->ignore();
}

void LyricsView::mouseReleaseEvent(QMouseEvent *event)
{
    if (event->button() != Qt::LeftButton || !m_press) {
        event->ignore();
        return;
    }
    const Press press = *std::exchange(m_press, std::nullopt);
    const QPoint pos = event->position().toPoint();
    if (press.onBackButton) {
        if (backButtonRect().contains(pos))
            followPlayback();
    } else if (press.dragging) {
        m_browseTimer.start();
    } else {
        // A click: lines with a time play from there; the others only scroll.
        const double seconds = seekTime(lineAt(pos));
        if (seconds >= 0) {
            Q_EMIT seekRequested(seconds);
            followPlayback();
        } else if (m_browsing) {
            m_browseTimer.start();
        }
    }
    updateCursor(pos);
    event->accept();
}

void LyricsView::mouseDoubleClickEvent(QMouseEvent *event)
{
    // A double click on a line is two clicks on it, not fullscreen.
    mousePressEvent(event);
}

void LyricsView::wheelEvent(QWheelEvent *event)
{
    const QPoint angle = event->angleDelta();
    if (m_doc.isEmpty() || std::abs(angle.x()) > std::abs(angle.y()) || angle.y() == 0) {
        event->ignore();
        return;
    }
    if (event->modifiers() & Qt::ControlModifier) {
        LyricsStyle style = m_style;
        style.scale = std::round((style.scale + 0.1 * angle.y() / 120.0) * 20) / 20;
        style.scale = std::clamp(style.scale, LyricsStyle::kMinScale, LyricsStyle::kMaxScale);
        if (style != m_style) {
            setLyricsStyle(style);
            style.save();
            Q_EMIT lyricsStyleChanged(m_style);
        }
        Q_EMIT message(tr("Lyrics Size"), QStringLiteral("%1%").arg(qRound(m_style.scale * 100)));
        event->accept();
        return;
    }
    // Over the lyrics the wheel scrolls them; beside them it stays the volume.
    if (!columnRect().contains(event->position().toPoint())) {
        event->ignore();
        return;
    }
    const QPoint pixels = event->pixelDelta();
    const double step = QFontMetrics(lineFont(false)).height() + m_gap;
    browseBy(!pixels.isNull() ? -pixels.y() : -step * angle.y() / 120.0);
    event->accept();
}

void LyricsView::leaveEvent(QEvent *event)
{
    QWidget::leaveEvent(event);
    m_backHovered = false;
    setHovered(-1);
}

void LyricsView::hideEvent(QHideEvent *event)
{
    QWidget::hideEvent(event);
    m_press.reset();
    m_hovered = -1;
    m_backHovered = false;
    if (m_browsing) {
        m_browsing = false;
        m_browseTimer.stop();
        scrollTo(targetScroll(), false);
    }
}

void LyricsView::resizeEvent(QResizeEvent *event)
{
    QWidget::resizeEvent(event);
    relayout();
    if (m_browsing)
        m_scroll = clampScroll(m_scroll);
    else
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
        p.fillRect(rect(), QColor(0, 0, 0, std::clamp(m_style.dim * 255 / 100, 0, 255)));
    }

    const int width = textWidth();
    const int left = textLeft();
    const int heading = headingHeight();
    // The heading takes the bottom strip; the OSD uses the top-left corner.
    const double centerY = (height() - heading) / 2.0;
    const int flags = int(m_style.align | Qt::AlignVCenter) | Qt::TextWordWrap;

    if (m_doc.isEmpty()) {
        QFont f = font();
        f.setPointSizeF(font().pointSizeF() * (isCompact() ? 1.0 : 1.2));
        p.setFont(f);
        p.setPen(Theme::TextSecondary);
        p.drawText(QRect(left, 0, width, height() - heading), Qt::AlignCenter | Qt::TextWordWrap, m_placeholder);
    } else {
        const bool synced = m_doc.isSynced();
        const QFont normal = lineFont(false);
        const QFont active = lineFont(true);
        const double halfHeight = std::max(1.0, (height() - heading) / 2.0);
        const int radius = visibleRadius();
        const int center = std::max(0, m_active);
        for (int i = 0; i < m_doc.lines.size(); ++i) {
            const Lyrics::Line &line = m_doc.lines[i];
            if (line.text.trimmed().isEmpty())
                continue;
            // A small window shows only the lines around the sung one.
            if (radius >= 0 && std::abs(i - center) > radius)
                continue;
            const QRect r = lineRect(i);
            if (r.bottom() < 0 || r.top() > height() - heading + 30)
                continue;
            // Lines fade out toward the top and bottom edges.
            const double fromCenter = std::abs(r.center().y() - centerY) / halfHeight;
            const double edge = radius >= 0 ? 1.0 : std::clamp(1.0 - std::pow(fromCenter, 2.2) * 0.85, 0.0, 1.0);

            const bool isActive = synced && i == m_active;
            const bool isHovered = i == m_hovered;
            QColor color;
            double alpha;
            if (isActive) {
                color = m_style.highlight;
                alpha = 1.0;
            } else if (synced) {
                const int distance = m_active < 0 ? i + 1 : std::abs(i - m_active);
                color = i < m_active ? kPastColor : kUpcomingColor;
                alpha = std::max(0.18, 0.62 - 0.11 * (distance - 1));
            } else {
                color = kUpcomingColor;
                alpha = 0.85;
            }
            // Browsing lifts the lines a little, so they are easy to pick.
            if (m_browsing && !isActive)
                alpha = std::max(alpha, 0.5);

            QFont f = isActive ? active : normal;
            if (isActive) {
                // Grow into the highlighted size.
                f.setPointSizeF(normal.pointSizeF() + (active.pointSizeF() - normal.pointSizeF()) * m_emphasis);
            }
            p.setFont(f);

            const double seconds = seekTime(i);
            if (isHovered && seconds >= 0) {
                // A line that can be played from: a soft plate and its time.
                const QRect text = p.fontMetrics().boundingRect(r, flags, line.text);
                const QRect plate = text.adjusted(-14, -5, 14, 5);
                p.setPen(Qt::NoPen);
                p.setBrush(withAlpha(Theme::Accent, 0.12));
                p.drawRoundedRect(plate, 10, 10);
                QFont timeFont = font();
                timeFont.setBold(true);
                const QString label = QStringLiteral("▶ ") + shortTime(seconds);
                const QFontMetrics tm(timeFont);
                const QSize pill(tm.horizontalAdvance(label) + 14, tm.height() + 6);
                const bool rightSide = m_style.align == Qt::AlignLeft;
                int x = rightSide ? plate.right() + 8 : plate.left() - 8 - pill.width();
                x = std::clamp(x, 4, std::max(4, this->width() - pill.width() - 4));
                const QRect pillRect(QPoint(x, plate.center().y() - pill.height() / 2), pill);
                p.setBrush(withAlpha(Theme::Accent, 0.9));
                p.drawRoundedRect(pillRect, pill.height() / 2.0, pill.height() / 2.0);
                p.setFont(timeFont);
                p.setPen(Theme::Surface);
                p.drawText(pillRect, Qt::AlignCenter, label);
                p.setFont(f);
                alpha = std::max(alpha, 0.95);
            }

            if (isActive && m_style.glow) {
                // A soft accent glow behind the sung line.
                p.setPen(withAlpha(Theme::Accent, 0.22 * m_emphasis));
                for (const QPoint &offset : {QPoint(0, 2), QPoint(1, 1), QPoint(-1, 1)})
                    p.drawText(r.translated(offset), flags, line.text);
            }
            p.setPen(withAlpha(color, alpha * edge));
            p.drawText(r, flags, line.text);
        }
    }

    // "Back to current line" while browsing, pointing where the sung line is.
    if (const QRect back = backButtonRect(); !back.isNull()) {
        p.setPen(QPen(withAlpha(Theme::Accent, m_backHovered ? 1.0 : 0.7), 1));
        p.setBrush(withAlpha(m_backHovered ? Theme::Raised.lighter(130) : Theme::Raised, 0.94));
        p.drawRoundedRect(QRectF(back).adjusted(0.5, 0.5, -0.5, -0.5), back.height() / 2.0, back.height() / 2.0);
        QFont f = font();
        f.setBold(true);
        p.setFont(f);
        p.setPen(m_backHovered ? Theme::AccentHover : Theme::TextPrimary);
        bool below = true;
        if (m_doc.isSynced() && !m_layout.isEmpty())
            below = lineRect(std::max(0, m_active)).center().y() >= centerY;
        else
            below = targetScroll() >= m_scroll;
        const QString arrow = below ? QStringLiteral("↓  ") : QStringLiteral("↑  ");
        p.drawText(back, Qt::AlignCenter,
                   arrow + (m_doc.isSynced() ? tr("Back to current line") : tr("Follow playback")));
    }

    if (heading == 0)
        return;
    // Heading along the bottom: a small cover, the title and the artist, over a fade.
    const int stripTop = height() - heading;
    QLinearGradient fade(0, stripTop - 30, 0, height());
    fade.setColorAt(0, QColor(0, 0, 0, 0));
    fade.setColorAt(1, m_opaque ? withAlpha(Theme::Surface, 0.95) : QColor(0, 0, 0, 150));
    p.fillRect(QRect(0, stripTop - 30, this->width(), heading + 30), fade);
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
    const int stripTextWidth = this->width() - textLeft - 16;
    p.drawText(QRect(textLeft, stripTop + 12, stripTextWidth, 22), Qt::AlignLeft | Qt::AlignVCenter,
               QFontMetrics(titleFont).elidedText(m_title, Qt::ElideRight, stripTextWidth));
    p.setFont(font());
    p.setPen(Theme::Accent);
    p.drawText(QRect(textLeft, stripTop + 32, stripTextWidth, 20), Qt::AlignLeft | Qt::AlignVCenter,
               QFontMetrics(font()).elidedText(m_artist, Qt::ElideRight, stripTextWidth));
}
