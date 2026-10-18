#include "QueuePanel.h"
#include "FileShare.h"
#include "Icons.h"
#include "PlayQueue.h"
#include "PlaylistOps.h"
#include "Theme.h"

#include <QDragMoveEvent>
#include <QDropEvent>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QMenu>
#include <QPainter>
#include <QScrollBar>
#include <QSet>
#include <QStyledItemDelegate>
#include <QToolButton>
#include <QVBoxLayout>

#include <algorithm>

namespace {

enum class Kind { Header, Current, Queued, Upcoming, More };

constexpr int kKindRole = Qt::UserRole;
constexpr int kIdRole = Qt::UserRole + 1;
// The entry's place in the queue followed by the rest of the context, as
// PlayQueue::move() counts; -1 for the playing entry and the headers.
constexpr int kPlaceRole = Qt::UserRole + 2;
// A header: the place an entry dropped on it takes.
constexpr int kDropPlaceRole = Qt::UserRole + 3;
// Entries of a big playlist shown after the queue; the rest are counted.
constexpr int kMaxUpcoming = 500;
constexpr int kPlayingBarWidth = 3;

Kind kindOf(const QListWidgetItem *item)
{
    return static_cast<Kind>(item->data(kKindRole).toInt());
}

bool isEntry(const QListWidgetItem *item)
{
    return item && (kindOf(item) == Kind::Queued || kindOf(item) == Kind::Upcoming);
}

// Section headers in small capitals, the playing entry marked like the
// playlist's, the rest as the list draws them.
class QueueItemDelegate : public QStyledItemDelegate
{
public:
    using QStyledItemDelegate::QStyledItemDelegate;

    QSize sizeHint(const QStyleOptionViewItem &option, const QModelIndex &index) const override
    {
        QSize size = QStyledItemDelegate::sizeHint(option, index);
        if (static_cast<Kind>(index.data(kKindRole).toInt()) == Kind::Header)
            size.setHeight(size.height() + 6);
        return size;
    }

    void paint(QPainter *painter, const QStyleOptionViewItem &option, const QModelIndex &index) const override
    {
        const Kind kind = static_cast<Kind>(index.data(kKindRole).toInt());
        if (kind == Kind::Header || kind == Kind::More) {
            painter->save();
            QFont font = option.font;
            font.setBold(kind == Kind::Header);
            font.setCapitalization(kind == Kind::Header ? QFont::AllUppercase : QFont::MixedCase);
            font.setLetterSpacing(QFont::AbsoluteSpacing, kind == Kind::Header ? 0.8 : 0);
            font.setPointSizeF(font.pointSizeF() * 0.85);
            painter->setFont(font);
            painter->setPen(kind == Kind::Header ? Theme::TextSecondary : Theme::TextDim);
            const QRect text = option.rect.adjusted(10, kind == Kind::Header ? 6 : 0, -10, 0);
            painter->drawText(text, Qt::AlignLeft | Qt::AlignVCenter,
                              QFontMetrics(font).elidedText(index.data().toString(), Qt::ElideRight, text.width()));
            painter->restore();
            return;
        }
        QStyleOptionViewItem opt(option);
        initStyleOption(&opt, index);
        if (kind == Kind::Current) {
            opt.font.setBold(true);
            opt.palette.setColor(QPalette::Text, Theme::Accent);
            opt.palette.setColor(QPalette::HighlightedText, Theme::Accent);
        }
        QStyledItemDelegate::paint(painter, opt, index);
        if (kind == Kind::Current)
            painter->fillRect(QRect(opt.rect.left(), opt.rect.top() + 3, kPlayingBarWidth, opt.rect.height() - 6), Theme::Accent);
    }
};

QString itemName(const PlayQueue *queue, qint64 id)
{
    const std::optional<PlayQueue::Item> item = queue->item(id);
    return item ? PlaylistOps::displayName({item->filename, item->title}) : QString();
}

} // namespace

QueueView::QueueView(QWidget *parent)
    : QListWidget(parent)
{
    setObjectName(QStringLiteral("QueueView"));
    setSelectionMode(QAbstractItemView::ExtendedSelection);
    setDragDropMode(QAbstractItemView::InternalMove);
    setDefaultDropAction(Qt::MoveAction);
    setUniformItemSizes(false);
    setTextElideMode(Qt::ElideRight);
    setContextMenuPolicy(Qt::CustomContextMenu);
}

QList<int> QueueView::selectedPlaces() const
{
    QList<int> places;
    for (const QListWidgetItem *item : selectedItems()) {
        if (isEntry(item))
            places.append(item->data(kPlaceRole).toInt());
    }
    std::sort(places.begin(), places.end());
    return places;
}

int QueueView::dropPlace(const QPoint &pos) const
{
    const QListWidgetItem *item = itemAt(pos);
    if (!item) {
        // Below the last entry: the end.
        int last = 0;
        for (int row = 0; row < count(); ++row) {
            if (isEntry(this->item(row)))
                last = this->item(row)->data(kPlaceRole).toInt() + 1;
        }
        return last;
    }
    if (isEntry(item)) {
        const QRect rect = visualItemRect(item);
        return item->data(kPlaceRole).toInt() + (pos.y() >= rect.center().y() ? 1 : 0);
    }
    return item->data(kDropPlaceRole).isValid() ? item->data(kDropPlaceRole).toInt() : -1;
}

void QueueView::dragMoveEvent(QDragMoveEvent *event)
{
    QListWidget::dragMoveEvent(event);
    if (event->source() != this || dropPlace(event->position().toPoint()) < 0)
        event->ignore();
}

void QueueView::dropEvent(QDropEvent *event)
{
    const int place = dropPlace(event->position().toPoint());
    if (event->source() == this && place >= 0) {
        // One by one, keeping their order (as the playlist does).
        int target = place;
        int shift = 0;
        for (int from : selectedPlaces()) {
            const int current = from < target ? from - shift : from;
            Q_EMIT moveRequested(current, target);
            if (from < target)
                ++shift;
            else
                ++target;
        }
    }
    // The list is rebuilt from the queue.
    event->setDropAction(Qt::IgnoreAction);
    event->accept();
}

void QueueView::keyPressEvent(QKeyEvent *event)
{
    if (event->key() == Qt::Key_Delete) {
        Q_EMIT removeRequested();
        event->accept();
        return;
    }
    if (event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter) {
        if (isEntry(currentItem()))
            Q_EMIT itemActivated(currentItem());
        event->accept();
        return;
    }
    QListWidget::keyPressEvent(event);
}

QueuePanel::QueuePanel(QWidget *parent)
    : QWidget(parent)
    , m_view(new QueueView(this))
    , m_source(new QLabel(this))
    , m_resetButton(new QToolButton(this))
    , m_empty(new QLabel(this))
{
    setObjectName(QStringLiteral("QueuePanel"));
    m_view->setItemDelegate(new QueueItemDelegate(m_view));

    // Where the songs come from, and the way back to the whole playlist.
    auto *sourceBar = new QFrame(this);
    sourceBar->setObjectName(QStringLiteral("QueueSourceBar"));
    auto *sourceLayout = new QHBoxLayout(sourceBar);
    sourceLayout->setContentsMargins(10, 6, 6, 6);
    sourceLayout->setSpacing(4);
    m_source->setObjectName(QStringLiteral("QueueSource"));
    m_source->setTextFormat(Qt::PlainText);
    m_resetButton->setObjectName(QStringLiteral("QueueResetButton"));
    m_resetButton->setIcon(skinIcon(IconType::Clear));
    m_resetButton->setIconSize(QSize(14, 14));
    m_resetButton->setToolTip(tr("Play the whole playlist instead of the search results"));
    m_resetButton->setAutoRaise(true);
    m_resetButton->setFocusPolicy(Qt::NoFocus);
    sourceLayout->addWidget(m_source, 1);
    sourceLayout->addWidget(m_resetButton);

    m_empty->setObjectName(QStringLiteral("QueueEmpty"));
    m_empty->setAlignment(Qt::AlignCenter);
    m_empty->setWordWrap(true);
    m_empty->setText(tr("Nothing is playing.\nOpen a song in the playlist, or a search result, and what plays next lines up here."));

    auto *bar = new QFrame(this);
    bar->setObjectName(QStringLiteral("QueueActionBar"));
    auto *barLayout = new QHBoxLayout(bar);
    barLayout->setContentsMargins(6, 5, 6, 6);
    barLayout->setSpacing(2);
    const auto iconButton = [bar](IconType icon, const QString &toolTip, const char *name) {
        auto *button = new QToolButton(bar);
        button->setObjectName(QString::fromLatin1(name));
        button->setProperty("barButton", true);
        button->setProperty("barIcon", true);
        button->setIcon(skinIcon(icon));
        button->setIconSize(QSize(12, 12));
        button->setToolTip(toolTip);
        button->setFocusPolicy(Qt::NoFocus);
        button->setAutoRepeat(true);
        return button;
    };
    m_upButton = iconButton(IconType::MoveUp, tr("Move Up"), "QueueMoveUpButton");
    m_downButton = iconButton(IconType::MoveDown, tr("Move Down"), "QueueMoveDownButton");
    m_nextButton = Theme::barButton(bar, tr("NEXT"), tr("Play the selected songs next"), "QueuePlayNextButton");
    m_removeButton = Theme::barButton(bar, tr("DEL"), tr("Take the selected songs out of the queue (Del)"), "QueueRemoveButton");
    m_clearButton = Theme::barButton(bar, tr("CLEAR"), tr("Clear the songs queued to play next"), "QueueClearButton");
    barLayout->addWidget(m_upButton);
    barLayout->addWidget(m_downButton);
    barLayout->addStretch();
    barLayout->addWidget(m_nextButton);
    barLayout->addWidget(m_removeButton);
    barLayout->addWidget(m_clearButton);

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    layout->addWidget(sourceBar);
    layout->addWidget(m_view, 1);
    layout->addWidget(m_empty, 1);
    layout->addWidget(bar);

    m_rebuildTimer.setSingleShot(true);
    m_rebuildTimer.setInterval(0);
    connect(&m_rebuildTimer, &QTimer::timeout, this, &QueuePanel::rebuild);

    connect(m_view, &QListWidget::itemActivated, this, [this](QListWidgetItem *item) {
        if (isEntry(item))
            Q_EMIT playRequested(item->data(kIdRole).toLongLong());
    });
    connect(m_view, &QueueView::moveRequested, this, [this](int from, int to) {
        if (m_queue)
            m_queue->move(from, to);
    });
    connect(m_view, &QueueView::removeRequested, this, [this] {
        if (m_queue)
            m_queue->remove(selectedIds());
    });
    connect(m_view, &QWidget::customContextMenuRequested, this, &QueuePanel::showContextMenu);
    connect(m_view, &QListWidget::itemSelectionChanged, this, &QueuePanel::updateActions);
    connect(m_resetButton, &QToolButton::clicked, this, [this] {
        if (m_queue)
            m_queue->resetToPlaylist();
    });
    connect(m_upButton, &QToolButton::clicked, this, [this] { shiftSelection(-1); });
    connect(m_downButton, &QToolButton::clicked, this, [this] { shiftSelection(1); });
    connect(m_nextButton, &QToolButton::clicked, this, [this] {
        if (m_queue)
            m_queue->playNext(selectedIds());
    });
    connect(m_removeButton, &QToolButton::clicked, this, [this] {
        if (m_queue)
            m_queue->remove(selectedIds());
    });
    connect(m_clearButton, &QToolButton::clicked, this, [this] {
        if (m_queue)
            m_queue->clearQueued();
    });
    rebuild();
}

void QueuePanel::setQueue(PlayQueue *queue)
{
    if (m_queue)
        m_queue->disconnect(this);
    m_queue = queue;
    if (m_queue)
        connect(m_queue, &PlayQueue::changed, this, &QueuePanel::scheduleRebuild);
    scheduleRebuild();
}

void QueuePanel::showEvent(QShowEvent *event)
{
    QWidget::showEvent(event);
    if (m_dirty)
        rebuild();
}

void QueuePanel::scheduleRebuild()
{
    m_dirty = true;
    // A hidden page catches up when it shows.
    if (isVisible() && !m_rebuildTimer.isActive())
        m_rebuildTimer.start();
}

QList<qint64> QueuePanel::selectedIds() const
{
    QList<qint64> ids;
    for (int row = 0; row < m_view->count(); ++row) {
        const QListWidgetItem *item = m_view->item(row);
        if (item->isSelected() && isEntry(item))
            ids.append(item->data(kIdRole).toLongLong());
    }
    return ids;
}

void QueuePanel::rebuild()
{
    m_rebuildTimer.stop();
    m_dirty = false;
    const qint64 current = m_queue ? m_queue->current() : -1;
    const QList<qint64> queued = m_queue ? m_queue->queued() : QList<qint64>();
    const QList<qint64> upcoming = m_queue ? m_queue->upcoming() : QList<qint64>();

    const bool search = m_queue && m_queue->source() == PlayQueue::Source::Search;
    const QString from = search ? tr("Search “%1”").arg(m_queue->query()) : tr("Playlist");
    m_source->setText(m_queue && m_queue->contextSize() > 0
                          ? tr("Playing from %1 · %n song(s)", nullptr, m_queue->contextSize()).arg(from)
                          : tr("Playing from %1").arg(from));
    m_resetButton->setVisible(search);

    const bool empty = current < 0 && queued.isEmpty() && upcoming.isEmpty();
    m_view->setVisible(!empty);
    m_empty->setVisible(empty);

    // Kept across the rebuild: the selection and the scroll position.
    QSet<qint64> selected;
    for (qint64 id : selectedIds())
        selected.insert(id);
    const int scroll = m_view->verticalScrollBar() ? m_view->verticalScrollBar()->value() : 0;

    m_view->setUpdatesEnabled(false);
    m_view->clear();
    const auto addHeader = [this](const QString &text, int dropPlace) {
        auto *item = new QListWidgetItem(text);
        item->setData(kKindRole, int(Kind::Header));
        item->setData(kPlaceRole, -1);
        if (dropPlace >= 0)
            item->setData(kDropPlaceRole, dropPlace);
        item->setFlags(Qt::ItemIsEnabled | Qt::ItemIsDropEnabled);
        m_view->addItem(item);
    };
    const auto addEntry = [this, &selected](qint64 id, Kind kind, int place) {
        auto *item = new QListWidgetItem(itemName(m_queue, id));
        item->setData(kKindRole, int(kind));
        item->setData(kIdRole, id);
        item->setData(kPlaceRole, place);
        if (kind == Kind::Current) {
            item->setFlags(Qt::ItemIsEnabled);
        } else {
            item->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable | Qt::ItemIsDragEnabled);
            item->setToolTip(m_queue->item(id) ? m_queue->item(id)->filename : QString());
        }
        m_view->addItem(item);
        if (selected.contains(id))
            item->setSelected(true);
    };

    if (current >= 0) {
        addHeader(tr("Now Playing"), 0);
        addEntry(current, Kind::Current, -1);
    }
    int place = 0;
    if (!queued.isEmpty()) {
        addHeader(tr("Next in Queue"), 0);
        for (qint64 id : queued)
            addEntry(id, Kind::Queued, place++);
    }
    if (!upcoming.isEmpty()) {
        addHeader(tr("Next from %1").arg(from), int(queued.size()));
        const int shown = std::min(int(upcoming.size()), kMaxUpcoming);
        for (int i = 0; i < shown; ++i)
            addEntry(upcoming[i], Kind::Upcoming, place++);
        if (upcoming.size() > shown) {
            auto *more = new QListWidgetItem(tr("and %n more", nullptr, int(upcoming.size()) - shown));
            more->setData(kKindRole, int(Kind::More));
            more->setData(kDropPlaceRole, place);
            more->setFlags(Qt::ItemIsEnabled | Qt::ItemIsDropEnabled);
            m_view->addItem(more);
        }
    }
    m_view->setUpdatesEnabled(true);
    m_view->verticalScrollBar()->setValue(scroll);
    updateActions();
}

void QueuePanel::updateActions()
{
    const bool selection = !selectedIds().isEmpty();
    m_upButton->setEnabled(selection);
    m_downButton->setEnabled(selection);
    m_nextButton->setEnabled(selection);
    m_removeButton->setEnabled(selection);
    m_clearButton->setEnabled(m_queue && !m_queue->queued().isEmpty());
}

void QueuePanel::shiftSelection(int delta)
{
    if (!m_queue)
        return;
    QList<int> places = m_view->selectedPlaces();
    if (places.isEmpty())
        return;
    const int total = int(m_queue->queued().size() + m_queue->upcoming().size());
    // Entries already at the edge stay; the others keep their order.
    if (delta < 0) {
        int edge = 0;
        for (int place : std::as_const(places)) {
            if (place == edge)
                ++edge;
            else
                m_queue->move(place, place - 1);
        }
    } else {
        int edge = total - 1;
        std::reverse(places.begin(), places.end());
        for (int place : std::as_const(places)) {
            if (place == edge)
                --edge;
            else
                m_queue->move(place, place + 2);
        }
    }
}

void QueuePanel::showContextMenu(const QPoint &pos)
{
    if (!m_queue)
        return;
    QListWidgetItem *item = m_view->itemAt(pos);
    if (isEntry(item) && !item->isSelected()) {
        m_view->clearSelection();
        item->setSelected(true);
    }
    const QList<qint64> ids = selectedIds();
    QMenu menu(this);
    menu.setObjectName(QStringLiteral("QueueContextMenu"));
    if (isEntry(item)) {
        const qint64 id = item->data(kIdRole).toLongLong();
        menu.addAction(skinIcon(IconType::Play), tr("Play Now"), this, [this, id] { Q_EMIT playRequested(id); });
    }
    if (!ids.isEmpty()) {
        menu.addAction(tr("Play Next"), this, [this, ids] { m_queue->playNext(ids); });
        menu.addAction(skinIcon(IconType::MoveUp), tr("Move Up"), this, [this] { shiftSelection(-1); });
        menu.addAction(skinIcon(IconType::MoveDown), tr("Move Down"), this, [this] { shiftSelection(1); });
        QAction *remove = menu.addAction(skinIcon(IconType::Remove), tr("Remove from Queue"), this,
                                         [this, ids] { m_queue->remove(ids); });
        remove->setShortcut(QKeySequence(Qt::Key_Delete));
    }
    menu.addSeparator();
    // Show in File Manager and Share: the playing entry too.
    QStringList entries, titles;
    const QList<qint64> shared = ids.isEmpty() && item && kindOf(item) == Kind::Current
                                     ? QList<qint64>{item->data(kIdRole).toLongLong()}
                                     : ids;
    for (qint64 id : shared) {
        if (const std::optional<PlayQueue::Item> entry = m_queue->item(id)) {
            entries.append(entry->filename);
            titles.append(itemName(m_queue, id));
        }
    }
    const int before = int(menu.actions().size());
    FileShare::addActions(&menu, entries, titles,
                          [this](const QString &label, const QString &value) { Q_EMIT message(label, value); });
    if (menu.actions().size() > before)
        menu.addSeparator();
    QAction *clear = menu.addAction(skinIcon(IconType::Clear), tr("Clear Queued Songs"), m_queue, &PlayQueue::clearQueued);
    clear->setEnabled(!m_queue->queued().isEmpty());
    if (m_queue->source() == PlayQueue::Source::Search)
        menu.addAction(tr("Play Whole Playlist Instead"), m_queue, &PlayQueue::resetToPlaylist);
    menu.exec(m_view->viewport()->mapToGlobal(pos));
}
