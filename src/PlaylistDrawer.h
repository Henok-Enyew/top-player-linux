#pragma once

#include "MediaDownloader.h"
#include "PlaylistOps.h"

#include <QHash>

#include <QFrame>
#include <QListWidget>
#include <QVariant>

class LibraryPanel;
class QLabel;
class QLineEdit;
class QMenu;
class QPropertyAnimation;
class QStackedWidget;
class QTabBar;
class QToolButton;

// List view that turns drag-and-drop into playlist requests instead of moving
// items itself; the list is rebuilt from mpv's playlist afterwards.
class PlaylistView : public QListWidget
{
    Q_OBJECT

public:
    explicit PlaylistView(QWidget *parent = nullptr);

    // Selected rows that the filter shows, ascending.
    QList<int> selectedVisibleRows() const;

Q_SIGNALS:
    // Move entry `from` so it takes the place of entry `to` (mpv playlist-move semantics).
    void moveRequested(int from, int to);
    // Files, folders and URLs dropped at playlist index `row` (-1 = end).
    // Folders are not expanded yet.
    void filesDropped(const QStringList &files, int row);
    void removeRequested(const QList<int> &rows);
    // The download button of entry `row` was clicked, at `globalPos`.
    void downloadButtonClicked(int row, const QPoint &globalPos);

protected:
    void mousePressEvent(QMouseEvent *event) override;
    bool viewportEvent(QEvent *event) override;
    void dragEnterEvent(QDragEnterEvent *event) override;
    void dragMoveEvent(QDragMoveEvent *event) override;
    void dropEvent(QDropEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;

private:
    int dropRow(QDropEvent *event) const;
};

// Collapsible right-hand panel with two pages: the playlist, mirroring mpv's
// "playlist" property, with a search filter and the playlist management
// actions; and the library of stored folders and playlists. Its left edge can
// be dragged to resize it, and it can be expanded over most of the video.
class PlaylistDrawer : public QFrame
{
    Q_OBJECT
    Q_PROPERTY(int drawerWidth READ drawerWidth WRITE setDrawerWidth)

public:
    enum Page { PlaylistPage, LibraryPage };

    // `parent` is the widget the drawer shares with the video; expanding it
    // fills most of its width.
    explicit PlaylistDrawer(QWidget *parent = nullptr);

    LibraryPanel *libraryPanel() const { return m_library; }
    Page currentPage() const;
    void setCurrentPage(Page page);

    // `durations` (seconds, negative if unknown) parallels `playlist` and may be empty.
    void setEntries(const QVariantList &playlist, const QList<double> &durations = {});
    void setDurations(const QList<double> &durations);
    // Shows a download's state ("Queued", "45%", "Saved", "Failed"; empty
    // for none) beside the entries with filename `entry`.
    void setDownloadStatus(const QString &entry, const QString &status);
    // Marks entry `row` as the one playing (-1: none) and scrolls it into
    // view, unless the pointer is over the list. Follows mpv's playlist-pos,
    // which changes without the playlist itself being reported again.
    void setPlayingRow(int row);
    int playingRow() const;

    // Hides the entries that don't match every word of `text` (in name or path).
    // The playlist itself is not changed.
    void setFilterText(const QString &text);
    QString filterText() const;

    bool isExpanded() const { return m_expanded; }
    void setExpanded(bool expanded, bool animate = true);

    int drawerWidth() const { return width(); }
    void setDrawerWidth(int width);

    // Expanded over the video, leaving only a strip of it visible.
    bool isWide() const { return m_wide; }
    void setWide(bool wide, bool animate = true);
    // The width the drawer opens at when not wide, as last dragged by the
    // user; remembered across runs if `save`. Leaves wide mode.
    int preferredWidth() const { return m_preferredWidth; }
    void setPreferredWidth(int width, bool save = true);
    // The width the drawer has when wide, and the most it can be dragged to.
    int maximumDrawerWidth() const;

Q_SIGNALS:
    void playRequested(int index);
    void moveRequested(int from, int to);
    void filesDropped(const QStringList &files, int row);
    void removeRequested(const QList<int> &rows);
    void addRequested();
    void addFolderRequested();
    void addUrlRequested();
    void openFolderRequested();
    // The move buttons: shift the selected `rows` (see PlaylistOps::shiftOrder).
    void shiftRequested(const QList<int> &rows, PlaylistOps::Shift shift);
    void clearRequested();
    void sortRequested(PlaylistOps::SortKey key, bool ascending);
    void reverseRequested();
    void shuffleRequested();
    void removeMissingRequested();
    void removeDuplicatesRequested();
    void openPlaylistRequested();
    void savePlaylistRequested();
    // Save entries that stream from the web (see DownloadQueue).
    void downloadRequested(const QList<int> &rows, MediaDownloader::Format format);
    void expandedChanged(bool expanded);
    void wideChanged(bool wide);

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    void buildMenus();
    // The bar along the bottom, as in PotPlayer: move buttons, ADD, DEL, SORT and more.
    QWidget *buildActionBar();
    // Updates the rows in place when only the playing entry or titles changed.
    bool updateEntriesInPlace(const QVariantList &playlist);
    // Checks the session options to match the saved settings.
    void syncOptions();
    void showContextMenu(const QPoint &pos);
    // The formats to download `rows` in.
    void addDownloadActions(QMenu *menu, const QList<int> &rows);
    QList<int> downloadableRows(const QList<int> &rows) const;
    void markPlaying(QListWidgetItem *item, bool playing);
    // Scrolls the playing entry into view if it moved since the last time.
    void revealPlaying();
    void applyFilter();
    void updateCount();
    // The width the drawer should have while open.
    int targetWidth() const;
    void animateTo(int width);

    PlaylistView *m_view;
    QLineEdit *m_filter;
    QLabel *m_countLabel;
    QMenu *m_sortMenu = nullptr;
    QMenu *m_moreMenu = nullptr;
    QMenu *m_addMenu = nullptr;
    QMenu *m_deleteMenu = nullptr;
    QAction *m_rememberAction = nullptr;
    QAction *m_resumeAction = nullptr;
    QTabBar *m_tabs;
    QStackedWidget *m_pages;
    LibraryPanel *m_library;
    QToolButton *m_wideButton = nullptr;
    QPropertyAnimation *m_animation;
    int m_preferredWidth;
    bool m_expanded = false;
    bool m_wide = false;
    // mpv's id of the entry last scrolled into view.
    QVariant m_revealedId;
    // Download states by entry filename, kept across rebuilds of the list.
    QHash<QString, QString> m_downloadStatus;
};
