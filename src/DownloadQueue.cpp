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

bool DownloadQueue::enqueue(const QString &entry, const QString &title, MediaDownloader::Format format)
{
    if (!canDownload(entry) || MediaDownloader::executable().isEmpty())
        return false;
    if ((m_current && m_current->entry == entry)
        || std::any_of(m_jobs.cbegin(), m_jobs.cend(), [&](const Job &job) { return job.entry == entry; }))
        return false;
    m_jobs.append({entry, title, format});
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
        if (!m_downloader->startEntry(job.entry, job.title, job.format, directory())) {
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
