#include "SubtitleDownloadDialog.h"
#include "MediaFiles.h"
#include "SubtitleHasher.h"
#include "Theme.h"

#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFileInfo>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QPainter>
#include <QProgressBar>
#include <QPushButton>
#include <QStyledItemDelegate>
#include <QTimer>
#include <QTreeWidget>
#include <QVBoxLayout>

namespace {

enum Column { LanguageColumn, TitleColumn, ProviderColumn, RatingColumn, FormatColumn, ColumnCount };

// Item data: the index into the results, and the value columns sort by.
constexpr int kResultRole = Qt::UserRole;
constexpr int kSortRole = Qt::UserRole + 1;
constexpr int kBadgeRole = Qt::UserRole + 2;

const QColor kErrorColor(0xFF, 0x6B, 0x5B);

// Sorts numeric columns by value instead of text.
class ResultItem : public QTreeWidgetItem
{
public:
    using QTreeWidgetItem::QTreeWidgetItem;

    bool operator<(const QTreeWidgetItem &other) const override
    {
        const int column = treeWidget() ? treeWidget()->sortColumn() : 0;
        const QVariant a = data(column, kSortRole);
        const QVariant b = other.data(column, kSortRole);
        if (a.isValid() && b.isValid())
            return a.toDouble() < b.toDouble();
        return QString::localeAwareCompare(text(column), other.text(column)) < 0;
    }
};

// Draws a rounded "pill" after the cell text, e.g. "HI" or "Exact match".
// Draws rounded "pills" after the cell text: "Exact match", "HI", ...
class BadgeDelegate : public QStyledItemDelegate
{
public:
    using QStyledItemDelegate::QStyledItemDelegate;

    void paint(QPainter *painter, const QStyleOptionViewItem &option, const QModelIndex &index) const override
    {
        const QStringList badges = index.data(kBadgeRole).toStringList();
        QStyleOptionViewItem opt(option);
        initStyleOption(&opt, index);
        QFont badgeFont = opt.font;
        badgeFont.setBold(true);
        const QFontMetrics metrics(opt.font);
        const QFontMetrics badgeMetrics(badgeFont);
        int badgesWidth = 0;
        for (const QString &badge : badges)
            badgesWidth += badgeMetrics.horizontalAdvance(badge) + 12 + 4;
        const QWidget *widget = opt.widget;
        QStyle *style = widget ? widget->style() : QApplication::style();
        const QRect textRect = style->subElementRect(QStyle::SE_ItemViewItemText, &opt, widget);
        if (badgesWidth > 0 && !opt.text.isEmpty())
            opt.text = metrics.elidedText(opt.text, Qt::ElideMiddle, std::max(0, textRect.width() - badgesWidth - 6));
        style->drawControl(QStyle::CE_ItemViewItem, &opt, painter, widget);
        if (badges.isEmpty())
            return;

        int x = textRect.left() + (opt.text.isEmpty() ? 0 : metrics.horizontalAdvance(opt.text) + 6);
        const int height = metrics.height() + 2;
        painter->save();
        painter->setRenderHint(QPainter::Antialiasing);
        painter->setFont(badgeFont);
        for (const QString &badge : badges) {
            const QRect pill(x, textRect.center().y() - height / 2 + 1, badgeMetrics.horizontalAdvance(badge) + 12, height);
            painter->setPen(Qt::NoPen);
            // The match badges in the accent color; others (HI) in grey.
            const bool match = badge != QLatin1String("HI");
            painter->setBrush(match ? Theme::Accent : Theme::TextDim);
            painter->drawRoundedRect(pill, height / 2.0, height / 2.0);
            painter->setPen(match ? Theme::OnAccent : Theme::Surface);
            painter->drawText(pill, Qt::AlignCenter, badge);
            x += pill.width() + 4;
        }
        painter->restore();
    }
};

} // namespace

SubtitleSettingsDialog::SubtitleSettingsDialog(QWidget *parent)
    : QDialog(parent)
    , m_apiKey(new QLineEdit(this))
    , m_besideVideo(new QCheckBox(tr("Save subtitles next to the video"), this))
{
    setWindowTitle(tr("Subtitle Download Settings"));
    setObjectName(QStringLiteral("SubtitleSettingsDialog"));
    m_apiKey->setObjectName(QStringLiteral("ApiKeyEdit"));
    m_apiKey->setText(SubtitleSearch::userApiKey());
    m_apiKey->setEchoMode(QLineEdit::PasswordEchoOnEdit);
    m_apiKey->setPlaceholderText(SubtitleSearch::builtInApiKey().isEmpty()
                                     ? tr("Optional: for exact (hash) matches")
                                     : tr("Optional: the built-in key is used otherwise"));
    m_apiKey->setMinimumWidth(320);
    m_besideVideo->setChecked(SubtitleSearch::saveBesideVideo());
    m_besideVideo->setToolTip(tr("Otherwise, or when the video's folder is read-only, they go to %1")
                                  .arg(SubtitleSearch::cacheDir()));

    auto *help = new QLabel(tr("Searching works without a key. An OpenSubtitles key adds exact matches by "
                               "movie hash; create one at "
                               "<a href=\"https://www.opensubtitles.com/consumers\">opensubtitles.com/consumers</a>."),
                            this);
    help->setOpenExternalLinks(true);
    help->setWordWrap(true);

    auto *form = new QFormLayout;
    form->addRow(tr("API key:"), m_apiKey);
    form->addRow(QString(), help);
    form->addRow(QString(), m_besideVideo);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    auto *layout = new QVBoxLayout(this);
    layout->addLayout(form);
    layout->addWidget(buttons);
}

void SubtitleSettingsDialog::accept()
{
    SubtitleSearch::setUserApiKey(m_apiKey->text());
    SubtitleSearch::setSaveBesideVideo(m_besideVideo->isChecked());
    QDialog::accept();
}

SubtitleDownloadDialog::SubtitleDownloadDialog(const QString &mediaPath, QWidget *parent)
    : QDialog(parent)
    , m_mediaPath(mediaPath)
    , m_parsed(SubtitleSearch::parseFileName(mediaPath))
    , m_finder(new SubtitleFinder(this))
    , m_query(new QLineEdit(this))
    , m_language(new QComboBox(this))
    , m_hashButton(new QPushButton(tr("Search by Hash (Exact Match)"), this))
    , m_nameButton(new QPushButton(tr("Search by Name"), this))
    , m_progress(new QProgressBar(this))
    , m_status(new QLabel(this))
    , m_table(new QTreeWidget(this))
    , m_settingsButton(new QPushButton(tr("Settings..."), this))
    , m_downloadButton(new QPushButton(tr("Download && Play"), this))
{
    setObjectName(QStringLiteral("SubtitleDownloadDialog"));
    setWindowTitle(tr("Download Subtitles"));
    resize(780, 460);

    m_query->setObjectName(QStringLiteral("SubtitleQuery"));
    m_query->setPlaceholderText(tr("Movie or show title"));
    m_query->setClearButtonEnabled(true);
    m_query->setText(m_parsed.title);

    m_language->setObjectName(QStringLiteral("SubtitleLanguage"));
    for (const SubtitleSearch::Language &language : SubtitleSearch::languages())
        m_language->addItem(language.name, language.code);
    m_language->setCurrentIndex(std::max(0, m_language->findData(SubtitleSearch::language())));

    // The hash identifies the exact release; files under 128 KiB have none.
    const QString local = MediaFiles::localPath(mediaPath);
    if (!local.isEmpty())
        m_hash = SubtitleHasher::hash(local);
    m_hashButton->setObjectName(QStringLiteral("SubtitleHashButton"));
    m_hashButton->setAutoDefault(false);
    m_hashButton->setEnabled(!m_hash.isEmpty());
    m_hashButton->setToolTip(m_hash.isEmpty()
                                 ? tr("Only local files of 128 KiB or more can be matched exactly")
                                 : tr("Recommended: finds subtitles made for this exact file (movie hash %1)").arg(m_hash));
    m_nameButton->setObjectName(QStringLiteral("SubtitleNameButton"));
    m_nameButton->setAutoDefault(false);

    // A thin, indeterminate bar while a request runs.
    m_progress->setObjectName(QStringLiteral("SubtitleProgress"));
    m_progress->setRange(0, 0);
    m_progress->setTextVisible(false);
    m_progress->setFixedHeight(4);
    m_progress->setVisible(false);
    m_status->setObjectName(QStringLiteral("SubtitleStatus"));
    m_status->setWordWrap(true);

    m_table->setObjectName(QStringLiteral("SubtitleResults"));
    m_table->setColumnCount(ColumnCount);
    m_table->setHeaderLabels({tr("Language"), tr("Subtitle Title / Release"), tr("Provider"), tr("Rating"), tr("Format")});
    m_table->setRootIsDecorated(false);
    m_table->setUniformRowHeights(true);
    m_table->setAlternatingRowColors(true);
    m_table->setSelectionMode(QAbstractItemView::SingleSelection);
    m_table->setItemDelegate(new BadgeDelegate(m_table));
    m_table->setSortingEnabled(true);
    m_table->sortByColumn(-1, Qt::AscendingOrder); // keep the ranking until a header is clicked
    QHeaderView *header = m_table->header();
    header->setStretchLastSection(false);
    header->setSectionResizeMode(QHeaderView::ResizeToContents);
    header->setSectionResizeMode(TitleColumn, QHeaderView::Stretch);

    m_settingsButton->setObjectName(QStringLiteral("SubtitleSettingsButton"));
    m_settingsButton->setAutoDefault(false);
    m_settingsButton->setFlat(true);
    m_downloadButton->setObjectName(QStringLiteral("SubtitleDownloadButton"));
    m_downloadButton->setDefault(true);
    auto *cancel = new QPushButton(tr("Cancel"), this);
    cancel->setObjectName(QStringLiteral("SubtitleCancelButton"));
    cancel->setAutoDefault(false);

    auto *searchRow = new QHBoxLayout;
    searchRow->addWidget(m_query, 1);
    searchRow->addWidget(m_language);
    searchRow->addWidget(m_hashButton);
    searchRow->addWidget(m_nameButton);
    auto *buttonRow = new QHBoxLayout;
    buttonRow->addWidget(m_settingsButton);
    buttonRow->addStretch();
    buttonRow->addWidget(m_downloadButton);
    buttonRow->addWidget(cancel);

    auto *layout = new QVBoxLayout(this);
    layout->addLayout(searchRow);
    layout->addWidget(m_progress);
    layout->addWidget(m_status);
    layout->addWidget(m_table, 1);
    layout->addLayout(buttonRow);

    connect(m_hashButton, &QPushButton::clicked, this, &SubtitleDownloadDialog::searchByHash);
    connect(m_nameButton, &QPushButton::clicked, this, &SubtitleDownloadDialog::searchByName);
    connect(m_query, &QLineEdit::returnPressed, this, &SubtitleDownloadDialog::searchByName);
    connect(m_settingsButton, &QPushButton::clicked, this, &SubtitleDownloadDialog::openSettings);
    connect(m_downloadButton, &QPushButton::clicked, this, &SubtitleDownloadDialog::downloadSelected);
    connect(cancel, &QPushButton::clicked, this, &SubtitleDownloadDialog::reject);
    connect(m_table, &QTreeWidget::itemSelectionChanged, this, &SubtitleDownloadDialog::updateButtons);
    connect(m_table, &QTreeWidget::itemActivated, this, &SubtitleDownloadDialog::downloadSelected);
    connect(m_finder, &SubtitleFinder::searchFinished, this, &SubtitleDownloadDialog::showResults);
    connect(m_finder, &SubtitleFinder::downloadFinished, this, [this](const QString &path) {
        setBusy(false);
        QString label = QFileInfo(path).fileName();
        if (QTreeWidgetItem *item = m_table->currentItem())
            label = item->text(LanguageColumn) + QStringLiteral(" / ") + label;
        Q_EMIT subtitleDownloaded(path, label);
        accept();
    });
    connect(m_finder, &SubtitleFinder::failed, this, [this](const QString &message) {
        setBusy(false);
        setStatus(message, true);
    });

    updateButtons();
    // Search right away: exactly when the file allows it, else by name.
    if (!m_hash.isEmpty())
        QTimer::singleShot(0, this, &SubtitleDownloadDialog::searchByHash);
    else if (!m_query->text().isEmpty())
        QTimer::singleShot(0, this, &SubtitleDownloadDialog::searchByName);
}

bool SubtitleDownloadDialog::isBusy() const
{
    return m_finder->isBusy();
}

void SubtitleDownloadDialog::searchByHash()
{
    search(SubtitleFinder::Mode::Hash);
}

void SubtitleDownloadDialog::searchByName()
{
    search(SubtitleFinder::Mode::Name);
}

void SubtitleDownloadDialog::search(SubtitleFinder::Mode mode)
{
    const QString text = m_query->text().trimmed();
    if (mode == SubtitleFinder::Mode::Name && text.isEmpty()) {
        setStatus(tr("Enter a title to search for."), true);
        return;
    }
    SubtitleQuery query;
    query.text = text;
    query.languages = {m_language->currentData().toString()};
    query.movieHash = mode == SubtitleFinder::Mode::Hash ? m_hash : QString();
    // The year and episode come from the file name; they don't apply to a
    // title the user typed.
    if (text == m_parsed.title) {
        query.year = m_parsed.year;
        query.season = m_parsed.season;
        query.episode = m_parsed.episode;
    }
    SubtitleSearch::setLanguage(m_language->currentData().toString());

    m_table->clear();
    m_results.clear();
    setBusy(true, mode == SubtitleFinder::Mode::Hash ? tr("Looking for subtitles made for this file...")
                                                     : tr("Searching for \u201c%1\u201d...").arg(text));
    m_finder->search(mode, query, MediaFiles::localPath(m_mediaPath));
}

void SubtitleDownloadDialog::showResults(const QList<SubtitleResult> &results, const QString &note)
{
    setBusy(false);
    m_results = results;
    m_table->setSortingEnabled(false);
    m_table->clear();
    for (int i = 0; i < results.size(); ++i) {
        const SubtitleResult &result = results[i];
        auto *item = new ResultItem(m_table);
        item->setData(0, kResultRole, i);
        item->setText(LanguageColumn, SubtitleSearch::languageName(result.language));
        item->setText(TitleColumn, result.fileName);
        QStringList tip;
        if (!result.release.isEmpty() && result.release != result.fileName)
            tip << result.release;
        if (!result.uploader.isEmpty())
            tip << tr("Uploaded by %1").arg(result.uploader);
        if (result.downloads > 0)
            tip << tr("%n download(s)", nullptr, static_cast<int>(result.downloads));
        if (result.machineTranslated)
            tip << tr("Machine translated");
        QStringList badges;
        if (result.hashMatch) {
            badges << tr("Exact match");
            tip.prepend(tr("Made for this exact file, so it is in sync."));
        } else if (result.releaseMatch) {
            badges << tr("Release match");
            tip.prepend(tr("Named like your file, so it is likely in sync."));
        }
        if (result.hearingImpaired) {
            badges << QStringLiteral("HI");
            tip << tr("For the hearing impaired");
        }
        item->setData(TitleColumn, kBadgeRole, badges);
        item->setToolTip(TitleColumn, tip.join(QLatin1Char('\n')));
        item->setText(ProviderColumn, result.provider);
        item->setText(RatingColumn, result.rating > 0 ? QString::number(result.rating, 'f', 1) : QStringLiteral("–"));
        item->setData(RatingColumn, kSortRole, result.rating);
        item->setTextAlignment(RatingColumn, Qt::AlignCenter);
        item->setText(FormatColumn, QLatin1Char('.') + result.format);
        item->setTextAlignment(FormatColumn, Qt::AlignCenter);
    }
    m_table->setSortingEnabled(true);
    if (m_table->topLevelItemCount() > 0)
        m_table->setCurrentItem(m_table->topLevelItem(0));

    QString status = results.isEmpty()
        ? tr("No subtitles found in %1. Try another title or language.").arg(m_language->currentText())
        : tr("%n subtitle(s) found", nullptr, static_cast<int>(results.size()));
    if (!note.isEmpty())
        status = note + QLatin1Char(' ') + status;
    setStatus(status, results.isEmpty());
    updateButtons();
}

void SubtitleDownloadDialog::downloadSelected()
{
    QTreeWidgetItem *item = m_table->currentItem();
    if (!item || isBusy())
        return;
    const int index = item->data(0, kResultRole).toInt();
    if (index < 0 || index >= m_results.size())
        return;
    const SubtitleResult &result = m_results[index];
    const QString path = SubtitleSearch::savePath(MediaFiles::localPath(m_mediaPath), result.language.toLower(),
                                                  result.format, SubtitleSearch::saveBesideVideo());
    setBusy(true, tr("Downloading %1...").arg(result.fileName));
    m_finder->download(result, path);
}

void SubtitleDownloadDialog::reject()
{
    // Esc or Cancel first stops a running request, then closes.
    if (isBusy()) {
        m_finder->cancel();
        setBusy(false);
        setStatus(tr("Cancelled."));
        return;
    }
    QDialog::reject();
}

void SubtitleDownloadDialog::setBusy(bool busy, const QString &status)
{
    m_progress->setVisible(busy);
    m_query->setEnabled(!busy);
    m_language->setEnabled(!busy);
    m_hashButton->setEnabled(!busy && !m_hash.isEmpty());
    m_nameButton->setEnabled(!busy);
    m_settingsButton->setEnabled(!busy);
    m_table->setEnabled(!busy);
    if (!status.isEmpty())
        setStatus(status);
    updateButtons();
}

void SubtitleDownloadDialog::setStatus(const QString &text, bool error)
{
    m_status->setText(text);
    QPalette palette = m_status->palette();
    palette.setColor(QPalette::WindowText, error ? kErrorColor : QApplication::palette().color(QPalette::WindowText));
    m_status->setPalette(palette);
}

void SubtitleDownloadDialog::openSettings()
{
    SubtitleSettingsDialog settings(this);
    settings.exec();
}

void SubtitleDownloadDialog::updateButtons()
{
    m_downloadButton->setEnabled(!isBusy() && m_table->currentItem() && !m_table->selectedItems().isEmpty());
}
