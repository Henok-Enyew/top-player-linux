#pragma once

#include <QListWidget>
#include <QTimer>
#include <QWidget>

class PlayQueue;
class QLabel;
class QToolButton;

// The list of the queue page: drag-and-drop asks the queue to move entries
// instead of moving items itself; the list is rebuilt from the queue.
class QueueView : public QListWidget
{
    Q_OBJECT

public:
    explicit QueueView(QWidget *parent = nullptr);

    // The selected entries' places in the queue then the rest of the context
    // (PlayQueue::move()), ascending.
    QList<int> selectedPlaces() const;

Q_SIGNALS:
    // Move the entry at place `from` before the one at `to` (PlayQueue::move()).
    void moveRequested(int from, int to);
    void removeRequested();

protected:
    void dragMoveEvent(QDragMoveEvent *event) override;
    void dropEvent(QDropEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;

private:
    // The place an entry dropped at `pos` takes, or -1 where it can't go.
    int dropPlace(const QPoint &pos) const;
};

// The drawer's Queue page: what is playing, the songs queued to play next,
// and the rest of what it plays from (the playlist, or a search's results),
// in the order they will play. Entries can be played, queued, moved and
// taken out of it without changing the playlist.
class QueuePanel : public QWidget
{
    Q_OBJECT

public:
    explicit QueuePanel(QWidget *parent = nullptr);

    void setQueue(PlayQueue *queue);

Q_SIGNALS:
    // Play queue entry `id` now.
    void playRequested(qint64 id);

protected:
    void showEvent(QShowEvent *event) override;

private:
    // Rebuilds the list (soon, and only while the page shows).
    void scheduleRebuild();
    void rebuild();
    void showContextMenu(const QPoint &pos);
    QList<qint64> selectedIds() const;
    // Moves the selected entries up or down one place, keeping them selected.
    void shiftSelection(int delta);
    void updateActions();

    PlayQueue *m_queue = nullptr;
    QueueView *m_view;
    QLabel *m_source;
    QToolButton *m_resetButton;
    QLabel *m_empty;
    QToolButton *m_upButton;
    QToolButton *m_downButton;
    QToolButton *m_nextButton;
    QToolButton *m_removeButton;
    QToolButton *m_clearButton;
    QTimer m_rebuildTimer;
    bool m_dirty = true;
};
