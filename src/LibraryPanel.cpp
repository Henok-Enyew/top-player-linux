#include "LibraryPanel.h"
#include "FileShare.h"
#include "Icons.h"
#include "MediaFiles.h"
#include "PlaylistOps.h"
#include "Theme.h"

#include <QDir>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QFrame>
#include <QHeaderView>
#include <QInputDialog>
#include <QKeyEvent>
#include <QMenu>
#include <QMessageBox>
#include <QMimeData>
#include <QMouseEvent>
#include <QToolButton>
#include <QTreeWidgetItemIterator>
#include <QUrl>
#include <QVBoxLayout>

#include <algorithm>

namespace {

// Item data. Section headers and placeholders have no type.
constexpr int kTypeRole = Qt::UserRole;
constexpr int kPathRole = Qt::UserRole + 1;
// Set once a folder's or playlist's children have been listed.
constexpr int kLoadedRole = Qt::UserRole + 2;
// Set on the folders and playlists stored in the library (not their contents).
constexpr int kStoredRole = Qt::UserRole + 3;
// Set on the rows that run an action when clicked (the hints in an empty section).
constexpr int kActionRole = Qt::UserRole + 4;
enum class RowAction { AddFolder = 1, SaveQueue };
// The theme color a row's text takes, so it can follow appearance changes.
constexpr int kToneRole = Qt::UserRole + 5;
enum class Tone { Accent = 1, Muted };
// Section headers remember whether they were folded under these keys.
const QString kFoldersKey = QStringLiteral("section:folders");
const QString kPlaylistsKey = QStringLiteral("section:playlists");

const QColor kMissingColor(0x8A, 0x5A, 0x5A);

using EntryType = LibraryPanel::EntryType;

void setTone(QTreeWidgetItem *item, Tone tone)
{
    item->setData(0, kToneRole, static_cast<int>(tone));
    item->setForeground(0, tone == Tone::Accent ? Theme::Accent : Theme::mix(Theme::TextSecondary, Theme::TextDim, 0.4));
}

bool hasType(const QTreeWidgetItem *item)
{
    return item && item->data(0, kTypeRole).isValid();
}

EntryType typeOf(const QTreeWidgetItem *item)
{
    return static_cast<EntryType>(item->data(0, kTypeRole).toInt());
}

QString pathOf(const QTreeWidgetItem *item)
{
    return item->data(0, kPathRole).toString();
}

QString itemKey(const QTreeWidgetItem *item)
{
    return QString::number(static_cast<int>(typeOf(item))) + QLatin1Char(':') + pathOf(item);
}

MediaLibrary::Kind kindOf(EntryType type)
{
    return type == EntryType::Folder ? MediaLibrary::Kind::Folder : MediaLibrary::Kind::Playlist;
}

// A hint row that does something when clicked, e.g. "Add a folder...".
QTreeWidgetItem *addActionRow(QTreeWidgetItem *parent, const QString &text, RowAction action)
{
    auto *item = new QTreeWidgetItem(parent, {text});
    item->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable);
    item->setData(0, kActionRole, static_cast<int>(action));
    item->setIcon(0, skinIcon(IconType::Add));
    setTone(item, Tone::Accent);
    item->setToolTip(0, text);
    return item;
}

QTreeWidgetItem *addPlaceholder(QTreeWidgetItem *parent, const QString &text)
{
    auto *item = new QTreeWidgetItem(parent, {text});
    item->setFlags(Qt::ItemIsEnabled);
    setTone(item, Tone::Muted);
    QFont font = item->font(0);
    font.setItalic(true);
    item->setFont(0, font);
    return item;
}

QTreeWidgetItem *addEntry(QTreeWidgetItem *parent, EntryType type, const QString &path, const QString &name)
{
    auto *item = new QTreeWidgetItem(parent, {name});
    item->setData(0, kTypeRole, static_cast<int>(type));
    item->setData(0, kPathRole, path);
    item->setToolTip(0, path);
    item->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable | Qt::ItemIsDragEnabled);
    switch (type) {
    case EntryType::Folder:
        item->setIcon(0, skinIcon(IconType::Folder));
        item->setChildIndicatorPolicy(QTreeWidgetItem::ShowIndicator);
        break;
    case EntryType::Playlist:
        item->setIcon(0, skinIcon(IconType::Playlist));
        item->setChildIndicatorPolicy(QTreeWidgetItem::ShowIndicator);
        break;
    case EntryType::File:
        break;
    }
    const QString local = MediaFiles::localPath(path);
    if (!local.isEmpty() && !QFileInfo::exists(local)) {
        item->setForeground(0, kMissingColor);
        item->setToolTip(0, QObject::tr("%1 (not found)").arg(path));
    }
    return item;
}

QFileInfoList naturallySorted(QFileInfoList list)
{
    std::sort(list.begin(), list.end(), [](const QFileInfo &a, const QFileInfo &b) {
        return MediaFiles::naturalLess(a.fileName(), b.fileName());
    });
    return list;
}

} // namespace

LibraryView::LibraryView(QWidget *parent)
    : QTreeWidget(parent)
{
    setObjectName(QStringLiteral("LibraryView"));
    setHeaderHidden(true);
    setColumnCount(1);
    setRootIsDecorated(true);
    setIndentation(14);
    setUniformRowHeights(true);
    setTextElideMode(Qt::ElideMiddle);
    setSelectionMode(QAbstractItemView::SingleSelection);
    setDragDropMode(QAbstractItemView::DragOnly);
    setDragEnabled(true);
    // Double-click plays; the arrows (or Left/Right) open folders and playlists.
    setExpandsOnDoubleClick(false);
    setIconSize(QSize(16, 16));
    header()->setStretchLastSection(true);
}

QMimeData *LibraryView::mimeData(const QList<QTreeWidgetItem *> &items) const
{
    QList<QUrl> urls;
    for (const QTreeWidgetItem *item : items) {
        if (!hasType(item))
            continue;
        const QString path = pathOf(item);
        const QString local = MediaFiles::localPath(path);
        urls.append(local.isEmpty() ? QUrl(path) : QUrl::fromLocalFile(local));
    }
    if (urls.isEmpty())
        return nullptr;
    auto *mime = new QMimeData;
    mime->setUrls(urls);
    return mime;
}

void LibraryView::mousePressEvent(QMouseEvent *event)
{
    // Left of the item's rectangle is the tree's arrow, which folds the row itself.
    const QModelIndex index = indexAt(event->position().toPoint());
    m_pressedOnBranch = index.isValid() && event->position().x() < visualRect(index).left();
    QTreeWidget::mousePressEvent(event);
}

void LibraryView::keyPressEvent(QKeyEvent *event)
{
    if (event->key() == Qt::Key_Delete) {
        if (currentItem())
            Q_EMIT removeRequested(currentItem());
        event->accept();
        return;
    }
    if (event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter) {
        if (currentItem())
            Q_EMIT itemActivated(currentItem(), 0);
        event->accept();
        return;
    }
    QTreeWidget::keyPressEvent(event);
}

LibraryPanel::LibraryPanel(QWidget *parent)
    : QWidget(parent)
    , m_view(new LibraryView(this))
{
    setObjectName(QStringLiteral("LibraryPanel"));

    // The bar along the bottom, like the playlist's.
    auto *bar = new QFrame(this);
    bar->setObjectName(QStringLiteral("LibraryActionBar"));
    QToolButton *addFolder = Theme::barButton(bar, tr("+ FOLDER"), tr("Add Folder to Library..."), "LibraryAddFolderButton");
    QToolButton *addPlaylist = Theme::barButton(bar, tr("+ LIST"), tr("Add Playlist File to Library..."), "LibraryAddPlaylistButton");
    QToolButton *saveQueue = Theme::barButton(bar, tr("SAVE"), tr("Save Current Playlist to Library..."), "LibrarySaveButton");
    m_removeButton = Theme::barButton(bar, tr("DEL"), tr("Remove from Library (Del)"), "LibraryRemoveButton");

    auto *tools = new QHBoxLayout(bar);
    tools->setContentsMargins(6, 5, 6, 6);
    tools->setSpacing(3);
    for (QToolButton *button : {addFolder, addPlaylist, saveQueue})
        tools->addWidget(button);
    tools->addStretch();
    tools->addWidget(m_removeButton);

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    layout->addWidget(m_view, 1);
    layout->addWidget(bar);

    m_view->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(m_view, &QWidget::customContextMenuRequested, this, &LibraryPanel::showContextMenu);
    connect(m_view, &QTreeWidget::itemExpanded, this, &LibraryPanel::populate);
    connect(m_view, &QTreeWidget::itemExpanded, this, [this](QTreeWidgetItem *item) {
        if (hasType(item))
            m_expandedKeys.insert(itemKey(item));
        else if (item == m_folders || item == m_playlists)
            m_collapsedSections.remove(item == m_folders ? kFoldersKey : kPlaylistsKey);
    });
    connect(m_view, &QTreeWidget::itemCollapsed, this, [this](QTreeWidgetItem *item) {
        if (hasType(item))
            m_expandedKeys.remove(itemKey(item));
        else if (item == m_folders || item == m_playlists)
            m_collapsedSections.insert(item == m_folders ? kFoldersKey : kPlaylistsKey);
    });
    // A click opens or folds a section, folder or playlist (as in PotPlayer),
    // and runs a hint row's action; a double click (or Enter) plays.
    connect(m_view, &QTreeWidget::itemClicked, this, &LibraryPanel::onItemClicked);
    connect(m_view, &QTreeWidget::itemActivated, this, [this](QTreeWidgetItem *item) {
        if (item && item->data(0, kActionRole).isValid()) {
            runRowAction(item);
            return;
        }
        if (!hasType(item))
            return;
        // An entry of a playlist plays the playlist from that entry.
        QTreeWidgetItem *parent = item->parent();
        if (typeOf(item) == EntryType::File && hasType(parent) && typeOf(parent) == EntryType::Playlist)
            Q_EMIT playRequested(EntryType::Playlist, pathOf(parent), parent->indexOfChild(item));
        else
            Q_EMIT playRequested(typeOf(item), pathOf(item));
    });
    connect(m_view, &LibraryView::removeRequested, this, &LibraryPanel::removeItem);
    connect(m_view, &QTreeWidget::currentItemChanged, this, &LibraryPanel::updateButtons);
    connect(addFolder, &QToolButton::clicked, this, &LibraryPanel::addFolderRequested);
    connect(addPlaylist, &QToolButton::clicked, this, &LibraryPanel::addPlaylistFileRequested);
    connect(saveQueue, &QToolButton::clicked, this, &LibraryPanel::saveQueueRequested);
    connect(m_removeButton, &QToolButton::clicked, this, [this] { removeItem(m_view->currentItem()); });
    connect(Theme::notifier(), &Theme::Notifier::changed, this, [this] {
        for (QTreeWidgetItemIterator it(m_view); *it; ++it) {
            if (const int tone = (*it)->data(0, kToneRole).toInt())
                setTone(*it, static_cast<Tone>(tone));
        }
    });

    rebuild();
}

void LibraryPanel::setLibrary(MediaLibrary *library)
{
    if (m_library)
        disconnect(m_library, nullptr, this, nullptr);
    m_library = library;
    if (m_library)
        connect(m_library, &MediaLibrary::changed, this, &LibraryPanel::rebuild);
    rebuild();
}

void LibraryPanel::rebuild()
{
    const QString current = m_view->currentItem() && hasType(m_view->currentItem()) ? itemKey(m_view->currentItem()) : QString();
    m_view->clear();

    // Section headers: clickable, foldable rows with an icon and a count.
    auto addSection = [this](const QString &title, IconType icon) {
        auto *section = new QTreeWidgetItem(m_view, {title});
        section->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable);
        section->setIcon(0, skinIcon(icon));
        QFont font = section->font(0);
        font.setBold(true);
        section->setFont(0, font);
        section->setChildIndicatorPolicy(QTreeWidgetItem::ShowIndicator);
        section->setToolTip(0, tr("Click to show or hide"));
        return section;
    };
    const QList<MediaLibrary::Item> folders = m_library ? m_library->items(MediaLibrary::Kind::Folder) : QList<MediaLibrary::Item>();
    const QList<MediaLibrary::Item> playlists = m_library ? m_library->items(MediaLibrary::Kind::Playlist) : QList<MediaLibrary::Item>();
    m_folders = addSection(tr("Folders (%1)").arg(folders.size()), IconType::Folder);
    m_playlists = addSection(tr("Playlists (%1)").arg(playlists.size()), IconType::Playlist);

    for (const MediaLibrary::Item &folder : folders)
        addEntry(m_folders, EntryType::Folder, folder.path, folder.name)->setData(0, kStoredRole, true);
    for (const MediaLibrary::Item &playlist : playlists)
        addEntry(m_playlists, EntryType::Playlist, playlist.path, playlist.name)->setData(0, kStoredRole, true);
    if (folders.isEmpty())
        addActionRow(m_folders, tr("Add a folder..."), RowAction::AddFolder);
    if (playlists.isEmpty())
        addActionRow(m_playlists, tr("Save the current playlist..."), RowAction::SaveQueue);

    // Expanding lists the children, which re-expand in turn.
    for (QTreeWidgetItem *section : {m_folders, m_playlists}) {
        section->setExpanded(!m_collapsedSections.contains(section == m_folders ? kFoldersKey : kPlaylistsKey));
        for (int i = 0; i < section->childCount(); ++i) {
            QTreeWidgetItem *item = section->child(i);
            if (hasType(item) && m_expandedKeys.contains(itemKey(item)))
                item->setExpanded(true);
        }
    }
    if (!current.isEmpty()) {
        for (QTreeWidgetItemIterator it(m_view); *it; ++it) {
            if (hasType(*it) && itemKey(*it) == current) {
                m_view->setCurrentItem(*it);
                break;
            }
        }
    }
    updateButtons();
}

void LibraryPanel::populate(QTreeWidgetItem *item)
{
    if (!hasType(item) || item->data(0, kLoadedRole).toBool())
        return;
    item->setData(0, kLoadedRole, true);
    const QString path = pathOf(item);

    if (typeOf(item) == EntryType::Folder) {
        const QDir dir(path);
        if (!dir.exists()) {
            addPlaceholder(item, tr("Folder not found"));
            return;
        }
        for (const QFileInfo &info : naturallySorted(dir.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot)))
            addEntry(item, EntryType::Folder, info.absoluteFilePath(), info.fileName());
        for (const QFileInfo &info : naturallySorted(dir.entryInfoList(QDir::Files))) {
            const QString file = info.absoluteFilePath();
            if (MediaFiles::isPlaylistFile(file))
                addEntry(item, EntryType::Playlist, file, info.fileName());
            else if (MediaFiles::isMediaFile(file))
                addEntry(item, EntryType::File, file, info.fileName());
        }
        if (item->childCount() == 0)
            addPlaceholder(item, tr("No media files"));
    } else if (typeOf(item) == EntryType::Playlist) {
        const QStringList entries = PlaylistOps::readPlaylist(path);
        for (const QString &entry : entries) {
            const QString local = MediaFiles::localPath(entry);
            addEntry(item, EntryType::File, entry, local.isEmpty() ? entry : QFileInfo(local).fileName());
        }
        if (entries.isEmpty())
            addPlaceholder(item, QFileInfo::exists(path) ? tr("Empty playlist") : tr("Playlist not found"));
    }

    // Subfolders that were open before the tree was rebuilt.
    for (int i = 0; i < item->childCount(); ++i) {
        QTreeWidgetItem *child = item->child(i);
        if (hasType(child) && typeOf(child) != EntryType::File && m_expandedKeys.contains(itemKey(child)))
            child->setExpanded(true);
    }
}

void LibraryPanel::onItemClicked(QTreeWidgetItem *item)
{
    if (!item)
        return;
    if (item->data(0, kActionRole).isValid()) {
        runRowAction(item);
        return;
    }
    const bool foldable = item == m_folders || item == m_playlists
        || (hasType(item) && typeOf(item) != EntryType::File);
    if (!foldable)
        return;
    // A click on the arrow already toggled the row.
    if (m_view->pressedOnBranch())
        return;
    if (!item->isExpanded())
        populate(item);
    item->setExpanded(!item->isExpanded());
}

void LibraryPanel::runRowAction(QTreeWidgetItem *item)
{
    switch (static_cast<RowAction>(item->data(0, kActionRole).toInt())) {
    case RowAction::AddFolder:
        Q_EMIT addFolderRequested();
        break;
    case RowAction::SaveQueue:
        Q_EMIT saveQueueRequested();
        break;
    }
}

void LibraryPanel::expandItem(QTreeWidgetItem *item)
{
    populate(item);
    item->setExpanded(true);
}

QTreeWidgetItem *LibraryPanel::findItem(EntryType type, const QString &path) const
{
    for (QTreeWidgetItemIterator it(m_view); *it; ++it) {
        if (hasType(*it) && typeOf(*it) == type && pathOf(*it) == path)
            return *it;
    }
    return nullptr;
}

void LibraryPanel::showContextMenu(const QPoint &pos)
{
    QMenu menu(this);
    menu.setObjectName(QStringLiteral("LibraryContextMenu"));
    QTreeWidgetItem *item = m_view->itemAt(pos);
    if (hasType(item)) {
        m_view->setCurrentItem(item);
        const EntryType type = typeOf(item);
        const QString path = pathOf(item);
        menu.addAction(skinIcon(IconType::Play), tr("Play"), this, [this, item] { Q_EMIT m_view->itemActivated(item, 0); });
        menu.addAction(skinIcon(IconType::Add), tr("Add to Playlist"), this,
                       [this, type, path] { Q_EMIT queueRequested(type, path); });
        menu.addSeparator();
        if (type == EntryType::File) {
            FileShare::addActions(&menu, {path}, {item->text(0)},
                                  [this](const QString &label, const QString &value) { Q_EMIT message(label, value); });
        } else {
            menu.addAction(skinIcon(IconType::Folder), tr("Show in File Manager"), this,
                           [path] { FileShare::showInFileManager({path}); });
        }
        if (type == EntryType::Folder) {
            menu.addAction(tr("Refresh"), this, [this, item] {
                qDeleteAll(item->takeChildren());
                item->setData(0, kLoadedRole, false);
                if (item->isExpanded())
                    populate(item);
            });
        }
        if (item->data(0, kStoredRole).toBool()) {
            menu.addSeparator();
            const QString suffix = QFileInfo(path).suffix().toLower();
            if (type == EntryType::Playlist && (suffix == QLatin1String("m3u") || suffix == QLatin1String("m3u8"))) {
                menu.addAction(tr("Replace with Current Playlist"), this, [this, item, path] {
                    Q_EMIT overwritePlaylistRequested(path);
                    qDeleteAll(item->takeChildren());
                    item->setData(0, kLoadedRole, false);
                    if (item->isExpanded())
                        populate(item);
                });
            }
            menu.addAction(tr("Rename..."), this, [this, item] { renameItem(item); });
            const bool owned = type == EntryType::Playlist && MediaLibrary::ownsPlaylist(path);
            menu.addAction(skinIcon(IconType::Remove), owned ? tr("Delete Playlist") : tr("Remove from Library"), this,
                           [this, item] { removeItem(item); });
        }
        menu.addSeparator();
    }
    menu.addAction(skinIcon(IconType::Folder), tr("Add Folder to Library..."), this, &LibraryPanel::addFolderRequested);
    menu.addAction(skinIcon(IconType::Open), tr("Add Playlist File to Library..."), this, &LibraryPanel::addPlaylistFileRequested);
    menu.addAction(skinIcon(IconType::Add), tr("Save Current Playlist to Library..."), this, &LibraryPanel::saveQueueRequested);
    menu.exec(m_view->viewport()->mapToGlobal(pos));
}

void LibraryPanel::renameItem(QTreeWidgetItem *item)
{
    if (!m_library || !item || !item->data(0, kStoredRole).toBool())
        return;
    bool ok = false;
    const QString name = QInputDialog::getText(this, tr("Rename"), tr("Name:"), QLineEdit::Normal, item->text(0), &ok);
    if (ok)
        m_library->rename(kindOf(typeOf(item)), pathOf(item), name);
}

void LibraryPanel::removeItem(QTreeWidgetItem *item)
{
    if (!m_library || !item || !item->data(0, kStoredRole).toBool())
        return;
    const EntryType type = typeOf(item);
    const QString path = pathOf(item);
    if (type == EntryType::Playlist && MediaLibrary::ownsPlaylist(path)) {
        const auto answer = QMessageBox::question(this, tr("Delete Playlist"),
                                                  tr("Delete the playlist \"%1\"?").arg(item->text(0)));
        if (answer != QMessageBox::Yes)
            return;
    }
    m_expandedKeys.remove(itemKey(item));
    m_library->remove(kindOf(type), path);
}

void LibraryPanel::updateButtons()
{
    QTreeWidgetItem *item = m_view->currentItem();
    m_removeButton->setEnabled(item && item->data(0, kStoredRole).toBool());
}
