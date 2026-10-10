#include "LyricsController.h"
#include "AudioArtwork.h"
#include "AudioController.h"
#include "LyricsDialogs.h"
#include "LyricsView.h"
#include "MediaFiles.h"
#include "MpvWidget.h"
#include "PlaylistSession.h"
#include "SyncEditorDialog.h"

#include <QFileDialog>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <QSettings>

#include <cmath>

namespace {

QString settingsFile()
{
    return PlaylistSession::configDir() + QStringLiteral("/settings.ini");
}

QString durationText(double seconds)
{
    const long long s = std::llround(seconds);
    return QStringLiteral("%1:%2").arg(s / 60, 2, 10, QLatin1Char('0')).arg(s % 60, 2, 10, QLatin1Char('0'));
}

} // namespace

LyricsController::LyricsController(MpvWidget *mpv, AudioController *audio, QWidget *dialogParent)
    : QObject(dialogParent)
    , m_mpv(mpv)
    , m_audio(audio)
    , m_dialogParent(dialogParent)
    , m_view(new LyricsView(mpv))
{
    m_view->setPlaceholder(tr("No lyrics for this track yet.\n\nRight-click › Lyrics to download them, load a file,\n"
                              "or get a ready-made prompt for an AI to write the LRC file."));
    connect(m_mpv, &MpvWidget::fileLoaded, this, &LyricsController::onFileLoaded);
    connect(m_view, &LyricsView::seekRequested, this, &LyricsController::seekTo);
    connect(m_view, &LyricsView::backgroundChanged, this, &LyricsController::updateVideoBlur);
    connect(m_view, &LyricsView::message, this, [this](const QString &label, const QString &value) {
        Q_EMIT message(label, value);
    });
    connect(m_audio, &AudioController::artworkChanged, m_view, &LyricsView::setArtwork);
    connect(m_mpv, &MpvWidget::propertyUpdated, this, [this](const QString &name, const QVariant &value) {
        if (name == QLatin1String("time-pos")) {
            if (value.isValid()) {
                m_position = value.toDouble();
                m_view->setPosition(m_position, m_duration);
            }
        } else if (name == QLatin1String("duration")) {
            m_duration = value.toDouble();
        } else if (name == QLatin1String("metadata")) {
            const LyricsClient::Query q = query();
            m_view->setHeading(q.title, q.artist);
        } else if (name == QLatin1String("idle-active") && value.toBool() && m_mpv->isIdle()) {
            m_track.clear();
            m_doc = {};
            m_lyricsPath.clear();
            m_view->hide();
        }
    });
}

bool LyricsController::autoShow()
{
    return QSettings(settingsFile(), QSettings::IniFormat).value(QStringLiteral("lyrics/autoshow"), true).toBool();
}

void LyricsController::setAutoShow(bool enabled)
{
    QSettings(settingsFile(), QSettings::IniFormat).setValue(QStringLiteral("lyrics/autoshow"), enabled);
}

bool LyricsController::canHaveLyrics() const
{
    return !m_mpv->isIdle();
}

void LyricsController::onFileLoaded()
{
    m_track = MediaFiles::localPath(m_mpv->mpvPropertyString(QStringLiteral("path")));
    m_duration = m_mpv->mpvProperty(QStringLiteral("duration")).toDouble();
    m_position = 0;
    m_doc = {};
    m_lyricsPath.clear();
    if (!m_track.isEmpty()) {
        QString found = Lyrics::findFor(m_track);
        // A .txt beside a video is more likely notes than lyrics.
        if (!m_mpv->isAudioOnly() && found.endsWith(QLatin1String(".txt"), Qt::CaseInsensitive))
            found.clear();
        if (!found.isEmpty()) {
            m_doc = Lyrics::load(found);
            m_lyricsPath = found;
        }
    }
    m_view->setDocument(m_doc);
    const LyricsClient::Query q = query();
    m_view->setHeading(q.title, q.artist);
    m_shown = !m_doc.isEmpty() && m_mpv->isAudioOnly() && autoShow();
    refreshView();
    Q_EMIT documentChanged();
}

void LyricsController::refreshView()
{
    if (!m_shown || !canHaveLyrics()) {
        m_view->hide();
        return;
    }
    m_view->setOpaque(m_mpv->isAudioOnly());
    m_view->setArtwork(m_mpv->isAudioOnly() ? m_audio->artwork() : QImage());
    m_view->setPosition(m_position, m_duration);
    // Created right after the audio view, so it sits above it and below the
    // start screen and the OSD.
    m_view->show();
}

void LyricsController::updateVideoBlur()
{
    const LyricsStyle &style = m_view->lyricsStyle();
    const bool overVideo = m_view->isVisible() && !m_mpv->isIdle() && !m_mpv->isAudioOnly();
    // Up to a strong Gaussian blur; the scrim darkens on top.
    const int sigma = overVideo ? qRound(style.videoBlur * 0.3) : 0;
    if (sigma == m_videoBlur)
        return;
    const QString label = QStringLiteral("@lyricsblur");
    if (m_videoBlur > 0)
        m_mpv->command({QStringLiteral("vf"), QStringLiteral("remove"), label});
    m_videoBlur = sigma;
    if (sigma > 0)
        m_mpv->command({QStringLiteral("vf"), QStringLiteral("add"),
                        label + QStringLiteral(":lavfi=[gblur=sigma=%1]").arg(sigma)});
}

void LyricsController::setShown(bool shown)
{
    if (shown && !canHaveLyrics()) {
        Q_EMIT message(tr("Open a song to show its lyrics"));
        return;
    }
    m_shown = shown;
    refreshView();
    Q_EMIT message(tr("Lyrics"), shown ? tr("On") : tr("Off"));
}

LyricsClient::Query LyricsController::query() const
{
    LyricsClient::Query q;
    const QString path = m_mpv->mpvPropertyString(QStringLiteral("path"));
    const QString fileTitle = QFileInfo(MediaFiles::localPath(path).isEmpty() ? path : MediaFiles::localPath(path))
                                  .completeBaseName();
    const AudioArtwork::TrackInfo info =
        AudioArtwork::trackInfo(m_mpv->mpvProperty(QStringLiteral("metadata")).toMap(), QString());
    q.title = info.title;
    q.artist = info.artist;
    q.album = info.album;
    q.duration = m_mpv->mpvProperty(QStringLiteral("duration")).toDouble();
    if (q.title.isEmpty()) {
        // "01. Artist - Title (Official Video)" -> Artist, Title
        QString name = fileTitle;
        name.replace(QLatin1Char('_'), QLatin1Char(' '));
        name.remove(QRegularExpression(QStringLiteral(R"(^\s*\d{1,3}\s*[.\-)]\s*)")));
        name.remove(QRegularExpression(QStringLiteral(R"(\s*[\(\[][^\)\]]*(official|lyric|audio|video|hd|4k)[^\)\]]*[\)\]])"),
                                       QRegularExpression::CaseInsensitiveOption));
        const int dash = name.indexOf(QLatin1String(" - "));
        if (dash > 0) {
            if (q.artist.isEmpty())
                q.artist = name.left(dash).trimmed();
            q.title = name.mid(dash + 3).trimmed();
        } else {
            q.title = name.trimmed();
        }
    }
    return q;
}

QString LyricsController::aiPrompt(const LyricsClient::Query &q)
{
    const QString title = q.title.isEmpty() ? QStringLiteral("(unknown title)") : q.title;
    const QString artist = q.artist.isEmpty() ? QStringLiteral("(unknown artist)") : q.artist;
    const bool hasLength = q.duration > 0;
    const QString length = hasLength ? durationText(q.duration) : QString();
    const QString fileName = QStringLiteral("%1 - %2.lrc").arg(q.artist.isEmpty() ? QStringLiteral("Artist") : q.artist, title);

    QString prompt;
    prompt += QStringLiteral("You are a lyrics transcription assistant. Create a time-synchronized LRC lyrics file for this song.\n\n");
    prompt += QStringLiteral("## Song\n");
    prompt += QStringLiteral("- Title: %1\n").arg(title);
    prompt += QStringLiteral("- Artist: %1\n").arg(artist);
    if (!q.album.isEmpty())
        prompt += QStringLiteral("- Album: %1\n").arg(q.album);
    if (hasLength)
        prompt += QStringLiteral("- Length: %1 (%2 seconds)\n").arg(length).arg(std::llround(q.duration));
    prompt += QStringLiteral("\n## Output format (LRC)\n");
    prompt += QStringLiteral("1. Give me the result as a downloadable file named \"%1\". If you cannot create files, "
                             "reply with ONLY the file content inside one ```lrc code block, with no other text.\n").arg(fileName);
    prompt += QStringLiteral("2. Begin with these header tags:\n");
    prompt += QStringLiteral("[ti:%1]\n[ar:%2]\n").arg(title, artist);
    if (!q.album.isEmpty())
        prompt += QStringLiteral("[al:%1]\n").arg(q.album);
    if (hasLength)
        prompt += QStringLiteral("[length:%1]\n").arg(length);
    prompt += QStringLiteral("3. Then one sung line per row, each starting with its start time as [mm:ss.xx] "
                             "(minutes:seconds.hundredths), for example: [00:12.40]First line of the song\n");
    prompt += QStringLiteral("4. Timestamps must be in ascending order");
    if (hasLength)
        prompt += QStringLiteral(" and lie between [00:00.00] and [%1.00]").arg(length);
    prompt += QStringLiteral(". Place them where each line is actually sung, following the song's real structure "
                             "(intro, verses, chorus, bridge, outro).\n");
    prompt += QStringLiteral("5. Mark long instrumental breaks with an empty timed line, e.g. [01:05.00]\n");
    prompt += QStringLiteral("6. Write every repeated chorus out in full each time it is sung. Do not add section labels "
                             "(\"Chorus\", \"Verse 1\"), translations, notes or emojis.\n");
    prompt += QStringLiteral("7. Keep the original language, spelling and line breaks of the lyrics.\n");
    prompt += QStringLiteral("8. If you do not know this song's lyrics with confidence, say so instead of inventing them.\n");
    return prompt;
}

bool LyricsController::applyText(const QString &text, const QString &source)
{
    QString cleaned = text;
    // Chat answers wrap the file in a ``` code block.
    static const QRegularExpression fence(QStringLiteral(R"(```[A-Za-z]*\s*\n([\s\S]*?)```)"));
    if (const QRegularExpressionMatch m = fence.match(cleaned); m.hasMatch())
        cleaned = m.captured(1);
    Lyrics::Document doc = Lyrics::parseLrc(cleaned);
    if (doc.isEmpty())
        return false;
    const LyricsClient::Query q = query();
    if (doc.title.isEmpty())
        doc.title = q.title;
    if (doc.artist.isEmpty())
        doc.artist = q.artist;
    if (doc.album.isEmpty())
        doc.album = q.album;
    setDocument(doc, true);
    Q_EMIT message(doc.isSynced() ? tr("Synced lyrics loaded:") : tr("Lyrics loaded:"), source);
    return true;
}

bool LyricsController::loadFile(const QString &path)
{
    const Lyrics::Document doc = Lyrics::load(path);
    if (doc.isEmpty()) {
        Q_EMIT message(tr("No lyrics in"), QFileInfo(path).fileName());
        return false;
    }
    m_doc = doc;
    m_lyricsPath = path;
    if (!m_track.isEmpty())
        Lyrics::setAssigned(m_track, path);
    m_view->setDocument(m_doc);
    m_shown = true;
    refreshView();
    Q_EMIT documentChanged();
    Q_EMIT message(tr("Lyrics loaded:"), QFileInfo(path).fileName());
    return true;
}

void LyricsController::setDocument(const Lyrics::Document &doc, bool save)
{
    m_doc = doc;
    if (save && !m_track.isEmpty()) {
        const QString path = Lyrics::storagePathFor(m_track);
        if (Lyrics::save(m_doc, path)) {
            Lyrics::setAssigned(m_track, path);
            m_lyricsPath = path;
        }
    }
    m_view->setDocument(m_doc);
    m_shown = !m_doc.isEmpty();
    refreshView();
    Q_EMIT documentChanged();
}

void LyricsController::removeLyrics()
{
    if (!m_track.isEmpty()) {
        Lyrics::setAssigned(m_track, QString());
        QFile::remove(Lyrics::storagePathFor(m_track));
    }
    m_doc = {};
    m_lyricsPath.clear();
    m_view->setDocument(m_doc);
    Q_EMIT documentChanged();
    Q_EMIT message(tr("Lyrics removed"));
}

void LyricsController::adjustOffset(double seconds)
{
    if (m_doc.isEmpty() || !m_doc.isSynced()) {
        Q_EMIT message(tr("No synced lyrics to adjust"));
        return;
    }
    // A positive LRC offset shows lines earlier.
    m_doc.offset -= seconds;
    const double offset = m_doc.offset;
    setDocument(m_doc, true);
    Q_EMIT message(tr("Lyrics Offset"), QStringLiteral("%1%2 ms").arg(-offset > 0 ? QStringLiteral("+") : QString())
                                            .arg(std::llround(-offset * 1000)));
}

void LyricsController::seekTo(double seconds)
{
    if (m_mpv->isIdle() || seconds < 0)
        return;
    if (m_duration > 0)
        seconds = std::min(seconds, m_duration);
    m_mpv->command({QStringLiteral("seek"), QString::number(seconds, 'f', 3), QStringLiteral("absolute+exact")});
    // Picking a line means wanting to hear it.
    if (m_mpv->mpvProperty(QStringLiteral("pause")).toBool())
        m_mpv->setMpvProperty(QStringLiteral("pause"), QStringLiteral("no"));
}

void LyricsController::openStyleDialog()
{
    if (m_styleDialog) {
        m_styleDialog->raise();
        m_styleDialog->activateWindow();
        return;
    }
    auto *dialog = new LyricsStyleDialog(m_view->lyricsStyle(), m_dialogParent);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    connect(dialog, &LyricsStyleDialog::styleChanged, m_view, &LyricsView::setLyricsStyle);
    m_styleDialog = dialog;
    // Something to look at while choosing.
    if (!m_shown && canHaveLyrics()) {
        m_shown = true;
        refreshView();
    }
    // Not modal: the lyrics keep scrolling and can be clicked while it is open.
    dialog->show();
}

void LyricsController::loadFileDialog()
{
    if (!canHaveLyrics()) {
        Q_EMIT message(tr("Open a song to load lyrics for"));
        return;
    }
    const QString dir = m_track.isEmpty() ? QString() : QFileInfo(m_track).absolutePath();
    const QString file = QFileDialog::getOpenFileName(m_dialogParent, tr("Load Lyrics File"), dir,
                                                      tr("Lyrics (*.lrc *.txt *.srt *.vtt);;All Files (*)"));
    if (!file.isEmpty())
        loadFile(file);
}

void LyricsController::openDownloadDialog()
{
    if (!canHaveLyrics()) {
        Q_EMIT message(tr("Open a song to find lyrics for"));
        return;
    }
    auto *dialog = new LyricsDownloadDialog(query(), m_dialogParent);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    const QString track = m_mpv->mpvPropertyString(QStringLiteral("path"));
    connect(dialog, &LyricsDownloadDialog::lyricsChosen, this, [this, track](const QString &text, const QString &label) {
        // The song may have changed while the dialog was open.
        if (m_mpv->mpvPropertyString(QStringLiteral("path")) == track)
            applyText(text, label);
    });
    connect(dialog, &LyricsDownloadDialog::aiPromptRequested, this, &LyricsController::openAiPromptDialog);
    dialog->open();
}

void LyricsController::openAiPromptDialog()
{
    if (!canHaveLyrics()) {
        Q_EMIT message(tr("Open a song to write a prompt for"));
        return;
    }
    auto *dialog = new AiLyricsPromptDialog(aiPrompt(query()), m_dialogParent);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    connect(dialog, &AiLyricsPromptDialog::lyricsPasted, this, [this, dialog](const QString &text) {
        if (applyText(text, tr("AI answer")))
            dialog->accept();
        else
            dialog->showError(tr("That doesn't look like lyrics. Copy the AI's whole answer (or the .lrc file's text)."));
    });
    connect(dialog, &AiLyricsPromptDialog::loadFileRequested, this, [this, dialog] {
        const QString file = QFileDialog::getOpenFileName(dialog, tr("Load Lyrics File"), {},
                                                          tr("Lyrics (*.lrc *.txt);;All Files (*)"));
        if (!file.isEmpty() && loadFile(file))
            dialog->accept();
    });
    dialog->open();
}

void LyricsController::openSyncEditor(bool subtitles)
{
    if (!canHaveLyrics()) {
        Q_EMIT message(subtitles ? tr("Open a video to sync its subtitles") : tr("Open a song to sync its lyrics"));
        return;
    }
    if (m_syncEditor)
        m_syncEditor->close();

    SyncEditorDialog *editor = nullptr;
    if (subtitles) {
        QString file;
        for (const QVariantMap &track : m_mpv->tracks(QStringLiteral("sub"))) {
            if (track.value(QStringLiteral("selected")).toBool() && track.value(QStringLiteral("external")).toBool())
                file = track.value(QStringLiteral("external-filename")).toString();
        }
        editor = new SyncEditorDialog(m_mpv, SyncEditorDialog::Mode::Subtitles, m_dialogParent);
        if (!file.isEmpty())
            editor->loadFile(file);
        connect(editor, &SyncEditorDialog::subtitlesSaved, this, [this](const QString &path) {
            // The new times already include any delay that was set by hand.
            m_mpv->setMpvProperty(QStringLiteral("sub-delay"), QStringLiteral("0"));
            m_mpv->addSubtitle(path);
            m_mpv->setMpvProperty(QStringLiteral("sub-visibility"), QStringLiteral("yes"));
            Q_EMIT message(tr("Synced subtitles loaded:"), QFileInfo(path).fileName());
        });
    } else {
        editor = new SyncEditorDialog(m_mpv, SyncEditorDialog::Mode::Lyrics, m_dialogParent);
        Lyrics::Document doc = m_doc;
        const LyricsClient::Query q = query();
        if (doc.title.isEmpty())
            doc.title = q.title;
        if (doc.artist.isEmpty())
            doc.artist = q.artist;
        if (doc.album.isEmpty())
            doc.album = q.album;
        editor->setDocument(doc);
        connect(editor, &SyncEditorDialog::lyricsSaved, this, [this](const Lyrics::Document &saved) {
            setDocument(saved, true);
            Q_EMIT message(tr("Lyrics synced and saved"));
        });
        // Show the lyrics while syncing, so the result can be watched live.
        connect(editor, &SyncEditorDialog::documentEdited, this, [this](const Lyrics::Document &edited) {
            m_doc = edited;
            m_view->setDocument(m_doc);
            if (!m_shown) {
                m_shown = true;
                refreshView();
            }
        });
    }
    editor->setAttribute(Qt::WA_DeleteOnClose);
    m_syncEditor = editor;
    editor->show();
}
