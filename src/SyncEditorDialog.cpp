#include "SyncEditorDialog.h"
#include "MpvWidget.h"
#include "Theme.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QKeyEvent>
#include <QLabel>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QStandardPaths>
#include <QTreeWidget>
#include <QVBoxLayout>

#include <cmath>

namespace {

constexpr double kSeekStep = 3.0;
constexpr double kNudgeStep = 0.1;
constexpr int kMaxUndo = 200;

enum Column { NumberColumn, StartColumn, EndColumn, TextColumn };

QString stamp(double seconds)
{
    return seconds < 0 ? QStringLiteral("--:--.--") : Lyrics::formatLrcTime(seconds);
}

} // namespace

SyncEditorDialog::SyncEditorDialog(MpvWidget *mpv, Mode mode, QWidget *parent)
    : QDialog(parent)
    , m_mpv(mpv)
    , m_mode(mode)
    , m_list(new QTreeWidget(this))
    , m_time(new QLabel(this))
    , m_hint(new QLabel(this))
    , m_shiftFollowing(new QCheckBox(tr("Shift following lines too"), this))
    , m_speed(new QComboBox(this))
    , m_playButton(new QPushButton(this))
{
    const bool subs = mode == Mode::Subtitles;
    setWindowTitle(subs ? tr("Subtitle Sync Editor") : tr("Lyrics Sync Editor"));
    setObjectName(QStringLiteral("SyncEditorDialog"));
    resize(720, 600);

    m_hint->setWordWrap(true);
    m_hint->setText(subs ? tr("Select a line, play, and press <b>Space</b> the moment it is spoken. Following lines "
                              "move with it, so syncing one line early on often fixes the whole file.")
                         : tr("Play the song and press <b>Space</b> the moment the highlighted line starts. "
                              "<b>Backspace</b> undoes, <b>←/→</b> seek 3 s, <b>[ / ]</b> nudge a line, "
                              "<b>P</b> plays or pauses."));

    // Transport.
    auto *back = new QPushButton(tr("⏪ 3s"), this);
    auto *forward = new QPushButton(tr("3s ⏩"), this);
    m_playButton->setText(tr("Play / Pause"));
    for (QPushButton *b : {back, forward, m_playButton})
        b->setFocusPolicy(Qt::NoFocus);
    m_speed->addItem(QStringLiteral("0.5x"), 0.5);
    m_speed->addItem(QStringLiteral("0.75x"), 0.75);
    m_speed->addItem(QStringLiteral("1.0x"), 1.0);
    m_speed->setCurrentIndex(2);
    m_speed->setFocusPolicy(Qt::NoFocus);
    m_speed->setToolTip(tr("Slower playback makes tapping easier"));
    QFont timeFont = m_time->font();
    timeFont.setPointSizeF(timeFont.pointSizeF() * 1.3);
    timeFont.setBold(true);
    m_time->setFont(timeFont);
    m_time->setStyleSheet(QStringLiteral("color: %1;").arg(Theme::hex(Theme::Accent)));
    m_time->setText(stamp(0));
    auto *transport = new QHBoxLayout;
    transport->addWidget(back);
    transport->addWidget(m_playButton);
    transport->addWidget(forward);
    transport->addWidget(new QLabel(tr("Speed:"), this));
    transport->addWidget(m_speed);
    transport->addStretch(1);
    transport->addWidget(m_time);

    // The lines.
    m_list->setObjectName(QStringLiteral("SyncLines"));
    m_list->setHeaderLabels({QStringLiteral("#"), tr("Start"), tr("End"), subs ? tr("Subtitle") : tr("Lyric")});
    m_list->setRootIsDecorated(false);
    m_list->setUniformRowHeights(!subs);
    m_list->setColumnHidden(EndColumn, !subs);
    m_list->header()->setSectionResizeMode(TextColumn, QHeaderView::Stretch);
    m_list->header()->setSectionResizeMode(NumberColumn, QHeaderView::ResizeToContents);
    m_list->header()->setStretchLastSection(true);
    m_list->installEventFilter(this);

    // Editing.
    auto *tapButton = new QPushButton(tr("Tap  (Space)"), this);
    tapButton->setObjectName(QStringLiteral("TapButton"));
    tapButton->setStyleSheet(QStringLiteral("QPushButton { background: %1; color: #0B0C0F; font-weight: bold; "
                                            "border-radius: 5px; padding: 6px 16px; }")
                                 .arg(Theme::hex(Theme::Accent)));
    auto *undoButton = new QPushButton(tr("Undo"), this);
    auto *earlier = new QPushButton(tr("-0.1s"), this);
    auto *later = new QPushButton(tr("+0.1s"), this);
    for (QPushButton *b : {tapButton, undoButton, earlier, later})
        b->setFocusPolicy(Qt::NoFocus);
    m_shiftFollowing->setChecked(subs);
    m_shiftFollowing->setFocusPolicy(Qt::NoFocus);
    auto *editing = new QHBoxLayout;
    editing->addWidget(tapButton);
    editing->addWidget(undoButton);
    editing->addSpacing(12);
    editing->addWidget(earlier);
    editing->addWidget(later);
    editing->addWidget(m_shiftFollowing);
    editing->addStretch(1);

    auto *buttons = new QDialogButtonBox(this);
    QPushButton *saveButton = buttons->addButton(subs ? tr("Save && Load Subtitles") : tr("Save Lyrics"),
                                                 QDialogButtonBox::AcceptRole);
    saveButton->setObjectName(QStringLiteral("SyncSaveButton"));
    QPushButton *textButton = nullptr;
    if (subs) {
        QPushButton *open = buttons->addButton(tr("Open Subtitle File..."), QDialogButtonBox::ActionRole);
        connect(open, &QPushButton::clicked, this, [this] {
            const QString file = QFileDialog::getOpenFileName(this, tr("Open Subtitle File"), {},
                                                              tr("Subtitles (*.srt *.vtt);;All Files (*)"));
            if (!file.isEmpty())
                loadFile(file);
        });
    } else {
        textButton = buttons->addButton(tr("Edit Text..."), QDialogButtonBox::ActionRole);
        connect(textButton, &QPushButton::clicked, this, &SyncEditorDialog::editText);
    }
    QPushButton *exportButton = buttons->addButton(tr("Export As..."), QDialogButtonBox::ActionRole);
    buttons->addButton(QDialogButtonBox::Close);
    for (QAbstractButton *b : buttons->buttons())
        b->setFocusPolicy(Qt::NoFocus);

    auto *layout = new QVBoxLayout(this);
    layout->addWidget(m_hint);
    layout->addLayout(transport);
    layout->addWidget(m_list, 1);
    layout->addLayout(editing);
    layout->addWidget(buttons);

    connect(back, &QPushButton::clicked, this, [this] {
        m_mpv->command({QStringLiteral("seek"), QString::number(-kSeekStep), QStringLiteral("relative+exact")});
    });
    connect(forward, &QPushButton::clicked, this, [this] {
        m_mpv->command({QStringLiteral("seek"), QString::number(kSeekStep), QStringLiteral("relative+exact")});
    });
    connect(m_playButton, &QPushButton::clicked, m_mpv, &MpvWidget::togglePause);
    connect(m_speed, &QComboBox::currentIndexChanged, this, [this] {
        m_mpv->setMpvProperty(QStringLiteral("speed"), QString::number(m_speed->currentData().toDouble()));
    });
    connect(tapButton, &QPushButton::clicked, this, [this] { tap(); });
    connect(undoButton, &QPushButton::clicked, this, &SyncEditorDialog::undo);
    connect(earlier, &QPushButton::clicked, this, [this] { nudge(-kNudgeStep); });
    connect(later, &QPushButton::clicked, this, [this] { nudge(kNudgeStep); });
    connect(saveButton, &QPushButton::clicked, this, [this] {
        if (!save().isEmpty())
            accept();
    });
    connect(exportButton, &QPushButton::clicked, this, &SyncEditorDialog::exportAs);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    // Double-click a line to hear it again from a moment before.
    connect(m_list, &QTreeWidget::itemDoubleClicked, this, [this](QTreeWidgetItem *item) {
        const int row = m_list->indexOfTopLevelItem(item);
        if (row >= 0 && m_doc.lines[row].start >= 0)
            m_mpv->command({QStringLiteral("seek"), QString::number(std::max(0.0, m_doc.lines[row].start - 1.0), 'f', 3),
                            QStringLiteral("absolute+exact")});
    });
    connect(m_mpv, &MpvWidget::propertyUpdated, this, [this](const QString &name, const QVariant &value) {
        if (name == QLatin1String("time-pos") && value.isValid())
            updatePlaying(value.toDouble());
    });
    // Back to normal speed once done.
    connect(this, &QDialog::finished, m_mpv, [mpv = m_mpv, this] {
        if (m_speed->currentData().toDouble() != 1.0)
            mpv->setMpvProperty(QStringLiteral("speed"), QStringLiteral("1"));
    });
    m_list->setFocus();
}

double SyncEditorDialog::now() const
{
    return m_mpv->mpvProperty(QStringLiteral("time-pos")).toDouble();
}

void SyncEditorDialog::setDocument(const Lyrics::Document &doc)
{
    m_doc = doc;
    m_undo.clear();
    rebuild();
    // Start at the first line without a time, else at the top.
    int first = 0;
    for (int i = 0; i < m_doc.lines.size(); ++i) {
        if (m_doc.lines[i].start < 0) {
            first = i;
            break;
        }
    }
    selectLine(first);
    if (m_doc.isEmpty() && m_mode == Mode::Lyrics)
        m_hint->setText(tr("No lyrics yet: press <b>Edit Text...</b> and paste the song's words, one line per row. "
                           "Then play the song and press <b>Space</b> as each line starts."));
}

bool SyncEditorDialog::loadFile(const QString &path)
{
    const Lyrics::Document doc = Lyrics::load(path);
    if (doc.isEmpty())
        return false;
    m_source = path;
    setWindowTitle(QStringLiteral("%1 – %2").arg(m_mode == Mode::Subtitles ? tr("Subtitle Sync Editor")
                                                                                 : tr("Lyrics Sync Editor"),
                                                      QFileInfo(path).fileName()));
    setDocument(doc);
    return true;
}

void SyncEditorDialog::rebuild()
{
    m_list->clear();
    m_playing = -1;
    for (int i = 0; i < m_doc.lines.size(); ++i) {
        auto *item = new QTreeWidgetItem(m_list);
        item->setText(NumberColumn, QString::number(i + 1));
        item->setForeground(NumberColumn, Theme::TextDim);
        updateRow(i);
    }
}

void SyncEditorDialog::updateRow(int index)
{
    QTreeWidgetItem *item = m_list->topLevelItem(index);
    if (!item)
        return;
    const Lyrics::Line &line = m_doc.lines[index];
    item->setText(StartColumn, stamp(line.start));
    item->setText(EndColumn, stamp(line.end));
    item->setText(TextColumn, line.text.isEmpty() ? tr("(instrumental break)") : QString(line.text).replace(QLatin1Char('\n'), QStringLiteral(" / ")));
    item->setForeground(StartColumn, line.start < 0 ? Theme::TextDim : Theme::Accent);
    item->setForeground(TextColumn, line.text.isEmpty() ? Theme::TextDim : Theme::TextPrimary);
}

int SyncEditorDialog::selectedLine() const
{
    return m_list->indexOfTopLevelItem(m_list->currentItem());
}

void SyncEditorDialog::selectLine(int index)
{
    if (index < 0 || index >= m_list->topLevelItemCount())
        return;
    m_list->setCurrentItem(m_list->topLevelItem(index));
    m_list->scrollToItem(m_list->currentItem(), QAbstractItemView::PositionAtCenter);
}

void SyncEditorDialog::setShiftFollowing(bool shift)
{
    m_shiftFollowing->setChecked(shift);
}

void SyncEditorDialog::pushUndo()
{
    m_undo.append({m_doc, selectedLine()});
    if (m_undo.size() > kMaxUndo)
        m_undo.removeFirst();
}

void SyncEditorDialog::changed()
{
    if (m_mode == Mode::Lyrics)
        Q_EMIT documentEdited(m_doc);
}

void SyncEditorDialog::tap(double seconds)
{
    const int row = selectedLine();
    if (row < 0)
        return;
    if (seconds < 0)
        seconds = now();
    // The lyrics view applies the file's offset; store times without it.
    if (m_mode == Mode::Lyrics)
        seconds += m_doc.offset;
    pushUndo();
    Lyrics::Line &line = m_doc.lines[row];
    const double previous = line.start;
    const double delta = previous >= 0 ? seconds - previous : 0;
    if (line.end > line.start && previous >= 0)
        line.end += delta; // a cue keeps its length
    line.start = seconds;
    updateRow(row);
    if (m_shiftFollowing->isChecked() && previous >= 0) {
        for (int i = row + 1; i < m_doc.lines.size(); ++i) {
            Lyrics::Line &next = m_doc.lines[i];
            if (next.start >= 0)
                next.start = std::max(0.0, next.start + delta);
            if (next.end >= 0)
                next.end = std::max(0.0, next.end + delta);
            updateRow(i);
        }
    }
    selectLine(row + 1);
    changed();
}

void SyncEditorDialog::nudge(double seconds)
{
    const int row = selectedLine();
    if (row < 0 || m_doc.lines[row].start < 0)
        return;
    pushUndo();
    const int last = m_shiftFollowing->isChecked() ? m_doc.lines.size() - 1 : row;
    for (int i = row; i <= last; ++i) {
        Lyrics::Line &line = m_doc.lines[i];
        if (line.start >= 0)
            line.start = std::max(0.0, line.start + seconds);
        if (line.end >= 0)
            line.end = std::max(0.0, line.end + seconds);
        updateRow(i);
    }
    changed();
}

bool SyncEditorDialog::undo()
{
    if (m_undo.isEmpty())
        return false;
    const UndoStep step = m_undo.takeLast();
    m_doc = step.doc;
    rebuild();
    // After undoing a tap, the line that was tapped is the one to tap again.
    selectLine(std::max(0, step.selected));
    changed();
    return true;
}

void SyncEditorDialog::setText(const QString &text)
{
    pushUndo();
    QStringList rows = text.split(QLatin1Char('\n'));
    while (!rows.isEmpty() && rows.last().trimmed().isEmpty())
        rows.removeLast();
    QList<Lyrics::Line> lines;
    for (int i = 0; i < rows.size(); ++i) {
        Lyrics::Line line;
        line.text = rows[i].trimmed();
        if (i < m_doc.lines.size()) {
            line.start = m_doc.lines[i].start;
            line.end = m_doc.lines[i].end;
        }
        lines.append(line);
    }
    m_doc.lines = lines;
    rebuild();
    selectLine(0);
    changed();
}

void SyncEditorDialog::editText()
{
    QDialog dialog(this);
    dialog.setWindowTitle(tr("Lyrics Text"));
    dialog.resize(520, 560);
    auto *edit = new QPlainTextEdit(&dialog);
    QStringList rows;
    for (const Lyrics::Line &line : std::as_const(m_doc.lines))
        rows.append(line.text);
    edit->setPlainText(rows.join(QLatin1Char('\n')));
    edit->setPlaceholderText(tr("Paste the lyrics here, one sung line per row. Empty rows mark breaks."));
    auto *box = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    auto *layout = new QVBoxLayout(&dialog);
    layout->addWidget(new QLabel(tr("Times stay with their line number."), &dialog));
    layout->addWidget(edit, 1);
    layout->addWidget(box);
    connect(box, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(box, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    if (dialog.exec() == QDialog::Accepted)
        setText(edit->toPlainText());
}

void SyncEditorDialog::updatePlaying(double seconds)
{
    m_time->setText(stamp(seconds));
    Lyrics::Document probe = m_doc;
    if (m_mode == Mode::Subtitles)
        probe.offset = 0;
    const int playing = Lyrics::activeLine(probe, seconds);
    if (playing == m_playing)
        return;
    auto setBold = [this](int row, bool bold) {
        if (QTreeWidgetItem *item = m_list->topLevelItem(row)) {
            QFont f = item->font(TextColumn);
            f.setBold(bold);
            item->setFont(TextColumn, f);
            item->setBackground(TextColumn, bold ? QBrush(QColor(0x00, 0xD2, 0xFF, 40)) : QBrush());
        }
    };
    setBold(m_playing, false);
    m_playing = playing;
    setBold(m_playing, true);
}

QString SyncEditorDialog::subtitleSavePath(const QString &original)
{
    const QFileInfo info(original);
    QString base = info.completeBaseName();
    if (!base.endsWith(QLatin1String(".synced")))
        base += QStringLiteral(".synced");
    const QString beside = info.absoluteDir().filePath(base + QStringLiteral(".srt"));
    if (!original.isEmpty() && QFileInfo(info.absolutePath()).isWritable())
        return beside;
    const QString cache = QStandardPaths::writableLocation(QStandardPaths::GenericCacheLocation)
                          + QStringLiteral("/top-player/subtitles");
    QDir().mkpath(cache);
    return cache + QLatin1Char('/') + (original.isEmpty() ? QStringLiteral("subtitles.synced") : base) + QStringLiteral(".srt");
}

QString SyncEditorDialog::save()
{
    if (m_mode == Mode::Lyrics) {
        Q_EMIT lyricsSaved(m_doc);
        return QStringLiteral("lyrics");
    }
    if (m_doc.isEmpty())
        return {};
    const QString path = subtitleSavePath(m_source);
    if (!Lyrics::save(m_doc, path)) {
        m_hint->setText(tr("<span style='color:#FF6B6B'>Could not write %1</span>").arg(path.toHtmlEscaped()));
        return {};
    }
    Q_EMIT subtitlesSaved(path);
    return path;
}

void SyncEditorDialog::exportAs()
{
    const bool subs = m_mode == Mode::Subtitles;
    const QString suggested = subs ? subtitleSavePath(m_source)
                                   : QDir::home().filePath(QStringLiteral("%1 - %2.lrc").arg(m_doc.artist, m_doc.title));
    const QString file = QFileDialog::getSaveFileName(this, tr("Export As"), suggested,
                                                      subs ? tr("SubRip (*.srt);;LRC (*.lrc)")
                                                           : tr("LRC lyrics (*.lrc);;SubRip (*.srt)"));
    if (file.isEmpty())
        return;
    if (!Lyrics::save(m_doc, file))
        m_hint->setText(tr("<span style='color:#FF6B6B'>Could not write %1</span>").arg(file.toHtmlEscaped()));
    else
        m_hint->setText(tr("Saved to %1").arg(file.toHtmlEscaped()));
}

bool SyncEditorDialog::isEditorKey(const QKeyEvent *event)
{
    if (event->modifiers() & (Qt::ControlModifier | Qt::AltModifier | Qt::MetaModifier))
        return false;
    switch (event->key()) {
    case Qt::Key_Space:
    case Qt::Key_Backspace:
    case Qt::Key_Left:
    case Qt::Key_Right:
    case Qt::Key_BracketLeft:
    case Qt::Key_BracketRight:
    case Qt::Key_P:
        return true;
    default:
        return false;
    }
}

bool SyncEditorDialog::event(QEvent *event)
{
    // The player's own shortcuts (Space pauses, the arrows seek 5 s, ...)
    // must not take the editor's keys.
    if (event->type() == QEvent::ShortcutOverride && isEditorKey(static_cast<QKeyEvent *>(event))) {
        event->accept();
        return true;
    }
    return QDialog::event(event);
}

bool SyncEditorDialog::handleKey(QKeyEvent *event)
{
    if (!isEditorKey(event))
        return false;
    switch (event->key()) {
    case Qt::Key_Space:
        tap();
        return true;
    case Qt::Key_Backspace:
        undo();
        return true;
    case Qt::Key_Left:
        m_mpv->command({QStringLiteral("seek"), QString::number(-kSeekStep), QStringLiteral("relative+exact")});
        return true;
    case Qt::Key_Right:
        m_mpv->command({QStringLiteral("seek"), QString::number(kSeekStep), QStringLiteral("relative+exact")});
        return true;
    case Qt::Key_BracketLeft:
        nudge(-kNudgeStep);
        return true;
    case Qt::Key_BracketRight:
        nudge(kNudgeStep);
        return true;
    case Qt::Key_P:
        m_mpv->togglePause();
        return true;
    default:
        return false;
    }
}

bool SyncEditorDialog::eventFilter(QObject *watched, QEvent *event)
{
    // The list would take Space and the arrows for itself.
    if (watched == m_list && event->type() == QEvent::ShortcutOverride
        && isEditorKey(static_cast<QKeyEvent *>(event))) {
        event->accept();
        return true;
    }
    if (watched == m_list && event->type() == QEvent::KeyPress && handleKey(static_cast<QKeyEvent *>(event)))
        return true;
    return QDialog::eventFilter(watched, event);
}

void SyncEditorDialog::keyPressEvent(QKeyEvent *event)
{
    if (!handleKey(event))
        QDialog::keyPressEvent(event);
}
