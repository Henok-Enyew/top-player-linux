#include "PlaylistController.h"
#include "DownloadQueue.h"
#include "MediaFiles.h"
#include "MediaLibrary.h"
#include "MediaProber.h"
#include "MpvWidget.h"
#include "PlaylistDrawer.h"
#include "PlaylistSession.h"

#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QFutureWatcher>
#include <QInputDialog>
#include <QUrl>
#include <QtConcurrent/QtConcurrentRun>

#include <algorithm>
#include <functional>
#include <numeric>

namespace {

// Duration labels are refreshed in batches while the prober works through a folder.
constexpr int kDurationRefreshMs = 150;
// The drawer follows mpv's playlist this long after its last change.
constexpr int kPlaylistRefreshMs = 30;
constexpr int kSaveDelayMs = 1000;
// While playing, the position is saved this often in case the player is killed.
constexpr int kPositionSaveMs = 30000;

} // namespace

PlaylistController::PlaylistController(MpvWidget *mpv, PlaylistDrawer *drawer, QWidget *dialogParent)
    : QObject(dialogParent)
    , m_mpv(mpv)
    , m_drawer(drawer)
    , m_dialogParent(dialogParent)
    , m_prober(new MediaProber(this))
    , m_library(new MediaLibrary(MediaLibrary::defaultFile(), this))
    , m_downloads(new DownloadQueue(this))
{
    using PlaylistOps::SortKey;
    connect(m_drawer, &PlaylistDrawer::playRequested, this, [this](int index) {
        m_mpv->command({QStringLiteral("playlist-play-index"), QString::number(index)});
    });
    connect(m_drawer, &PlaylistDrawer::moveRequested, this, [this](int from, int to) {
        m_mpv->command({QStringLiteral("playlist-move"), QString::number(from), QString::number(to)});
    });
    connect(m_drawer, &PlaylistDrawer::removeRequested, this, &PlaylistController::removeRows);
    connect(m_drawer, &PlaylistDrawer::filesDropped, this, &PlaylistController::addEntries);
    connect(m_drawer, &PlaylistDrawer::addRequested, this, &PlaylistController::addFilesDialog);
    connect(m_drawer, &PlaylistDrawer::addFolderRequested, this, &PlaylistController::addFolderDialog);
    connect(m_drawer, &PlaylistDrawer::addUrlRequested, this, &PlaylistController::addUrlDialog);
    connect(m_drawer, &PlaylistDrawer::shiftRequested, this, &PlaylistController::shiftRows);
    connect(m_drawer, &PlaylistDrawer::clearRequested, this, &PlaylistController::clear);
    connect(m_drawer, &PlaylistDrawer::sortRequested, this, &PlaylistController::sort);
    connect(m_drawer, &PlaylistDrawer::reverseRequested, this, &PlaylistController::reverse);
    connect(m_drawer, &PlaylistDrawer::shuffleRequested, this, &PlaylistController::shuffle);
    connect(m_drawer, &PlaylistDrawer::removeMissingRequested, this, &PlaylistController::removeMissing);
    connect(m_drawer, &PlaylistDrawer::removeDuplicatesRequested, this, &PlaylistController::removeDuplicates);
    connect(m_drawer, &PlaylistDrawer::savePlaylistRequested, this, &PlaylistController::savePlaylistDialog);
    connect(m_drawer, &PlaylistDrawer::downloadRequested, this, [this](const QList<int> &rows, MediaDownloader::Format format) {
        refresh();
        int queued = 0;
        QString name;
        for (int row : rows) {
            if (row < 0 || row >= m_entries.size())
                continue;
            const PlaylistOps::Entry &entry = m_entries[row];
            // Spotify songs and streams carry their name as the title.
            if (m_downloads->enqueue(entry.filename, entry.title, format)) {
                ++queued;
                name = PlaylistOps::displayName(entry);
            }
        }
        if (queued > 0)
            Q_EMIT message(tr("Downloading"), queued == 1 ? name : tr("%n entries", nullptr, queued));
        else if (MediaDownloader::executable().isEmpty())
            Q_EMIT message(tr("Install yt-dlp to download"));
    });
    connect(m_downloads, &DownloadQueue::statusChanged, m_drawer, &PlaylistDrawer::setDownloadStatus);
    connect(m_downloads, &DownloadQueue::finished, this,
            [this](const QString &, bool ok, const QStringList &files, const QString &error) {
                if (ok)
                    Q_EMIT message(tr("Downloaded:"), QFileInfo(files.value(0)).fileName());
                else
                    Q_EMIT message(tr("Download failed"), error.section(QLatin1Char('\n'), 0, 0).left(120));
            });

    m_playlistTimer.setSingleShot(true);
    m_playlistTimer.setInterval(kPlaylistRefreshMs);
    connect(&m_playlistTimer, &QTimer::timeout, this, &PlaylistController::updateDrawer);

    LibraryPanel *library = m_drawer->libraryPanel();
    library->setLibrary(m_library);
    connect(library, &LibraryPanel::playRequested, this, &PlaylistController::playFromLibrary);
    connect(library, &LibraryPanel::queueRequested, this, &PlaylistController::queueFromLibrary);
    connect(library, &LibraryPanel::addFolderRequested, this, &PlaylistController::addFolderToLibraryDialog);
    connect(library, &LibraryPanel::addPlaylistFileRequested, this, &PlaylistController::addPlaylistFileToLibraryDialog);
    connect(library, &LibraryPanel::saveQueueRequested, this, &PlaylistController::saveQueueToLibraryDialog);
    connect(library, &LibraryPanel::overwritePlaylistRequested, this, [this](const QString &path) {
        refresh();
        if (m_entries.isEmpty())
            Q_EMIT message(tr("Playlist is empty"));
        else
            savePlaylist(path);
    });

    m_durationTimer.setSingleShot(true);
    m_durationTimer.setInterval(kDurationRefreshMs);
    connect(&m_durationTimer, &QTimer::timeout, this, [this] { m_drawer->setDurations(durations()); });
    connect(m_prober, &MediaProber::durationKnown, this, [this] {
        if (!m_durationTimer.isActive())
            m_durationTimer.start();
        scheduleSave();
    });
    connect(m_prober, &MediaProber::finished, this, [this] {
        if (const auto pending = std::exchange(m_pendingSort, std::nullopt))
            sort(pending->first, pending->second);
    });

    m_saveTimer.setSingleShot(true);
    m_saveTimer.setInterval(kSaveDelayMs);
    connect(&m_saveTimer, &QTimer::timeout, this, [this] { saveSession(); });
    m_positionTimer.setInterval(kPositionSaveMs);
    connect(&m_positionTimer, &QTimer::timeout, this, [this] {
        if (!m_mpv->isIdle())
            saveSession();
    });
    // The playing entry changes without the playlist changing.
    connect(m_mpv, &MpvWidget::propertyUpdated, this, [this](const QString &name, const QVariant &value) {
        if (name != QLatin1String("playlist-pos"))
            return;
        // The highlight follows at once: mpv doesn't always report the
        // playlist again when only the playing entry changes.
        if (value.isValid() && value.toInt() >= 0)
            m_drawer->setPlayingRow(value.toInt());
        scheduleSave();
    });
}

void PlaylistController::setPlaylist(const QVariantList &playlist)
{
    m_playlist = playlist;
    m_entries = PlaylistOps::fromMpv(playlist);
    if (!m_playlistTimer.isActive())
        m_playlistTimer.start();
}

void PlaylistController::updateDrawer()
{
    m_drawer->setEntries(m_playlist, durations());
    QStringList files;
    files.reserve(m_entries.size());
    for (const PlaylistOps::Entry &entry : std::as_const(m_entries))
        files.append(entry.filename);
    // Most reports only move the playing entry; the files are probed already.
    if (files != m_probedFiles) {
        m_prober->probe(files);
        m_probedFiles = files;
    }
    scheduleSave();
}

void PlaylistController::refresh()
{
    // The last report of the playlist can lag behind commands sent since, e.g.
    // by an action just before; operate on mpv's current playlist instead.
    m_playlist = m_mpv->mpvProperty(QStringLiteral("playlist")).toList();
    m_entries = PlaylistOps::fromMpv(m_playlist);
}

QList<double> PlaylistController::durations() const
{
    QList<double> result;
    result.reserve(m_entries.size());
    for (const PlaylistOps::Entry &entry : m_entries)
        result.append(m_prober->duration(entry.filename));
    return result;
}

QList<PlaylistOps::Entry> PlaylistController::entries()
{
    refresh();
    QList<PlaylistOps::Entry> result = m_entries;
    for (PlaylistOps::Entry &entry : result) {
        entry.duration = m_prober->duration(entry.filename);
        const QString local = MediaFiles::localPath(entry.filename);
        const QFileInfo info(local);
        entry.size = !local.isEmpty() && info.isFile() ? info.size() : -1;
    }
    return result;
}

void PlaylistController::addFilesDialog()
{
    const QStringList files = QFileDialog::getOpenFileNames(m_dialogParent, tr("Add to Playlist"), {},
                                                            MediaFiles::mediaFileFilter());
    if (!files.isEmpty())
        m_mpv->insertFiles(files);
}

void PlaylistController::addFolderDialog()
{
    const QString folder = QFileDialog::getExistingDirectory(m_dialogParent, tr("Add Folder to Playlist"));
    if (!folder.isEmpty())
        addFolder(folder);
}

namespace {

// "Music" for /home/me/Music, "/" for the root.
QString folderName(const QString &folder)
{
    const QString name = QFileInfo(QDir::cleanPath(folder)).fileName();
    return name.isEmpty() ? QDir::cleanPath(folder) : name;
}

// "5 items from Music", or with `added`, "Added 5 items from Music".
QString itemsFrom(qsizetype count, const QString &source, bool added = false)
{
    if (added) {
        return count == 1 ? PlaylistController::tr("Added 1 item from %1").arg(source)
                          : PlaylistController::tr("Added %1 items from %2").arg(count).arg(source);
    }
    return count == 1 ? PlaylistController::tr("1 item from %1").arg(source)
                      : PlaylistController::tr("%1 items from %2").arg(count).arg(source);
}

} // namespace

void PlaylistController::addFolder(const QString &folder)
{
    const QString name = folderName(folder);
    // A folder that is gone would otherwise be queued as a file.
    if (!QFileInfo(folder).isDir()) {
        Q_EMIT message(tr("Folder not found"), name);
        return;
    }
    Q_EMIT message(tr("Scanning Folder"), name);
    // Opening something else meanwhile replaces the playlist this was meant for.
    const quint64 ticket = m_mpv->replaceTicket();
    MediaFiles::expandFoldersAsync({folder}, this, [this, name, ticket](const QStringList &files) {
        if (ticket != m_mpv->replaceTicket())
            return;
        if (files.isEmpty()) {
            Q_EMIT message(tr("No media files in"), name);
            return;
        }
        // One batch for mpv, however many files: playback goes on meanwhile.
        m_mpv->insertFiles(files);
        Q_EMIT message(tr("Added to Playlist"), itemsFrom(files.size(), name, true));
    });
}

void PlaylistController::openFolder(const QString &folder)
{
    const QString name = folderName(folder);
    // A folder that is gone would otherwise be queued as a file.
    if (!QFileInfo(folder).isDir()) {
        Q_EMIT message(tr("Folder not found"), name);
        return;
    }
    Q_EMIT message(tr("Scanning Folder"), name);
    // A later open, or a slower scan finishing after a faster one, must not
    // put this folder back: only the newest request is played.
    const quint64 ticket = m_mpv->reserveReplace();
    MediaFiles::expandFoldersAsync({folder}, this, [this, name, ticket](const QStringList &files) {
        if (ticket != m_mpv->replaceTicket())
            return;
        if (files.isEmpty()) {
            Q_EMIT message(tr("No media files in"), name);
            return;
        }
        m_mpv->continueReplace();
        m_mpv->loadFiles(files);
        Q_EMIT message(tr("Opened Folder"), itemsFrom(files.size(), name));
    });
}

void PlaylistController::openPlaylistDialog()
{
    const QString path = QFileDialog::getOpenFileName(m_dialogParent, tr("Open Playlist"), {}, MediaFiles::playlistFileFilter());
    if (!path.isEmpty())
        openPlaylist(path);
}

void PlaylistController::openPlaylist(const QString &path)
{
    const QString name = QFileInfo(path).fileName();
    const quint64 ticket = m_mpv->reserveReplace();
    auto *watcher = new QFutureWatcher<PlaylistOps::PlaylistContents>(this);
    connect(watcher, &QFutureWatcherBase::finished, this, [this, watcher, name, ticket] {
        watcher->deleteLater();
        if (ticket != m_mpv->replaceTicket())
            return;
        const PlaylistOps::PlaylistContents contents = watcher->result();
        if (!contents.readable) {
            Q_EMIT message(tr("Could not open playlist"), name);
            return;
        }
        if (contents.entries.isEmpty()) {
            Q_EMIT message(contents.missing > 0 ? tr("Playlist files not found") : tr("Playlist is empty"), name);
            return;
        }
        QStringList files;
        QStringList titles;
        for (const PlaylistOps::Entry &entry : contents.entries) {
            files.append(entry.filename);
            titles.append(entry.title);
        }
        m_mpv->continueReplace();
        m_mpv->loadTitledFiles(files, titles);
        QString summary = itemsFrom(files.size(), name);
        if (contents.missing > 0)
            summary += QStringLiteral(" · ") + tr("%1 missing skipped").arg(contents.missing);
        Q_EMIT message(tr("Opened Playlist"), summary);
    });
    watcher->setFuture(QtConcurrent::run(&PlaylistOps::loadPlaylist, path));
}

void PlaylistController::addEntries(const QStringList &entries, int row)
{
    const quint64 ticket = m_mpv->replaceTicket();
    MediaFiles::expandFoldersAsync(entries, this, [this, row, ticket](const QStringList &files) {
        if (!files.isEmpty() && ticket == m_mpv->replaceTicket())
            m_mpv->insertFiles(files, row);
    });
}

void PlaylistController::openEntries(const QStringList &entries, const QStringList &subtitles)
{
    const quint64 ticket = m_mpv->reserveReplace();
    MediaFiles::expandFoldersAsync(entries, this, [this, subtitles, ticket](const QStringList &files) {
        if (ticket != m_mpv->replaceTicket())
            return;
        if (files.isEmpty()) {
            Q_EMIT message(tr("No media files found"));
            return;
        }
        m_mpv->continueReplace();
        m_mpv->loadFiles(files, subtitles);
    });
}

void PlaylistController::addUrlDialog()
{
    bool ok = false;
    const QString text = QInputDialog::getText(m_dialogParent, tr("Add URL"), tr("Video or audio URL:"),
                                               QLineEdit::Normal, {}, &ok).trimmed();
    if (!ok || text.isEmpty())
        return;
    const QUrl url = text.contains(QLatin1String("://")) ? QUrl(text) : QUrl::fromUserInput(text);
    if (!url.isValid() || url.scheme().isEmpty()) {
        Q_EMIT message(tr("Invalid URL"), text);
        return;
    }
    m_mpv->insertFiles({url.isLocalFile() ? url.toLocalFile() : (text.contains(QLatin1String("://")) ? text : url.toString())});
    Q_EMIT message(tr("Added to Playlist"), text);
}

void PlaylistController::savePlaylistDialog()
{
    refresh();
    if (m_entries.isEmpty()) {
        Q_EMIT message(tr("Playlist is empty"));
        return;
    }
    QString filter;
    QString path = QFileDialog::getSaveFileName(m_dialogParent, tr("Save Playlist"),
                                                QDir::home().filePath(tr("Playlist") + QStringLiteral(".m3u8")),
                                                MediaFiles::playlistSaveFilter(), &filter);
    if (path.isEmpty())
        return;
    const QString suffix = QFileInfo(path).suffix().toLower();
    if (suffix != QLatin1String("m3u") && suffix != QLatin1String("m3u8"))
        path += filter.contains(QLatin1String("*.m3u)")) ? QStringLiteral(".m3u") : QStringLiteral(".m3u8");
    savePlaylist(path);
}

bool PlaylistController::savePlaylist(const QString &path)
{
    QString error;
    if (!PlaylistOps::writeM3u(path, entries(), &error)) {
        Q_EMIT message(tr("Could not save playlist"), error);
        return false;
    }
    Q_EMIT message(tr("Playlist Saved"), QFileInfo(path).fileName());
    return true;
}

void PlaylistController::sort(PlaylistOps::SortKey key, bool ascending)
{
    refresh();
    if (key == PlaylistOps::SortKey::Duration) {
        // Wait for the durations still being read, so the order is complete.
        const bool waiting = std::any_of(m_entries.cbegin(), m_entries.cend(), [this](const PlaylistOps::Entry &entry) {
            return !m_prober->hasResult(entry.filename) && m_prober->isBusy();
        });
        if (waiting) {
            m_pendingSort = std::make_pair(key, ascending);
            Q_EMIT message(tr("Reading durations..."));
            return;
        }
    }
    applyOrder(PlaylistOps::sortOrder(entries(), key, ascending));
}

void PlaylistController::reverse()
{
    refresh();
    QList<int> order(m_entries.size());
    std::iota(order.rbegin(), order.rend(), 0);
    applyOrder(order);
}

void PlaylistController::shiftRows(const QList<int> &rows, PlaylistOps::Shift shift)
{
    refresh();
    // The drawer keeps the moved entries selected (it follows mpv's entry ids).
    applyOrder(PlaylistOps::shiftOrder(static_cast<int>(m_entries.size()), rows, shift));
}

void PlaylistController::shuffle()
{
    refresh();
    if (m_entries.size() < 2)
        return;
    m_mpv->command({QStringLiteral("playlist-shuffle")});
    Q_EMIT message(tr("Playlist Shuffled"));
}

void PlaylistController::applyOrder(const QList<int> &order)
{
    // mpv runs the commands in order, so each move sees the previous ones done.
    for (const auto &[from, to] : PlaylistOps::movesForOrder(order))
        m_mpv->command({QStringLiteral("playlist-move"), QString::number(from), QString::number(to)});
}

void PlaylistController::removeRows(QList<int> rows)
{
    // Remove from the bottom up so earlier indexes stay valid.
    std::sort(rows.begin(), rows.end(), std::greater<>());
    rows.erase(std::unique(rows.begin(), rows.end()), rows.end());
    for (int row : std::as_const(rows))
        m_mpv->command({QStringLiteral("playlist-remove"), QString::number(row)});
}

void PlaylistController::clear()
{
    m_mpv->command({QStringLiteral("playlist-clear")});
}

void PlaylistController::removeMissing()
{
    refresh();
    const QList<int> rows = PlaylistOps::missingRows(m_entries);
    removeRows(rows);
    Q_EMIT message(tr("Removed Missing Files"), QString::number(rows.size()));
}

void PlaylistController::removeDuplicates()
{
    refresh();
    int current = -1;
    for (int row = 0; row < m_playlist.size(); ++row) {
        if (m_playlist[row].toMap().value(QStringLiteral("current")).toBool())
            current = row;
    }
    const QList<int> rows = PlaylistOps::duplicateRows(m_entries, current);
    removeRows(rows);
    Q_EMIT message(tr("Removed Duplicates"), QString::number(rows.size()));
}

bool PlaylistController::startSession(bool restore, bool handoff)
{
    m_sessionStarted = true;
    m_positionTimer.start();
    if (!restore || (!handoff && !PlaylistSession::rememberPlaylist()))
        return false;
    const std::optional<PlaylistSession::State> state = PlaylistSession::load();
    if (!state || state->entries.isEmpty())
        return false;

    QStringList files;
    for (const PlaylistOps::Entry &entry : state->entries) {
        files.append(entry.filename);
        m_prober->setDuration(entry.filename, entry.duration);
    }
    if (handoff || PlaylistSession::resumePlayback())
        m_mpv->restorePlaylist(files, state->current, state->position);
    else
        m_mpv->restorePlaylist(files, state->current);
    return true;
}

void PlaylistController::saveSession(bool handoff)
{
    m_saveTimer.stop();
    if (!handoff && (!m_sessionStarted || !PlaylistSession::rememberPlaylist()))
        return;

    // Read live: the last report may lag behind commands just sent.
    PlaylistSession::State state;
    state.entries = PlaylistOps::fromMpv(m_mpv->mpvProperty(QStringLiteral("playlist")).toList());
    for (PlaylistOps::Entry &entry : state.entries) {
        entry.duration = m_prober->duration(entry.filename);
        // Relative paths from the command line would not survive a different working directory.
        const QString local = MediaFiles::localPath(entry.filename);
        if (local == entry.filename && QFileInfo(local).isRelative())
            entry.filename = QFileInfo(local).absoluteFilePath();
    }
    const bool idle = m_mpv->isIdle();
    state.current = idle ? m_mpv->lastPlaylistPos() : m_mpv->mpvProperty(QStringLiteral("playlist-pos")).toInt();
    if (state.current >= state.entries.size())
        state.current = -1;
    // Songs start over next time; only a video picks up where it was left.
    if (!idle && (handoff || !m_mpv->isAudioOnly()) && !m_mpv->mpvProperty(QStringLiteral("eof-reached")).toBool())
        state.position = std::max(0.0, m_mpv->mpvProperty(QStringLiteral("time-pos")).toDouble());
    PlaylistSession::save(state);
}

void PlaylistController::scheduleSave()
{
    if (m_sessionStarted)
        m_saveTimer.start();
}

void PlaylistController::addFolderToLibraryDialog()
{
    const QString folder = QFileDialog::getExistingDirectory(m_dialogParent, tr("Add Folder to Library"));
    if (folder.isEmpty())
        return;
    if (m_library->add(MediaLibrary::Kind::Folder, folder))
        Q_EMIT message(tr("Added to Library"), QFileInfo(folder).fileName());
    else
        Q_EMIT message(tr("Already in Library"), QFileInfo(folder).fileName());
}

void PlaylistController::addPlaylistFileToLibraryDialog()
{
    const QStringList files = QFileDialog::getOpenFileNames(m_dialogParent, tr("Add Playlist to Library"), {},
                                                            MediaFiles::playlistFileFilter());
    int added = 0;
    for (const QString &file : files)
        added += m_library->add(MediaLibrary::Kind::Playlist, file) ? 1 : 0;
    if (added > 0)
        Q_EMIT message(tr("Added to Library"), tr("%n playlist(s)", nullptr, added));
}

void PlaylistController::saveQueueToLibraryDialog()
{
    refresh();
    if (m_entries.isEmpty()) {
        Q_EMIT message(tr("Playlist is empty"));
        return;
    }
    // Suggest the folder the queue comes from, e.g. a season of a show.
    QString suggestion = tr("Playlist");
    const QString first = MediaFiles::localPath(m_entries.first().filename);
    if (!first.isEmpty())
        suggestion = QFileInfo(first).absoluteDir().dirName();
    bool ok = false;
    const QString name = QInputDialog::getText(m_dialogParent, tr("Save to Library"), tr("Playlist name:"),
                                               QLineEdit::Normal, suggestion, &ok);
    if (ok && !name.trimmed().isEmpty())
        saveQueueToLibrary(name);
}

QString PlaylistController::saveQueueToLibrary(const QString &name)
{
    refresh();
    if (m_entries.isEmpty()) {
        Q_EMIT message(tr("Playlist is empty"));
        return {};
    }
    QDir().mkpath(MediaLibrary::playlistsDir());
    const QString path = MediaLibrary::newPlaylistPath(name);
    if (!savePlaylist(path))
        return {};
    m_library->add(MediaLibrary::Kind::Playlist, path, name);
    Q_EMIT message(tr("Saved to Library"), name.trimmed());
    return path;
}

void PlaylistController::playFromLibrary(LibraryPanel::EntryType type, const QString &path, int start)
{
    using EntryType = LibraryPanel::EntryType;
    switch (type) {
    case EntryType::Folder: {
        const quint64 ticket = m_mpv->reserveReplace();
        MediaFiles::expandFoldersAsync({path}, this, [this, path, ticket](const QStringList &files) {
            if (ticket != m_mpv->replaceTicket())
                return;
            if (files.isEmpty()) {
                Q_EMIT message(tr("No media files in"), QFileInfo(path).fileName());
                return;
            }
            m_mpv->continueReplace();
            m_mpv->loadFiles(files);
        });
        break;
    }
    case EntryType::Playlist:
        if (start < 0) {
            openPlaylist(path);
        } else {
            const QStringList files = PlaylistOps::readPlaylist(path);
            if (!files.isEmpty())
                m_mpv->playFiles(files, start);
        }
        break;
    case EntryType::File: {
        // Play the file's folder from that file on, so the next episode follows.
        const QString local = MediaFiles::localPath(path);
        if (local.isEmpty()) {
            m_mpv->loadFiles({path});
            break;
        }
        const QFileInfo info(local);
        const quint64 ticket = m_mpv->reserveReplace();
        MediaFiles::expandFoldersAsync({info.absolutePath()}, this, [this, path, info, ticket](const QStringList &files) {
            if (ticket != m_mpv->replaceTicket())
                return;
            const qsizetype index = files.indexOf(info.absoluteFilePath());
            m_mpv->continueReplace();
            if (index < 0)
                m_mpv->loadFiles({path});
            else
                m_mpv->playFiles(files, static_cast<int>(index));
        });
        break;
    }
    }
}

void PlaylistController::queueFromLibrary(LibraryPanel::EntryType type, const QString &path)
{
    using EntryType = LibraryPanel::EntryType;
    switch (type) {
    case EntryType::Folder:
        addFolder(path);
        break;
    case EntryType::Playlist: {
        const QStringList files = PlaylistOps::readPlaylist(path);
        if (files.isEmpty()) {
            Q_EMIT message(tr("Playlist is empty"));
            return;
        }
        m_mpv->insertFiles(files);
        Q_EMIT message(tr("Added to Playlist"), itemsFrom(files.size(), QFileInfo(path).fileName(), true));
        break;
    }
    case EntryType::File:
        m_mpv->insertFiles({path});
        Q_EMIT message(tr("Added to Playlist"), QFileInfo(MediaFiles::localPath(path)).fileName());
        break;
    }
}
