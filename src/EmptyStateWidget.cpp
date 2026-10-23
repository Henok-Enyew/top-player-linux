#include "EmptyStateWidget.h"
#include "Icons.h"
#include "Theme.h"

#include <QDragEnterEvent>
#include <QDropEvent>
#include <QGraphicsOpacityEffect>
#include <QGridLayout>
#include <QLabel>
#include <QMimeData>
#include <QPainter>
#include <QPainterPath>
#include <QPropertyAnimation>
#include <QPushButton>
#include <QRadialGradient>
#include <QStyle>
#include <QVBoxLayout>

#include <algorithm>

namespace {

constexpr int kFadeInMs = 200;
constexpr int kFadeOutMs = 280;
// Logo sizes tried from largest to smallest; 0 hides the logo.
constexpr int kLogoSizes[] = {96, 64, 0};
// Space kept free around the content.
constexpr int kMargin = 16;
constexpr int kDropInset = 12;

} // namespace

EmptyStateWidget::EmptyStateWidget(QWidget *parent)
    : QWidget(parent)
    , m_content(new QWidget(this))
    , m_logo(new QLabel(m_content))
    , m_title(new QLabel(m_content))
    , m_hint(new QLabel(tr("or drag and drop files and folders here"), m_content))
    , m_buttonGrid(new QGridLayout)
    , m_opacity(new QGraphicsOpacityEffect(this))
    , m_fade(new QPropertyAnimation(m_opacity, "opacity", this))
{
    setObjectName(QStringLiteral("EmptyState"));
    setAcceptDrops(true);
    setFocusPolicy(Qt::NoFocus);

    m_content->setObjectName(QStringLiteral("EmptyStateContent"));
    m_logo->setObjectName(QStringLiteral("EmptyStateLogo"));
    m_logo->setAlignment(Qt::AlignCenter);
    m_title->setObjectName(QStringLiteral("EmptyStateTitle"));
    m_title->setAlignment(Qt::AlignCenter);
    Theme::bind(m_title, [this] {
        m_title->setText(QStringLiteral("<span style='color:%1'>Top</span> <span style='color:%2'>Player</span>")
                             .arg(Theme::hex(Theme::TextPrimary), Theme::hex(Theme::Accent)));
    });
    m_title->setAccessibleName(tr("Top Player"));
    m_hint->setObjectName(QStringLiteral("EmptyStateHint"));
    m_hint->setAlignment(Qt::AlignCenter);

    m_buttonGrid->setSpacing(10);
    addButton(QStringLiteral("OpenFileButton"), tr("Open File..."), IconType::Open);
    addButton(QStringLiteral("OpenFolderButton"), tr("Open Folder..."), IconType::Folder);
    addButton(QStringLiteral("OpenUrlButton"), tr("Open URL / Stream..."), IconType::Url);
    addButton(QStringLiteral("OpenPlaylistButton"), tr("Open Playlist..."), IconType::Playlist);
    addButton(QStringLiteral("SearchYouTubeButton"), tr("Search YouTube..."), IconType::Search);
    addButton(QStringLiteral("SearchSpotifyButton"), tr("Search Spotify..."), IconType::Search);
    connect(m_buttons[0], &QPushButton::clicked, this, &EmptyStateWidget::openFileRequested);
    connect(m_buttons[1], &QPushButton::clicked, this, &EmptyStateWidget::openFolderRequested);
    connect(m_buttons[2], &QPushButton::clicked, this, &EmptyStateWidget::openUrlRequested);
    connect(m_buttons[3], &QPushButton::clicked, this, &EmptyStateWidget::openPlaylistRequested);
    connect(m_buttons[4], &QPushButton::clicked, this, &EmptyStateWidget::searchYouTubeRequested);
    connect(m_buttons[5], &QPushButton::clicked, this, &EmptyStateWidget::searchSpotifyRequested);

    auto *content = new QVBoxLayout(m_content);
    content->setContentsMargins(0, 0, 0, 0);
    content->setSpacing(0);
    content->addWidget(m_logo);
    content->addSpacing(14);
    content->addWidget(m_title);
    content->addSpacing(22);
    content->addLayout(m_buttonGrid);
    content->addSpacing(18);
    content->addWidget(m_hint);

    // Centered at its preferred size, however large the video area is.
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(kMargin, kMargin, kMargin, kMargin);
    layout->addWidget(m_content, 0, Qt::AlignCenter);

    // The opacity effect renders offscreen, so it only runs while fading.
    m_opacity->setEnabled(false);
    setGraphicsEffect(m_opacity);
    connect(m_fade, &QPropertyAnimation::finished, this, [this] {
        m_opacity->setEnabled(false);
        if (!m_active)
            hide();
    });

    parent->installEventFilter(this);
    setGeometry(parent->rect());
    setColumns(2);
}

QPushButton *EmptyStateWidget::addButton(const QString &objectName, const QString &text, IconType icon)
{
    auto *button = new QPushButton(skinIcon(icon), text, m_content);
    button->setObjectName(objectName);
    // Compact buttons show only their icon, and the text as the tooltip.
    button->setToolTip(text);
    button->setProperty("emptyStateAction", true);
    button->setIconSize(QSize(22, 22));
    button->setCursor(Qt::PointingHandCursor);
    // Keep the keyboard on the player: Space and the arrows stay hotkeys.
    button->setFocusPolicy(Qt::NoFocus);
    m_buttons.append(button);
    return button;
}

void EmptyStateWidget::setActive(bool active, bool animate)
{
    if (active == m_active)
        return;
    m_active = active;
    // While it fades out, clicks and drags go to the player underneath.
    setAttribute(Qt::WA_TransparentForMouseEvents, !active);
    m_fade->stop();
    if (!animate) {
        m_opacity->setEnabled(false);
        setVisible(active);
        return;
    }
    if (active) {
        // Start from transparent unless it is still partly visible from a fade-out.
        if (!isVisible())
            m_opacity->setOpacity(0);
        show();
    }
    m_opacity->setEnabled(true);
    m_fade->setDuration(active ? kFadeInMs : kFadeOutMs);
    m_fade->setStartValue(m_opacity->opacity());
    m_fade->setEndValue(active ? 1.0 : 0.0);
    m_fade->start();
}

bool EmptyStateWidget::eventFilter(QObject *watched, QEvent *event)
{
    if (watched == parent() && event->type() == QEvent::Resize)
        setGeometry(parentWidget()->rect());
    return QWidget::eventFilter(watched, event);
}

void EmptyStateWidget::resizeEvent(QResizeEvent *event)
{
    QWidget::resizeEvent(event);
    fitContent();
}

void EmptyStateWidget::setColumns(int columns)
{
    if (columns == m_columns)
        return;
    m_columns = columns;
    for (QPushButton *button : std::as_const(m_buttons))
        m_buttonGrid->removeWidget(button);
    for (int i = 0; i < m_buttons.size(); ++i)
        m_buttonGrid->addWidget(m_buttons[i], i / columns, i % columns);
}

void EmptyStateWidget::fitContent()
{
    const QSize available = size() - QSize(2 * kMargin, 2 * kMargin);
    auto fits = [&] {
        m_content->layout()->activate();
        return m_buttonGrid->sizeHint().width() <= available.width()
            && m_content->sizeHint().height() <= available.height();
    };

    // The labels never decide the button layout: a title too wide for the
    // area gets smaller, then goes, and so does the hint.
    setCompact(m_title, false);
    if (m_title->sizeHint().width() > available.width())
        setCompact(m_title, true);
    const bool titleFits = m_title->sizeHint().width() <= available.width();
    const bool hintFits = m_hint->sizeHint().width() <= available.width();
    m_title->setVisible(titleFits);
    m_hint->setVisible(hintFits);

    // Two columns of buttons when there is room, one on narrow areas.
    for (QPushButton *button : std::as_const(m_buttons))
        setCompact(button, false);
    setColumns(2);
    if (!fits())
        setColumns(1);

    // Then drop decoration until the buttons fit: shrink and hide the logo,
    // then the hint, then the title.
    const qreal dpr = devicePixelRatioF();
    for (int logoSize : kLogoSizes) {
        m_logo->setVisible(logoSize > 0);
        if (logoSize > 0)
            m_logo->setPixmap(appLogo(logoSize, dpr));
        if (fits())
            return;
    }
    m_hint->hide();
    if (fits())
        return;
    m_title->hide();
    // Short but wide areas: one row of buttons.
    setColumns(m_buttons.size());
    if (fits())
        return;
    // Tiny areas: icon-only buttons in a row or a square, with the title
    // back if there is room for it now.
    for (QPushButton *button : std::as_const(m_buttons))
        setCompact(button, true);
    for (int columns : {int(m_buttons.size()), 2}) {
        setColumns(columns);
        m_title->setVisible(titleFits);
        if (fits())
            return;
        m_title->hide();
        if (fits())
            return;
    }
}

void EmptyStateWidget::setCompact(QWidget *widget, bool compact)
{
    if (widget->property("compact").toBool() == compact)
        return;
    widget->setProperty("compact", compact);
    if (auto *button = qobject_cast<QPushButton *>(widget))
        button->setText(compact ? QString() : button->toolTip());
    // Re-apply the stylesheet so the widget picks up the property.
    widget->style()->unpolish(widget);
    widget->style()->polish(widget);
    widget->updateGeometry();
}

void EmptyStateWidget::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    // A soft spotlight behind the content, warmed by a faint wash of the
    // accent above it: enough to tie the page to the theme, quiet enough
    // that the logo and the buttons stay what the eye finds first.
    const QPointF center = rect().center();
    const qreal reach = std::max(width(), height());
    QRadialGradient spotlight(center, reach * 0.75);
    spotlight.setColorAt(0, Theme::Panel);
    spotlight.setColorAt(1, Theme::Surface);
    p.fillRect(rect(), spotlight);
    QRadialGradient wash(QPointF(width() * 0.5, height() * 0.3), reach * 0.5);
    wash.setColorAt(0, Theme::withAlpha(Theme::Accent, Theme::isLight() ? 0.05 : 0.07));
    wash.setColorAt(1, Theme::withAlpha(Theme::Accent, 0));
    p.fillRect(rect(), wash);

    if (m_dropHighlight) {
        const QRectF zone = QRectF(rect()).adjusted(kDropInset, kDropInset, -kDropInset, -kDropInset);
        p.setBrush(Theme::withAlpha(Theme::Accent, 0.07));
        p.setPen(QPen(Theme::Accent, 2, Qt::DashLine));
        p.drawRoundedRect(zone, 10, 10);
    }
}

void EmptyStateWidget::setDropHighlight(bool highlight)
{
    if (highlight == m_dropHighlight)
        return;
    m_dropHighlight = highlight;
    m_hint->setProperty("dropTarget", highlight);
    // Re-apply the stylesheet so the hint picks up the property.
    m_hint->style()->unpolish(m_hint);
    m_hint->style()->polish(m_hint);
    update();
}

void EmptyStateWidget::dragEnterEvent(QDragEnterEvent *event)
{
    if (!event->mimeData()->hasUrls())
        return;
    event->acceptProposedAction();
    setDropHighlight(true);
}

void EmptyStateWidget::dragLeaveEvent(QDragLeaveEvent *event)
{
    QWidget::dragLeaveEvent(event);
    setDropHighlight(false);
}

void EmptyStateWidget::dropEvent(QDropEvent *event)
{
    setDropHighlight(false);
    const QList<QUrl> urls = event->mimeData()->urls();
    if (urls.isEmpty())
        return;
    event->acceptProposedAction();
    Q_EMIT urlsDropped(urls);
}
