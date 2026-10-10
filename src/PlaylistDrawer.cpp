#include "PlaylistDrawer.h"
#include "DownloadQueue.h"
#include "Icons.h"
#include "LibraryPanel.h"
#include "MpvWidget.h"
#include "PlaylistSession.h"
#include "Theme.h"
#include "TimeFormat.h"

#include <QAction>
#include <QApplication>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMimeData>
#include <QPainter>
#include <QMouseEvent>
#include <QPropertyAnimation>
#include <QSet>
#include <QStackedWidget>
#include <QStyledItemDelegate>
#include <QToolTip>
#include <QTabBar>
#include <QTimer>
#include <QToolButton>
#include <QUrl>
#include <QVBoxLayout>

#include <algorithm>
#include <utility>

namespace {

constexpr int kDefaultWidth = 280;
constexpr int kMinimumWidth = 200;
// Video left visible beside the drawer at its widest.
constexpr int kMinimumVideoWidth = 160;
constexpr int kHandleWidth = 5;
constexpr int kAnimationMs = 180;
const QColor kPlayingColor = Theme::Accent;
const QColor kDurationColor = Theme::TextSecondary;
// Item data: the entry's filename, and its duration text.
constexpr int kFilenameRole = Qt::UserRole;
constexpr int kDurationRole = Qt::UserRole + 1;
// Set on the entry that is playing.
constexpr int kPlayingRole = Qt::UserRole + 2;
// mpv's id of the entry, which stays with it when the playlist is reordered.
constexpr int kIdRole = Qt::UserRole + 3;
// A download's state shown in place of the duration, and whether the entry
// streams from the web and can be downloaded.
constexpr int kDownloadRole = Qt::UserRole + 4;
constexpr int kDownloadableRole = Qt::UserRole + 5;
constexpr int kDownloadIconSize = 14;
constexpr int kDurationGap = 8;
constexpr int kPlayingBarWidth = 3;

// Dropped files, folders and URLs, without subtitle files. Folders are
// expanded later, off the GUI thread.
QStringList droppedEntries(const QMimeData *mime)
{
    QStringList entries;
    for (const QUrl &url : mime->urls()) {
        const QString path = url.isLocalFile() ? url.toLocalFile() : url.toString();
        if (!url.isLocalFile() || !MpvWidget::isSubtitleFile(path))
            entries.append(path);
    }
    return entries;
}

// Short duration text: m:ss below an hour, h:mm:ss above.
QString durationText(double seconds)
{
    if (seconds < 0)
        return {};
    const QString full = formatTime(seconds);
    if (seconds >= 3600)
        return full.startsWith(QLatin1Char('0')) ? full.mid(1) : full;
    const QString minutes = full.mid(3);
    return minutes.startsWith(QLatin1Char('0')) ? minutes.mid(1) : minutes;
}

// Where an entry's download button is: a square at the right end of its row.
QRect downloadButtonRect(const QRect &itemRect)
{
    const int side = itemRect.height();
    return QRect(itemRect.right() - side - 2, itemRect.top(), side, side);
}

bool downloadInProgress(const QString &status)
{
    return !status.isEmpty() && status != DownloadQueue::tr("Saved") && status != DownloadQueue::tr("Failed");
}

// Draws the entry's duration (or download state) right-aligned, eliding the
// name before it, and the download button on online entries under the pointer.
class PlaylistItemDelegate : public QStyledItemDelegate
{
public:
    using QStyledItemDelegate::QStyledItemDelegate;

    void paint(QPainter *painter, const QStyleOptionViewItem &option, const QModelIndex &index) const override
    {
        QStyleOptionViewItem opt(option);
        initStyleOption(&opt, index);
        const QString download = index.data(kDownloadRole).toString();
        const QString duration = download.isEmpty() ? index.data(kDurationRole).toString()
                                                    : QStringLiteral("\u2193 ") + download;
        const bool button = (opt.state & QStyle::State_MouseOver) && index.data(kDownloadableRole).toBool()
                            && !downloadInProgress(download);
        const QWidget *widget = opt.widget;
        QStyle *style = widget ? widget->style() : QApplication::style();
        QRect textRect = style->subElementRect(QStyle::SE_ItemViewItemText, &opt, widget);
        const QRect buttonRect = downloadButtonRect(opt.rect);
        if (button)
            textRect.setRight(buttonRect.left() - 2);
        int durationWidth = 0;
        if (!duration.isEmpty())
            durationWidth = opt.fontMetrics.horizontalAdvance(duration) + kDurationGap;
        if (!duration.isEmpty() || button)
            opt.text = opt.fontMetrics.elidedText(opt.text, opt.textElideMode, textRect.width() - durationWidth);
        style->drawControl(QStyle::CE_ItemViewItem, &opt, painter, widget);
        if (index.data(kPlayingRole).toBool()) {
            // An accent bar along the left edge marks the entry that is playing.
            painter->fillRect(QRect(opt.rect.left(), opt.rect.top() + 3, kPlayingBarWidth, opt.rect.height() - 6), kPlayingColor);
        }
        if (button) {
            const QPixmap icon = skinIcon(IconType::Download).pixmap(QSize(kDownloadIconSize, kDownloadIconSize));
            const QRect iconRect(QPoint(), QSize(kDownloadIconSize, kDownloadIconSize));
            painter->drawPixmap(iconRect.translated(buttonRect.center() - iconRect.center()), icon);
        }
        if (duration.isEmpty())
            return;
        painter->save();
        const bool selected = opt.state & QStyle::State_Selected;
        painter->setPen(!download.isEmpty() ? (download == DownloadQueue::tr("Failed") ? QColor(0xFF, 0x6B, 0x5B) : Theme::Accent)
                        : selected          ? Theme::TextPrimary
                                            : kDurationColor);
        painter->setFont(opt.font);
        painter->drawText(textRect, Qt::AlignRight | Qt::AlignVCenter, duration);
        painter->restore();
    }
};

// Strip along the drawer's left edge: drag it to resize the drawer,
// double-click it to expand or restore the drawer.
class ResizeHandle : public QWidget
{
public:
    explicit ResizeHandle(PlaylistDrawer *drawer)
        : QWidget(drawer)
        , m_drawer(drawer)
    {
        setObjectName(QStringLiteral("PlaylistResizeHandle"));
        setFixedWidth(kHandleWidth);
        setCursor(Qt::SizeHorCursor);
        setAttribute(Qt::WA_StyledBackground);
        setAttribute(Qt::WA_Hover);
        setToolTip(PlaylistDrawer::tr("Drag to resize, double-click to expand"));
    }

protected:
    void mousePressEvent(QMouseEvent *event) override
    {
        if (event->button() != Qt::LeftButton)
            return QWidget::mousePressEvent(event);
        m_grabOffset = qRound(event->position().x());
        m_dragging = true;
        event->accept();
    }

    void mouseMoveEvent(QMouseEvent *event) override
    {
        if (!m_dragging)
            return QWidget::mouseMoveEvent(event);
        const int right = m_drawer->mapToGlobal(QPoint(m_drawer->width(), 0)).x();
        m_drawer->setPreferredWidth(right - qRound(event->globalPosition().x()) + m_grabOffset, false);
        event->accept();
    }

    void mouseReleaseEvent(QMouseEvent *event) override
    {
        if (!m_dragging || event->button() != Qt::LeftButton)
            return QWidget::mouseReleaseEvent(event);
        m_dragging = false;
        m_drawer->setPreferredWidth(m_drawer->preferredWidth());
        event->accept();
    }

    void mouseDoubleClickEvent(QMouseEvent *event) override
    {
        m_dragging = false;
        m_drawer->setWide(!m_drawer->isWide());
        event->accept();
    }

private:
    PlaylistDrawer *m_drawer;
    int m_grabOffset = 0;
    bool m_dragging = false;
};

} // namespace

PlaylistView::PlaylistView(QWidget *parent)
    : QListWidget(parent)
{
    setObjectName(QStringLiteral("PlaylistView"));
    setSelectionMode(QAbstractItemView::ExtendedSelection);
    setDragDropMode(QAbstractItemView::DragDrop);
    setDefaultDropAction(Qt::MoveAction);
    setDropIndicatorShown(true);
    setAcceptDrops(true);
    setTextElideMode(Qt::ElideMiddle);
    setUniformItemSizes(true);
    // Rows under the pointer show their download button.
    setMouseTracking(true);
    viewport()->setAttribute(Qt::WA_Hover);
}

void PlaylistView::mousePressEvent(QMouseEvent *event)
{
    const QPoint pos = event->position().toPoint();
    const QModelIndex index = indexAt(pos);
    if (event->button() == Qt::LeftButton && index.isValid() && index.data(kDownloadableRole).toBool()
        && !downloadInProgress(index.data(kDownloadRole).toString())
        && downloadButtonRect(visualRect(index)).contains(pos)) {
        Q_EMIT downloadButtonClicked(index.row(), viewport()->mapToGlobal(downloadButtonRect(visualRect(index)).bottomLeft()));
        event->accept();
        return;
    }
    QListWidget::mousePressEvent(event);
}

bool PlaylistView::viewportEvent(QEvent *event)
{
    if (event->type() == QEvent::ToolTip) {
        const auto *help = static_cast<QHelpEvent *>(event);
        const QModelIndex index = indexAt(help->pos());
        if (index.isValid() && index.data(kDownloadableRole).toBool()
            && downloadButtonRect(visualRect(index)).contains(help->pos())) {
            QToolTip::showText(help->globalPos(), PlaylistDrawer::tr("Download (video or audio)"), viewport());
            return true;
        }
    }
    return QListWidget::viewportEvent(event);
}

QList<int> PlaylistView::selectedVisibleRows() const
{
    // Select All also selects the rows the filter hides; leave those alone.
    QList<int> rows;
    for (const QModelIndex &index : selectionModel()->selectedRows()) {
        if (!isRowHidden(index.row()))
            rows.append(index.row());
    }
    std::sort(rows.begin(), rows.end());
    return rows;
}

void PlaylistView::dragEnterEvent(QDragEnterEvent *event)
{
    if (event->source() == this || event->mimeData()->hasUrls())
        QListWidget::dragEnterEvent(event);
    else
        event->ignore();
}

void PlaylistView::dragMoveEvent(QDragMoveEvent *event)
{
    QListWidget::dragMoveEvent(event);
    // External file drops are copies; QListWidget only accepts its own item data.
    if (event->source() != this && event->mimeData()->hasUrls()) {
        event->setDropAction(Qt::CopyAction);
        event->accept();
    }
}

int PlaylistView::dropRow(QDropEvent *event) const
{
    const QModelIndex index = indexAt(event->position().toPoint());
    if (!index.isValid())
        return -1;
    switch (dropIndicatorPosition()) {
    case QAbstractItemView::BelowItem:
        return index.row() + 1;
    case QAbstractItemView::OnItem:
        // Dropping on an item inserts before or after it depending on the half.
        return visualRect(index).center().y() < event->position().y() ? index.row() + 1 : index.row();
    case QAbstractItemView::AboveItem:
        return index.row();
    case QAbstractItemView::OnViewport:
        break;
    }
    return -1;
}

void PlaylistView::dropEvent(QDropEvent *event)
{
    const int row = dropRow(event);
    if (event->source() == this) {
        // Move the dragged entries one by one, keeping their order.
        const QList<int> rows = selectedVisibleRows();
        int target = row < 0 ? count() : row;
        int shift = 0; // entries above the target that were already moved below it
        for (int from : std::as_const(rows)) {
            const int current = from < target ? from - shift : from;
            Q_EMIT moveRequested(current, target);
            if (from < target)
                ++shift;
            else
                ++target;
        }
    } else {
        const QStringList entries = droppedEntries(event->mimeData());
        if (!entries.isEmpty())
            Q_EMIT filesDropped(entries, row);
    }
    // The playlist is rebuilt from mpv, so the view must not move anything itself.
    event->setDropAction(Qt::IgnoreAction);
    event->accept();
}

void PlaylistView::keyPressEvent(QKeyEvent *event)
{
    if (event->key() == Qt::Key_Delete) {
        const QList<int> rows = selectedVisibleRows();
        if (!rows.isEmpty())
            Q_EMIT removeRequested(rows);
        event->accept();
        return;
    }
    if (event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter) {
        if (currentRow() >= 0)
            Q_EMIT itemActivated(currentItem());
        event->accept();
        return;
    }
    QListWidget::keyPressEvent(event);
}

PlaylistDrawer::PlaylistDrawer(QWidget *parent)
    : QFrame(parent)
    , m_view(new PlaylistView(this))
    , m_filter(new QLineEdit(this))
    , m_countLabel(new QLabel(this))
    , m_tabs(new QTabBar(this))
    , m_pages(new QStackedWidget(this))
    , m_library(new LibraryPanel(this))
    , m_animation(new QPropertyAnimation(this, "drawerWidth", this))
    , m_preferredWidth(std::max(kMinimumWidth, PlaylistSession::drawerWidth(kDefaultWidth)))
{
    setObjectName(QStringLiteral("PlaylistDrawer"));
    m_view->setItemDelegate(new PlaylistItemDelegate(m_view));
    m_view->setContextMenuPolicy(Qt::CustomContextMenu);
    buildMenus();

    auto makeButton = [this](IconType icon, const QString &toolTip, const char *name) {
        auto *button = new QToolButton(this);
        button->setObjectName(QString::fromLatin1(name));
        button->setIcon(skinIcon(icon));
        button->setIconSize(QSize(16, 16));
        button->setToolTip(toolTip);
        button->setAutoRaise(true);
        button->setFocusPolicy(Qt::NoFocus);
        return button;
    };

    // Page tabs, the entry count and the expand button.
    auto *header = new QHBoxLayout;
    header->setContentsMargins(6, 4, 6, 2);
    header->setSpacing(0);
    m_tabs->setObjectName(QStringLiteral("PlaylistTabs"));
    m_tabs->addTab(tr("Playlist"));
    m_tabs->addTab(tr("Library"));
    m_tabs->setDrawBase(false);
    m_tabs->setExpanding(false);
    m_tabs->setFocusPolicy(Qt::NoFocus);
    m_countLabel->setObjectName(QStringLiteral("PlaylistCount"));
    header->addWidget(m_tabs);
    header->addSpacing(2);
    header->addWidget(m_countLabel);
    header->addStretch();
    QToolButton *more = makeButton(IconType::More, tr("More"), "PlaylistMenuButton");
    more->setMenu(m_moreMenu);
    more->setPopupMode(QToolButton::InstantPopup);
    more->setProperty("hideMenuIndicator", true);
    header->addWidget(more);
    m_wideButton = makeButton(IconType::Expand, tr("Expand Playlist"), "PlaylistExpandButton");
    header->addWidget(m_wideButton);

    // Search field, with shuffle beside it.
    auto *tools = new QHBoxLayout;
    tools->setContentsMargins(8, 4, 6, 6);
    tools->setSpacing(4);
    m_filter->setObjectName(QStringLiteral("PlaylistFilter"));
    m_filter->setPlaceholderText(tr("Search playlist"));
    m_filter->setClearButtonEnabled(true);
    m_filter->addAction(skinIcon(IconType::Search), QLineEdit::LeadingPosition);
    m_filter->installEventFilter(this);
    QToolButton *shuffle = makeButton(IconType::Shuffle, tr("Shuffle"), "PlaylistShuffleButton");
    tools->addWidget(m_filter, 1);
    tools->addWidget(shuffle);

    auto *playlistPage = new QWidget(this);
    auto *playlistLayout = new QVBoxLayout(playlistPage);
    playlistLayout->setContentsMargins(0, 0, 0, 0);
    playlistLayout->setSpacing(0);
    playlistLayout->addLayout(tools);
    playlistLayout->addWidget(m_view, 1);
    playlistLayout->addWidget(buildActionBar());
    m_pages->addWidget(playlistPage);
    m_pages->addWidget(m_library);

    auto *content = new QVBoxLayout;
    content->setContentsMargins(0, 0, 0, 0);
    content->setSpacing(0);
    content->addLayout(header);
    content->addWidget(m_pages, 1);

    auto *layout = new QHBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    layout->addWidget(new ResizeHandle(this));
    layout->addLayout(content, 1);

    connect(m_tabs, &QTabBar::currentChanged, this, [this, more](int index) {
        m_pages->setCurrentIndex(index);
        m_countLabel->setVisible(index == PlaylistPage);
        more->setVisible(index == PlaylistPage);
    });
    connect(m_wideButton, &QToolButton::clicked, this, [this] { setWide(!m_wide); });
    // Follow the window's width while open, e.g. to stay wide.
    if (parentWidget())
        parentWidget()->installEventFilter(this);

    m_view->viewport()->installEventFilter(this);
    connect(m_view, &QListWidget::itemActivated, this,
            [this](QListWidgetItem *item) { Q_EMIT playRequested(m_view->row(item)); });
    connect(m_view, &PlaylistView::moveRequested, this, &PlaylistDrawer::moveRequested);
    connect(m_view, &PlaylistView::filesDropped, this, &PlaylistDrawer::filesDropped);
    connect(m_view, &PlaylistView::removeRequested, this, &PlaylistDrawer::removeRequested);
    connect(m_view, &QWidget::customContextMenuRequested, this, &PlaylistDrawer::showContextMenu);
    connect(m_view, &PlaylistView::downloadButtonClicked, this, [this](int row, const QPoint &globalPos) {
        QMenu menu(this);
        menu.setObjectName(QStringLiteral("PlaylistDownloadMenu"));
        // The selection if the row is part of it, else just the row.
        QList<int> rows = m_view->selectedVisibleRows();
        if (!rows.contains(row))
            rows = {row};
        addDownloadActions(&menu, rows);
        menu.exec(globalPos);
    });
    connect(m_filter, &QLineEdit::textChanged, this, &PlaylistDrawer::applyFilter);
    connect(shuffle, &QToolButton::clicked, this, &PlaylistDrawer::shuffleRequested);

    m_animation->setDuration(kAnimationMs);
    m_animation->setEasingCurve(QEasingCurve::OutCubic);
    connect(m_animation, &QPropertyAnimation::finished, this, [this] {
        if (!m_expanded)
            hide();
    });

    setDrawerWidth(0);
    hide();
    setEntries({});
}

void PlaylistDrawer::buildMenus()
{
    using PlaylistOps::SortKey;
    m_sortMenu = new QMenu(tr("Sort"), this);
    m_sortMenu->setObjectName(QStringLiteral("PlaylistSortMenu"));
    auto addSort = [this](const QString &text, SortKey key, bool ascending) {
        m_sortMenu->addAction(text, this, [this, key, ascending] { Q_EMIT sortRequested(key, ascending); });
    };
    addSort(tr("By Name (A to Z)"), SortKey::Name, true);
    addSort(tr("By Name (Z to A)"), SortKey::Name, false);
    m_sortMenu->addSeparator();
    addSort(tr("By Duration (Shortest First)"), SortKey::Duration, true);
    addSort(tr("By Duration (Longest First)"), SortKey::Duration, false);
    m_sortMenu->addSeparator();
    addSort(tr("By File Path"), SortKey::Path, true);
    addSort(tr("By File Size (Smallest First)"), SortKey::Size, true);
    addSort(tr("By File Size (Largest First)"), SortKey::Size, false);
    m_sortMenu->addSeparator();
    m_sortMenu->addAction(tr("Reverse Order"), this, &PlaylistDrawer::reverseRequested);
    m_sortMenu->addAction(skinIcon(IconType::Shuffle), tr("Shuffle"), this, &PlaylistDrawer::shuffleRequested);

    m_addMenu = new QMenu(tr("Add"), this);
    m_addMenu->setObjectName(QStringLiteral("PlaylistAddMenu"));
    m_addMenu->addAction(skinIcon(IconType::Add), tr("Add Files..."), this, &PlaylistDrawer::addRequested);
    m_addMenu->addAction(skinIcon(IconType::Folder), tr("Add Folder..."), this, &PlaylistDrawer::addFolderRequested);
    m_addMenu->addAction(skinIcon(IconType::Url), tr("Add URL..."), this, &PlaylistDrawer::addUrlRequested);
    m_addMenu->addSeparator();
    // Opening replaces the playlist, adding keeps it: PotPlayer has both.
    m_addMenu->addAction(skinIcon(IconType::Open), tr("Open Folder (Replace Playlist)..."), this, &PlaylistDrawer::openFolderRequested);
    m_addMenu->addAction(tr("Open Playlist..."), this, &PlaylistDrawer::openPlaylistRequested);

    m_deleteMenu = new QMenu(tr("Delete"), this);
    m_deleteMenu->setObjectName(QStringLiteral("PlaylistDeleteMenu"));
    QAction *removeSelected = m_deleteMenu->addAction(skinIcon(IconType::Remove), tr("Remove Selected"), this, [this] {
        const QList<int> rows = m_view->selectedVisibleRows();
        if (!rows.isEmpty())
            Q_EMIT removeRequested(rows);
    });
    removeSelected->setShortcut(QKeySequence(Qt::Key_Delete));
    removeSelected->setShortcutContext(Qt::WidgetShortcut); // the list handles Del itself
    m_deleteMenu->addAction(tr("Remove Missing/Inaccessible Files"), this, &PlaylistDrawer::removeMissingRequested);
    m_deleteMenu->addAction(tr("Remove Duplicates"), this, &PlaylistDrawer::removeDuplicatesRequested);
    m_deleteMenu->addSeparator();
    m_deleteMenu->addAction(skinIcon(IconType::Clear), tr("Clear Playlist"), this, &PlaylistDrawer::clearRequested);
    connect(m_deleteMenu, &QMenu::aboutToShow, this, [this, removeSelected] {
        removeSelected->setEnabled(!m_view->selectedVisibleRows().isEmpty());
    });

    m_moreMenu = new QMenu(this);
    m_moreMenu->setObjectName(QStringLiteral("PlaylistMoreMenu"));
    m_moreMenu->addAction(tr("Open Playlist..."), this, &PlaylistDrawer::openPlaylistRequested);
    m_moreMenu->addAction(tr("Save Playlist..."), this, &PlaylistDrawer::savePlaylistRequested);
    m_moreMenu->addSeparator();
    m_moreMenu->addAction(tr("Remove Missing/Inaccessible Files"), this, &PlaylistDrawer::removeMissingRequested);
    m_moreMenu->addAction(tr("Remove Duplicates"), this, &PlaylistDrawer::removeDuplicatesRequested);
    m_moreMenu->addAction(skinIcon(IconType::Clear), tr("Clear Playlist"), this, &PlaylistDrawer::clearRequested);
    m_moreMenu->addSeparator();
    m_rememberAction = m_moreMenu->addAction(tr("Remember Playlist on Exit"));
    m_rememberAction->setCheckable(true);
    m_resumeAction = m_moreMenu->addAction(tr("Resume Playback Position"));
    m_resumeAction->setCheckable(true);
    connect(m_moreMenu, &QMenu::aboutToShow, this, &PlaylistDrawer::syncOptions);
    // triggered, unlike toggled, only fires for the user's clicks.
    connect(m_rememberAction, &QAction::triggered, this, [this](bool on) {
        PlaylistSession::setRememberPlaylist(on);
        m_resumeAction->setEnabled(on);
    });
    connect(m_resumeAction, &QAction::triggered, this, &PlaylistSession::setResumePlayback);
}

QWidget *PlaylistDrawer::buildActionBar()
{
    auto *bar = new QFrame(this);
    bar->setObjectName(QStringLiteral("PlaylistActionBar"));
    auto *layout = new QHBoxLayout(bar);
    layout->setContentsMargins(6, 5, 6, 6);
    layout->setSpacing(2);

    const auto moveButton = [this, bar](IconType icon, const QString &toolTip, const char *name, PlaylistOps::Shift shift) {
        auto *button = new QToolButton(bar);
        button->setObjectName(QString::fromLatin1(name));
        button->setProperty("barButton", true);
        button->setProperty("barIcon", true);
        button->setIcon(skinIcon(icon));
        button->setIconSize(QSize(12, 12));
        button->setToolTip(toolTip);
        button->setFocusPolicy(Qt::NoFocus);
        button->setAutoRepeat(shift == PlaylistOps::Shift::Up || shift == PlaylistOps::Shift::Down);
        connect(button, &QToolButton::clicked, this, [this, shift] {
            const QList<int> rows = m_view->selectedVisibleRows();
            if (!rows.isEmpty())
                Q_EMIT shiftRequested(rows, shift);
        });
        return button;
    };
    using PlaylistOps::Shift;
    layout->addWidget(moveButton(IconType::MoveTop, tr("Move to Top"), "PlaylistMoveTopButton", Shift::Top));
    layout->addWidget(moveButton(IconType::MoveUp, tr("Move Up"), "PlaylistMoveUpButton", Shift::Up));
    layout->addWidget(moveButton(IconType::MoveDown, tr("Move Down"), "PlaylistMoveDownButton", Shift::Down));
    layout->addWidget(moveButton(IconType::MoveBottom, tr("Move to Bottom"), "PlaylistMoveBottomButton", Shift::Bottom));
    layout->addStretch();

    const auto textButton = [bar](const QString &text, const QString &toolTip, const char *name, QMenu *menu) {
        QToolButton *button = Theme::barButton(bar, text, toolTip, name);
        button->setMenu(menu);
        button->setPopupMode(QToolButton::InstantPopup);
        button->setProperty("hideMenuIndicator", true);
        return button;
    };
    layout->addWidget(textButton(tr("ADD"), tr("Add files, folders or URLs"), "PlaylistAddButton", m_addMenu));
    layout->addWidget(textButton(tr("DEL"), tr("Remove entries"), "PlaylistRemoveButton", m_deleteMenu));
    layout->addWidget(textButton(tr("SORT"), tr("Sort the playlist"), "PlaylistSortButton", m_sortMenu));
    return bar;
}

void PlaylistDrawer::syncOptions()
{
    m_rememberAction->setChecked(PlaylistSession::rememberPlaylist());
    m_resumeAction->setChecked(PlaylistSession::resumePlayback());
    m_resumeAction->setEnabled(m_rememberAction->isChecked());
}

void PlaylistDrawer::showContextMenu(const QPoint &pos)
{
    QMenu menu(this);
    menu.setObjectName(QStringLiteral("PlaylistContextMenu"));
    const QList<int> rows = m_view->selectedVisibleRows();
    QListWidgetItem *item = m_view->itemAt(pos);
    if (item) {
        const int row = m_view->row(item);
        menu.addAction(skinIcon(IconType::Play), tr("Play"), this, [this, row] { Q_EMIT playRequested(row); });
    }
    // Download: the selection if the clicked entry is part of it, else that entry.
    QList<int> downloadRows = rows;
    if (item && !rows.contains(m_view->row(item)))
        downloadRows = {m_view->row(item)};
    if (!downloadableRows(downloadRows).isEmpty()) {
        QMenu *download = menu.addMenu(skinIcon(IconType::Download),
                                       downloadRows.size() > 1 ? tr("Download Selected") : tr("Download"));
        download->setObjectName(QStringLiteral("PlaylistContextDownloadMenu"));
        addDownloadActions(download, downloadRows);
    }
    QList<int> all;
    for (int row = 0; row < m_view->count(); ++row)
        all.append(row);
    if (const int online = int(downloadableRows(all).size()); online > 1) {
        QMenu *downloadAll = menu.addMenu(tr("Download All Online Entries (%1)").arg(online));
        downloadAll->setObjectName(QStringLiteral("PlaylistDownloadAllMenu"));
        addDownloadActions(downloadAll, all);
    }
    QAction *remove = menu.addAction(skinIcon(IconType::Remove), tr("Remove Selected"), this,
                                     [this, rows] { Q_EMIT removeRequested(rows); });
    remove->setShortcut(QKeySequence(Qt::Key_Delete));
    remove->setEnabled(!rows.isEmpty());
    menu.addSeparator();
    if (!rows.isEmpty()) {
        using PlaylistOps::Shift;
        menu.addAction(skinIcon(IconType::MoveTop), tr("Move to Top"), this, [this, rows] { Q_EMIT shiftRequested(rows, Shift::Top); });
        menu.addAction(skinIcon(IconType::MoveBottom), tr("Move to Bottom"), this, [this, rows] { Q_EMIT shiftRequested(rows, Shift::Bottom); });
    }
    menu.addSeparator();
    for (QAction *action : m_addMenu->actions())
        menu.addAction(action);
    menu.addSeparator();
    menu.addMenu(m_sortMenu)->setIcon(skinIcon(IconType::Sort));
    menu.addSeparator();
    // Cleanup, persistence and options are shared with the "More" button.
    for (QAction *action : m_moreMenu->actions())
        menu.addAction(action);
    syncOptions();
    menu.exec(m_view->viewport()->mapToGlobal(pos));
}

QList<int> PlaylistDrawer::downloadableRows(const QList<int> &rows) const
{
    QList<int> result;
    for (int row : rows) {
        if (QListWidgetItem *item = m_view->item(row); item && item->data(kDownloadableRole).toBool())
            result.append(row);
    }
    return result;
}

void PlaylistDrawer::addDownloadActions(QMenu *menu, const QList<int> &rows)
{
    using Format = MediaDownloader::Format;
    const QList<int> targets = downloadableRows(rows);
    const QList<QPair<QString, Format>> formats{
        {tr("Video (Best Quality, MP4)"), Format::Best},
        {tr("Video (1080p, MP4)"), Format::Max1080},
        {tr("Video (720p, MP4)"), Format::Max720},
        {tr("Audio Only (MP3)"), Format::AudioMp3},
    };
    for (const auto &[label, format] : formats) {
        QAction *action = menu->addAction(label, this, [this, targets, format = format] {
            Q_EMIT downloadRequested(targets, format);
        });
        action->setEnabled(!targets.isEmpty() && !MediaDownloader::executable().isEmpty());
        if (format == Format::AudioMp3)
            menu->insertSeparator(action);
    }
    if (MediaDownloader::executable().isEmpty()) {
        menu->addSeparator();
        menu->addAction(tr("Install yt-dlp to download"))->setEnabled(false);
    }
}

void PlaylistDrawer::setDownloadStatus(const QString &entry, const QString &status)
{
    if (status.isEmpty())
        m_downloadStatus.remove(entry);
    else
        m_downloadStatus.insert(entry, status);
    for (int row = 0; row < m_view->count(); ++row) {
        QListWidgetItem *item = m_view->item(row);
        if (item->data(kFilenameRole).toString() == entry && item->data(kDownloadRole).toString() != status)
            item->setData(kDownloadRole, status);
    }
}

bool PlaylistDrawer::eventFilter(QObject *watched, QEvent *event)
{
    if (watched == parentWidget() && event->type() == QEvent::Resize) {
        if (m_expanded && m_animation->state() != QAbstractAnimation::Running)
            setDrawerWidth(targetWidth());
        return QFrame::eventFilter(watched, event);
    }
    if (watched == m_view->viewport() && event->type() == QEvent::Resize && m_revealedId.isValid()) {
        // The window often resizes to the new video just after it starts:
        // keep the playing entry in view.
        const QVariant revealed = std::exchange(m_revealedId, QVariant());
        const int row = playingRow();
        if (row >= 0 && m_view->item(row)->data(kIdRole) != revealed)
            m_revealedId = revealed;
        else
            QTimer::singleShot(0, this, &PlaylistDrawer::revealPlaying);
        return QFrame::eventFilter(watched, event);
    }
    if (watched == m_filter && (event->type() == QEvent::ShortcutOverride || event->type() == QEvent::KeyPress)) {
        // Keys the search field uses itself, which the player would otherwise take as hotkeys.
        const auto *key = static_cast<QKeyEvent *>(event);
        const bool handled = key->key() == Qt::Key_Down || key->key() == Qt::Key_Escape
            || key->key() == Qt::Key_Return || key->key() == Qt::Key_Enter;
        if (!handled || key->modifiers() & ~Qt::KeypadModifier)
            return QFrame::eventFilter(watched, event);
        event->accept();
        if (event->type() == QEvent::ShortcutOverride)
            return true;
        if (key->key() == Qt::Key_Escape) {
            m_filter->clear();
            return true;
        }
        // Down or Return: continue in the list, at the first match.
        for (int row = 0; row < m_view->count(); ++row) {
            if (!m_view->isRowHidden(row)) {
                m_view->setCurrentRow(row);
                m_view->setFocus();
                break;
            }
        }
        return true;
    }
    return QFrame::eventFilter(watched, event);
}

bool PlaylistDrawer::updateEntriesInPlace(const QVariantList &playlist)
{
    if (playlist.size() != m_view->count())
        return false;
    for (int i = 0; i < playlist.size(); ++i) {
        const QVariantMap entry = playlist[i].toMap();
        const QListWidgetItem *item = m_view->item(i);
        if (item->data(kIdRole) != entry.value(QStringLiteral("id"))
            || item->data(kFilenameRole).toString() != entry.value(QStringLiteral("filename")).toString())
            return false;
    }
    // Same files in the same order (the usual change: the next entry starts
    // playing). Touch only what differs, keeping selection and scroll position.
    for (int i = 0; i < playlist.size(); ++i) {
        const QVariantMap entry = playlist[i].toMap();
        QListWidgetItem *item = m_view->item(i);
        const QString filename = entry.value(QStringLiteral("filename")).toString();
        const QString text = QStringLiteral("%1. %2").arg(i + 1).arg(
            PlaylistOps::displayName({filename, entry.value(QStringLiteral("title")).toString()}));
        if (item->text() != text)
            item->setText(text);
        markPlaying(item, entry.value(QStringLiteral("current")).toBool());
    }
    return true;
}

void PlaylistDrawer::markPlaying(QListWidgetItem *item, bool playing)
{
    if (item->data(kPlayingRole).toBool() == playing)
        return;
    QFont font = m_view->font();
    font.setBold(playing);
    item->setFont(font);
    item->setData(Qt::ForegroundRole, playing ? QVariant(kPlayingColor) : QVariant());
    item->setData(kPlayingRole, playing);
}

void PlaylistDrawer::setPlayingRow(int row)
{
    for (int i = 0; i < m_view->count(); ++i)
        markPlaying(m_view->item(i), i == row);
    revealPlaying();
}

int PlaylistDrawer::playingRow() const
{
    for (int i = 0; i < m_view->count(); ++i) {
        if (m_view->item(i)->data(kPlayingRole).toBool())
            return i;
    }
    return -1;
}

void PlaylistDrawer::revealPlaying()
{
    const int row = playingRow();
    if (row < 0) {
        m_revealedId = {};
        return;
    }
    QListWidgetItem *item = m_view->item(row);
    const QVariant id = item->data(kIdRole);
    // Once per new entry, and not while the user is looking through the list.
    if (id == m_revealedId || m_view->isRowHidden(row) || m_view->viewport()->underMouse())
        return;
    m_revealedId = id;
    // Centered, so the songs around it show too (and a scroll bar appearing
    // afterwards can't push it out again).
    if (!m_view->viewport()->rect().contains(m_view->visualItemRect(item)))
        m_view->scrollToItem(item, QAbstractItemView::PositionAtCenter);
}

void PlaylistDrawer::setEntries(const QVariantList &playlist, const QList<double> &durations)
{
    if (updateEntriesInPlace(playlist)) {
        setDurations(durations);
        applyFilter();
        revealPlaying();
        return;
    }
    // Entries keep their selection when the playlist is reordered (e.g. by
    // the move buttons), so they can be moved again right away.
    QSet<qlonglong> selectedIds;
    for (const QListWidgetItem *item : m_view->selectedItems()) {
        if (item->data(kIdRole).isValid())
            selectedIds.insert(item->data(kIdRole).toLongLong());
    }
    const QVariant currentId = m_view->currentItem() ? m_view->currentItem()->data(kIdRole) : QVariant();
    const int previousRow = m_view->currentRow();
    // Rebuild in one go: no repaints in between, and each item is complete
    // before it is added, so the view hears of it once.
    m_view->setUpdatesEnabled(false);
    m_view->clear();
    QFont playingFont = m_view->font();
    playingFont.setBold(true);
    for (int i = 0; i < playlist.size(); ++i) {
        const QVariantMap entry = playlist[i].toMap();
        const QString filename = entry.value(QStringLiteral("filename")).toString();
        const QString name = PlaylistOps::displayName({filename, entry.value(QStringLiteral("title")).toString()});

        auto *item = new QListWidgetItem(QStringLiteral("%1. %2").arg(i + 1).arg(name));
        item->setToolTip(filename);
        item->setData(kFilenameRole, filename);
        item->setData(kIdRole, entry.value(QStringLiteral("id")));
        if (DownloadQueue::canDownload(filename))
            item->setData(kDownloadableRole, true);
        if (const QString status = m_downloadStatus.value(filename); !status.isEmpty())
            item->setData(kDownloadRole, status);
        if (i < durations.size())
            item->setData(kDurationRole, durationText(durations[i]));
        if (entry.value(QStringLiteral("current")).toBool()) {
            item->setFont(playingFont);
            item->setForeground(kPlayingColor);
            item->setData(kPlayingRole, true);
        }
        m_view->addItem(item);
    }
    int currentRow = -1;
    if (!selectedIds.isEmpty() || currentId.isValid()) {
        for (int row = 0; row < m_view->count(); ++row) {
            QListWidgetItem *item = m_view->item(row);
            const QVariant id = item->data(kIdRole);
            if (id.isValid() && selectedIds.contains(id.toLongLong()))
                item->setSelected(true);
            if (id.isValid() && id == currentId)
                currentRow = row;
        }
    }
    if (currentRow < 0 && previousRow >= 0 && previousRow < m_view->count())
        currentRow = previousRow;
    if (currentRow >= 0) {
        m_view->setCurrentRow(currentRow, QItemSelectionModel::NoUpdate);
        if (!selectedIds.isEmpty())
            m_view->scrollToItem(m_view->item(currentRow));
    }
    applyFilter();
    m_view->setUpdatesEnabled(true);
    // A rebuilt list starts at the top: show the playing entry again, unless
    // the entries just moved by the user are being kept in view.
    if (selectedIds.isEmpty()) {
        m_revealedId = {};
        revealPlaying();
    }
}

void PlaylistDrawer::setDurations(const QList<double> &durations)
{
    for (int row = 0; row < m_view->count() && row < durations.size(); ++row) {
        QListWidgetItem *item = m_view->item(row);
        const QString text = durationText(durations[row]);
        if (item->data(kDurationRole).toString() != text)
            item->setData(kDurationRole, text);
    }
}

void PlaylistDrawer::setFilterText(const QString &text)
{
    m_filter->setText(text);
}

QString PlaylistDrawer::filterText() const
{
    return m_filter->text();
}

void PlaylistDrawer::applyFilter()
{
    const QStringList words = m_filter->text().split(QLatin1Char(' '), Qt::SkipEmptyParts);
    for (int row = 0; row < m_view->count(); ++row) {
        const QListWidgetItem *item = m_view->item(row);
        // The text starts with the row number, which is not part of the name.
        const QString name = item->text().section(QLatin1Char(' '), 1);
        const QString path = item->data(kFilenameRole).toString();
        const bool matches = std::all_of(words.cbegin(), words.cend(), [&](const QString &word) {
            return name.contains(word, Qt::CaseInsensitive) || path.contains(word, Qt::CaseInsensitive);
        });
        if (m_view->isRowHidden(row) == matches)
            m_view->setRowHidden(row, !matches);
    }
    updateCount();
}

void PlaylistDrawer::updateCount()
{
    const int total = m_view->count();
    if (m_filter->text().trimmed().isEmpty()) {
        m_countLabel->setText(QStringLiteral("(%1)").arg(total));
        return;
    }
    int shown = 0;
    for (int row = 0; row < total; ++row)
        shown += m_view->isRowHidden(row) ? 0 : 1;
    m_countLabel->setText(QStringLiteral("(%1/%2)").arg(shown).arg(total));
}

void PlaylistDrawer::setExpanded(bool expanded, bool animate)
{
    if (expanded == m_expanded)
        return;
    m_expanded = expanded;
    m_animation->stop();
    if (expanded)
        show();
    if (animate) {
        animateTo(expanded ? targetWidth() : 0);
    } else {
        setDrawerWidth(expanded ? targetWidth() : 0);
        setVisible(expanded);
    }
    Q_EMIT expandedChanged(expanded);
}

void PlaylistDrawer::animateTo(int width)
{
    m_animation->stop();
    m_animation->setStartValue(this->width());
    m_animation->setEndValue(width);
    m_animation->start();
}

PlaylistDrawer::Page PlaylistDrawer::currentPage() const
{
    return static_cast<Page>(m_tabs->currentIndex());
}

void PlaylistDrawer::setCurrentPage(Page page)
{
    m_tabs->setCurrentIndex(page);
}

int PlaylistDrawer::maximumDrawerWidth() const
{
    const int available = parentWidget() ? parentWidget()->width() - kMinimumVideoWidth : m_preferredWidth;
    return std::max(kMinimumWidth, available);
}

int PlaylistDrawer::targetWidth() const
{
    return m_wide ? maximumDrawerWidth() : std::min(m_preferredWidth, maximumDrawerWidth());
}

void PlaylistDrawer::setWide(bool wide, bool animate)
{
    if (wide == m_wide)
        return;
    m_wide = wide;
    m_wideButton->setIcon(skinIcon(wide ? IconType::Collapse : IconType::Expand));
    m_wideButton->setToolTip(wide ? tr("Restore Playlist Size") : tr("Expand Playlist"));
    if (m_expanded) {
        if (animate)
            animateTo(targetWidth());
        else
            setDrawerWidth(targetWidth());
    }
    Q_EMIT wideChanged(wide);
}

void PlaylistDrawer::setPreferredWidth(int width, bool save)
{
    // At most what fits beside the video now.
    m_preferredWidth = std::clamp(width, kMinimumWidth, std::max(kMinimumWidth, maximumDrawerWidth()));
    setWide(false, false);
    if (m_expanded) {
        m_animation->stop();
        setDrawerWidth(targetWidth());
    }
    if (save)
        PlaylistSession::setDrawerWidth(m_preferredWidth);
}

void PlaylistDrawer::setDrawerWidth(int width)
{
    setFixedWidth(width);
}
