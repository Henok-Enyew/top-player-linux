#include "Lyrics.h"
#include "PlaylistSession.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSettings>
#include <QStringDecoder>

#include <algorithm>
#include <cmath>

namespace {

constexpr double kDefaultCueSeconds = 4.0;

QString lyricsDir()
{
    return PlaylistSession::configDir() + QStringLiteral("/lyrics");
}

QString trackHash(const QString &trackPath)
{
    const QByteArray path = QFileInfo(trackPath).absoluteFilePath().toUtf8();
    return QString::fromLatin1(QCryptographicHash::hash(path, QCryptographicHash::Sha1).toHex());
}

QString assignmentsFile()
{
    return lyricsDir() + QStringLiteral("/assigned.ini");
}

void sortByStart(QList<Lyrics::Line> &lines)
{
    std::stable_sort(lines.begin(), lines.end(),
                     [](const Lyrics::Line &a, const Lyrics::Line &b) { return a.start < b.start; });
}

} // namespace

namespace Lyrics {

bool Document::isSynced() const
{
    bool any = false;
    for (const Line &line : lines) {
        if (line.start >= 0)
            any = true;
        else if (!line.text.trimmed().isEmpty())
            return false;
    }
    return any;
}

double parseTime(const QString &text)
{
    // [hh:]mm:ss[.,]fraction
    static const QRegularExpression re(QStringLiteral(R"(^\s*(?:(\d+):)?(\d+):(\d+)(?:[.,:](\d+))?\s*$)"));
    const QRegularExpressionMatch m = re.match(text);
    if (!m.hasMatch())
        return -1;
    const double hours = m.captured(1).isEmpty() ? 0 : m.captured(1).toDouble();
    const double minutes = m.captured(2).toDouble();
    const double seconds = m.captured(3).toDouble();
    const QString fraction = m.captured(4);
    const double frac = fraction.isEmpty() ? 0 : fraction.toDouble() / std::pow(10.0, fraction.size());
    return hours * 3600 + minutes * 60 + seconds + frac;
}

QString formatLrcTime(double seconds)
{
    const long long cs = std::llround(std::max(0.0, seconds) * 100);
    return QStringLiteral("%1:%2.%3")
        .arg(cs / 6000, 2, 10, QLatin1Char('0'))
        .arg((cs / 100) % 60, 2, 10, QLatin1Char('0'))
        .arg(cs % 100, 2, 10, QLatin1Char('0'));
}

QString formatSrtTime(double seconds)
{
    const long long ms = std::llround(std::max(0.0, seconds) * 1000);
    return QStringLiteral("%1:%2:%3,%4")
        .arg(ms / 3600000, 2, 10, QLatin1Char('0'))
        .arg((ms / 60000) % 60, 2, 10, QLatin1Char('0'))
        .arg((ms / 1000) % 60, 2, 10, QLatin1Char('0'))
        .arg(ms % 1000, 3, 10, QLatin1Char('0'));
}

Document parseLrc(const QString &text)
{
    static const QRegularExpression stamp(QStringLiteral(R"(^\[(\d+:\d+(?:[.:]\d+)?)\])"));
    static const QRegularExpression tag(QStringLiteral(R"(^\[([A-Za-z#]+):(.*)\]\s*$)"));
    static const QRegularExpression wordStamp(QStringLiteral(R"(<\d+:\d+(?:[.:]\d+)?>)"));

    Document doc;
    QList<Line> synced;
    QList<Line> plain;
    QString body = text;
    if (body.startsWith(QChar(0xFEFF)))
        body.remove(0, 1);
    const QStringList rows = body.split(QRegularExpression(QStringLiteral("\r\n|\r|\n")));
    for (const QString &raw : rows) {
        QString row = raw.trimmed();
        QList<double> times;
        for (QRegularExpressionMatch m = stamp.match(row); m.hasMatch(); m = stamp.match(row)) {
            times.append(parseTime(m.captured(1)));
            row = row.mid(m.capturedLength()).trimmed();
        }
        if (times.isEmpty()) {
            if (const QRegularExpressionMatch m = tag.match(row); m.hasMatch()) {
                const QString key = m.captured(1).toLower();
                const QString value = m.captured(2).trimmed();
                if (key == QLatin1String("ti"))
                    doc.title = value;
                else if (key == QLatin1String("ar"))
                    doc.artist = value;
                else if (key == QLatin1String("al"))
                    doc.album = value;
                else if (key == QLatin1String("offset"))
                    doc.offset = value.toDouble() / 1000.0;
                else if (key == QLatin1String("length"))
                    doc.length = std::max(0.0, parseTime(value));
                continue;
            }
            plain.append({-1, -1, raw.trimmed()});
            continue;
        }
        row.remove(wordStamp);
        for (double t : std::as_const(times)) {
            if (t >= 0)
                synced.append({t, -1, row.trimmed()});
        }
    }
    if (!synced.isEmpty()) {
        sortByStart(synced);
        doc.lines = synced;
    } else {
        // Plain lyrics: keep stanza breaks, drop blank lines at either end.
        while (!plain.isEmpty() && plain.first().text.isEmpty())
            plain.removeFirst();
        while (!plain.isEmpty() && plain.last().text.isEmpty())
            plain.removeLast();
        doc.lines = plain;
    }
    return doc;
}

Document parseSrt(const QString &text)
{
    static const QRegularExpression arrow(
        QStringLiteral(R"(^\s*([\d:.,]+)\s*-->\s*([\d:.,]+))"));
    Document doc;
    const QStringList rows = text.split(QRegularExpression(QStringLiteral("\r\n|\r|\n")));
    Line *cue = nullptr;
    for (const QString &row : rows) {
        if (const QRegularExpressionMatch m = arrow.match(row); m.hasMatch()) {
            const double start = parseTime(m.captured(1));
            const double end = parseTime(m.captured(2));
            if (start < 0) {
                cue = nullptr;
                continue;
            }
            doc.lines.append({start, end, QString()});
            cue = &doc.lines.last();
            continue;
        }
        if (row.trimmed().isEmpty()) {
            cue = nullptr;
            continue;
        }
        if (!cue)
            continue; // a cue number, the WEBVTT header, NOTE blocks
        cue->text = cue->text.isEmpty() ? row.trimmed() : cue->text + QLatin1Char('\n') + row.trimmed();
    }
    sortByStart(doc.lines);
    return doc;
}

QString decode(const QByteArray &data)
{
    if (data.startsWith("\xFF\xFE") || data.startsWith("\xFE\xFF")) {
        QStringDecoder decoder(data.startsWith("\xFF\xFE") ? QStringConverter::Utf16LE : QStringConverter::Utf16BE);
        return decoder.decode(data.mid(2));
    }
    const QByteArray body = data.startsWith("\xEF\xBB\xBF") ? data.mid(3) : data;
    const QString text = QString::fromUtf8(body);
    // Invalid UTF-8 does not survive the round trip: an old 8-bit file.
    return text.toUtf8() == body ? text : QString::fromLatin1(body);
}

Document load(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return {};
    const QString text = decode(file.readAll());
    const QString suffix = QFileInfo(path).suffix().toLower();
    if (suffix == QLatin1String("srt") || suffix == QLatin1String("vtt"))
        return parseSrt(text);
    return parseLrc(text);
}

QString toLrc(const Document &doc)
{
    QString out;
    auto tag = [&out](const char *key, const QString &value) {
        if (!value.isEmpty())
            out += QStringLiteral("[%1:%2]\n").arg(QLatin1String(key), value);
    };
    tag("ti", doc.title);
    tag("ar", doc.artist);
    tag("al", doc.album);
    if (doc.length > 0)
        tag("length", formatLrcTime(doc.length).left(5));
    if (std::abs(doc.offset) > 1e-4)
        tag("offset", QString::number(std::llround(doc.offset * 1000)));
    tag("re", QStringLiteral("Top Player"));
    for (const Line &line : doc.lines) {
        if (line.start >= 0)
            out += QStringLiteral("[%1]%2\n").arg(formatLrcTime(line.start), line.text);
        else
            out += line.text + QLatin1Char('\n');
    }
    return out;
}

QString toSrt(const Document &doc)
{
    QList<Line> lines;
    for (const Line &line : doc.lines) {
        if (line.start >= 0 && !line.text.trimmed().isEmpty())
            lines.append(line);
    }
    sortByStart(lines);
    QString out;
    for (int i = 0; i < lines.size(); ++i) {
        double end = lines[i].end;
        if (end <= lines[i].start) {
            end = lines[i].start + kDefaultCueSeconds;
            if (i + 1 < lines.size())
                end = std::min(end, lines[i + 1].start);
        }
        out += QStringLiteral("%1\n%2 --> %3\n%4\n\n")
                   .arg(i + 1)
                   .arg(formatSrtTime(lines[i].start), formatSrtTime(end), lines[i].text);
    }
    return out;
}

bool save(const Document &doc, const QString &path)
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly))
        return false;
    const QString suffix = QFileInfo(path).suffix().toLower();
    const QString text = suffix == QLatin1String("srt") ? toSrt(doc) : toLrc(doc);
    file.write(text.toUtf8());
    return file.commit();
}

int activeLine(const Document &doc, double seconds)
{
    const double t = seconds + doc.offset;
    int active = -1;
    for (int i = 0; i < doc.lines.size(); ++i) {
        const Line &line = doc.lines[i];
        if (line.start < 0)
            continue;
        if (line.start > t)
            break;
        active = i;
    }
    // Subtitle cues end; a gap after one shows nothing.
    if (active >= 0 && doc.lines[active].end > doc.lines[active].start && t >= doc.lines[active].end)
        return -1;
    return active;
}

QString storagePathFor(const QString &trackPath)
{
    return lyricsDir() + QLatin1Char('/') + trackHash(trackPath) + QStringLiteral(".lrc");
}

void setAssigned(const QString &trackPath, const QString &lyricsPath)
{
    QDir().mkpath(lyricsDir());
    QSettings(assignmentsFile(), QSettings::IniFormat)
        .setValue(QStringLiteral("assigned/") + trackHash(trackPath), QFileInfo(lyricsPath).absoluteFilePath());
}

QString findFor(const QString &trackPath)
{
    if (trackPath.isEmpty())
        return {};
    const QString assigned = QSettings(assignmentsFile(), QSettings::IniFormat)
                                 .value(QStringLiteral("assigned/") + trackHash(trackPath)).toString();
    if (!assigned.isEmpty() && QFileInfo(assigned).isFile())
        return assigned;
    if (const QString stored = storagePathFor(trackPath); QFileInfo(stored).isFile())
        return stored;
    const QFileInfo track(trackPath);
    const QDir dir = track.absoluteDir();
    const QString base = track.completeBaseName();
    for (const char *suffix : {"lrc", "LRC", "txt"}) {
        const QString candidate = dir.filePath(base + QLatin1Char('.') + QLatin1String(suffix));
        if (QFileInfo(candidate).isFile())
            return candidate;
    }
    return {};
}

} // namespace Lyrics
