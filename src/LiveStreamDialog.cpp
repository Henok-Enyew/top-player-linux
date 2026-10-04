#include "LiveStreamDialog.h"
#include "Icons.h"
#include "PlaylistSession.h"

#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QComboBox>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QImage>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QPainter>
#include <QPixmap>
#include <QPushButton>
#include <QSaveFile>
#include <QScrollBar>
#include <QSettings>
#include <QTabBar>
#include <QTimer>
#include <QToolButton>
#include <QTreeWidget>
#include <QVBoxLayout>

using StreamCatalog::Station;

namespace {

constexpr int kStationRole = Qt::UserRole; // index into m_stations
constexpr int kLogoSize = 24;
constexpr int kMaxLogoDownloads = 6;
constexpr int kMaxGenres = 40; // Radio-Browser has thousands of tags
const QString kIndexCode = QStringLiteral("index");

enum Column { NameColumn, InfoColumn, BitrateColumn };

QString settingsFile()
{
    return PlaylistSession::configDir() + QStringLiteral("/settings.ini");
}

QString logoCacheFile(const QString &url)
{
    const QByteArray hash = QCryptographicHash::hash(url.toUtf8(), QCryptographicHash::Sha1).toHex();
    return StreamCatalog::cacheDir() + QStringLiteral("/logos/") + QString::fromLatin1(hash);
}

// Scales a logo onto a square canvas, so rows line up.
QIcon logoIcon(const QImage &image)
{
    constexpr qreal kScale = 2; // sharp on high-DPI screens
    const int pixels = qRound(kLogoSize * kScale);
    QPixmap canvas(pixels, pixels);
    canvas.fill(Qt::transparent);
    const QImage scaled = image.scaled(pixels, pixels, Qt::KeepAspectRatio, Qt::SmoothTransformation);
    QPainter painter(&canvas);
    painter.drawImage(QPoint((pixels - scaled.width()) / 2, (pixels - scaled.height()) / 2), scaled);
    painter.end();
    canvas.setDevicePixelRatio(kScale);
    return QIcon(canvas);
}

// Sorts the bitrate column by value instead of text.
class StationItem : public QTreeWidgetItem
{
public:
    using QTreeWidgetItem::QTreeWidgetItem;

    bool operator<(const QTreeWidgetItem &other) const override
    {
        const int column = treeWidget() ? treeWidget()->sortColumn() : NameColumn;
        if (column == BitrateColumn)
            return data(BitrateColumn, Qt::UserRole).toInt() < other.data(BitrateColumn, Qt::UserRole).toInt();
        return text(column).localeAwareCompare(other.text(column)) < 0;
    }
};

} // namespace

LiveStreamDialog::LiveStreamDialog(QWidget *parent)
    : QDialog(parent)
    , m_fetcher(new StreamFetcher(this))
    , m_tabs(new QTabBar(this))
    , m_country(new QComboBox(this))
    , m_genre(new QComboBox(this))
    , m_filter(new QLineEdit(this))
    , m_hideGeoBlocked(new QCheckBox(tr("Hide geo-blocked"), this))
    , m_refresh(new QToolButton(this))
    , m_view(new QTreeWidget(this))
    , m_status(new QLabel(this))
    , m_play(new QPushButton(skinIcon(IconType::Play), tr("Play"), this))
    , m_queue(new QPushButton(skinIcon(IconType::Add), tr("Add to Playlist"), this))
    , m_placeholder(skinIcon(IconType::Url))
{
    setObjectName(QStringLiteral("LiveStreamDialog"));
    setWindowTitle(tr("Live TV & Radio"));
    resize(720, 520);

    m_tabs->setObjectName(QStringLiteral("LiveStreamTabs"));
    m_tabs->addTab(tr("Live TV"));
    m_tabs->addTab(tr("Online Radio"));
    m_tabs->setDrawBase(false);
    m_tabs->setExpanding(false);

    m_country->setObjectName(QStringLiteral("LiveStreamCountry"));
    m_country->setMinimumContentsLength(14);
    m_country->setMaxVisibleItems(20);
    m_country->setToolTip(tr("Country, or all countries"));
    m_genre->setObjectName(QStringLiteral("LiveStreamGenre"));
    m_genre->setMinimumContentsLength(10);
    m_genre->setMaxVisibleItems(20);
    m_genre->setToolTip(tr("Category"));
    m_hideGeoBlocked->setObjectName(QStringLiteral("LiveStreamHideGeoBlocked"));
    m_hideGeoBlocked->setToolTip(tr("Hide channels that only play in their own country"));
    m_filter->setObjectName(QStringLiteral("LiveStreamFilter"));
    m_filter->setPlaceholderText(tr("Search by name, country, language or genre"));
    m_filter->setClearButtonEnabled(true);
    m_filter->addAction(skinIcon(IconType::Search), QLineEdit::LeadingPosition);
    m_refresh->setObjectName(QStringLiteral("LiveStreamRefresh"));
    m_refresh->setText(tr("Refresh"));
    m_refresh->setToolTip(tr("Reload the list from the server"));
    m_refresh->setToolButtonStyle(Qt::ToolButtonTextOnly);

    m_view->setObjectName(QStringLiteral("LiveStreamView"));
    m_view->setColumnCount(3);
    m_view->setHeaderLabels({tr("Channel"), tr("Country / Genre"), tr("Bitrate")});
    m_view->setRootIsDecorated(false);
    m_view->setUniformRowHeights(true);
    m_view->setIconSize(QSize(kLogoSize, kLogoSize));
    m_view->setAlternatingRowColors(true);
    m_view->setSortingEnabled(true);
    m_view->sortByColumn(-1, Qt::AscendingOrder); // the source's order until a header is clicked
    m_view->setContextMenuPolicy(Qt::CustomContextMenu);
    m_view->header()->setStretchLastSection(false);
    m_view->header()->setSectionResizeMode(NameColumn, QHeaderView::Stretch);
    m_view->header()->setSectionResizeMode(InfoColumn, QHeaderView::Interactive);
    m_view->header()->setSectionResizeMode(BitrateColumn, QHeaderView::ResizeToContents);
    m_view->header()->resizeSection(InfoColumn, 220);

    m_status->setObjectName(QStringLiteral("LiveStreamStatus"));
    m_play->setObjectName(QStringLiteral("LiveStreamPlay"));
    m_queue->setObjectName(QStringLiteral("LiveStreamQueue"));
    m_play->setDefault(true);
    auto *close = new QPushButton(tr("Close"), this);

    auto *filters = new QHBoxLayout;
    filters->addWidget(m_country);
    filters->addWidget(m_genre);
    filters->addWidget(m_filter, 1);
    filters->addWidget(m_hideGeoBlocked);
    filters->addWidget(m_refresh);
    auto *buttons = new QHBoxLayout;
    buttons->addWidget(m_status, 1);
    buttons->addWidget(m_queue);
    buttons->addWidget(m_play);
    buttons->addWidget(close);
    auto *layout = new QVBoxLayout(this);
    layout->addWidget(m_tabs);
    layout->addLayout(filters);
    layout->addWidget(m_view, 1);
    layout->addLayout(buttons);

    const QSettings settings(settingsFile(), QSettings::IniFormat);
    m_tabs->setCurrentIndex(settings.value(QStringLiteral("live/source"), 0).toInt() == Radio ? Radio : Tv);
    m_hideGeoBlocked->setChecked(settings.value(QStringLiteral("live/hideGeoBlocked"), true).toBool());
    m_hideGeoBlocked->setVisible(source() == Tv);
    fillCountries();

    connect(m_fetcher, &StreamFetcher::loaded, this, &LiveStreamDialog::onLoaded);
    connect(m_fetcher, &StreamFetcher::failed, this, &LiveStreamDialog::onFailed);
    connect(m_tabs, &QTabBar::currentChanged, this, [this] {
        QSettings(settingsFile(), QSettings::IniFormat).setValue(QStringLiteral("live/source"), source());
        m_hideGeoBlocked->setVisible(source() == Tv);
        fillCountries();
        reload();
    });
    connect(m_country, &QComboBox::activated, this, [this] {
        const QString key = source() == Radio ? QStringLiteral("live/radioCountry") : QStringLiteral("live/tvCountry");
        QSettings(settingsFile(), QSettings::IniFormat).setValue(key, country());
        reload();
    });
    connect(m_filter, &QLineEdit::textChanged, this, &LiveStreamDialog::applyFilter);
    connect(m_genre, &QComboBox::currentIndexChanged, this, &LiveStreamDialog::applyFilter);
    connect(m_hideGeoBlocked, &QCheckBox::toggled, this, [this](bool hide) {
        QSettings(settingsFile(), QSettings::IniFormat).setValue(QStringLiteral("live/hideGeoBlocked"), hide);
        applyFilter();
    });
    connect(m_refresh, &QToolButton::clicked, this, [this] { reload(true); });
    connect(m_view, &QTreeWidget::itemActivated, this, &LiveStreamDialog::playCurrent);
    connect(m_view, &QWidget::customContextMenuRequested, this, &LiveStreamDialog::showContextMenu);
    connect(m_view->verticalScrollBar(), &QScrollBar::valueChanged, this, &LiveStreamDialog::requestVisibleLogos);
    connect(m_view, &QTreeWidget::currentItemChanged, this, [this](QTreeWidgetItem *item) {
        m_play->setEnabled(item);
        m_queue->setEnabled(item);
    });
    connect(m_play, &QPushButton::clicked, this, &LiveStreamDialog::playCurrent);
    connect(m_queue, &QPushButton::clicked, this, &LiveStreamDialog::queueCurrent);
    connect(close, &QPushButton::clicked, this, &QDialog::close);
    m_play->setEnabled(false);
    m_queue->setEnabled(false);

    reload();
}

LiveStreamDialog::Source LiveStreamDialog::source() const
{
    return m_tabs->currentIndex() == Radio ? Radio : Tv;
}

void LiveStreamDialog::setSource(Source source)
{
    m_tabs->setCurrentIndex(source);
}

QString LiveStreamDialog::country() const
{
    return m_country->currentData().toString();
}

void LiveStreamDialog::setCountry(const QString &code)
{
    const int index = m_country->findData(code.toLower());
    if (index < 0 || index == m_country->currentIndex())
        return;
    m_country->setCurrentIndex(index);
    reload();
}

void LiveStreamDialog::setFilterText(const QString &text)
{
    m_filter->setText(text);
}

void LiveStreamDialog::setGenre(const QString &genre)
{
    const int index = genre.isEmpty() ? 0 : m_genre->findData(genre, Qt::UserRole, Qt::MatchFixedString);
    if (index >= 0)
        m_genre->setCurrentIndex(index);
}

QString LiveStreamDialog::genre() const
{
    return m_genre->currentData().toString();
}

void LiveStreamDialog::fillGenres()
{
    // Keeps the chosen genre across lists that have it, e.g. "News" from country to country.
    const QString current = genre();
    const QSignalBlocker blocker(m_genre);
    m_genre->clear();
    m_genre->addItem(tr("All Categories"), QString());
    QStringList list = StreamCatalog::genres(m_stations, kMaxGenres);
    std::sort(list.begin(), list.end(), [](const QString &a, const QString &b) { return a.localeAwareCompare(b) < 0; });
    for (const QString &genre : std::as_const(list))
        m_genre->addItem(genre, genre);
    m_genre->setCurrentIndex(std::max(0, m_genre->findData(current, Qt::UserRole, Qt::MatchFixedString)));
}

void LiveStreamDialog::markUnavailable(const QString &url)
{
    m_unavailable.insert(url);
    const QColor grey = palette().color(QPalette::Disabled, QPalette::Text);
    for (int row = 0; row < m_view->topLevelItemCount(); ++row) {
        QTreeWidgetItem *item = m_view->topLevelItem(row);
        const Station *station = stationOf(item);
        if (!station || station->url != url)
            continue;
        for (int column = 0; column < m_view->columnCount(); ++column)
            item->setForeground(column, grey);
        item->setToolTip(NameColumn, tr("Could not be played: offline, or not available in your region\n%1").arg(url));
    }
}

QList<Station> LiveStreamDialog::alternatives(const Station &station) const
{
    QList<Station> result;
    // Any feed of the channel will do: "EBS.us@HD" or "EBS.us@SD".
    const QString channel = station.id.section(QLatin1Char('@'), 0, 0);
    if (channel.isEmpty())
        return result;
    for (const Station &other : m_stations) {
        if (other.id.section(QLatin1Char('@'), 0, 0) == channel && other.url != station.url && !m_unavailable.contains(other.url))
            result.append(other);
    }
    // Ones that play anywhere first, then by resolution.
    std::stable_sort(result.begin(), result.end(), [](const Station &a, const Station &b) {
        if (a.isGeoBlocked() != b.isGeoBlocked())
            return !a.isGeoBlocked();
        return a.quality.left(a.quality.size() - 1).toInt() > b.quality.left(b.quality.size() - 1).toInt();
    });
    return result;
}

void LiveStreamDialog::fillCountries()
{
    const QSettings settings(settingsFile(), QSettings::IniFormat);
    const QString saved = settings.value(source() == Radio ? QStringLiteral("live/radioCountry") : QStringLiteral("live/tvCountry"),
                                         QStringLiteral("et")).toString();
    const QSignalBlocker blocker(m_country);
    m_country->clear();
    const QList<StreamCatalog::Country> countries = StreamCatalog::countries();
    for (int i = 0; i < countries.size(); ++i) {
        m_country->addItem(countries[i].name, countries[i].code);
        if (i == 0) {
            // Ethiopia and all countries stay pinned above the rest.
            if (source() == Tv)
                m_country->addItem(tr("All Countries"), kIndexCode);
            m_country->insertSeparator(m_country->count());
        }
    }
    m_country->setCurrentIndex(std::max(0, m_country->findData(saved)));
}

QString LiveStreamDialog::cacheKey() const
{
    if (source() == Radio)
        return QStringLiteral("radio-%1.json").arg(country());
    return country() == kIndexCode ? QStringLiteral("tv-index.category.m3u") : QStringLiteral("tv-%1.m3u").arg(country());
}

QUrl LiveStreamDialog::sourceUrl() const
{
    if (source() == Radio)
        return StreamCatalog::radioCountryUrl(country());
    return country() == kIndexCode ? StreamCatalog::tvCategoryIndexUrl() : StreamCatalog::tvCountryUrl(country());
}

void LiveStreamDialog::reload(bool refresh)
{
    m_loadingKey = cacheKey();
    m_loading = true;
    m_refresh->setEnabled(false);
    m_stations.clear();
    m_view->clear();
    m_statusText.clear();
    m_status->setText(tr("Loading %1...").arg(m_country->currentText()));
    m_fetcher->fetch(m_loadingKey, sourceUrl(), refresh);
}

void LiveStreamDialog::onLoaded(const QString &key, const QByteArray &data, const QDateTime &fetched, bool fromCache)
{
    if (key != m_loadingKey)
        return; // the user picked another list meanwhile
    m_loading = false;
    m_refresh->setEnabled(true);
    m_stations = key.endsWith(QLatin1String(".json")) ? StreamCatalog::parseRadioBrowser(data) : StreamCatalog::parseM3u(data);
    if (fromCache) {
        const qint64 hours = fetched.secsTo(QDateTime::currentDateTime()) / 3600;
        if (hours < 1)
            m_statusText = tr("updated less than an hour ago");
        else
            m_statusText = hours == 1 ? tr("updated an hour ago") : tr("updated %1 hours ago").arg(hours);
        if (hours >= 24)
            m_statusText = tr("offline copy, %1").arg(m_statusText);
    }
    populate();
}

void LiveStreamDialog::onFailed(const QString &key, const QString &error)
{
    if (key != m_loadingKey)
        return;
    m_loading = false;
    m_refresh->setEnabled(true);
    m_status->setText(tr("Could not load the list: %1").arg(error));
}

void LiveStreamDialog::populate()
{
    m_view->setUpdatesEnabled(false);
    m_view->clear();
    const bool radio = source() == Radio;
    // Names the country where it isn't the one picked, e.g. in All Countries.
    const QString listCountry = StreamCatalog::countryName(country());
    QList<QTreeWidgetItem *> items;
    items.reserve(m_stations.size());
    for (int i = 0; i < m_stations.size(); ++i) {
        const Station &station = m_stations[i];
        auto *item = new StationItem;
        item->setText(NameColumn, station.name);
        QStringList info;
        if (!station.country.isEmpty() && !radio && station.country != listCountry)
            info.append(station.country);
        if (!station.genre.isEmpty())
            info.append(station.genre);
        if (!station.language.isEmpty())
            info.append(station.language);
        item->setText(InfoColumn, info.join(QStringLiteral(" · ")));
        item->setToolTip(NameColumn, station.url);
        item->setToolTip(InfoColumn, item->text(InfoColumn));
        if (station.bitrate > 0)
            item->setText(BitrateColumn, tr("%1 kbps").arg(station.bitrate));
        else
            item->setText(BitrateColumn, station.quality);
        item->setData(BitrateColumn, Qt::UserRole, station.bitrate > 0 ? station.bitrate : station.quality.left(station.quality.size() - 1).toInt());
        item->setTextAlignment(BitrateColumn, Qt::AlignRight | Qt::AlignVCenter);
        item->setData(NameColumn, kStationRole, i);
        item->setIcon(NameColumn, m_logos.value(station.logo, m_placeholder));
        items.append(item);
    }
    m_view->addTopLevelItems(items);
    m_view->setUpdatesEnabled(true);
    for (const QString &url : std::as_const(m_unavailable))
        markUnavailable(url);
    fillGenres();
    applyFilter();
}

void LiveStreamDialog::applyFilter()
{
    const QString filter = m_filter->text();
    const QString genre = this->genre();
    const bool hideGeoBlocked = source() == Tv && m_hideGeoBlocked->isChecked();
    for (int row = 0; row < m_view->topLevelItemCount(); ++row) {
        QTreeWidgetItem *item = m_view->topLevelItem(row);
        const Station &station = *stationOf(item);
        item->setHidden((hideGeoBlocked && station.isGeoBlocked()) || !StreamCatalog::hasGenre(station, genre)
                        || !StreamCatalog::matches(station, filter));
    }
    updateStatus();
    QTimer::singleShot(0, this, &LiveStreamDialog::requestVisibleLogos);
}

void LiveStreamDialog::updateStatus()
{
    if (m_loading)
        return;
    int shown = 0;
    for (int row = 0; row < m_view->topLevelItemCount(); ++row)
        shown += m_view->topLevelItem(row)->isHidden() ? 0 : 1;
    const int total = static_cast<int>(m_stations.size());
    QString text;
    if (source() == Radio)
        text = total == 1 ? tr("1 station") : tr("%1 stations").arg(total);
    else
        text = total == 1 ? tr("1 channel") : tr("%1 channels").arg(total);
    if (shown != total)
        text = tr("%1 of %2").arg(shown).arg(text);
    if (!m_statusText.isEmpty())
        text += QStringLiteral(" · ") + m_statusText;
    m_status->setText(text);
}

const Station *LiveStreamDialog::stationOf(const QTreeWidgetItem *item) const
{
    if (!item)
        return nullptr;
    const int index = item->data(NameColumn, kStationRole).toInt();
    return index >= 0 && index < m_stations.size() ? &m_stations[index] : nullptr;
}

QList<Station> LiveStreamDialog::visibleStations() const
{
    QList<Station> result;
    for (int row = 0; row < m_view->topLevelItemCount(); ++row) {
        const QTreeWidgetItem *item = m_view->topLevelItem(row);
        if (!item->isHidden())
            result.append(*stationOf(item));
    }
    return result;
}

void LiveStreamDialog::playCurrent()
{
    if (const Station *station = stationOf(m_view->currentItem()))
        Q_EMIT playRequested(*station, source() == Radio);
}

void LiveStreamDialog::queueCurrent()
{
    if (const Station *station = stationOf(m_view->currentItem()))
        Q_EMIT queueRequested(*station);
}

void LiveStreamDialog::showContextMenu(const QPoint &pos)
{
    QTreeWidgetItem *item = m_view->itemAt(pos);
    const Station *station = stationOf(item);
    if (!station)
        return;
    m_view->setCurrentItem(item);
    const Station copy = *station;
    QMenu menu(this);
    menu.setObjectName(QStringLiteral("LiveStreamContextMenu"));
    menu.addAction(skinIcon(IconType::Play), tr("Play"), this, &LiveStreamDialog::playCurrent);
    menu.addAction(skinIcon(IconType::Add), tr("Add to Current Playlist"), this, [this, copy] { Q_EMIT queueRequested(copy); });
    menu.addSeparator();
    menu.addAction(tr("Copy Stream URL"), this, [copy] { QApplication::clipboard()->setText(copy.url); });
    menu.exec(m_view->viewport()->mapToGlobal(pos));
}

void LiveStreamDialog::requestVisibleLogos()
{
    // Only the rows on screen, so that a list of thousands doesn't download every logo.
    QTreeWidgetItem *item = m_view->itemAt(QPoint(1, 1));
    const int bottom = m_view->viewport()->height();
    for (; item && m_view->visualItemRect(item).top() < bottom; item = m_view->itemBelow(item)) {
        const Station *station = stationOf(item);
        if (!station || station->logo.isEmpty() || m_logos.contains(station->logo)
            || m_failedLogos.contains(station->logo) || m_logoRequests.contains(station->logo)) {
            continue;
        }
        // From the disk cache, or downloaded.
        QImage image(logoCacheFile(station->logo));
        if (!image.isNull()) {
            applyLogo(station->logo, logoIcon(image));
            continue;
        }
        m_logoRequests.insert(station->logo);
        m_logoQueue.append(station->logo);
    }
    startLogoDownloads();
}

void LiveStreamDialog::startLogoDownloads()
{
    while (m_logoDownloads < kMaxLogoDownloads && !m_logoQueue.isEmpty()) {
        const QString url = m_logoQueue.takeFirst();
        QNetworkRequest request{QUrl(url)};
        request.setTransferTimeout(15000);
        QNetworkReply *reply = m_fetcher->network()->get(request);
        ++m_logoDownloads;
        connect(reply, &QNetworkReply::finished, this, [this, reply, url] {
            reply->deleteLater();
            --m_logoDownloads;
            m_logoRequests.remove(url);
            // A failed reply is closed; reading it only logs warnings.
            const bool ok = reply->error() == QNetworkReply::NoError;
            const QByteArray data = ok ? reply->readAll() : QByteArray();
            const QImage image = QImage::fromData(data);
            if (!ok || image.isNull()) {
                m_failedLogos.insert(url); // keeps the placeholder
            } else {
                QDir().mkpath(StreamCatalog::cacheDir() + QStringLiteral("/logos"));
                QSaveFile file(logoCacheFile(url));
                if (file.open(QIODevice::WriteOnly)) {
                    file.write(data);
                    file.commit();
                }
                applyLogo(url, logoIcon(image));
            }
            startLogoDownloads();
        });
    }
}

void LiveStreamDialog::applyLogo(const QString &url, const QIcon &icon)
{
    m_logos.insert(url, icon);
    for (int row = 0; row < m_view->topLevelItemCount(); ++row) {
        QTreeWidgetItem *item = m_view->topLevelItem(row);
        const Station *station = stationOf(item);
        if (station && station->logo == url)
            item->setIcon(NameColumn, icon);
    }
}
