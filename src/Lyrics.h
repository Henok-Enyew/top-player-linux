#pragma once

#include <QList>
#include <QString>

// Timed text shared by the lyrics display and the sync editor: song lyrics
// (LRC or plain text) and subtitles (SRT, WebVTT). Also finds the lyrics
// belonging to a track and saves new ones.
namespace Lyrics {

struct Line {
    double start = -1; // seconds; -1 while the line is not synced
    double end = -1;   // seconds; -1 if unknown (lyrics run to the next line)
    QString text;
};

struct Document {
    QList<Line> lines;
    // LRC [offset:] tag, in seconds; positive shows lines earlier.
    double offset = 0;
    QString title;
    QString artist;
    QString album;
    double length = 0; // LRC [length:], seconds

    bool isEmpty() const { return lines.isEmpty(); }
    // True if every non-empty line has a start time.
    bool isSynced() const;
};

// Parses LRC ("[01:23.45]text", several stamps per line, [ar:], [ti:],
// [al:], [length:], [offset:] tags; A2 word stamps "<01:23.45>" are
// stripped). Text without any stamps becomes unsynced plain lyrics.
Document parseLrc(const QString &text);
// SubRip and WebVTT cues; formatting tags are kept as written.
Document parseSrt(const QString &text);
// By file suffix (.lrc, .srt, .vtt, otherwise LRC/plain). Empty on error.
Document load(const QString &path);
// Decodes a lyrics or subtitle file: UTF-8 (with or without BOM), UTF-16
// with BOM, else Latin-1.
QString decode(const QByteArray &data);

// "[mm:ss.xx]" stamps; unsynced lines are written as plain text.
QString toLrc(const Document &doc);
// Cues without an end time end at the next cue (or 4 seconds later).
QString toSrt(const Document &doc);
bool save(const Document &doc, const QString &path);

// "01:23.45" / "1:02:03,456" style stamp formatting and parsing.
QString formatLrcTime(double seconds);
QString formatSrtTime(double seconds);
// Accepts mm:ss, mm:ss.xx, hh:mm:ss,mmm and hh:mm:ss.mmm; -1 if invalid.
double parseTime(const QString &text);

// Index of the line playing at `seconds` (with the document offset
// applied), or -1 before the first line or for unsynced lyrics.
int activeLine(const Document &doc, double seconds);

// Where the lyrics of `trackPath` are kept: a .lrc (or .txt) next to the
// track, else the copy saved in ~/.config/top-player/lyrics. Empty if none.
QString findFor(const QString &trackPath);
// The file lyrics downloaded or synced for `trackPath` are saved to:
// ~/.config/top-player/lyrics/<hash>.lrc (the track's folder may be read-only).
QString storagePathFor(const QString &trackPath);
// Remembers `lyricsPath` as the lyrics of `trackPath` (a file the user loaded).
void setAssigned(const QString &trackPath, const QString &lyricsPath);

} // namespace Lyrics
