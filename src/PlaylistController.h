#pragma once

#include "LibraryPanel.h"
#include "PlaylistOps.h"

#include <QElapsedTimer>
#include <QObject>
#include <QTimer>
#include <QVariant>

#include <optional>
#include <utility>

class DownloadQueue;
class MediaLibrary;
class MediaProber;
class MpvWidget;
class PlayQueue;
class PlaylistDrawer;
class QWidget;

// Carries out the playlist drawer's requests on mpv's playlist, which stays
// the single source of truth: reordering is done with playlist-move commands
// (like drag-and-drop), so the playing entry keeps playing. The order entries
// play in is the play queue's (see PlayQueue), which follows the playlist.
// Also keeps the playlist saved between runs, and plays and stores the
// library's folders and playlists.
class PlaylistController : public QObject
{
    Q_OBJECT

public:
    PlaylistController(MpvWidget *mpv, PlaylistDrawer *drawer, QWidget *dialogParent);

    // Mirrors a report of mpv's "playlist" property.
    void setPlaylist(const QVariantList &playlist);

    void addFilesDialog();
    void addFolderDialog();
    // Queues the media files in `folder` and its subfolders. The folder is
    // scanned in a worker thread; the files are queued when it is done.
    void addFolder(const QString &folder);
    // Queues `entries` at playlist index `row` (-1 appends), expanding
    // folders in a worker thread first.
    void addEntries(const QStringList &entries, int row = -1);
    void savePlaylistDialog();
    bool savePlaylist(const QString &path);
    void openPlaylistDialog();
    // Replaces the playlist with the entries of an .m3u/.m3u8/.pls file, read
    // in a worker thread. Titles are kept; local files that no longer exist
    // are left out, and the OSD says what was opened.
    void openPlaylist(const QString &path);
    // Replaces the playlist with the media files in `folder` and its subfolders.
    void openFolder(const QString &folder);
    // Replaces the playlist with `entries`, expanding folders in a worker
    // thread first; `subtitles` are added to the first file.
    void openEntries(const QStringList &entries, const QStringList &subtitles = {});
    // Asks for a URL and queues it.
    void addUrlDialog();

    void sort(PlaylistOps::SortKey key, bool ascending);
    void reverse();
    // Puts the playlist itself in a random order (unlike shuffle play, which
    // only changes the order the queue plays it in).
    void shuffle();

    // The order entries play in: shuffle, the context and the manual queue.
    PlayQueue *queue() const { return m_queue; }
    // Plays playlist entry `row` as the user opened it: the entries the
    // drawer's search shows become what plays next, or the whole playlist
    // without a search.
    void playRow(int row);
    // Plays queue entry `id` now.
    void playQueued(qint64 id);
    // Queues the entries at `rows` to play next, or after the queue.
    void playRowsNext(const QList<int> &rows);
    void enqueueRows(const QList<int> &rows);
    // The move buttons: shifts the entries at `rows`.
    void shiftRows(const QList<int> &rows, PlaylistOps::Shift shift);
    void removeRows(QList<int> rows);
    void clear();
    void removeMissing();
    void removeDuplicates();

    // Starts saving the queue (when enabled) and, if `restore`, reopens the
    // saved one. Returns true if a queue was restored. A `handoff` (the player
    // restarting itself) restores the queue and position whatever the settings.
    bool startSession(bool restore, bool handoff = false);
    // Saves the queue now, if enabled and the session has started. A
    // `handoff` saves it in any case, with the position even for a song.
    void saveSession(bool handoff = false);

    // mpv's current playlist, with durations (where known) and file sizes.
    QList<PlaylistOps::Entry> entries();

    MediaLibrary *library() const { return m_library; }
    // Saves online entries of the playlist (the drawer's Download actions).
    DownloadQueue *downloads() const { return m_downloads; }
    void addFolderToLibraryDialog();
    void addPlaylistFileToLibraryDialog();
    void saveQueueToLibraryDialog();
    // Saves the queue as a playlist in the library. Returns its path, or an
    // empty string if the queue is empty or can't be written.
    QString saveQueueToLibrary(const QString &name);
    void playFromLibrary(LibraryPanel::EntryType type, const QString &path, int start = -1);
    void queueFromLibrary(LibraryPanel::EntryType type, const QString &path);

Q_SIGNALS:
    void message(const QString &label, const QString &value = QString());

private:
    // Re-reads the playlist from mpv.
    void refresh();
    // Rebuilds the drawer and probes new entries for the last reported playlist.
    void updateDrawer();
    void applyOrder(const QList<int> &order);
    void scheduleSave();
    // Hands the last read playlist to the queue.
    void syncQueue();
    // mpv's playlist ids of `rows`.
    QList<qint64> idsOf(const QList<int> &rows);
    // Plays queue entry `id` and expects it to be the next to start.
    void playId(qint64 id);
    void expect(qint64 id);
    // Tells the queue which entry mpv is playing.
    void followPlayingEntry();
    QList<double> durations() const;

    MpvWidget *m_mpv;
    PlaylistDrawer *m_drawer;
    QWidget *m_dialogParent;
    MediaProber *m_prober;
    MediaLibrary *m_library;
    DownloadQueue *m_downloads;
    PlayQueue *m_queue;
    // The entry the queue just sent mpv to: other entries starting meanwhile
    // (mpv moving on from a failed file by itself) are not the user's choice.
    qint64 m_expectedId = -1;
    QElapsedTimer m_expectedSince;
    QVariantList m_playlist;
    QList<PlaylistOps::Entry> m_entries;
    // A duration sort waiting for the prober to finish.
    std::optional<std::pair<PlaylistOps::SortKey, bool>> m_pendingSort;
    // The files last handed to the prober.
    QStringList m_probedFiles;
    QTimer m_durationTimer;
    // Coalesces playlist reports, so a burst of changes rebuilds the drawer once.
    QTimer m_playlistTimer;
    QTimer m_saveTimer;
    QTimer m_positionTimer;
    bool m_sessionStarted = false;
};
