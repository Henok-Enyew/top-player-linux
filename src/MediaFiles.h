#pragma once

#include <QSet>
#include <QString>
#include <QStringList>

#include <functional>

class QObject;

// File types the player opens, and helpers for the open dialogs.
namespace MediaFiles {

bool isMediaFile(const QString &path);
bool isPlaylistFile(const QString &path);

// QFileDialog name filters.
QString mediaFileFilter();
QString playlistFileFilter();
// Filter for saving playlists: only the formats the player writes (.m3u8, .m3u).
QString playlistSaveFilter();

// Media files in `folder` and its subfolders, in natural order
// ("Episode 2" before "Episode 10"), folders after the files beside them.
QStringList mediaFilesInFolder(const QString &folder);

// `entries` (paths or URLs) with each local folder replaced by its media
// files, as listed by mediaFilesInFolder().
QStringList expandFolders(const QStringList &entries);
// Runs expandFolders() in a worker thread, so big or slow folders don't
// freeze the window, then calls `done` in `context`'s thread. Nothing is
// called if `context` is destroyed first.
void expandFoldersAsync(const QStringList &entries, QObject *context, std::function<void(const QStringList &)> done);

// Natural, case-insensitive order: runs of digits compare by value. Returns
// <0, 0 or >0; only identical strings compare equal.
int naturalCompare(const QString &a, const QString &b);
inline bool naturalLess(const QString &a, const QString &b) { return naturalCompare(a, b) < 0; }

// The local file an mpv playlist entry refers to (a path or a file:// URL),
// or an empty string for streams and other URLs.
QString localPath(const QString &entry);

// What checkLocalFiles() found.
struct Availability {
    QSet<QString> missing; // entries whose file is gone or unreadable
    bool complete = true;  // false if the check ran out of time
};
// Checks that the local files among `entries` (paths or file:// URLs) still
// exist and can be read, in a worker thread, for at most `timeoutMs`: a
// file on a mount that stopped answering (a network share, a Flatpak
// document portal entry) must not freeze the window. Entries not checked in
// time are not in `missing`. Streams are never checked.
Availability checkLocalFiles(const QStringList &entries, int timeoutMs);

} // namespace MediaFiles
