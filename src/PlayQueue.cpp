#include "PlayQueue.h"
#include "PlaylistOps.h"

#include <QRandomGenerator>
#include <QSet>

#include <algorithm>

PlayQueue::PlayQueue(QObject *parent)
    : QObject(parent)
{
}

std::optional<PlayQueue::Item> PlayQueue::item(qint64 id) const
{
    const int row = rowOf(id);
    if (row < 0)
        return std::nullopt;
    return m_playlist[row];
}

QList<qint64> PlayQueue::playlistOrder(const QList<qint64> &members) const
{
    QList<qint64> order = members;
    std::sort(order.begin(), order.end(), [this](qint64 a, qint64 b) { return rowOf(a) < rowOf(b); });
    return order;
}

bool PlayQueue::matchesQuery(const Item &item) const
{
    // As the drawer's search matches: every word, in the name or the path.
    const QStringList words = m_query.split(QLatin1Char(' '), Qt::SkipEmptyParts);
    const QString name = PlaylistOps::displayName({item.filename, item.title});
    return std::all_of(words.cbegin(), words.cend(), [&](const QString &word) {
        return name.contains(word, Qt::CaseInsensitive) || item.filename.contains(word, Qt::CaseInsensitive);
    });
}

void PlayQueue::shuffleFrom(int first)
{
    if (first < m_order.size())
        std::shuffle(m_order.begin() + first, m_order.end(), *QRandomGenerator::global());
}

void PlayQueue::takeIntoContext(qint64 id)
{
    const qsizetype index = m_order.indexOf(id);
    if (index < 0)
        return;
    m_order.removeAt(index);
    if (index <= m_cursor)
        --m_cursor;
    m_order.insert(m_cursor + 1, id);
    ++m_cursor;
}

void PlayQueue::setPlaylist(const QList<Item> &playlist)
{
    const bool same = playlist.size() == m_playlist.size()
        && std::equal(playlist.cbegin(), playlist.cend(), m_playlist.cbegin(), [](const Item &a, const Item &b) {
               return a.id == b.id && a.filename == b.filename && a.title == b.title;
           });
    if (same)
        return;
    const QHash<qint64, int> previous = m_index;
    m_playlist = playlist;
    m_index.clear();
    m_index.reserve(playlist.size());
    for (int row = 0; row < playlist.size(); ++row)
        m_index.insert(playlist[row].id, row);

    m_queued.erase(std::remove_if(m_queued.begin(), m_queued.end(), [this](qint64 id) { return !contains(id); }),
                   m_queued.end());
    if (!contains(m_current))
        m_current = -1;

    // The context keeps the entries still there; the cursor stays on the last
    // played one that is, so the entry after a removed one plays next.
    QList<qint64> survivors;
    int cursor = -1;
    for (int i = 0; i < m_order.size(); ++i) {
        if (!contains(m_order[i]))
            continue;
        survivors.append(m_order[i]);
        if (i <= m_cursor)
            cursor = int(survivors.size()) - 1;
    }
    const qint64 anchor = cursor >= 0 ? survivors[cursor] : -1;

    if (survivors.isEmpty() && m_source == Source::Search) {
        // Every result is gone (e.g. the playlist was replaced): the playlist plays on.
        m_source = Source::Playlist;
        m_query.clear();
    }
    QList<qint64> added;
    for (const Item &item : playlist) {
        // Only entries new to the playlist join: the others were left out on purpose.
        if (previous.contains(item.id) && !survivors.isEmpty())
            continue;
        if (survivors.contains(item.id))
            continue;
        if (m_source == Source::Playlist || matchesQuery(item))
            added.append(item.id);
    }

    if (!m_shuffle && !m_customOrder) {
        m_order = playlistOrder(survivors + added);
        m_cursor = anchor >= 0 ? int(m_order.indexOf(anchor)) : -1;
    } else {
        m_order = survivors;
        m_cursor = cursor;
        for (qint64 id : std::as_const(added)) {
            // New entries come up at random among the ones still to play.
            const int first = m_cursor + 1;
            const int position = m_shuffle ? first + int(QRandomGenerator::global()->bounded(int(m_order.size()) - first + 1))
                                           : int(m_order.size());
            m_order.insert(position, id);
        }
    }
    Q_EMIT changed();
}

void PlayQueue::start(qint64 id, const QList<qint64> &scope, const QString &query)
{
    if (!contains(id))
        return;
    QList<qint64> members;
    if (scope.isEmpty()) {
        m_source = Source::Playlist;
        m_query.clear();
        for (const Item &item : std::as_const(m_playlist))
            members.append(item.id);
    } else {
        m_source = Source::Search;
        m_query = query.trimmed();
        QSet<qint64> seen;
        for (qint64 member : scope) {
            if (contains(member) && !seen.contains(member)) {
                seen.insert(member);
                members.append(member);
            }
        }
        if (!seen.contains(id))
            members.append(id);
    }
    m_customOrder = false;
    m_queued.removeAll(id);
    if (m_shuffle) {
        members.removeAll(id);
        m_order = QList<qint64>{id} + members;
        shuffleFrom(1);
        m_cursor = 0;
    } else {
        m_order = playlistOrder(members);
        m_cursor = int(m_order.indexOf(id));
    }
    m_current = id;
    Q_EMIT changed();
}

void PlayQueue::setCurrent(qint64 id)
{
    if (id == m_current || !contains(id))
        return;
    if (m_queued.contains(id)) {
        // A queued entry started some other way: it has had its turn.
        m_queued.removeAll(id);
        takeIntoContext(id);
    } else if (m_order.contains(id)) {
        if (m_shuffle || m_customOrder)
            takeIntoContext(id);
        else
            m_cursor = int(m_order.indexOf(id));
    } else {
        start(id);
        return;
    }
    m_current = id;
    Q_EMIT changed();
}

std::optional<qint64> PlayQueue::next(bool wrap)
{
    if (!m_queued.isEmpty()) {
        const qint64 id = m_queued.takeFirst();
        // Played from the queue, it doesn't come again later in the context.
        takeIntoContext(id);
        m_current = id;
        Q_EMIT changed();
        return id;
    }
    if (m_cursor + 1 < m_order.size()) {
        m_current = m_order[++m_cursor];
        Q_EMIT changed();
        return m_current;
    }
    if (!wrap || m_order.isEmpty())
        return std::nullopt;
    if (m_shuffle && m_order.size() > 1) {
        // A new round in a new order, not starting with what just played.
        shuffleFrom(0);
        if (m_order.first() == m_current)
            std::swap(m_order.first(), m_order.last());
    }
    m_cursor = 0;
    m_current = m_order.first();
    Q_EMIT changed();
    return m_current;
}

std::optional<qint64> PlayQueue::previous(bool wrap)
{
    if (m_cursor >= 0 && m_cursor < m_order.size() && m_order[m_cursor] != m_current) {
        // A queued entry is playing: back to the context's entry before it.
        m_current = m_order[m_cursor];
    } else if (m_cursor > 0) {
        m_current = m_order[--m_cursor];
    } else if (wrap && !m_order.isEmpty()) {
        m_cursor = int(m_order.size()) - 1;
        m_current = m_order[m_cursor];
    } else {
        return std::nullopt;
    }
    Q_EMIT changed();
    return m_current;
}

void PlayQueue::setShuffle(bool on)
{
    if (m_shuffle == on)
        return;
    m_shuffle = on;
    m_customOrder = false;
    if (on) {
        if (m_order.contains(m_current)) {
            m_order.removeAll(m_current);
            m_order.prepend(m_current);
            shuffleFrom(1);
            m_cursor = 0;
        } else {
            shuffleFrom(0);
            m_cursor = -1;
        }
    } else {
        // The playlist's order again, carrying on after the last played entry.
        const qint64 anchor = m_cursor >= 0 && m_cursor < m_order.size() ? m_order[m_cursor] : -1;
        m_order = playlistOrder(m_order);
        m_cursor = anchor >= 0 ? int(m_order.indexOf(anchor)) : -1;
    }
    Q_EMIT changed();
}

void PlayQueue::resetToPlaylist()
{
    if (m_source == Source::Playlist)
        return;
    if (contains(m_current)) {
        const QList<qint64> queued = m_queued;
        start(m_current);
        // start() takes the playing entry out of the queue; it isn't in it.
        m_queued = queued;
        return;
    }
    m_source = Source::Playlist;
    m_query.clear();
    m_customOrder = false;
    m_order.clear();
    for (const Item &item : std::as_const(m_playlist))
        m_order.append(item.id);
    if (m_shuffle)
        shuffleFrom(0);
    m_cursor = -1;
    Q_EMIT changed();
}

QList<qint64> PlayQueue::upcoming() const
{
    QList<qint64> result;
    for (int i = m_cursor + 1; i < m_order.size(); ++i) {
        // A queued entry shows (and plays) once, in the queue.
        if (!m_queued.contains(m_order[i]))
            result.append(m_order[i]);
    }
    return result;
}

void PlayQueue::playNext(const QList<qint64> &ids)
{
    QList<qint64> front;
    for (qint64 id : ids) {
        if (contains(id) && !front.contains(id))
            front.append(id);
    }
    if (front.isEmpty())
        return;
    for (qint64 id : std::as_const(front))
        m_queued.removeAll(id);
    m_queued = front + m_queued;
    Q_EMIT changed();
}

void PlayQueue::enqueue(const QList<qint64> &ids)
{
    bool added = false;
    for (qint64 id : ids) {
        if (contains(id) && !m_queued.contains(id)) {
            m_queued.append(id);
            added = true;
        }
    }
    if (added)
        Q_EMIT changed();
}

void PlayQueue::remove(const QList<qint64> &ids)
{
    bool removed = false;
    for (qint64 id : ids) {
        removed |= m_queued.removeAll(id) > 0;
        // Entries already played stay, so Previous still finds them.
        const qsizetype index = m_order.indexOf(id);
        if (index > m_cursor) {
            m_order.removeAt(index);
            removed = true;
        }
    }
    if (removed)
        Q_EMIT changed();
}

void PlayQueue::clearQueued()
{
    if (m_queued.isEmpty())
        return;
    m_queued.clear();
    Q_EMIT changed();
}

void PlayQueue::move(int from, int to)
{
    const QList<qint64> rest = upcoming();
    const int queuedCount = int(m_queued.size());
    const int total = queuedCount + int(rest.size());
    if (from < 0 || from >= total || to < 0 || to > total || to == from || to == from + 1)
        return;
    const bool fromQueue = from < queuedCount;
    const qint64 id = fromQueue ? m_queued[from] : rest[from - queuedCount];

    // The list without the entry, and where it goes in it.
    QList<qint64> list = m_queued + rest;
    list.removeAt(from);
    const int target = to > from ? to - 1 : to;
    const int queuedLeft = queuedCount - (fromQueue ? 1 : 0);
    const bool toQueue = target < queuedLeft || (target == queuedLeft && fromQueue);

    if (fromQueue)
        m_queued.removeAt(from);
    if (toQueue) {
        m_queued.insert(target, id);
    } else {
        // Before the context entry now at `target`, or last.
        if (const qsizetype index = m_order.indexOf(id); index >= 0) {
            m_order.removeAt(index);
            if (index <= m_cursor)
                --m_cursor;
        }
        const qsizetype before = target < list.size() ? m_order.indexOf(list[target]) : -1;
        m_order.insert(before >= 0 ? before : m_order.size(), id);
        m_customOrder = true;
    }
    Q_EMIT changed();
}

void PlayQueue::jumpTo(qint64 id)
{
    if (const qsizetype index = m_queued.indexOf(id); index >= 0) {
        m_queued = m_queued.mid(index + 1);
        takeIntoContext(id);
    } else if (const qsizetype index = m_order.indexOf(id); index > m_cursor) {
        m_cursor = int(index);
    } else {
        return;
    }
    m_current = id;
    Q_EMIT changed();
}
