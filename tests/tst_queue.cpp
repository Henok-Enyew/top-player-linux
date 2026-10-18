// The play queue on its own: the order entries play in, kept apart from the
// playlist (shuffle, the manual queue, a search's results as the context),
// and how it follows the playlist changing under it.

#include "PlayQueue.h"

#include <QSet>
#include <QSignalSpy>
#include <QTest>

#include <algorithm>

namespace {

// Entries 1..n named "song <id>.mp3", or `names` in order.
QList<PlayQueue::Item> playlist(int count, const QStringList &names = {})
{
    QList<PlayQueue::Item> items;
    for (int id = 1; id <= count; ++id)
        items.append({id, names.value(id - 1, QStringLiteral("/music/song %1.mp3").arg(id)), QString()});
    return items;
}

QList<qint64> ids(std::initializer_list<qint64> list)
{
    return QList<qint64>(list);
}

// Plays the context through from the current entry; the entries played, in order.
QList<qint64> playThrough(PlayQueue &queue, bool wrap = false, int limit = 100)
{
    QList<qint64> played;
    while (limit-- > 0) {
        const std::optional<qint64> id = queue.next(wrap);
        if (!id)
            break;
        played.append(*id);
    }
    return played;
}

} // namespace

class QueueTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void followsPlaylistOrder();
    void previousAndWrap();
    void shuffleKeepsPlaylist();
    void shuffleOffCarriesOn();
    void searchContext();
    void searchContextOutlivesClearedSearch();
    void newEntriesJoinMatchingSearch();
    void manualQueueFirst();
    void queuedEntryPlaysOnce();
    void moveAndRemove();
    void jumpTo();
    void removedEntriesLeave();
    void externalStartResetsContext();
    void replacedPlaylistDropsSearch();
};

void QueueTest::followsPlaylistOrder()
{
    PlayQueue queue;
    queue.setPlaylist(playlist(5));
    queue.start(2);
    QCOMPARE(queue.current(), 2);
    QCOMPARE(queue.upcoming(), ids({3, 4, 5}));
    QCOMPARE(playThrough(queue), ids({3, 4, 5}));
    // Nothing after the last entry, and nothing changes.
    QVERIFY(!queue.next(false));
    QCOMPARE(queue.current(), 5);
}

void QueueTest::previousAndWrap()
{
    PlayQueue queue;
    queue.setPlaylist(playlist(3));
    queue.start(1);
    QVERIFY(!queue.previous(false));
    QCOMPARE(queue.previous(true), std::optional<qint64>(3));
    QCOMPARE(queue.next(true), std::optional<qint64>(1));
    QCOMPARE(queue.next(false), std::optional<qint64>(2));
    QCOMPARE(queue.previous(false), std::optional<qint64>(1));
}

void QueueTest::shuffleKeepsPlaylist()
{
    PlayQueue queue;
    const QList<PlayQueue::Item> items = playlist(30);
    queue.setPlaylist(items);
    queue.start(1);
    queue.setShuffle(true);
    QVERIFY(queue.isShuffled());
    // The playlist is untouched; the order to play it in is not.
    QCOMPARE(queue.playlist().size(), items.size());
    for (int i = 0; i < items.size(); ++i)
        QCOMPARE(queue.playlist()[i].id, items[i].id);
    QCOMPARE(queue.current(), 1);
    QList<qint64> upcoming = queue.upcoming();
    QCOMPARE(upcoming.size(), 29);
    QVERIFY(upcoming != ids({2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25,
                             26, 27, 28, 29, 30}));
    std::sort(upcoming.begin(), upcoming.end());
    QCOMPARE(upcoming.first(), 2);
    QCOMPARE(upcoming.last(), 30);

    // Every entry once per round; with repeat, a new round in a new order.
    const QList<qint64> round = playThrough(queue);
    QCOMPARE(QSet<qint64>(round.cbegin(), round.cend()).size(), 29);
    const std::optional<qint64> first = queue.next(true);
    QVERIFY(first && *first != round.last());
}

void QueueTest::shuffleOffCarriesOn()
{
    PlayQueue queue;
    queue.setPlaylist(playlist(10));
    queue.start(1);
    queue.setShuffle(true);
    const qint64 playing = *queue.next(false);
    queue.setShuffle(false);
    // The playlist's order again, from the entry playing.
    QCOMPARE(queue.current(), playing);
    QList<qint64> expected;
    for (qint64 id = playing + 1; id <= 10; ++id)
        expected.append(id);
    QCOMPARE(queue.upcoming(), expected);
}

void QueueTest::searchContext()
{
    PlayQueue queue;
    queue.setPlaylist(playlist(6, {QStringLiteral("/m/Love Me.mp3"), QStringLiteral("/m/Other.mp3"),
                                   QStringLiteral("/m/Lovely.mp3"), QStringLiteral("/m/Else.mp3"),
                                   QStringLiteral("/m/love story.mp3"), QStringLiteral("/m/Last.mp3")}));
    // "love" found 1, 3 and 5; 3 was opened.
    queue.start(3, ids({1, 3, 5}), QStringLiteral("love"));
    QCOMPARE(queue.source(), PlayQueue::Source::Search);
    QCOMPARE(queue.query(), QStringLiteral("love"));
    QCOMPARE(queue.contextSize(), 3);
    QCOMPARE(queue.upcoming(), ids({5}));
    QCOMPARE(queue.next(false), std::optional<qint64>(5));
    QVERIFY(!queue.next(false));
    // Repeat goes round the results only.
    QCOMPARE(queue.next(true), std::optional<qint64>(1));
    QCOMPARE(queue.previous(true), std::optional<qint64>(5));

    // Shuffle shuffles the results, not the playlist.
    queue.setShuffle(true);
    QList<qint64> rest = queue.upcoming();
    std::sort(rest.begin(), rest.end());
    QCOMPARE(rest, ids({1, 3}));

    // Back to the whole playlist, from the entry playing.
    queue.setShuffle(false);
    queue.resetToPlaylist();
    QCOMPARE(queue.source(), PlayQueue::Source::Playlist);
    QCOMPARE(queue.current(), 5);
    QCOMPARE(queue.upcoming(), ids({6}));
}

void QueueTest::searchContextOutlivesClearedSearch()
{
    // The queue doesn't know about the search field: only opening an entry
    // changes the context. Another search's results, or none, replace it.
    PlayQueue queue;
    queue.setPlaylist(playlist(6));
    queue.start(2, ids({2, 4, 6}), QStringLiteral("even"));
    QCOMPARE(queue.next(false), std::optional<qint64>(4));
    queue.start(3, ids({1, 3}), QStringLiteral("odd"));
    QCOMPARE(queue.upcoming(), QList<qint64>());
    QCOMPARE(queue.previous(false), std::optional<qint64>(1));
    queue.start(3);
    QCOMPARE(queue.source(), PlayQueue::Source::Playlist);
    QCOMPARE(queue.upcoming(), ids({4, 5, 6}));
}

void QueueTest::newEntriesJoinMatchingSearch()
{
    PlayQueue queue;
    QList<PlayQueue::Item> items = playlist(3, {QStringLiteral("/m/love.mp3"), QStringLiteral("/m/x.mp3"),
                                                QStringLiteral("/m/lovely.mp3")});
    queue.setPlaylist(items);
    queue.start(1, ids({1, 3}), QStringLiteral("love"));
    items.append({4, QStringLiteral("/m/more love.mp3"), QString()});
    items.append({5, QStringLiteral("/m/nothing.mp3"), QString()});
    queue.setPlaylist(items);
    QCOMPARE(queue.upcoming(), ids({3, 4}));
}

void QueueTest::manualQueueFirst()
{
    PlayQueue queue;
    queue.setPlaylist(playlist(6));
    queue.start(1);
    queue.enqueue(ids({5}));
    queue.playNext(ids({4}));
    queue.enqueue(ids({6, 5})); // 5 is queued already
    QCOMPARE(queue.queued(), ids({4, 5, 6}));
    // A queued entry shows once, in the queue.
    QCOMPARE(queue.upcoming(), ids({2, 3}));
    QCOMPARE(playThrough(queue), ids({4, 5, 6, 2, 3}));
}

void QueueTest::queuedEntryPlaysOnce()
{
    PlayQueue queue;
    queue.setPlaylist(playlist(5));
    queue.start(1);
    queue.playNext(ids({4}));
    QCOMPARE(queue.next(false), std::optional<qint64>(4));
    // 4 had its turn: the context carries on after it without playing it again.
    QCOMPARE(playThrough(queue), ids({2, 3, 5}));
    // Previous from a queued entry goes back to the context's entry before it.
    queue.start(1);
    queue.playNext(ids({3}));
    queue.next(false);
    QCOMPARE(queue.previous(false), std::optional<qint64>(1));
}

void QueueTest::moveAndRemove()
{
    PlayQueue queue;
    queue.setPlaylist(playlist(6));
    queue.start(1);
    queue.enqueue(ids({6}));
    // Places: 6 | 2 3 4 5. Move 4 before 2: still part of the context.
    queue.move(3, 1);
    QCOMPARE(queue.queued(), ids({6}));
    QCOMPARE(queue.upcoming(), ids({4, 2, 3, 5}));
    // Move 5 before 6: it joins the queue.
    queue.move(4, 0);
    QCOMPARE(queue.queued(), ids({5, 6}));
    QCOMPARE(queue.upcoming(), ids({4, 2, 3}));
    // Move 6 past the queue: back into the context, after 2.
    queue.move(1, 4);
    QCOMPARE(queue.queued(), ids({5}));
    QCOMPARE(queue.upcoming(), ids({4, 2, 6, 3}));
    // Removing takes entries out of the queue and the context, not the playlist.
    queue.remove(ids({5, 2}));
    QCOMPARE(queue.queued(), QList<qint64>());
    QCOMPARE(queue.upcoming(), ids({4, 6, 3}));
    QCOMPARE(queue.playlist().size(), 6);
    // A reordered context keeps its order when the playlist changes.
    QList<PlayQueue::Item> items = playlist(7);
    queue.setPlaylist(items);
    QCOMPARE(queue.upcoming(), ids({4, 6, 3, 7}));
}

void QueueTest::jumpTo()
{
    PlayQueue queue;
    queue.setPlaylist(playlist(6));
    queue.start(1);
    queue.enqueue(ids({5, 6}));
    // A queued entry: the ones queued before it are dropped.
    queue.jumpTo(6);
    QCOMPARE(queue.current(), 6);
    QCOMPARE(queue.queued(), QList<qint64>());
    QCOMPARE(queue.upcoming(), ids({2, 3, 4, 5}));
    // An entry further on: the ones before it count as played.
    queue.jumpTo(4);
    QCOMPARE(queue.upcoming(), ids({5}));
    QCOMPARE(queue.previous(false), std::optional<qint64>(3));
}

void QueueTest::removedEntriesLeave()
{
    PlayQueue queue;
    QList<PlayQueue::Item> items = playlist(5);
    queue.setPlaylist(items);
    queue.start(2);
    queue.enqueue(ids({4}));
    QSignalSpy changed(&queue, &PlayQueue::changed);
    // The playing entry and a queued one are removed from the playlist: the
    // entry after the playing one still plays next.
    items.removeAt(3); // 4
    items.removeAt(1); // 2
    queue.setPlaylist(items);
    QVERIFY(!changed.isEmpty());
    QCOMPARE(queue.current(), -1);
    QCOMPARE(queue.queued(), QList<qint64>());
    QCOMPARE(queue.upcoming(), ids({3, 5}));
    // The same playlist again changes nothing.
    changed.clear();
    queue.setPlaylist(items);
    QVERIFY(changed.isEmpty());
}

void QueueTest::externalStartResetsContext()
{
    PlayQueue queue;
    queue.setPlaylist(playlist(6));
    queue.start(2, ids({2, 4}), QStringLiteral("x"));
    // An entry of the context started by other means plays on in it...
    queue.setCurrent(4);
    QCOMPARE(queue.source(), PlayQueue::Source::Search);
    // ...any other starts a playlist context.
    queue.setCurrent(5);
    QCOMPARE(queue.source(), PlayQueue::Source::Playlist);
    QCOMPARE(queue.upcoming(), ids({6}));
}

void QueueTest::replacedPlaylistDropsSearch()
{
    PlayQueue queue;
    queue.setPlaylist(playlist(4));
    queue.start(1, ids({1, 2}), QStringLiteral("x"));
    queue.setShuffle(true);
    // A new playlist (new ids): the search's results are gone.
    QList<PlayQueue::Item> items;
    for (qint64 id = 10; id < 15; ++id)
        items.append({id, QStringLiteral("/n/%1.mp3").arg(id), QString()});
    queue.setPlaylist(items);
    QCOMPARE(queue.source(), PlayQueue::Source::Playlist);
    QCOMPARE(queue.contextSize(), 5);
    // mpv starts one of them: it comes first, the rest follow shuffled.
    queue.setCurrent(12);
    QCOMPARE(queue.current(), 12);
    QCOMPARE(queue.upcoming().size(), 4);
}

QTEST_GUILESS_MAIN(QueueTest)
#include "tst_queue.moc"
