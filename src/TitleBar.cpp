#include "TitleBar.h"
#include "Icons.h"
#include "Theme.h"

#include <QApplication>
#include <QEnterEvent>
#include <QEvent>
#include <QHBoxLayout>
#include <QLabel>
#include <QMouseEvent>
#include <QPainter>
#include <QSignalBlocker>
#include <QToolButton>
#include <QVariantAnimation>

namespace {

constexpr int kButtonWidth = 40;
constexpr int kButtonHeight = 26;
constexpr int kFadeMs = 140;

// A window button that fades a rounded highlight in and out under the mouse
// (red for the close button) instead of switching flat squares on and off.
class WindowButton : public QToolButton
{
public:
    WindowButton(const QString &name, IconType icon, const QString &toolTip, QWidget *parent, bool danger = false)
        : QToolButton(parent)
        , m_danger(danger)
        , m_fade(new QVariantAnimation(this))
    {
        setObjectName(name);
        setIcon(danger ? skinIcon(icon, Qt::white) : skinIcon(icon));
        setIconSize(QSize(16, 16));
        setToolTip(toolTip);
        setAutoRaise(true);
        setFocusPolicy(Qt::NoFocus);
        setFixedSize(kButtonWidth, kButtonHeight);
        setAttribute(Qt::WA_Hover);
        m_fade->setDuration(kFadeMs);
        m_fade->setEasingCurve(QEasingCurve::OutCubic);
        connect(m_fade, &QVariantAnimation::valueChanged, this, [this](const QVariant &value) {
            m_hover = value.toReal();
            update();
        });
    }

protected:
    void enterEvent(QEnterEvent *event) override
    {
        fadeTo(1.0);
        QToolButton::enterEvent(event);
    }

    void leaveEvent(QEvent *event) override
    {
        fadeTo(0.0);
        QToolButton::leaveEvent(event);
    }

    void paintEvent(QPaintEvent *) override
    {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        const QRectF pill = QRectF(rect()).adjusted(3, 2, -3, -2);
        QColor background = m_danger ? QColor(0xE5, 0x48, 0x4D) : QColor(255, 255, 255, 28);
        const bool pressed = isDown();
        background.setAlphaF(background.alphaF() * (pressed ? 1.0 : m_hover) * (pressed && !m_danger ? 1.6 : 1.0));
        if (isChecked() && !m_danger)
            background = QColor(Theme::Accent.red(), Theme::Accent.green(), Theme::Accent.blue(), 40 + int(30 * m_hover));
        if (background.alpha() > 0) {
            p.setPen(Qt::NoPen);
            p.setBrush(background);
            p.drawRoundedRect(pill, 6, 6);
        }
        const QIcon::Mode mode = m_hover > 0.5 || pressed ? QIcon::Active : QIcon::Normal;
        icon().paint(&p, QRect(QPoint(), iconSize()).translated((width() - iconSize().width()) / 2,
                                                                 (height() - iconSize().height()) / 2),
                     Qt::AlignCenter, mode, isChecked() ? QIcon::On : QIcon::Off);
    }

private:
    void fadeTo(qreal target)
    {
        m_fade->stop();
        m_fade->setStartValue(m_hover);
        m_fade->setEndValue(target);
        m_fade->start();
    }

    bool m_danger;
    qreal m_hover = 0;
    QVariantAnimation *m_fade;
};

} // namespace

TitleBar::TitleBar(QWidget *window)
    : QFrame(window)
    , m_window(window)
    , m_title(new QLabel(this))
{
    setObjectName(QStringLiteral("TitleBar"));
    m_title->setObjectName(QStringLiteral("TitleLabel"));
    // The label shows an elided copy of the title, so it must not dictate its own width.
    m_title->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);

    auto *logo = new QLabel(this);
    logo->setObjectName(QStringLiteral("AppLogoLabel"));
    logo->setPixmap(appLogo(16, devicePixelRatioF()));
    auto *appName = new QLabel(QApplication::applicationDisplayName(), this);
    appName->setObjectName(QStringLiteral("AppNameLabel"));

    auto *mini = new WindowButton(QStringLiteral("MiniPlayerButton"), IconType::MiniPlayer, tr("Mini Player (Ctrl+M)"), this);
    m_pinButton = new WindowButton(QStringLiteral("PinButton"), IconType::Pin, tr("Always on Top (Ctrl+T)"), this);
    m_pinButton->setCheckable(true);
    auto *minimize = new WindowButton(QStringLiteral("MinimizeButton"), IconType::Minimize, tr("Minimize"), this);
    m_maximizeButton = new WindowButton(QStringLiteral("MaximizeButton"), IconType::Maximize, tr("Maximize"), this);
    auto *fullScreen = new WindowButton(QStringLiteral("TitleFullScreenButton"), IconType::Fullscreen, tr("Fullscreen (Enter)"), this);
    auto *close = new WindowButton(QStringLiteral("CloseButton"), IconType::Close, tr("Close"), this, true);

    auto *layout = new QHBoxLayout(this);
    layout->setContentsMargins(10, 0, 4, 0);
    layout->setSpacing(0);
    layout->addWidget(logo);
    layout->addSpacing(6);
    layout->addWidget(appName);
    layout->addSpacing(10);
    layout->addWidget(m_title, 1);
    layout->addWidget(mini);
    layout->addWidget(m_pinButton);
    layout->addSpacing(4);
    layout->addWidget(minimize);
    layout->addWidget(m_maximizeButton);
    layout->addWidget(fullScreen);
    layout->addWidget(close);

    connect(mini, &QToolButton::clicked, this, &TitleBar::miniPlayerRequested);
    connect(m_pinButton, &QToolButton::toggled, this, &TitleBar::pinToggled);
    connect(minimize, &QToolButton::clicked, m_window, &QWidget::showMinimized);
    connect(m_maximizeButton, &QToolButton::clicked, this,
            [this] { m_window->isMaximized() ? m_window->showNormal() : m_window->showMaximized(); });
    connect(fullScreen, &QToolButton::clicked, this, &TitleBar::fullScreenRequested);
    connect(close, &QToolButton::clicked, m_window, &QWidget::close);
    m_window->installEventFilter(this);
}

void TitleBar::setTitle(const QString &title)
{
    m_fullTitle = title;
    updateElidedTitle();
}

void TitleBar::setPinned(bool pinned)
{
    const QSignalBlocker blocker(m_pinButton);
    m_pinButton->setChecked(pinned);
}

bool TitleBar::eventFilter(QObject *watched, QEvent *event)
{
    if (watched == m_window && event->type() == QEvent::WindowStateChange)
        updateMaximizeButton();
    return QFrame::eventFilter(watched, event);
}

void TitleBar::mouseDoubleClickEvent(QMouseEvent *event)
{
    if (event->button() == Qt::LeftButton) {
        m_window->isMaximized() ? m_window->showNormal() : m_window->showMaximized();
        event->accept();
        return;
    }
    QFrame::mouseDoubleClickEvent(event);
}

void TitleBar::resizeEvent(QResizeEvent *event)
{
    QFrame::resizeEvent(event);
    updateElidedTitle();
}

void TitleBar::updateMaximizeButton()
{
    const bool maximized = m_window->isMaximized();
    m_maximizeButton->setIcon(skinIcon(maximized ? IconType::Restore : IconType::Maximize));
    m_maximizeButton->setToolTip(maximized ? tr("Restore") : tr("Maximize"));
}

void TitleBar::updateElidedTitle()
{
    m_title->setText(m_title->fontMetrics().elidedText(m_fullTitle, Qt::ElideMiddle, std::max(0, m_title->width())));
    m_title->setToolTip(m_fullTitle);
}
