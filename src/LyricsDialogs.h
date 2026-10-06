#pragma once

#include "LyricsClient.h"
#include "LyricsView.h"

#include <QDialog>

#include <functional>

class QButtonGroup;
class QCheckBox;
class QFontComboBox;
class QLabel;
class QLineEdit;
class QSlider;
class QPlainTextEdit;
class QPushButton;
class QTreeWidget;

// Lyrics -> Download Lyrics...: searches LRCLIB (and lyrics.ovh) by title
// and artist, previews the matches, and hands the chosen one back.
class LyricsDownloadDialog : public QDialog
{
    Q_OBJECT

public:
    LyricsDownloadDialog(const LyricsClient::Query &query, QWidget *parent = nullptr);

    LyricsClient *client() const { return m_client; }
    const QList<LyricsClient::Result> &results() const { return m_results; }
    QTreeWidget *resultList() const { return m_list; }

public Q_SLOTS:
    void search();
    // Emits lyricsChosen() for the selected result and closes.
    void useSelected();

Q_SIGNALS:
    // `text` is LRC or plain lyrics; `label` names it for the OSD.
    void lyricsChosen(const QString &text, const QString &label);
    void aiPromptRequested();

private:
    void showResults(const QList<LyricsClient::Result> &results);
    void updatePreview();

    LyricsClient::Query m_query;
    LyricsClient *m_client;
    QList<LyricsClient::Result> m_results;
    QLineEdit *m_title;
    QLineEdit *m_artist;
    QPushButton *m_searchButton;
    QTreeWidget *m_list;
    QPlainTextEdit *m_preview;
    QLabel *m_status;
    QPushButton *m_useButton;
};

// Lyrics -> Generate with AI...: a structured prompt naming the song, its
// artist and length that the user copies into any chat AI. The answer (an
// .lrc file, or its text) comes back through Paste or Load File.
class AiLyricsPromptDialog : public QDialog
{
    Q_OBJECT

public:
    AiLyricsPromptDialog(const QString &prompt, QWidget *parent = nullptr);

    QString prompt() const;
    void showError(const QString &message);

Q_SIGNALS:
    void lyricsPasted(const QString &text);
    void loadFileRequested();

private:
    QPlainTextEdit *m_prompt;
    QLabel *m_status;
};

// Lyrics -> Lyrics Appearance...: font, size, line spacing, alignment, the
// highlight color and the dimming behind lyrics over a video. Every change
// shows at once on the lyrics (styleChanged()); Cancel puts the old look back.
class LyricsStyleDialog : public QDialog
{
    Q_OBJECT

public:
    LyricsStyleDialog(const LyricsStyle &style, QWidget *parent = nullptr);

    LyricsStyle lyricsStyle() const { return m_style; }
    LyricsStyle originalStyle() const { return m_original; }
    void setLyricsStyle(const LyricsStyle &style);

Q_SIGNALS:
    // The look being edited, for a live preview.
    void styleChanged(const LyricsStyle &style);

private:
    void syncControls();
    void edit(const std::function<void(LyricsStyle &)> &change);

    LyricsStyle m_style;
    LyricsStyle m_original;
    bool m_syncing = false;
    QCheckBox *m_defaultFont;
    QFontComboBox *m_font;
    QSlider *m_size;
    QLabel *m_sizeLabel;
    QSlider *m_spacing;
    QLabel *m_spacingLabel;
    QButtonGroup *m_align;
    QButtonGroup *m_colors;
    QPushButton *m_customColor;
    QCheckBox *m_bold;
    QCheckBox *m_glow;
    QSlider *m_dim;
    QLabel *m_dimLabel;
};
