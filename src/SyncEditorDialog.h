#pragma once

#include "Lyrics.h"

#include <QDialog>

class MpvWidget;
class QCheckBox;
class QComboBox;
class QLabel;
class QPushButton;
class QTreeWidget;

// Syncs lyrics or subtitles by ear: play the media and tap (Space) the
// moment the highlighted line starts; the line gets the playback time and
// the next one is selected. Single lines can be nudged by 0.1 s, a whole
// file shifted from any line on, and every step undone.
class SyncEditorDialog : public QDialog
{
    Q_OBJECT

public:
    enum class Mode { Lyrics, Subtitles };

    SyncEditorDialog(MpvWidget *mpv, Mode mode, QWidget *parent = nullptr);

    Mode mode() const { return m_mode; }
    void setDocument(const Lyrics::Document &doc);
    const Lyrics::Document &document() const { return m_doc; }
    // Loads a lyrics or subtitle file to sync.
    bool loadFile(const QString &path);
    QString sourceFile() const { return m_source; }

    int selectedLine() const;
    void selectLine(int index);
    // Stamps the selected line with `seconds` (the playback time if < 0),
    // then selects the next line.
    void tap(double seconds = -1);
    // Moves the selected line (and the following ones if "shift following"
    // is on) by `seconds`.
    void nudge(double seconds);
    void setShiftFollowing(bool shift);
    bool undo();
    // Replaces the text, keeping each line's time by position.
    void setText(const QString &text);

    // Lyrics: emits lyricsSaved(). Subtitles: writes an .srt (next to the
    // original if possible) and emits subtitlesSaved(). Returns the path
    // written, or empty.
    QString save();
    // Where save() writes synced subtitles for `original`.
    static QString subtitleSavePath(const QString &original);

Q_SIGNALS:
    void lyricsSaved(const Lyrics::Document &doc);
    void subtitlesSaved(const QString &path);
    // Every change in Lyrics mode, so the lyrics view can follow live.
    void documentEdited(const Lyrics::Document &doc);

protected:
    bool event(QEvent *event) override;
    bool eventFilter(QObject *watched, QEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;

private:
    static bool isEditorKey(const QKeyEvent *event);
    bool handleKey(QKeyEvent *event);
    void pushUndo();
    void changed();
    void rebuild();
    void updateRow(int index);
    void updatePlaying(double seconds);
    void editText();
    void exportAs();
    double now() const;

    MpvWidget *m_mpv;
    Mode m_mode;
    Lyrics::Document m_doc;
    struct UndoStep {
        Lyrics::Document doc;
        int selected;
    };
    QList<UndoStep> m_undo;
    QString m_source;
    int m_playing = -1;
    QTreeWidget *m_list;
    QLabel *m_time;
    QLabel *m_hint;
    QCheckBox *m_shiftFollowing;
    QComboBox *m_speed;
    QPushButton *m_playButton;
};
