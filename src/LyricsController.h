#pragma once

#include "Lyrics.h"
#include "LyricsClient.h"

#include <QImage>
#include <QObject>
#include <QPointer>

class AudioController;
class LyricsView;
class MpvWidget;
class LyricsStyleDialog;
class SyncEditorDialog;
class QWidget;

// Lyrics for the playing file: finds them (a .lrc next to the track, or
// ones downloaded, loaded or synced before), shows them in a LyricsView over
// the video, and opens the download, AI prompt and sync dialogs.
class LyricsController : public QObject
{
    Q_OBJECT

public:
    LyricsController(MpvWidget *mpv, AudioController *audio, QWidget *dialogParent);

    LyricsView *view() const { return m_view; }
    const Lyrics::Document &document() const { return m_doc; }
    QString lyricsPath() const { return m_lyricsPath; }
    // The local file playing, or empty.
    QString trackPath() const { return m_track; }

    bool isShown() const { return m_shown; }
    void setShown(bool shown);
    void toggle() { setShown(!m_shown); }
    // Show lyrics by themselves when an audio file with lyrics opens (default on).
    static bool autoShow();
    static void setAutoShow(bool enabled);

    // What the online search and the AI prompt start from: the tags, else
    // "Artist - Title" taken from the file name.
    LyricsClient::Query query() const;
    // A ready-to-paste prompt asking a chat AI for the song's LRC file.
    static QString aiPrompt(const LyricsClient::Query &query);

    // Uses `text` (LRC or plain) as the lyrics of the playing track, saved
    // to ~/.config/top-player/lyrics. Returns false if it holds no lyrics.
    bool applyText(const QString &text, const QString &source);
    // Uses a lyrics file (.lrc, .txt, .srt) the user picked.
    bool loadFile(const QString &path);
    void setDocument(const Lyrics::Document &doc, bool save);
    void removeLyrics();
    // Shifts the lyrics by `seconds` (positive: later) and saves.
    void adjustOffset(double seconds);

    void loadFileDialog();
    void openDownloadDialog();
    void openAiPromptDialog();
    // Opens the sync editor for the lyrics, or for the selected subtitles.
    void openSyncEditor(bool subtitles);
    // Lyrics -> Lyrics Appearance...: font, size, colors, with a live preview.
    void openStyleDialog();
    // Plays from `seconds` (a clicked lyrics line).
    void seekTo(double seconds);

Q_SIGNALS:
    void message(const QString &label, const QString &value = QString());
    void documentChanged();

private:
    void onFileLoaded();
    void refreshView();
    bool canHaveLyrics() const;

    MpvWidget *m_mpv;
    AudioController *m_audio;
    QWidget *m_dialogParent;
    LyricsView *m_view;
    Lyrics::Document m_doc;
    QString m_lyricsPath;
    QString m_track;
    bool m_shown = false;
    double m_position = 0;
    double m_duration = 0;
    QPointer<SyncEditorDialog> m_syncEditor;
    QPointer<LyricsStyleDialog> m_styleDialog;
};
