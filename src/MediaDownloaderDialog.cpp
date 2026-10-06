#include "MediaDownloaderDialog.h"
#include "PlaylistSession.h"

#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QProgressBar>
#include <QPushButton>
#include <QSettings>
#include <QStandardPaths>
#include <QVBoxLayout>

namespace {

const QColor kErrorColor(0xFF, 0x6B, 0x5B);

QString settingsFile()
{
    return PlaylistSession::configDir() + QStringLiteral("/settings.ini");
}

} // namespace

MediaDownloaderDialog::MediaDownloaderDialog(QWidget *parent)
    : QDialog(parent)
    , m_downloader(new MediaDownloader(this))
    , m_url(new QLineEdit(this))
    , m_site(new QLabel(this))
    , m_format(new QComboBox(this))
    , m_directory(new QLineEdit(this))
    , m_play(new QCheckBox(tr("Play immediately upon download"), this))
    , m_progress(new QProgressBar(this))
    , m_speed(new QLabel(this))
    , m_status(new QLabel(this))
    , m_warning(new QLabel(this))
    , m_downloadButton(new QPushButton(tr("Download"), this))
    , m_streamButton(new QPushButton(tr("Direct Stream (No Download)"), this))
{
    setObjectName(QStringLiteral("MediaDownloaderDialog"));
    setWindowTitle(tr("Download from URL"));
    setMinimumWidth(560);
    const QSettings settings(settingsFile(), QSettings::IniFormat);

    m_url->setObjectName(QStringLiteral("DownloaderUrl"));
    m_url->setPlaceholderText(tr("https://www.youtube.com/watch?v=...  or  https://open.spotify.com/track/..."));
    m_url->setClearButtonEnabled(true);
    // Offer a link that is already on the clipboard.
    const QString clipboard = QGuiApplication::clipboard()->text().trimmed();
    if (MediaDownloader::isKnownSite(clipboard))
        m_url->setText(MediaDownloader::normalizeUrl(clipboard));
    m_site->setObjectName(QStringLiteral("DownloaderSite"));

    m_format->setObjectName(QStringLiteral("DownloaderFormat"));
    m_format->addItem(tr("Best Video + Audio (MP4)"), int(MediaDownloader::Format::Best));
    m_format->addItem(tr("4K (2160p) Video"), int(MediaDownloader::Format::Max2160));
    m_format->addItem(tr("1080p Video"), int(MediaDownloader::Format::Max1080));
    m_format->addItem(tr("720p Video"), int(MediaDownloader::Format::Max720));
    m_format->addItem(tr("Audio Only (.mp3)"), int(MediaDownloader::Format::AudioMp3));
    m_format->setCurrentIndex(std::max(0, m_format->findData(settings.value(QStringLiteral("downloader/format"), 0).toInt())));

    m_directory->setObjectName(QStringLiteral("DownloaderDirectory"));
    m_directory->setText(defaultDirectory());
    auto *browse = new QPushButton(tr("Browse..."), this);
    browse->setAutoDefault(false);
    auto *directoryRow = new QHBoxLayout;
    directoryRow->addWidget(m_directory, 1);
    directoryRow->addWidget(browse);

    m_play->setObjectName(QStringLiteral("DownloaderPlay"));
    m_play->setChecked(settings.value(QStringLiteral("downloader/play"), true).toBool());

    m_progress->setObjectName(QStringLiteral("DownloaderProgress"));
    m_progress->setRange(0, 1000);
    m_progress->setTextVisible(false);
    m_progress->setFixedHeight(6);
    m_progress->setVisible(false);
    m_speed->setObjectName(QStringLiteral("DownloaderSpeed"));
    m_status->setObjectName(QStringLiteral("DownloaderStatus"));
    m_status->setWordWrap(true);

    // Without yt-dlp only plain media URLs can play; say how to get it.
    m_warning->setObjectName(QStringLiteral("DownloaderWarning"));
    m_warning->setWordWrap(true);
    m_warning->setTextFormat(Qt::RichText);
    m_warning->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_warning->setText(tr("<b>yt-dlp is not installed.</b> Install it to download and stream from these sites:<br>"
                          "<code>sudo dnf install yt-dlp</code> (Fedora), <code>sudo apt install yt-dlp</code> "
                          "(Ubuntu, Debian) or <code>pip install --user yt-dlp</code>"));
    m_warning->setStyleSheet(QStringLiteral("QLabel { background: #2a2418; border: 1px solid #5a4a20; "
                                            "border-radius: 4px; padding: 8px; color: #f0d9a8; }"));
    m_warning->setVisible(MediaDownloader::executable().isEmpty());

    auto *form = new QFormLayout;
    form->addRow(tr("URL:"), m_url);
    form->addRow(QString(), m_site);
    form->addRow(tr("Format:"), m_format);
    form->addRow(tr("Save to:"), directoryRow);
    form->addRow(QString(), m_play);

    m_downloadButton->setObjectName(QStringLiteral("DownloaderDownloadButton"));
    m_downloadButton->setDefault(true);
    m_streamButton->setObjectName(QStringLiteral("DownloaderStreamButton"));
    m_streamButton->setAutoDefault(false);
    auto *close = new QPushButton(tr("Close"), this);
    close->setAutoDefault(false);
    auto *statusRow = new QHBoxLayout;
    statusRow->addWidget(m_status, 1);
    statusRow->addWidget(m_speed);
    auto *buttons = new QHBoxLayout;
    buttons->addWidget(m_streamButton);
    buttons->addStretch();
    buttons->addWidget(m_downloadButton);
    buttons->addWidget(close);

    auto *layout = new QVBoxLayout(this);
    layout->addWidget(m_warning);
    layout->addLayout(form);
    layout->addWidget(m_progress);
    layout->addLayout(statusRow);
    layout->addLayout(buttons);

    connect(m_url, &QLineEdit::textChanged, this, &MediaDownloaderDialog::updateState);
    connect(m_url, &QLineEdit::returnPressed, this, &MediaDownloaderDialog::startDownload);
    connect(m_directory, &QLineEdit::textChanged, this, &MediaDownloaderDialog::updateState);
    connect(browse, &QPushButton::clicked, this, [this] {
        const QString dir = QFileDialog::getExistingDirectory(this, tr("Save Downloads To"), directory());
        if (!dir.isEmpty())
            m_directory->setText(dir);
    });
    connect(m_downloadButton, &QPushButton::clicked, this, &MediaDownloaderDialog::startDownload);
    connect(m_streamButton, &QPushButton::clicked, this, &MediaDownloaderDialog::streamDirectly);
    connect(close, &QPushButton::clicked, this, &MediaDownloaderDialog::reject);

    connect(m_downloader, &MediaDownloader::progress, this, [this](const MediaDownloader::Progress &progress) {
        m_progress->setValue(qRound(progress.percent * 10));
        // yt-dlp prints no stage lines when quiet; tell them apart by progress.
        setStatus(progress.percent < 100 ? tr("Downloading...") : tr("Finishing..."));
        QStringList parts;
        parts << QStringLiteral("%1%").arg(progress.percent, 0, 'f', 1);
        if (!progress.total.isEmpty())
            parts << tr("of %1").arg(progress.total);
        if (!progress.speed.isEmpty())
            parts << QStringLiteral("· %1").arg(progress.speed);
        if (!progress.eta.isEmpty())
            parts << tr("· ETA %1").arg(progress.eta);
        m_speed->setText(parts.join(QLatin1Char(' ')));
    });
    connect(m_downloader, &MediaDownloader::stageChanged, this, [this](const QString &stage) { setStatus(stage); });
    connect(m_downloader, &MediaDownloader::finished, this, [this](bool ok, const QString &path, const QString &error) {
        setBusy(false);
        if (!ok) {
            setStatus(error, true);
            return;
        }
        m_progress->setValue(1000);
        const QStringList files = m_downloader->files();
        if (files.size() > 1)
            Q_EMIT downloadedMany(files, m_play->isChecked());
        else
            Q_EMIT downloaded(path, m_play->isChecked());
        accept();
    });
    connect(m_downloader, &MediaDownloader::spotifyResolved, this,
            [this](const QList<MediaDownloader::SpotifyTrack> &tracks, const QString &collection, const QString &error) {
                if (!m_resolving)
                    return;
                m_resolving = false;
                setBusy(false);
                if (tracks.isEmpty()) {
                    setStatus(error, true);
                    return;
                }
                QStringList urls;
                QStringList titles;
                for (const MediaDownloader::SpotifyTrack &track : tracks) {
                    urls << MediaDownloader::spotifyStreamUrl(track);
                    titles << (track.artist.isEmpty() ? track.title : track.artist + QStringLiteral(" - ") + track.title);
                }
                Q_UNUSED(collection);
                Q_EMIT tracksStreamRequested(urls, titles);
                accept();
            });
    updateState();
}

QString MediaDownloaderDialog::defaultDirectory()
{
    const QString saved = QSettings(settingsFile(), QSettings::IniFormat).value(QStringLiteral("downloader/directory")).toString();
    if (!saved.isEmpty() && QFileInfo(saved).isDir())
        return saved;
    for (const auto location : {QStandardPaths::MoviesLocation, QStandardPaths::DownloadLocation}) {
        const QString dir = QStandardPaths::writableLocation(location);
        if (!dir.isEmpty() && QFileInfo(dir).isDir() && dir != QDir::homePath())
            return dir;
    }
    return QDir::homePath();
}

QString MediaDownloaderDialog::url() const
{
    return MediaDownloader::normalizeUrl(m_url->text());
}

MediaDownloader::Format MediaDownloaderDialog::format() const
{
    return MediaDownloader::Format(m_format->currentData().toInt());
}

QString MediaDownloaderDialog::directory() const
{
    return QDir::cleanPath(QDir::fromNativeSeparators(m_directory->text().trimmed()));
}

bool MediaDownloaderDialog::playWhenDone() const
{
    return m_play->isChecked();
}

void MediaDownloaderDialog::updateState()
{
    const QString link = url();
    const QString site = MediaDownloader::platformName(link);
    const bool spotify = MediaDownloader::isSpotifyUrl(link);
    if (m_url->text().trimmed().isEmpty())
        m_site->setText(tr("Paste a link from YouTube, Spotify, TikTok, Instagram, X, Vimeo, ..."));
    else if (link.isEmpty())
        m_site->setText(tr("This is not a web link."));
    else if (spotify) {
        const std::optional<MediaDownloader::SpotifyLink> parsed = MediaDownloader::parseSpotifyLink(link);
        const QString type = parsed ? parsed->type : QString();
        m_site->setText(type == QLatin1String("album")      ? tr("Spotify album: every song is saved as an MP3")
                        : type == QLatin1String("playlist") ? tr("Spotify playlist: every song is saved as an MP3")
                        : type == QLatin1String("artist")   ? tr("Spotify artist: their top songs are saved as MP3s")
                                                            : tr("Spotify link: the song is found on YouTube and saved as an MP3"));
    } else if (!site.isEmpty())
        m_site->setText(tr("%1 link").arg(site));
    else
        m_site->setText(tr("Other site: yt-dlp will try it"));

    // Spotify has songs only: the format is MP3 while such a link is entered.
    const int audio = m_format->findData(int(MediaDownloader::Format::AudioMp3));
    if (spotify && m_formatBeforeSpotify < 0) {
        m_formatBeforeSpotify = m_format->currentIndex();
        m_format->setCurrentIndex(audio);
    } else if (!spotify && m_formatBeforeSpotify >= 0) {
        m_format->setCurrentIndex(m_formatBeforeSpotify);
        m_formatBeforeSpotify = -1;
    }
    m_format->setToolTip(spotify ? tr("Spotify songs are always saved as MP3") : QString());
    m_format->setEnabled(!isBusy() && !spotify);

    const bool haveTool = !MediaDownloader::executable().isEmpty();
    const bool busy = isBusy();
    m_downloadButton->setEnabled(haveTool && !busy && !link.isEmpty() && !directory().isEmpty());
    // mpv streams these sites through yt-dlp as well.
    m_streamButton->setEnabled(haveTool && !busy && !link.isEmpty());
}

void MediaDownloaderDialog::setStatus(const QString &text, bool error)
{
    m_status->setText(text);
    QPalette palette = m_status->palette();
    palette.setColor(QPalette::WindowText, error ? kErrorColor : QApplication::palette().color(QPalette::WindowText));
    m_status->setPalette(palette);
}

void MediaDownloaderDialog::setBusy(bool busy)
{
    m_url->setEnabled(!busy);
    m_format->setEnabled(!busy && !isSpotify());
    m_directory->setEnabled(!busy);
    m_progress->setVisible(busy || m_progress->value() > 0);
    if (!busy)
        m_speed->clear();
    updateState();
}

void MediaDownloaderDialog::startDownload()
{
    const QString link = url();
    if (link.isEmpty() || isBusy() || MediaDownloader::executable().isEmpty())
        return;
    if (!QDir().mkpath(directory()) || !QFileInfo(directory()).isWritable()) {
        setStatus(tr("Can't save to %1.").arg(directory()), true);
        return;
    }
    QSettings settings(settingsFile(), QSettings::IniFormat);
    settings.setValue(QStringLiteral("downloader/directory"), directory());
    if (!isSpotify())
        settings.setValue(QStringLiteral("downloader/format"), int(format()));
    settings.setValue(QStringLiteral("downloader/play"), m_play->isChecked());

    m_progress->setValue(0);
    m_progress->setVisible(true);
    if (!m_downloader->start(link, format(), directory())) {
        setStatus(tr("Could not start yt-dlp."), true);
        return;
    }
    setBusy(true);
}

void MediaDownloaderDialog::streamDirectly()
{
    const QString link = url();
    if (link.isEmpty() || isBusy())
        return;
    if (MediaDownloader::isSpotifyUrl(link)) {
        // Read the songs first; each streams from its YouTube match.
        m_resolving = true;
        setBusy(true);
        setStatus(tr("Reading the Spotify link..."));
        m_downloader->resolveSpotify(link);
        return;
    }
    Q_EMIT streamRequested(link, MediaDownloader::streamFormat(format()));
    accept();
}

void MediaDownloaderDialog::reject()
{
    // Esc first stops a running download, then closes.
    if (isBusy()) {
        m_resolving = false;
        m_downloader->cancel();
        setBusy(false);
        m_progress->setVisible(false);
        setStatus(tr("Download cancelled."));
        return;
    }
    QDialog::reject();
}
