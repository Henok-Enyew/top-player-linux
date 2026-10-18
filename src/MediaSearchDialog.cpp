#include "MediaSearchDialog.h"
#include "DownloadQueue.h"
#include "HostTools.h"
#include "MediaDownloaderDialog.h"
#include "PlaylistSession.h"
#include "Theme.h"

#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QHideEvent>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPainter>
#include <QPainterPath>
#include <QProgressBar>
#include <QPushButton>
#include <QSettings>
#include <QStyle>
#include <QStyledItemDelegate>
#include <QTabBar>
#include <QToolButton>
#include <QTreeWidget>
#include <QVBoxLayout>

#include <algorithm>
#include <utility>

namespace {

using Kind = MediaSearch::Kind;

// Videos listed per page of a YouTube search, and at most.
constexpr int kYouTubePage = 20;
constexpr int kYouTubeMax = 200;
// Spotify answers results up to this offset.
constexpr int kSpotifyMaxOffset = 1000;
// Cover art kept in memory before the cache starts over.
constexpr int kArtCacheSize = 600;
constexpr int kArtTimeoutMs = 15000;

const QSize kVideoIcon(96, 54);
const QSize kCoverIcon(48, 48);

enum Column { TitleColumn, ArtistColumn, InfoColumn, ExtraColumn, StatusColumn, ColumnCount };

constexpr int kUrlRole = Qt::UserRole;
constexpr int kArtRole = Qt::UserRole + 1;

QString settingsFile()
{
    return PlaylistSession::configDir() + QStringLiteral("/settings.ini");
}

// Paints a placeholder where art is still loading, and the download state
// in the theme's colors (read while painting, so they follow the theme).
class ResultDelegate : public QStyledItemDelegate
{
public:
    using QStyledItemDelegate::QStyledItemDelegate;

protected:
    void initStyleOption(QStyleOptionViewItem *option, const QModelIndex &index) const override
    {
        QStyledItemDelegate::initStyleOption(option, index);
        if (index.column() == TitleColumn) {
            // Room for the art before it arrives.
            option->features |= QStyleOptionViewItem::HasDecoration;
        } else if (index.column() == StatusColumn) {
            const QString status = index.data().toString();
            QColor color = Theme::TextSecondary;
            if (status == DownloadQueue::tr("Saved"))
                color = Theme::Accent;
            else if (status == DownloadQueue::tr("Failed"))
                color = Theme::TextPrimary;
            if (status == DownloadQueue::tr("Failed"))
                option->font.setBold(true);
            option->palette.setColor(QPalette::Text, color);
            option->palette.setColor(QPalette::HighlightedText, color);
        } else {
            option->palette.setColor(QPalette::Text, Theme::TextSecondary);
        }
    }

    void paint(QPainter *painter, const QStyleOptionViewItem &option, const QModelIndex &index) const override
    {
        QStyledItemDelegate::paint(painter, option, index);
        if (index.column() != TitleColumn || !index.data(Qt::DecorationRole).isNull())
            return;
        QStyleOptionViewItem opt = option;
        initStyleOption(&opt, index);
        const QWidget *widget = opt.widget;
        const QStyle *style = widget ? widget->style() : QApplication::style();
        const QRect rect = style->subElementRect(QStyle::SE_ItemViewItemDecoration, &opt, widget);
        painter->save();
        painter->setRenderHint(QPainter::Antialiasing);
        painter->setPen(Qt::NoPen);
        painter->setBrush(Theme::Raised);
        painter->drawRoundedRect(QRectF(rect).adjusted(0.5, 0.5, -0.5, -0.5), 3, 3);
        painter->restore();
    }
};

// `image` scaled and cropped to fill `size`.
QPixmap fitArt(const QImage &image, const QSize &size, qreal dpr)
{
    const QSize target = size * dpr;
    const QImage scaled = image.scaled(target, Qt::KeepAspectRatioByExpanding, Qt::SmoothTransformation);
    QPixmap pixmap = QPixmap::fromImage(scaled.copy((scaled.width() - target.width()) / 2,
                                                    (scaled.height() - target.height()) / 2,
                                                    target.width(), target.height()));
    pixmap.setDevicePixelRatio(dpr);
    return pixmap;
}

} // namespace

MediaSearchDialog::MediaSearchDialog(MediaSearch::Source source, QWidget *parent)
    : QDialog(parent)
    , m_source(source)
    , m_search(new MediaSearch(this))
    , m_queue(new DownloadQueue(this))
    , m_resolver(new MediaDownloader(this))
    , m_warning(new QLabel(this))
    , m_query(new QLineEdit(this))
    , m_searchButton(new QPushButton(tr("Search"), this))
    , m_view(new QTreeWidget(this))
    , m_moreButton(new QPushButton(tr("Load More"), this))
    , m_directory(new QLineEdit(this))
    , m_play(new QCheckBox(tr("Play immediately upon download"), this))
    , m_progress(new QProgressBar(this))
    , m_statusLabel(new QLabel(this))
    , m_cancelButton(new QPushButton(tr("Cancel Downloads"), this))
    , m_playButton(new QPushButton(tr("Play Now"), this))
    , m_queueButton(new QPushButton(tr("Add to Playlist"), this))
    , m_mp3Button(new QPushButton(tr("Download MP3"), this))
    , m_network(new QNetworkAccessManager(this))
{
    const bool spotify = source == MediaSearch::Source::Spotify;
    setObjectName(spotify ? QStringLiteral("SpotifySearchDialog") : QStringLiteral("YouTubeSearchDialog"));
    setWindowTitle(spotify ? tr("Search Spotify") : tr("Search YouTube"));
    resize(820, 600);
    const QSettings settings(settingsFile(), QSettings::IniFormat);

    // Without yt-dlp nothing can be played or saved; say how to get it.
    m_warning->setObjectName(QStringLiteral("MediaSearchWarning"));
    m_warning->setWordWrap(true);
    m_warning->setTextFormat(Qt::RichText);
    m_warning->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_warning->setText(tr("<b>yt-dlp is not installed.</b> Install it to download and stream from these sites:<br>"
                          "<code>sudo dnf install yt-dlp</code> (Fedora), <code>sudo apt install yt-dlp</code> "
                          "(Ubuntu, Debian) or <code>pip install --user yt-dlp</code>"));
    m_warning->setVisible(MediaDownloader::executable().isEmpty());

    auto *layout = new QVBoxLayout(this);
    layout->addWidget(m_warning);

    if (spotify) {
        // Spotify answers searches only from registered apps: the user's own.
        m_accessToggle = new QToolButton(this);
        m_accessToggle->setObjectName(QStringLiteral("SpotifyAccessToggle"));
        m_accessToggle->setText(tr("Spotify API access"));
        m_accessToggle->setCheckable(true);
        m_accessToggle->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
        m_accessToggle->setAutoRaise(true);
        m_accessPanel = new QWidget(this);
        m_accessPanel->setObjectName(QStringLiteral("SpotifyAccessPanel"));
        auto *explanation = new QLabel(m_accessPanel);
        explanation->setObjectName(QStringLiteral("SpotifyAccessHelp"));
        explanation->setWordWrap(true);
        explanation->setTextFormat(Qt::RichText);
        explanation->setOpenExternalLinks(true);
        explanation->setTextInteractionFlags(Qt::TextBrowserInteraction);
        explanation->setText(
            tr("Optional. Without it, songs, albums and playlists are found in Deezer's free catalog "
               "(the same songs, saved the same way). Spotify itself answers only registered apps whose "
               "owner has <b>Spotify Premium</b>: create a free app on the "
               "<a href=\"https://developer.spotify.com/dashboard\">Spotify Developer Dashboard</a> "
               "(any name and description, Redirect URI <code>http://127.0.0.1:8888/callback</code>, "
               "tick “Web API”), then copy its <b>Client ID</b> and <b>Client Secret</b> here. "
               "They are kept in Top Player's settings on this computer."));
        m_clientId = new QLineEdit(m_accessPanel);
        m_clientId->setObjectName(QStringLiteral("SpotifyClientId"));
        m_clientId->setText(settings.value(QStringLiteral("spotify/clientId")).toString());
        m_clientSecret = new QLineEdit(m_accessPanel);
        m_clientSecret->setObjectName(QStringLiteral("SpotifyClientSecret"));
        m_clientSecret->setEchoMode(QLineEdit::Password);
        m_clientSecret->setText(settings.value(QStringLiteral("spotify/clientSecret")).toString());
        auto *form = new QFormLayout;
        form->addRow(tr("Client ID:"), m_clientId);
        form->addRow(tr("Client Secret:"), m_clientSecret);
        auto *panel = new QVBoxLayout(m_accessPanel);
        panel->setContentsMargins(18, 0, 0, 6);
        panel->addWidget(explanation);
        panel->addLayout(form);
        layout->addWidget(m_accessToggle, 0, Qt::AlignLeft);
        layout->addWidget(m_accessPanel);
        connect(m_accessToggle, &QToolButton::toggled, this, &MediaSearchDialog::setAccessPanelOpen);
        connect(m_clientId, &QLineEdit::editingFinished, this, &MediaSearchDialog::saveCredentials);
        connect(m_clientSecret, &QLineEdit::editingFinished, this, &MediaSearchDialog::saveCredentials);
        m_search->setSpotifyCredentials(m_clientId->text(), m_clientSecret->text());
        // Closed: searching works without it (through Deezer's catalog).
        setAccessPanelOpen(false);
    }

    m_query->setObjectName(QStringLiteral("MediaSearchQuery"));
    m_query->setClearButtonEnabled(true);
    m_query->setPlaceholderText(spotify ? tr("Song, artist, album or playlist...") : tr("Song or video name..."));
    m_searchButton->setObjectName(QStringLiteral("MediaSearchButton"));
    auto *searchRow = new QHBoxLayout;
    searchRow->addWidget(m_query, 1);
    searchRow->addWidget(m_searchButton);
    layout->addLayout(searchRow);

    if (spotify) {
        m_tabs = new QTabBar(this);
        m_tabs->setObjectName(QStringLiteral("MediaSearchTabs"));
        m_tabs->setDrawBase(false);
        m_tabs->setExpanding(false);
        m_tabs->addTab(tr("Songs"));
        m_tabs->addTab(tr("Albums"));
        m_tabs->addTab(tr("Playlists"));
        layout->addWidget(m_tabs);
        connect(m_tabs, &QTabBar::currentChanged, this, [this] { showResults(); });
    }

    m_view->setObjectName(QStringLiteral("MediaSearchResults"));
    m_view->setColumnCount(ColumnCount);
    m_view->setRootIsDecorated(false);
    m_view->setUniformRowHeights(true);
    m_view->setSelectionMode(QAbstractItemView::ExtendedSelection);
    m_view->setIconSize(spotify ? kCoverIcon : kVideoIcon);
    m_view->setItemDelegate(new ResultDelegate(m_view));
    m_view->header()->setStretchLastSection(false);
    m_view->header()->setSectionResizeMode(TitleColumn, QHeaderView::Stretch);
    for (int column = ArtistColumn; column < ColumnCount; ++column)
        m_view->header()->setSectionResizeMode(column, QHeaderView::ResizeToContents);
    layout->addWidget(m_view, 1);

    m_moreButton->setObjectName(QStringLiteral("MediaSearchMore"));
    m_moreButton->setVisible(false);
    layout->addWidget(m_moreButton, 0, Qt::AlignHCenter);

    auto *options = new QFormLayout;
    if (!spotify) {
        m_format = new QComboBox(this);
        m_format->setObjectName(QStringLiteral("MediaSearchFormat"));
        m_format->addItem(tr("Best Video + Audio (MP4)"), int(MediaDownloader::Format::Best));
        m_format->addItem(tr("4K (2160p) Video"), int(MediaDownloader::Format::Max2160));
        m_format->addItem(tr("1080p Video"), int(MediaDownloader::Format::Max1080));
        m_format->addItem(tr("720p Video"), int(MediaDownloader::Format::Max720));
        m_format->setCurrentIndex(
            std::max(0, m_format->findData(settings.value(QStringLiteral("search/videoFormat"), 0).toInt())));
        m_format->setToolTip(tr("The quality of Download Video, Play Now and Add to Playlist"));
        options->addRow(tr("Video quality:"), m_format);
    }
    m_directory->setObjectName(QStringLiteral("MediaSearchDirectory"));
    m_directory->setText(MediaDownloaderDialog::defaultDirectory());
    auto *browse = new QPushButton(tr("Browse..."), this);
    auto *directoryRow = new QHBoxLayout;
    directoryRow->addWidget(m_directory, 1);
    directoryRow->addWidget(browse);
    options->addRow(tr("Save to:"), directoryRow);
    m_play->setObjectName(QStringLiteral("MediaSearchPlayWhenDone"));
    m_play->setChecked(settings.value(QStringLiteral("downloader/play"), true).toBool());
    options->addRow(QString(), m_play);
    layout->addLayout(options);

    m_progress->setObjectName(QStringLiteral("MediaSearchProgress"));
    m_progress->setRange(0, 1000);
    m_progress->setTextVisible(false);
    m_progress->setFixedHeight(6);
    m_progress->setVisible(false);
    m_statusLabel->setObjectName(QStringLiteral("MediaSearchStatus"));
    m_statusLabel->setWordWrap(true);
    m_cancelButton->setObjectName(QStringLiteral("MediaSearchCancel"));
    m_cancelButton->setVisible(false);
    auto *statusRow = new QHBoxLayout;
    statusRow->addWidget(m_statusLabel, 1);
    statusRow->addWidget(m_cancelButton);
    layout->addWidget(m_progress);
    layout->addLayout(statusRow);

    m_playButton->setObjectName(QStringLiteral("MediaSearchPlay"));
    m_playButton->setToolTip(tr("Stream the selection now (double-click a result does the same)"));
    m_queueButton->setObjectName(QStringLiteral("MediaSearchQueue"));
    m_queueButton->setToolTip(tr("Add the selection to the end of the playlist"));
    m_mp3Button->setObjectName(QStringLiteral("MediaSearchDownloadMp3"));
    auto *close = new QPushButton(tr("Close"), this);
    auto *buttons = new QHBoxLayout;
    buttons->addWidget(m_playButton);
    buttons->addWidget(m_queueButton);
    buttons->addStretch();
    if (!spotify) {
        m_videoButton = new QPushButton(tr("Download Video"), this);
        m_videoButton->setObjectName(QStringLiteral("MediaSearchDownloadVideo"));
        buttons->addWidget(m_videoButton);
        connect(m_videoButton, &QPushButton::clicked, this, [this] { downloadSelected(videoFormat()); });
    }
    buttons->addWidget(m_mp3Button);
    buttons->addWidget(close);
    layout->addLayout(buttons);

    // Enter in the query searches; no button takes it from there.
    for (QPushButton *button : findChildren<QPushButton *>()) {
        button->setAutoDefault(false);
        button->setDefault(false);
    }

    m_query->installEventFilter(this);
    connect(m_query, &QLineEdit::returnPressed, this, &MediaSearchDialog::startSearch);
    connect(m_query, &QLineEdit::textChanged, this, &MediaSearchDialog::updateState);
    connect(m_searchButton, &QPushButton::clicked, this, &MediaSearchDialog::startSearch);
    connect(m_moreButton, &QPushButton::clicked, this, &MediaSearchDialog::loadMore);
    connect(m_view, &QTreeWidget::itemSelectionChanged, this, &MediaSearchDialog::updateState);
    connect(m_view, &QTreeWidget::itemDoubleClicked, this, [this](QTreeWidgetItem *item) {
        const QString url = item->data(TitleColumn, kUrlRole).toString();
        const QList<MediaSearch::Result> all = results();
        const auto it = std::find_if(all.cbegin(), all.cend(), [&](const MediaSearch::Result &r) { return r.url == url; });
        if (it != all.cend())
            stream({*it}, false);
    });
    connect(m_directory, &QLineEdit::textChanged, this, &MediaSearchDialog::updateState);
    connect(browse, &QPushButton::clicked, this, [this] {
        const QString dir = HostTools::realPath(QFileDialog::getExistingDirectory(this, tr("Save Downloads To"), directory()));
        if (!dir.isEmpty())
            m_directory->setText(dir);
    });
    connect(m_playButton, &QPushButton::clicked, this, &MediaSearchDialog::playSelected);
    connect(m_queueButton, &QPushButton::clicked, this, &MediaSearchDialog::queueSelected);
    connect(m_mp3Button, &QPushButton::clicked, this, [this] { downloadSelected(MediaDownloader::Format::AudioMp3); });
    connect(m_cancelButton, &QPushButton::clicked, this, &MediaSearchDialog::cancelDownloads);
    connect(close, &QPushButton::clicked, this, &MediaSearchDialog::reject);

    connect(m_search, &MediaSearch::finished, this, &MediaSearchDialog::onResults);
    connect(m_queue, &DownloadQueue::statusChanged, this, &MediaSearchDialog::onStatusChanged);
    connect(m_queue, &DownloadQueue::finished, this, &MediaSearchDialog::onDownloadFinished);
    connect(m_resolver, &MediaDownloader::spotifyResolved, this,
            [this](const QList<MediaDownloader::SpotifyTrack> &tracks, const QString &, const QString &error) {
                if (m_toResolve.isEmpty())
                    return;
                const MediaSearch::Result done = m_toResolve.takeFirst();
                if (tracks.isEmpty())
                    setStatus(tr("Couldn't read “%1”: %2").arg(done.title, error), true);
                for (const MediaDownloader::SpotifyTrack &track : tracks) {
                    m_resolvedUrls << MediaDownloader::spotifyStreamUrl(track);
                    m_resolvedTitles << (track.artist.isEmpty() ? track.title : track.artist + QStringLiteral(" - ") + track.title);
                }
                resolveNext();
            });

    updateHeader();
    updateState();
    m_query->setFocus();
}

MediaSearchDialog::~MediaSearchDialog()
{
    // Hiding while being destroyed takes the focus from the fields, which
    // would save them through a half-destroyed dialog.
    for (QObject *child : findChildren<QObject *>())
        child->disconnect(this);
    cancelArt();
}

MediaSearch::Kind MediaSearchDialog::currentKind() const
{
    if (!m_tabs)
        return Kind::Video;
    switch (m_tabs->currentIndex()) {
    case 1:
        return Kind::Album;
    case 2:
        return Kind::Playlist;
    default:
        return Kind::Track;
    }
}

void MediaSearchDialog::setCurrentKind(MediaSearch::Kind kind)
{
    if (m_tabs)
        m_tabs->setCurrentIndex(kind == Kind::Album ? 1 : kind == Kind::Playlist ? 2 : 0);
}

QList<MediaSearch::Result> MediaSearchDialog::selectedResults() const
{
    QSet<QString> selected;
    for (QTreeWidgetItem *item : m_view->selectedItems())
        selected.insert(item->data(TitleColumn, kUrlRole).toString());
    // In the order they are listed.
    QList<MediaSearch::Result> out;
    for (const MediaSearch::Result &result : results()) {
        if (selected.contains(result.url))
            out.append(result);
    }
    return out;
}

QString MediaSearchDialog::directory() const
{
    return QDir::cleanPath(QDir::fromNativeSeparators(m_directory->text().trimmed()));
}

bool MediaSearchDialog::playWhenDone() const
{
    return m_play->isChecked();
}

MediaDownloader::Format MediaSearchDialog::videoFormat() const
{
    return m_format ? MediaDownloader::Format(m_format->currentData().toInt()) : MediaDownloader::Format::AudioMp3;
}

void MediaSearchDialog::setQuery(const QString &query)
{
    m_query->setText(query);
}

void MediaSearchDialog::setAccessPanelOpen(bool open)
{
    if (!m_accessToggle)
        return;
    if (m_accessToggle->isChecked() != open)
        m_accessToggle->setChecked(open);
    m_accessToggle->setArrowType(open ? Qt::DownArrow : Qt::RightArrow);
    m_accessPanel->setVisible(open);
}

void MediaSearchDialog::saveCredentials()
{
    if (!m_clientId)
        return;
    QSettings settings(settingsFile(), QSettings::IniFormat);
    settings.setValue(QStringLiteral("spotify/clientId"), m_clientId->text().trimmed());
    settings.setValue(QStringLiteral("spotify/clientSecret"), m_clientSecret->text().trimmed());
    m_search->setSpotifyCredentials(m_clientId->text(), m_clientSecret->text());
}

void MediaSearchDialog::saveSettings()
{
    QSettings settings(settingsFile(), QSettings::IniFormat);
    settings.setValue(QStringLiteral("downloader/directory"), directory());
    settings.setValue(QStringLiteral("downloader/play"), m_play->isChecked());
    if (m_format)
        settings.setValue(QStringLiteral("search/videoFormat"), int(videoFormat()));
}

void MediaSearchDialog::startSearch()
{
    const QString query = m_query->text().simplified();
    if (query.isEmpty())
        return;
    cancelArt();
    m_loadingMore = false;
    m_lastQuery = query;
    m_results.clear();
    m_totals.clear();
    m_nextOffset.clear();
    m_youtubeMore = false;
    m_view->clear();
    if (m_source == MediaSearch::Source::YouTube) {
        m_youtubeCount = kYouTubePage;
        if (!m_search->searchYouTube(query, m_youtubeCount)) {
            setStatus(tr("Install yt-dlp to search YouTube."), true);
            updateState();
            return;
        }
        setStatus(tr("Searching YouTube for “%1”...").arg(query));
    } else {
        saveCredentials();
        setStatus(tr("Searching Spotify for “%1”...").arg(query));
        m_search->searchSpotify(query, {Kind::Track, Kind::Album, Kind::Playlist});
    }
    updateState();
}

void MediaSearchDialog::loadMore()
{
    if (m_lastQuery.isEmpty() || m_search->isBusy())
        return;
    m_loadingMore = true;
    if (m_source == MediaSearch::Source::YouTube) {
        m_youtubeCount = std::min(kYouTubeMax, m_youtubeCount + kYouTubePage);
        if (!m_search->searchYouTube(m_lastQuery, m_youtubeCount)) {
            m_loadingMore = false;
            return;
        }
    } else {
        const Kind kind = currentKind();
        m_search->searchSpotify(m_lastQuery, {kind}, m_nextOffset.value(kind));
    }
    setStatus(tr("Loading more results..."));
    updateState();
}

void MediaSearchDialog::onResults(const MediaSearch::Page &page)
{
    const bool more = std::exchange(m_loadingMore, false);
    if (page.error != MediaSearch::Error::None) {
        setStatus(page.errorText, true);
        if (page.error == MediaSearch::Error::Credentials) {
            setAccessPanelOpen(true);
            (m_clientId->text().trimmed().isEmpty() ? m_clientId : m_clientSecret)->setFocus();
        }
        updateState();
        return;
    }

    if (page.source == MediaSearch::Source::YouTube) {
        // A longer search: only the videos not listed yet are new.
        QList<MediaSearch::Result> &list = m_results[Kind::Video];
        QSet<QString> known;
        for (const MediaSearch::Result &result : std::as_const(list))
            known.insert(result.id);
        QList<MediaSearch::Result> added;
        for (const MediaSearch::Result &result : page.results) {
            if (!known.contains(result.id)) {
                known.insert(result.id);
                added.append(result);
            }
        }
        list.append(added);
        m_youtubeMore = page.results.size() >= m_youtubeCount && m_youtubeCount < kYouTubeMax && !added.isEmpty();
        addItems(added);
    } else {
        // The credentials work: the panel can make room for the results.
        setAccessPanelOpen(false);
        for (auto it = page.totals.cbegin(); it != page.totals.cend(); ++it) {
            m_totals.insert(it.key(), it.value());
            m_nextOffset.insert(it.key(), page.offset + m_search->spotifyPageSize());
        }
        QList<MediaSearch::Result> added;
        for (const MediaSearch::Result &result : page.results) {
            QList<MediaSearch::Result> &list = m_results[result.kind];
            if (std::none_of(list.cbegin(), list.cend(), [&](const MediaSearch::Result &r) { return r.url == result.url; })) {
                list.append(result);
                if (result.kind == currentKind())
                    added.append(result);
            }
        }
        if (more)
            addItems(added);
        else
            showResults();
    }

    const int shown = int(results().size());
    if (m_source == MediaSearch::Source::YouTube && m_results.value(Kind::Video).isEmpty())
        setStatus(tr("Nothing found on YouTube for “%1”.").arg(page.query));
    else if (m_source == MediaSearch::Source::Spotify && m_results.isEmpty())
        setStatus(tr("Nothing found on Spotify for “%1”.").arg(page.query));
    else if (!more || shown > 0)
        setStatus(m_source == MediaSearch::Source::YouTube ? tr("%n video(s) found.", nullptr, shown)
                                                           : tr("Select results, then play, queue or download them."));
    // E.g. that Spotify refused and the results come from Deezer.
    if (!page.notice.isEmpty())
        setStatus(page.notice);
    updateState();
}

void MediaSearchDialog::showResults()
{
    m_view->clear();
    updateHeader();
    addItems(results());
    updateState();
}

void MediaSearchDialog::updateHeader()
{
    const Kind kind = currentKind();
    QStringList labels;
    switch (kind) {
    case Kind::Video:
        labels = QStringList{tr("Title"), tr("Channel"), tr("Length"), tr("Views"), tr("Status")};
        break;
    case Kind::Track:
        labels = QStringList{tr("Title"), tr("Artist"), tr("Album"), tr("Length"), tr("Status")};
        break;
    case Kind::Album:
        labels = QStringList{tr("Album"), tr("Artist"), tr("Songs"), tr("Year"), tr("Status")};
        break;
    case Kind::Playlist:
        labels = QStringList{tr("Playlist"), tr("Owner"), tr("Songs"), QString(), tr("Status")};
        break;
    }
    m_view->setHeaderLabels(labels);
    m_view->setColumnHidden(ExtraColumn, kind == Kind::Playlist);
    if (m_tabs) {
        const QList<QPair<Kind, QString>> tabs{{Kind::Track, tr("Songs")}, {Kind::Album, tr("Albums")}, {Kind::Playlist, tr("Playlists")}};
        for (int i = 0; i < tabs.size(); ++i) {
            const int total = m_totals.value(tabs[i].first, -1);
            m_tabs->setTabText(i, total < 0 ? tabs[i].second : QStringLiteral("%1 (%2)").arg(tabs[i].second).arg(total));
        }
    }
    m_mp3Button->setText(kind == Kind::Album || kind == Kind::Playlist ? tr("Download All as MP3") : tr("Download MP3"));
}

void MediaSearchDialog::addItems(const QList<MediaSearch::Result> &results)
{
    for (const MediaSearch::Result &result : results) {
        auto *item = new QTreeWidgetItem(m_view);
        item->setData(TitleColumn, kUrlRole, result.url);
        item->setData(TitleColumn, kArtRole, result.thumbnail);
        item->setText(TitleColumn, result.title);
        item->setToolTip(TitleColumn, result.title);
        item->setText(ArtistColumn, result.artists);
        item->setToolTip(ArtistColumn, result.artists);
        switch (result.kind) {
        case Kind::Video:
            item->setText(InfoColumn, result.live ? tr("LIVE") : MediaSearch::formatDuration(result.duration));
            item->setText(ExtraColumn, MediaSearch::formatViews(result.views));
            break;
        case Kind::Track:
            item->setText(InfoColumn, result.album);
            item->setToolTip(InfoColumn, result.album);
            item->setText(ExtraColumn, MediaSearch::formatDuration(result.duration));
            break;
        case Kind::Album:
        case Kind::Playlist:
            item->setText(InfoColumn, result.trackCount > 0 ? QString::number(result.trackCount) : QString());
            item->setText(ExtraColumn, result.released);
            break;
        }
        for (int column : {InfoColumn, ExtraColumn, StatusColumn})
            item->setTextAlignment(column, Qt::AlignRight | Qt::AlignVCenter);
        item->setText(StatusColumn, m_status.value(result.url));
        item->setToolTip(StatusColumn, m_errors.value(result.url));
        const QPixmap art = artFor(result.thumbnail);
        if (!art.isNull())
            item->setIcon(TitleColumn, QIcon(art));
        else
            requestArt(result.thumbnail);
    }
}

void MediaSearchDialog::updateState()
{
    const bool haveTool = !MediaDownloader::executable().isEmpty();
    m_warning->setVisible(!haveTool);
    const bool selection = !m_view->selectedItems().isEmpty();
    const bool youtube = m_source == MediaSearch::Source::YouTube;
    m_searchButton->setEnabled(!m_query->text().trimmed().isEmpty() && (haveTool || !youtube));
    m_playButton->setEnabled(haveTool && selection && m_toResolve.isEmpty());
    m_queueButton->setEnabled(haveTool && selection && m_toResolve.isEmpty());
    m_mp3Button->setEnabled(haveTool && selection && !directory().isEmpty());
    if (m_videoButton)
        m_videoButton->setEnabled(haveTool && selection && !directory().isEmpty());

    bool more = false;
    if (youtube) {
        more = m_youtubeMore;
    } else if (m_totals.contains(currentKind())) {
        const int next = m_nextOffset.value(currentKind());
        more = next < m_totals.value(currentKind()) && next < kSpotifyMaxOffset;
    }
    m_moreButton->setVisible(more);
    m_moreButton->setEnabled(!m_search->isBusy());
}

void MediaSearchDialog::setStatus(const QString &text, bool error)
{
    m_statusLabel->setText(text);
    // The skin shows errors in their own style.
    if (m_statusLabel->property("error").toBool() != error) {
        m_statusLabel->setProperty("error", error);
        m_statusLabel->style()->unpolish(m_statusLabel);
        m_statusLabel->style()->polish(m_statusLabel);
    }
}

void MediaSearchDialog::playSelected()
{
    stream(selectedResults(), false);
}

void MediaSearchDialog::queueSelected()
{
    stream(selectedResults(), true);
}

void MediaSearchDialog::stream(const QList<MediaSearch::Result> &results, bool append)
{
    if (results.isEmpty() || MediaDownloader::executable().isEmpty() || !m_toResolve.isEmpty())
        return;
    m_resolvedUrls.clear();
    m_resolvedTitles.clear();
    m_resolveAppend = append;
    for (const MediaSearch::Result &result : results) {
        switch (result.kind) {
        case Kind::Video:
            m_resolvedUrls << result.url;
            m_resolvedTitles << result.title;
            break;
        case Kind::Track:
            m_resolvedUrls << MediaDownloader::spotifyStreamUrl(result.track());
            m_resolvedTitles << result.displayName();
            break;
        case Kind::Album:
        case Kind::Playlist:
            // Their songs are read from Spotify first.
            m_toResolve.append(result);
            break;
        }
    }
    resolveNext();
}

void MediaSearchDialog::resolveNext()
{
    if (!m_toResolve.isEmpty()) {
        const MediaSearch::Result &next = m_toResolve.first();
        setStatus(tr("Reading the songs of “%1”...").arg(next.title));
        m_resolver->resolveSpotify(next.url);
        updateState();
        return;
    }
    updateState();
    if (m_resolvedUrls.isEmpty())
        return;
    const QString format = MediaDownloader::streamFormat(m_source == MediaSearch::Source::Spotify
                                                             ? MediaDownloader::Format::AudioMp3
                                                             : videoFormat());
    const qsizetype count = m_resolvedUrls.size();
    if (m_resolveAppend) {
        Q_EMIT tracksQueueRequested(m_resolvedUrls, m_resolvedTitles, format);
        setStatus(tr("Added %n entry(s) to the playlist.", nullptr, int(count)));
    } else {
        Q_EMIT tracksStreamRequested(m_resolvedUrls, m_resolvedTitles, format);
        setStatus(count == 1 ? tr("Playing “%1”.").arg(m_resolvedTitles.first())
                             : tr("Playing %n entries.", nullptr, int(count)));
    }
    m_resolvedUrls.clear();
    m_resolvedTitles.clear();
}

void MediaSearchDialog::downloadSelected(MediaDownloader::Format format)
{
    const QList<MediaSearch::Result> selected = selectedResults();
    if (selected.isEmpty() || MediaDownloader::executable().isEmpty())
        return;
    if (!QDir().mkpath(directory()) || !QFileInfo(directory()).isWritable()) {
        setStatus(tr("Can't save to %1.").arg(directory()), true);
        return;
    }
    saveSettings();
    if (m_queue->pending() == 0) {
        // A new batch.
        m_batch.clear();
        m_saved = 0;
        m_failed = 0;
        m_playedInBatch = false;
    }
    int queued = 0;
    for (const MediaSearch::Result &result : selected) {
        m_errors.remove(result.url);
        bool ok = false;
        switch (result.kind) {
        case Kind::Video:
            ok = m_queue->enqueue(result.url, result.title, format, directory());
            break;
        case Kind::Track:
            ok = m_queue->enqueueTrack(result.url, result.track(), directory());
            break;
        case Kind::Album:
        case Kind::Playlist:
            // Every song, found on YouTube and saved as a tagged MP3.
            ok = m_queue->enqueue(result.url, result.title, MediaDownloader::Format::AudioMp3, directory());
            break;
        }
        if (ok) {
            m_batch.insert(result.url);
            ++queued;
        }
    }
    updateProgress();
    if (queued == 0)
        setStatus(tr("Already downloading."));
}

void MediaSearchDialog::onStatusChanged(const QString &entry, const QString &status)
{
    if (status.isEmpty())
        m_status.remove(entry);
    else
        m_status.insert(entry, status);
    for (int i = 0; i < m_view->topLevelItemCount(); ++i) {
        QTreeWidgetItem *item = m_view->topLevelItem(i);
        if (item->data(TitleColumn, kUrlRole).toString() == entry) {
            item->setText(StatusColumn, status);
            item->setToolTip(StatusColumn, m_errors.value(entry));
        }
    }
    updateProgress();
}

void MediaSearchDialog::onDownloadFinished(const QString &entry, bool ok, const QStringList &files, const QString &error)
{
    if (ok) {
        ++m_saved;
        const bool play = m_play->isChecked() && !m_playedInBatch;
        if (m_play->isChecked())
            m_playedInBatch = true;
        if (files.size() > 1)
            Q_EMIT downloadedMany(files, play);
        else if (!files.isEmpty())
            Q_EMIT downloaded(files.first(), play);
    } else {
        ++m_failed;
        m_errors.insert(entry, error);
        for (int i = 0; i < m_view->topLevelItemCount(); ++i) {
            QTreeWidgetItem *item = m_view->topLevelItem(i);
            if (item->data(TitleColumn, kUrlRole).toString() == entry)
                item->setToolTip(StatusColumn, error);
        }
    }
    if (m_queue->pending() == 0) {
        if (m_failed == 0)
            setStatus(tr("Saved %n download(s) to %1.", nullptr, m_saved).arg(QDir::toNativeSeparators(directory())));
        else
            setStatus(tr("Saved %1 of %2. Failed: %3").arg(m_saved).arg(m_saved + m_failed)
                          .arg(error.isEmpty() ? tr("see the Status column") : error.section(QLatin1Char('\n'), 0, 0)),
                      true);
    }
    updateProgress();
}

void MediaSearchDialog::updateProgress()
{
    const bool busy = m_queue->pending() > 0;
    m_progress->setVisible(busy);
    m_cancelButton->setVisible(busy);
    if (!busy || m_batch.isEmpty())
        return;
    double done = 0;
    for (const QString &entry : std::as_const(m_batch)) {
        const QString status = m_status.value(entry);
        if (status.endsWith(QLatin1Char('%')))
            done += status.chopped(1).toDouble() / 100.0;
        else if (status != DownloadQueue::tr("Queued"))
            done += 1; // saved, failed or cancelled
    }
    m_progress->setValue(qRound(done * 1000 / m_batch.size()));
    const QString left = tr("Downloading to %1... %n left", nullptr, m_queue->pending())
                             .arg(QDir::toNativeSeparators(directory()));
    setStatus(m_failed > 0 ? left + QLatin1Char(' ') + tr("(%n failed)", nullptr, m_failed) : left);
}

void MediaSearchDialog::cancelDownloads()
{
    if (m_queue->pending() == 0)
        return;
    m_queue->cancelAll();
    m_batch.clear();
    updateProgress();
    setStatus(tr("Downloads cancelled."));
}

void MediaSearchDialog::requestArt(const QUrl &url)
{
    const QString key = url.toString();
    if (url.isEmpty() || !url.isValid() || m_art.contains(key) || m_artReplies.contains(key))
        return;
    QNetworkRequest request(url);
    request.setTransferTimeout(kArtTimeoutMs);
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
    QNetworkReply *reply = m_network->get(request);
    m_artReplies.insert(key, reply);
    connect(reply, &QNetworkReply::finished, this, [this, reply, key] {
        reply->deleteLater();
        if (m_artReplies.value(key) != reply)
            return;
        m_artReplies.remove(key);
        if (reply->error() != QNetworkReply::NoError)
            return;
        const QImage image = QImage::fromData(reply->readAll());
        if (image.isNull())
            return;
        if (m_art.size() >= kArtCacheSize)
            m_art.clear();
        const QPixmap art = fitArt(image, m_view->iconSize(), devicePixelRatioF());
        m_art.insert(key, art);
        for (int i = 0; i < m_view->topLevelItemCount(); ++i) {
            QTreeWidgetItem *item = m_view->topLevelItem(i);
            if (item->data(TitleColumn, kArtRole).toUrl().toString() == key)
                item->setIcon(TitleColumn, QIcon(art));
        }
    });
}

void MediaSearchDialog::cancelArt()
{
    const auto replies = std::exchange(m_artReplies, {});
    for (const QPointer<QNetworkReply> &reply : replies) {
        if (reply)
            reply->abort();
    }
}

bool MediaSearchDialog::eventFilter(QObject *watched, QEvent *event)
{
    // Enter searches, even where the player's window takes it for fullscreen.
    if (watched == m_query && event->type() == QEvent::ShortcutOverride) {
        const int key = static_cast<QKeyEvent *>(event)->key();
        if (key == Qt::Key_Return || key == Qt::Key_Enter) {
            event->accept();
            return true;
        }
    }
    return QDialog::eventFilter(watched, event);
}

void MediaSearchDialog::hideEvent(QHideEvent *event)
{
    // Downloads go on in the background; searches and art stop.
    m_search->cancel();
    m_loadingMore = false;
    cancelArt();
    QDialog::hideEvent(event);
    updateState();
}

void MediaSearchDialog::reject()
{
    if (m_queue->pending() > 0) {
        QMessageBox box(QMessageBox::Question, windowTitle(),
                        tr("%n download(s) are not finished yet.", nullptr, m_queue->pending()), QMessageBox::NoButton, this);
        box.setObjectName(QStringLiteral("MediaSearchCloseQuestion"));
        box.setInformativeText(tr("They can go on in the background while you close this window."));
        QPushButton *keep = box.addButton(tr("Keep Downloading"), QMessageBox::AcceptRole);
        QPushButton *stop = box.addButton(tr("Stop Downloads"), QMessageBox::DestructiveRole);
        box.addButton(QMessageBox::Cancel);
        box.setDefaultButton(keep);
        box.exec();
        if (box.clickedButton() == stop)
            cancelDownloads();
        else if (box.clickedButton() != keep)
            return;
    }
    if (!m_toResolve.isEmpty()) {
        m_toResolve.clear();
        m_resolver->cancel();
    }
    QDialog::reject();
}
