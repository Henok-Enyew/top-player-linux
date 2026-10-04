#pragma once

#include "LyricsClient.h"

#include <QDialog>

class QLabel;
class QLineEdit;
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
