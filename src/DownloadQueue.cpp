#include "DownloadQueue.h"
#include "MediaDownloaderDialog.h"

#include <cmath>
#include <utility>

DownloadQueue::DownloadQueue(QObject *parent)
    : QObject(parent)
    , m_downloader(new MediaDownloader(this))
{
    connect(m_downloader, &MediaDownloader::progress, this, [this](const MediaDownloader::Progress &progress) {
        if (m_current && progress.percent >= 0)
            setStatus(m_current->entry, QStringLiteral("%1%").arg(std::lround(std::min(progress.percent, 99.0))));
    });
    connect(m_downloader, &MediaDownloader::finished, this, [this](bool ok, const QString &, const QString &error) {
        if (!m_current)
            return;
        const Job job = *std::exchange(m_current, std::nullopt);
        setStatus(job.entry, ok ? tr("Saved") : tr("Failed"));
        Q_EMIT finished(job.entry, ok, ok ? m_downloader->files() : QStringList(), error);
        startNext();
    });
}

bool DownloadQueue::canDownload(const QString &entry)
{
    return !MediaDownloader::downloadSource(entry).isEmpty();
}

QString DownloadQueue::directory() const
{
    return m_directory.isEmpty() ? MediaDownloaderDialog::defaultDirectory() : m_directory;
}

bool DownloadQueue::enqueue(const QString &entry, const QString &title, MediaDownloader::Format format,
                            const QString &directory)
{
    if (!canDownload(entry))
        return false;
    return add({entry, title, format, directory, std::nullopt});
}

bool DownloadQueue::enqueueTrack(const QString &entry, const MediaDownloader::SpotifyTrack &track,
                                 const QString &directory)
{
    if (entry.isEmpty() || track.title.isEmpty())
        return false;
    return add({entry, track.title, MediaDownloader::Format::AudioMp3, directory, track});
}

bool DownloadQueue::add(const Job &job)
{
    if (MediaDownloader::executable().isEmpty())
        return false;
    const QString &entry = job.entry;
    if ((m_current && m_current->entry == entry)
        || std::any_of(m_jobs.cbegin(), m_jobs.cend(), [&](const Job &queued) { return queued.entry == entry; }))
        return false;
    m_jobs.append(job);
    setStatus(entry, tr("Queued"));
    if (!m_current)
        startNext();
    return true;
}

void DownloadQueue::startNext()
{
    while (!m_current && !m_jobs.isEmpty()) {
        const Job job = m_jobs.takeFirst();
        m_current = job;
        setStatus(job.entry, QStringLiteral("0%"));
        const QString folder = job.directory.isEmpty() ? directory() : job.directory;
        const bool started = job.track ? m_downloader->startTracks({*job.track}, folder)
                                       : m_downloader->startEntry(job.entry, job.title, job.format, folder);
        if (!started) {
            m_current.reset();
            setStatus(job.entry, tr("Failed"));
            Q_EMIT finished(job.entry, false, {}, tr("Could not start yt-dlp."));
        }
    }
}

void DownloadQueue::cancelAll()
{
    for (const Job &job : std::exchange(m_jobs, {}))
        setStatus(job.entry, QString());
    if (const auto current = std::exchange(m_current, std::nullopt)) {
        m_downloader->cancel();
        setStatus(current->entry, QString());
    }
}

void DownloadQueue::setStatus(const QString &entry, const QString &status)
{
    if (m_status.value(entry) == status)
        return;
    if (status.isEmpty())
        m_status.remove(entry);
    else
        m_status.insert(entry, status);
    Q_EMIT statusChanged(entry, status);
}
