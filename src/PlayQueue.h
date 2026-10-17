#pragma once

#include <QHash>
#include <QList>
#include <QObject>
#include <QString>

#include <optional>

// The order entries play in, kept apart from the playlist: shuffle and the
// queue's own reordering change this, never the playlist.
//
// Entries are named by mpv's playlist entry ids, which stay with an entry when
// the playlist is reordered. What plays next comes from, in turn:
//  - the manual queue ("Play Next", "Add to Queue"), in the order given;
//  - the context the playing entry was started from: the whole playlist, or
//    the results of the search the drawer showed when it was opened. A
//    context lasts until an entry is opened from another one (or without a
//    search); clearing the search leaves it as it is.
class PlayQueue : public QObject
{
    Q_OBJECT

public:
    struct Item {
        qint64 id = -1;
        QString filename;
        QString title;
    };
    enum class Source { Playlist, Search };

    explicit PlayQueue(QObject *parent = nullptr);

    // The playlist as mpv reports it, in order. Entries that left it leave the
    // queue and the context; new ones join a playlist context, and a search
    // context if they match its search.
    void setPlaylist(const QList<Item> &playlist);
    const QList<Item> &playlist() const { return m_playlist; }
    bool contains(qint64 id) const { return m_index.contains(id); }
    // The playlist row of `id`, or -1.
    int rowOf(qint64 id) const { return m_index.value(id, -1); }
    std::optional<Item> item(qint64 id) const;

    // `id` was opened by the user from `scope`: the entries a search showed
    // (by `query`), or the whole playlist if `scope` is empty.
    void start(qint64 id, const QList<qint64> &scope = {}, const QString &query = QString());
    // `id` started playing by other means (a new playlist, the session
    // restored, a file failing). An entry of the context or the queue plays
    // on in it; any other starts a new playlist context.
    void setCurrent(qint64 id);
    // Steps to the entry that plays next, or back, and returns it. Wraps
    // around the context if `wrap` (repeat all); a shuffled context is
    // shuffled again for the next round. Returns nothing (and changes nothing)
    // if there is no such entry.
    std::optional<qint64> next(bool wrap);
    std::optional<qint64> previous(bool wrap);

    bool isShuffled() const { return m_shuffle; }
    // Shuffles the context, keeping the playing entry first; off returns to
    // the playlist's order, carrying on from the playing entry.
    void setShuffle(bool on);

    qint64 current() const { return m_current; }
    Source source() const { return m_source; }
    // The search the context comes from (empty for the playlist).
    QString query() const { return m_query; }
    // The size of the context (the playlist, or the search results).
    int contextSize() const { return int(m_order.size()); }
    // Plays the rest of the whole playlist instead of the search results.
    void resetToPlaylist();

    // The manual queue, in play order.
    QList<qint64> queued() const { return m_queued; }
    // The rest of the context, in play order (after the queue).
    QList<qint64> upcoming() const;

    // Puts `ids` first in the queue, keeping their order.
    void playNext(const QList<qint64> &ids);
    // Adds `ids` to the end of the queue.
    void enqueue(const QList<qint64> &ids);
    // Takes `ids` out of the queue and the rest of the context (not the playlist).
    void remove(const QList<qint64> &ids);
    void clearQueued();
    // Moves the entry at `from` of the list made of queued() then upcoming()
    // to `to` (an index in the same list, before the move). Entries of the
    // context moved in among the queued ones join the queue; queued ones moved
    // past the queue go back to the context there.
    void move(int from, int to);
    // Skips to `id` of the queue or the rest of the context: queued entries
    // before it are dropped, context entries before it count as played.
    void jumpTo(qint64 id);

Q_SIGNALS:
    // The queue, the context or the playing entry changed.
    void changed();

private:
    QList<qint64> playlistOrder(const QList<qint64> &members) const;
    bool matchesQuery(const Item &item) const;
    void shuffleFrom(int first);
    // Plays `id` as part of the context: moved just after the last played one.
    void takeIntoContext(qint64 id);

    QList<Item> m_playlist;
    QHash<qint64, int> m_index;
    QList<qint64> m_queued;
    // The context in play order; m_cursor is its last played entry (-1: none yet).
    QList<qint64> m_order;
    int m_cursor = -1;
    qint64 m_current = -1;
    Source m_source = Source::Playlist;
    QString m_query;
    bool m_shuffle = false;
    // The context's order was changed by hand: it no longer follows the playlist's.
    bool m_customOrder = false;
};
