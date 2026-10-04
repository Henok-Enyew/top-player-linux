#include "ResumePrompt.h"
#include "Theme.h"

#include <QApplication>
#include <QEvent>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPushButton>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>

namespace {

constexpr int kCardWidth = 380;
constexpr int kBottomGap = 28;

// "4:05", or "1:02:03" past an hour.
QString shortTime(double seconds)
{
    const long long total = std::max(0LL, static_cast<long long>(std::floor(seconds)));
    if (total >= 3600)
        return QStringLiteral("%1:%2:%3").arg(total / 3600).arg((total / 60) % 60, 2, 10, QLatin1Char('0'))
            .arg(total % 60, 2, 10, QLatin1Char('0'));
    return QStringLiteral("%1:%2").arg(total / 60).arg(total % 60, 2, 10, QLatin1Char('0'));
}

} // namespace

ResumePrompt::ResumePrompt(QWidget *parent)
    : QWidget(parent)
    , m_heading(new QLabel(tr("Continue watching?"), this))
    , m_title(new QLabel(this))
    , m_detail(new QLabel(this))
    , m_countdown(new QLabel(this))
    , m_resume(new QPushButton(this))
    , m_startOver(new QPushButton(tr("Start Over"), this))
{
    setObjectName(QStringLiteral("ResumePrompt"));
    setAttribute(Qt::WA_TranslucentBackground);
    setFocusPolicy(Qt::StrongFocus);
    hide();
    parent->installEventFilter(this);

    m_heading->setStyleSheet(QStringLiteral("color: %1; font-weight: bold; font-size: 11pt;").arg(Theme::hex(Theme::TextPrimary)));
    m_title->setStyleSheet(QStringLiteral("color: %1;").arg(Theme::hex(Theme::TextSecondary)));
    m_detail->setStyleSheet(QStringLiteral("color: %1;").arg(Theme::hex(Theme::Accent)));
    m_countdown->setStyleSheet(QStringLiteral("color: %1; font-size: 8pt;").arg(Theme::hex(Theme::TextDim)));
    m_resume->setObjectName(QStringLiteral("ResumeButton"));
    m_startOver->setObjectName(QStringLiteral("StartOverButton"));
    m_resume->setDefault(true);
    m_resume->setCursor(Qt::PointingHandCursor);
    m_startOver->setCursor(Qt::PointingHandCursor);
    m_resume->setStyleSheet(QStringLiteral(
        "QPushButton { background: %1; color: #0B0C0F; border: none; border-radius: 6px; padding: 7px 14px; font-weight: bold; }"
        "QPushButton:hover { background: %2; }").arg(Theme::hex(Theme::Accent), Theme::hex(Theme::AccentHover)));
    m_startOver->setStyleSheet(QStringLiteral(
        "QPushButton { background: %1; color: %2; border: 1px solid %3; border-radius: 6px; padding: 7px 14px; }"
        "QPushButton:hover { border-color: %4; color: %5; }")
                                   .arg(Theme::hex(Theme::Raised), Theme::hex(Theme::TextSecondary), Theme::hex(Theme::Border),
                                        Theme::hex(Theme::Accent), Theme::hex(Theme::TextPrimary)));
    m_title->setMaximumWidth(kCardWidth - 40);

    auto *buttons = new QHBoxLayout;
    buttons->setSpacing(8);
    buttons->addWidget(m_resume, 1);
    buttons->addWidget(m_startOver);

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(20, 16, 20, 22);
    layout->setSpacing(4);
    layout->addWidget(m_heading);
    layout->addWidget(m_title);
    layout->addWidget(m_detail);
    layout->addSpacing(8);
    layout->addLayout(buttons);
    layout->addWidget(m_countdown);

    connect(m_resume, &QPushButton::clicked, this, [this] {
        dismiss();
        Q_EMIT resumeChosen();
    });
    connect(m_startOver, &QPushButton::clicked, this, [this] {
        dismiss();
        Q_EMIT startOverChosen();
    });
    m_tick.setInterval(1000);
    connect(&m_tick, &QTimer::timeout, this, [this] {
        if (--m_secondsLeft <= 0) {
            m_resume->click();
            return;
        }
        updateCountdown();
    });
}

void ResumePrompt::ask(const QString &title, double position, double duration, bool audio)
{
    m_heading->setText(audio ? tr("Continue listening?") : tr("Continue watching?"));
    m_title->setText(m_title->fontMetrics().elidedText(title, Qt::ElideMiddle, kCardWidth - 40));
    m_title->setToolTip(title);
    const QString at = shortTime(position);
    m_detail->setText(duration > 0 ? tr("You stopped at %1 of %2").arg(at, shortTime(duration))
                                   : tr("You stopped at %1").arg(at));
    m_resume->setText(tr("▶  Resume from %1").arg(at));
    m_progress = duration > 0 ? std::clamp(position / duration, 0.0, 1.0) : 0;
    m_secondsLeft = kCountdownSeconds;
    updateCountdown();
    place();
    show();
    raise();
    m_resume->setFocus();
    m_tick.start();
}

void ResumePrompt::dismiss()
{
    m_tick.stop();
    const bool hadFocus = isAncestorOf(QApplication::focusWidget());
    hide();
    // Hand the keyboard back to the player.
    if (hadFocus)
        parentWidget()->setFocus();
}

void ResumePrompt::updateCountdown()
{
    m_countdown->setText(tr("Resuming in %n second(s)  ·  Enter resume, Esc start over", nullptr, m_secondsLeft));
}

void ResumePrompt::place()
{
    adjustSize();
    resize(kCardWidth, sizeHint().height());
    const QWidget *p = parentWidget();
    move((p->width() - width()) / 2, std::max(8, p->height() - height() - kBottomGap));
}

bool ResumePrompt::eventFilter(QObject *watched, QEvent *event)
{
    if (watched == parent() && event->type() == QEvent::Resize && isVisible())
        place();
    return QWidget::eventFilter(watched, event);
}

bool ResumePrompt::event(QEvent *event)
{
    // Enter, Escape and the letters are window shortcuts (fullscreen, ...);
    // while the card has the focus they answer it instead.
    if (event->type() == QEvent::ShortcutOverride) {
        switch (static_cast<QKeyEvent *>(event)->key()) {
        case Qt::Key_Return:
        case Qt::Key_Enter:
        case Qt::Key_Escape:
        case Qt::Key_R:
        case Qt::Key_S:
            event->accept();
            return true;
        default:
            break;
        }
    }
    return QWidget::event(event);
}

void ResumePrompt::keyPressEvent(QKeyEvent *event)
{
    switch (event->key()) {
    case Qt::Key_Return:
    case Qt::Key_Enter:
    case Qt::Key_R:
        m_resume->click();
        return;
    case Qt::Key_Escape:
    case Qt::Key_S:
        m_startOver->click();
        return;
    default:
        QWidget::keyPressEvent(event);
    }
}

// Clicks on the card stay on it: underneath, a click would pause the video.
void ResumePrompt::mousePressEvent(QMouseEvent *event)
{
    event->accept();
}

void ResumePrompt::mouseDoubleClickEvent(QMouseEvent *event)
{
    event->accept();
}

void ResumePrompt::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    const QRectF card = QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5);
    QPainterPath path;
    path.addRoundedRect(card, 12, 12);
    QColor panel = Theme::Panel;
    panel.setAlpha(242);
    p.fillPath(path, panel);
    p.setPen(QPen(Theme::Border, 1));
    p.drawPath(path);
    // Where the file was left, as a thin bar along the bottom edge.
    p.save();
    p.setClipPath(path);
    const QRectF track(card.left(), card.bottom() - 4, card.width(), 4);
    p.fillRect(track, Theme::Raised);
    p.fillRect(QRectF(track.left(), track.top(), track.width() * m_progress, track.height()), Theme::Accent);
    p.restore();
}
