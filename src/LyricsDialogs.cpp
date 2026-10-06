#include "LyricsDialogs.h"
#include "Theme.h"

#include <QApplication>
#include <QButtonGroup>
#include <QCheckBox>
#include <QColorDialog>
#include <QClipboard>
#include <QDialogButtonBox>
#include <QFontComboBox>
#include <QFontDatabase>
#include <QGroupBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QPainter>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSlider>
#include <QToolButton>
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

// Highlight colors offered as swatches; anything else is "Custom...".
struct Swatch {
    const char *name;
    QRgb color;
};
const Swatch kSwatches[] = {
    {QT_TRANSLATE_NOOP("LyricsStyleDialog", "White"), 0xFFFFFF},
    {QT_TRANSLATE_NOOP("LyricsStyleDialog", "Cyan"), 0x00D2FF},
    {QT_TRANSLATE_NOOP("LyricsStyleDialog", "Gold"), 0xFFD166},
    {QT_TRANSLATE_NOOP("LyricsStyleDialog", "Rose"), 0xFF7AA8},
    {QT_TRANSLATE_NOOP("LyricsStyleDialog", "Lime"), 0x9BE564},
    {QT_TRANSLATE_NOOP("LyricsStyleDialog", "Violet"), 0xB69CFF},
};

QIcon swatchIcon(const QColor &color, int size = 18)
{
    QPixmap pixmap(size * 2, size * 2);
    pixmap.setDevicePixelRatio(2);
    pixmap.fill(Qt::transparent);
    QPainter p(&pixmap);
    p.setRenderHint(QPainter::Antialiasing);
    p.setPen(QPen(QColor(255, 255, 255, 70), 1));
    p.setBrush(color);
    p.drawEllipse(QRectF(1.5, 1.5, size - 3, size - 3));
    return QIcon(pixmap);
}

QLabel *valueLabel(QWidget *parent)
{
    auto *label = new QLabel(parent);
    label->setMinimumWidth(44);
    label->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    label->setStyleSheet(QStringLiteral("color: %1;").arg(Theme::hex(Theme::Accent)));
    return label;
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

LyricsStyleDialog::LyricsStyleDialog(const LyricsStyle &style, QWidget *parent)
    : QDialog(parent)
    , m_style(style)
    , m_original(style)
    , m_defaultFont(new QCheckBox(tr("Player font"), this))
    , m_font(new QFontComboBox(this))
    , m_size(new QSlider(Qt::Horizontal, this))
    , m_sizeLabel(valueLabel(this))
    , m_spacing(new QSlider(Qt::Horizontal, this))
    , m_spacingLabel(valueLabel(this))
    , m_align(new QButtonGroup(this))
    , m_colors(new QButtonGroup(this))
    , m_customColor(new QPushButton(tr("Custom..."), this))
    , m_bold(new QCheckBox(tr("Bold current line"), this))
    , m_glow(new QCheckBox(tr("Glow behind current line"), this))
    , m_dim(new QSlider(Qt::Horizontal, this))
    , m_dimLabel(valueLabel(this))
{
    setObjectName(QStringLiteral("LyricsStyleDialog"));
    setWindowTitle(tr("Lyrics Appearance"));
    setMinimumWidth(460);

    // Text
    auto *text = new QGroupBox(tr("Text"), this);
    auto *textForm = new QFormLayout(text);
    m_font->setObjectName(QStringLiteral("LyricsFontCombo"));
    m_defaultFont->setObjectName(QStringLiteral("LyricsDefaultFont"));
    auto *fontRow = new QHBoxLayout;
    fontRow->addWidget(m_font, 1);
    fontRow->addWidget(m_defaultFont);
    textForm->addRow(tr("Font:"), fontRow);

    m_size->setObjectName(QStringLiteral("LyricsSizeSlider"));
    m_size->setRange(qRound(LyricsStyle::kMinScale * 100), qRound(LyricsStyle::kMaxScale * 100));
    m_size->setSingleStep(5);
    m_size->setPageStep(10);
    auto *sizeRow = new QHBoxLayout;
    sizeRow->addWidget(m_size, 1);
    sizeRow->addWidget(m_sizeLabel);
    textForm->addRow(tr("Size:"), sizeRow);

    m_spacing->setObjectName(QStringLiteral("LyricsSpacingSlider"));
    m_spacing->setRange(10, 200);
    m_spacing->setSingleStep(5);
    auto *spacingRow = new QHBoxLayout;
    spacingRow->addWidget(m_spacing, 1);
    spacingRow->addWidget(m_spacingLabel);
    textForm->addRow(tr("Line spacing:"), spacingRow);

    auto *alignRow = new QHBoxLayout;
    alignRow->setSpacing(4);
    const QList<QPair<QString, Qt::Alignment>> alignments{
        {tr("Left"), Qt::AlignLeft}, {tr("Center"), Qt::AlignHCenter}, {tr("Right"), Qt::AlignRight}};
    for (const auto &[label, align] : alignments) {
        auto *button = new QToolButton(this);
        button->setText(label);
        button->setCheckable(true);
        button->setMinimumWidth(64);
        button->setObjectName(QStringLiteral("LyricsAlign") + label);
        m_align->addButton(button, int(align));
        alignRow->addWidget(button);
    }
    alignRow->addStretch();
    textForm->addRow(tr("Alignment:"), alignRow);

    // Current line
    auto *highlight = new QGroupBox(tr("Current line"), this);
    auto *highlightLayout = new QVBoxLayout(highlight);
    auto *swatches = new QHBoxLayout;
    swatches->setSpacing(4);
    // Unchecked by hand when the color is a custom one.
    m_colors->setExclusive(false);
    int id = 0;
    for (const Swatch &swatch : kSwatches) {
        auto *button = new QToolButton(this);
        button->setIcon(swatchIcon(QColor(swatch.color)));
        button->setIconSize(QSize(18, 18));
        button->setCheckable(true);
        button->setToolTip(tr(swatch.name));
        m_colors->addButton(button, id++);
        swatches->addWidget(button);
    }
    m_customColor->setObjectName(QStringLiteral("LyricsCustomColor"));
    m_customColor->setAutoDefault(false);
    swatches->addSpacing(6);
    swatches->addWidget(m_customColor);
    swatches->addStretch();
    highlightLayout->addLayout(swatches);
    auto *toggles = new QHBoxLayout;
    toggles->addWidget(m_bold);
    toggles->addWidget(m_glow);
    toggles->addStretch();
    highlightLayout->addLayout(toggles);

    // Over videos
    auto *video = new QGroupBox(tr("Over videos"), this);
    auto *videoForm = new QFormLayout(video);
    m_dim->setObjectName(QStringLiteral("LyricsDimSlider"));
    m_dim->setRange(0, 100);
    m_dim->setSingleStep(5);
    auto *dimRow = new QHBoxLayout;
    dimRow->addWidget(m_dim, 1);
    dimRow->addWidget(m_dimLabel);
    videoForm->addRow(tr("Darken video:"), dimRow);

    auto *hint = new QLabel(tr("Tip: drag or scroll the lyrics to look ahead, click a timed line to play from it, "
                               "and Ctrl+scroll over them to resize."), this);
    hint->setWordWrap(true);
    hint->setStyleSheet(QStringLiteral("color: %1;").arg(Theme::hex(Theme::TextSecondary)));

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel | QDialogButtonBox::RestoreDefaults, this);
    buttons->button(QDialogButtonBox::RestoreDefaults)->setObjectName(QStringLiteral("LyricsStyleReset"));

    auto *layout = new QVBoxLayout(this);
    layout->addWidget(text);
    layout->addWidget(highlight);
    layout->addWidget(video);
    layout->addWidget(hint);
    layout->addWidget(buttons);

    connect(m_defaultFont, &QCheckBox::toggled, this, [this](bool useDefault) {
        edit([this, useDefault](LyricsStyle &s) { s.family = useDefault ? QString() : m_font->currentFont().family(); });
    });
    connect(m_font, &QFontComboBox::currentFontChanged, this, [this](const QFont &font) {
        edit([font](LyricsStyle &s) { s.family = font.family(); });
    });
    connect(m_size, &QSlider::valueChanged, this, [this](int value) {
        edit([value](LyricsStyle &s) { s.scale = value / 100.0; });
    });
    connect(m_spacing, &QSlider::valueChanged, this, [this](int value) {
        edit([value](LyricsStyle &s) { s.spacing = value / 100.0; });
    });
    connect(m_align, &QButtonGroup::idClicked, this, [this](int align) {
        edit([align](LyricsStyle &s) { s.align = Qt::Alignment(align); });
    });
    connect(m_colors, &QButtonGroup::idClicked, this, [this](int index) {
        edit([index](LyricsStyle &s) { s.highlight = QColor(kSwatches[index].color); });
    });
    connect(m_customColor, &QPushButton::clicked, this, [this] {
        const QColor color = QColorDialog::getColor(m_style.highlight, this, tr("Current Line Color"));
        if (color.isValid())
            edit([color](LyricsStyle &s) { s.highlight = color; });
    });
    connect(m_bold, &QCheckBox::toggled, this, [this](bool on) { edit([on](LyricsStyle &s) { s.bold = on; }); });
    connect(m_glow, &QCheckBox::toggled, this, [this](bool on) { edit([on](LyricsStyle &s) { s.glow = on; }); });
    connect(m_dim, &QSlider::valueChanged, this, [this](int value) { edit([value](LyricsStyle &s) { s.dim = value; }); });
    connect(buttons->button(QDialogButtonBox::RestoreDefaults), &QPushButton::clicked, this,
            [this] { setLyricsStyle(LyricsStyle()); });
    connect(buttons, &QDialogButtonBox::accepted, this, [this] {
        m_style.save();
        accept();
    });
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(this, &QDialog::rejected, this, [this] {
        // Put the old look back.
        m_style = m_original;
        Q_EMIT styleChanged(m_style);
    });

    syncControls();
}

void LyricsStyleDialog::setLyricsStyle(const LyricsStyle &style)
{
    m_style = style;
    syncControls();
    Q_EMIT styleChanged(m_style);
}

void LyricsStyleDialog::edit(const std::function<void(LyricsStyle &)> &change)
{
    if (m_syncing)
        return;
    change(m_style);
    syncControls();
    Q_EMIT styleChanged(m_style);
}

void LyricsStyleDialog::syncControls()
{
    m_syncing = true;
    m_defaultFont->setChecked(m_style.family.isEmpty());
    m_font->setEnabled(!m_style.family.isEmpty());
    // Unticking "Player font" starts from the font the lyrics have now.
    m_font->setCurrentFont(m_style.family.isEmpty() ? QApplication::font() : QFont(m_style.family));
    m_size->setValue(qRound(m_style.scale * 100));
    m_sizeLabel->setText(QStringLiteral("%1%").arg(qRound(m_style.scale * 100)));
    m_spacing->setValue(qRound(m_style.spacing * 100));
    m_spacingLabel->setText(QStringLiteral("%1%").arg(qRound(m_style.spacing * 100)));
    if (QAbstractButton *button = m_align->button(int(m_style.align)))
        button->setChecked(true);
    bool preset = false;
    for (int i = 0; i < int(std::size(kSwatches)); ++i) {
        const bool match = QColor(kSwatches[i].color) == m_style.highlight;
        m_colors->button(i)->setChecked(match);
        preset = preset || match;
    }
    m_customColor->setIcon(preset ? QIcon() : swatchIcon(m_style.highlight, 14));
    m_bold->setChecked(m_style.bold);
    m_glow->setChecked(m_style.glow);
    m_dim->setValue(m_style.dim);
    m_dimLabel->setText(QStringLiteral("%1%").arg(m_style.dim));
    m_syncing = false;
}
