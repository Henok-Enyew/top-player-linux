#include "LyricsDialogs.h"
#include "Theme.h"

#include <QApplication>
#include <QClipboard>
#include <QDialogButtonBox>
#include <QFontDatabase>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSplitter>
#include <QTreeWidget>
#include <QVBoxLayout>

#include <cmath>

namespace {

QString duration(double seconds)
{
    if (seconds <= 0)
        return QStringLiteral("-");
    const long long s = std::llround(seconds);
    return QStringLiteral("%1:%2").arg(s / 60).arg(s % 60, 2, 10, QLatin1Char('0'));
}

} // namespace

LyricsDownloadDialog::LyricsDownloadDialog(const LyricsClient::Query &query, QWidget *parent)
    : QDialog(parent)
    , m_query(query)
    , m_client(new LyricsClient(this))
    , m_title(new QLineEdit(query.title, this))
    , m_artist(new QLineEdit(query.artist, this))
    , m_searchButton(new QPushButton(tr("Search"), this))
    , m_list(new QTreeWidget(this))
    , m_preview(new QPlainTextEdit(this))
    , m_status(new QLabel(this))
    , m_useButton(new QPushButton(tr("Use Lyrics"), this))
{
    setWindowTitle(tr("Download Lyrics"));
    resize(760, 520);

    m_title->setObjectName(QStringLiteral("LyricsTitleEdit"));
    m_artist->setObjectName(QStringLiteral("LyricsArtistEdit"));
    m_title->setPlaceholderText(tr("Song title"));
    m_artist->setPlaceholderText(tr("Artist (optional, but helps)"));
    auto *form = new QFormLayout;
    form->addRow(tr("Title:"), m_title);
    auto *artistRow = new QHBoxLayout;
    artistRow->addWidget(m_artist, 1);
    artistRow->addWidget(m_searchButton);
    form->addRow(tr("Artist:"), artistRow);

    m_list->setObjectName(QStringLiteral("LyricsResults"));
    m_list->setHeaderLabels({tr("Title"), tr("Artist"), tr("Album"), tr("Length"), tr("Type")});
    m_list->setRootIsDecorated(false);
    m_list->setAlternatingRowColors(true);
    m_list->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    m_preview->setReadOnly(true);
    m_preview->setObjectName(QStringLiteral("LyricsPreview"));
    m_preview->setPlaceholderText(tr("Select a result to preview its lyrics"));

    auto *splitter = new QSplitter(Qt::Vertical, this);
    splitter->addWidget(m_list);
    splitter->addWidget(m_preview);
    splitter->setSizes({220, 200});

    m_status->setWordWrap(true);
    m_status->setStyleSheet(QStringLiteral("color: %1;").arg(Theme::hex(Theme::TextSecondary)));
    m_status->setText(tr("Lyrics come from LRCLIB (free, time-synced) with lyrics.ovh as a fallback."));

    auto *buttons = new QDialogButtonBox(this);
    auto *aiButton = buttons->addButton(tr("Generate with AI..."), QDialogButtonBox::ActionRole);
    buttons->addButton(m_useButton, QDialogButtonBox::AcceptRole);
    buttons->addButton(QDialogButtonBox::Cancel);
    m_useButton->setEnabled(false);
    m_useButton->setDefault(false);
    m_searchButton->setDefault(true);

    auto *layout = new QVBoxLayout(this);
    layout->addLayout(form);
    layout->addWidget(splitter, 1);
    layout->addWidget(m_status);
    layout->addWidget(buttons);

    connect(m_searchButton, &QPushButton::clicked, this, &LyricsDownloadDialog::search);
    connect(m_title, &QLineEdit::returnPressed, this, &LyricsDownloadDialog::search);
    connect(m_artist, &QLineEdit::returnPressed, this, &LyricsDownloadDialog::search);
    connect(m_list, &QTreeWidget::currentItemChanged, this, &LyricsDownloadDialog::updatePreview);
    connect(m_list, &QTreeWidget::itemDoubleClicked, this, &LyricsDownloadDialog::useSelected);
    connect(m_useButton, &QPushButton::clicked, this, &LyricsDownloadDialog::useSelected);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(aiButton, &QPushButton::clicked, this, [this] {
        // The prompt dialog opens from the player, once this one is gone.
        reject();
        Q_EMIT aiPromptRequested();
    });
    connect(m_client, &LyricsClient::searchFinished, this, &LyricsDownloadDialog::showResults);
    connect(m_client, &LyricsClient::failed, this, [this](const QString &message) {
        m_searchButton->setEnabled(true);
        m_status->setText(message);
    });

    // Start right away when there is something to search for.
    if (!m_query.title.trimmed().isEmpty())
        QMetaObject::invokeMethod(this, &LyricsDownloadDialog::search, Qt::QueuedConnection);
}

void LyricsDownloadDialog::search()
{
    LyricsClient::Query q = m_query;
    q.title = m_title->text();
    q.artist = m_artist->text();
    m_results.clear();
    m_list->clear();
    m_preview->clear();
    m_useButton->setEnabled(false);
    m_searchButton->setEnabled(false);
    m_status->setText(tr("Searching..."));
    m_client->search(q);
}

void LyricsDownloadDialog::showResults(const QList<LyricsClient::Result> &results)
{
    m_searchButton->setEnabled(true);
    m_results = results;
    m_list->clear();
    for (const LyricsClient::Result &r : results) {
        auto *item = new QTreeWidgetItem(m_list);
        item->setText(0, r.title);
        item->setText(1, r.artist);
        item->setText(2, r.album);
        item->setText(3, duration(r.duration));
        item->setText(4, r.instrumental ? tr("Instrumental") : r.isSynced() ? tr("Synced") : tr("Plain"));
        item->setToolTip(4, r.source);
        if (r.isSynced())
            item->setForeground(4, Theme::Accent);
    }
    if (results.isEmpty()) {
        m_status->setText(tr("No lyrics found. Check the spelling, try without the artist, "
                             "or use \"Generate with AI...\"."));
        return;
    }
    m_status->setText(tr("%n result(s). Synced lyrics follow the song line by line.", nullptr, results.size()));
    m_list->setCurrentItem(m_list->topLevelItem(0));
}

void LyricsDownloadDialog::updatePreview()
{
    const int row = m_list->indexOfTopLevelItem(m_list->currentItem());
    if (row < 0 || row >= m_results.size()) {
        m_preview->clear();
        m_useButton->setEnabled(false);
        return;
    }
    const LyricsClient::Result &r = m_results[row];
    m_preview->setPlainText(r.instrumental && r.text().isEmpty() ? tr("(Instrumental)") : r.text());
    m_useButton->setEnabled(!r.text().trimmed().isEmpty());
}

void LyricsDownloadDialog::useSelected()
{
    const int row = m_list->indexOfTopLevelItem(m_list->currentItem());
    if (row < 0 || row >= m_results.size() || m_results[row].text().trimmed().isEmpty())
        return;
    const LyricsClient::Result &r = m_results[row];
    Q_EMIT lyricsChosen(r.text(), QStringLiteral("%1 – %2 (%3)").arg(r.artist, r.title, r.source));
    accept();
}

AiLyricsPromptDialog::AiLyricsPromptDialog(const QString &prompt, QWidget *parent)
    : QDialog(parent)
    , m_prompt(new QPlainTextEdit(prompt, this))
    , m_status(new QLabel(this))
{
    setWindowTitle(tr("Generate Lyrics with AI"));
    resize(700, 560);

    auto *intro = new QLabel(tr("<b>1.</b> Copy this prompt and send it to any chat AI (ChatGPT, Claude, Gemini, ...).<br>"
                                "<b>2.</b> Download the .lrc file it gives you and load it, or copy its whole answer "
                                "and press <i>Paste AI Answer</i>."),
                             this);
    intro->setWordWrap(true);
    m_prompt->setObjectName(QStringLiteral("AiPrompt"));
    m_prompt->setReadOnly(false); // the user may tweak it before copying
    QFont mono = QFontDatabase::systemFont(QFontDatabase::FixedFont);
    m_prompt->setFont(mono);
    m_status->setWordWrap(true);

    auto *copy = new QPushButton(tr("Copy Prompt"), this);
    copy->setObjectName(QStringLiteral("CopyPromptButton"));
    auto *paste = new QPushButton(tr("Paste AI Answer"), this);
    paste->setObjectName(QStringLiteral("PasteAnswerButton"));
    auto *load = new QPushButton(tr("Load .lrc File..."), this);
    auto *close = new QPushButton(tr("Close"), this);
    copy->setDefault(true);

    auto *buttons = new QHBoxLayout;
    buttons->addWidget(copy);
    buttons->addStretch(1);
    buttons->addWidget(paste);
    buttons->addWidget(load);
    buttons->addWidget(close);

    auto *layout = new QVBoxLayout(this);
    layout->addWidget(intro);
    layout->addWidget(m_prompt, 1);
    layout->addWidget(m_status);
    layout->addLayout(buttons);

    connect(copy, &QPushButton::clicked, this, [this] {
        QGuiApplication::clipboard()->setText(m_prompt->toPlainText());
        m_status->setStyleSheet(QStringLiteral("color: %1;").arg(Theme::hex(Theme::Accent)));
        m_status->setText(tr("Prompt copied. Paste it into your AI chat."));
    });
    connect(paste, &QPushButton::clicked, this, [this] {
        const QString text = QGuiApplication::clipboard()->text();
        if (text.trimmed().isEmpty() || text == m_prompt->toPlainText()) {
            showError(tr("Copy the AI's answer first, then press Paste AI Answer."));
            return;
        }
        Q_EMIT lyricsPasted(text);
    });
    connect(load, &QPushButton::clicked, this, &AiLyricsPromptDialog::loadFileRequested);
    connect(close, &QPushButton::clicked, this, &QDialog::reject);
}

QString AiLyricsPromptDialog::prompt() const
{
    return m_prompt->toPlainText();
}

void AiLyricsPromptDialog::showError(const QString &message)
{
    m_status->setStyleSheet(QStringLiteral("color: #FF6B6B;"));
    m_status->setText(message);
}
