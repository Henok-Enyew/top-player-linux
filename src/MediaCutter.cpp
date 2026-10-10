#include "MediaCutter.h"
#include "HostTools.h"

#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>

#include <algorithm>
#include <cmath>

namespace {

constexpr qsizetype kLogTail = 4000;
// How far before the In-point the input seek lands.
constexpr double kPrerollSeconds = 5;

// Seconds with millisecond precision, as ffmpeg's -ss/-to accept them.
QString seconds(double value)
{
    return QString::number(std::max(0.0, value), 'f', 3);
}

} // namespace

MediaCutter::MediaCutter(QObject *parent)
    : QObject(parent)
{
}

MediaCutter::~MediaCutter()
{
    cancel();
}

QString MediaCutter::ffmpegPath()
{
    return HostTools::find(QStringLiteral("ffmpeg"));
}

QString MediaCutter::audioSuffix(AudioFormat format)
{
    switch (format) {
    case AudioFormat::Mp3:
        return QStringLiteral("mp3");
    case AudioFormat::Aac:
        return QStringLiteral("aac");
    case AudioFormat::Flac:
        return QStringLiteral("flac");
    }
    return QStringLiteral("mp3");
}

QStringList MediaCutter::arguments(const Job &job)
{
    // A fast seek on the input to a little before the In-point, then the
    // exact range on the output. (Seeking the input alone would, with stream
    // copy, start the clip at a keyframe before the In-point.) With stream
    // copy the picture starts at the first keyframe inside the range.
    const double preroll = std::min(job.start, kPrerollSeconds);
    const double seek = job.start - preroll;
    QStringList args{QStringLiteral("-hide_banner"), QStringLiteral("-nostdin"), QStringLiteral("-y")};
    if (seek > 0)
        args << QStringLiteral("-ss") << seconds(seek);
    args << QStringLiteral("-i") << job.input
         << QStringLiteral("-ss") << seconds(preroll)
         << QStringLiteral("-to") << seconds(preroll + job.end - job.start);
    if (job.mode == Mode::StreamCopy) {
        // Matroska takes every stream; other containers get ffmpeg's default
        // pick (one video, one audio), as their subtitle support varies.
        if (QFileInfo(job.output).suffix().compare(QLatin1String("mkv"), Qt::CaseInsensitive) == 0)
            args << QStringLiteral("-map") << QStringLiteral("0");
        else
            args << QStringLiteral("-sn");
        args << QStringLiteral("-c") << QStringLiteral("copy")
             << QStringLiteral("-avoid_negative_ts") << QStringLiteral("make_zero");
    } else {
        args << QStringLiteral("-vn") << QStringLiteral("-sn") << QStringLiteral("-map") << QStringLiteral("0:a:0");
        switch (job.audioFormat) {
        case AudioFormat::Mp3:
            args << QStringLiteral("-c:a") << QStringLiteral("libmp3lame") << QStringLiteral("-q:a") << QStringLiteral("2");
            break;
        case AudioFormat::Aac:
            args << QStringLiteral("-c:a") << QStringLiteral("aac") << QStringLiteral("-b:a") << QStringLiteral("192k")
                 << QStringLiteral("-f") << QStringLiteral("adts");
            break;
        case AudioFormat::Flac:
            args << QStringLiteral("-c:a") << QStringLiteral("flac");
            break;
        }
    }
    args << job.output;
    return args;
}

QString MediaCutter::formatTimestamp(double value)
{
    const qint64 ms = std::llround(std::max(0.0, value) * 1000);
    return QStringLiteral("%1:%2:%3.%4")
        .arg(ms / 3600000, 2, 10, QLatin1Char('0'))
        .arg((ms / 60000) % 60, 2, 10, QLatin1Char('0'))
        .arg((ms / 1000) % 60, 2, 10, QLatin1Char('0'))
        .arg(ms % 1000, 3, 10, QLatin1Char('0'));
}

double MediaCutter::parseTimestamp(const QString &text)
{
    static const QRegularExpression pattern(
        QStringLiteral("^\\s*(?:(?:(\\d+):)?(\\d{1,2}):)?(\\d+(?:[.,]\\d{1,3})?)\\s*$"));
    const QRegularExpressionMatch match = pattern.match(text);
    if (!match.hasMatch())
        return -1;
    const double hours = match.captured(1).toDouble();
    const double minutes = match.captured(2).toDouble();
    QString secondsText = match.captured(3);
    secondsText.replace(QLatin1Char(','), QLatin1Char('.'));
    const double secs = secondsText.toDouble();
    // "1:75" is not a time, but plain "75" (seconds) is.
    if (!match.captured(2).isEmpty() && secs >= 60)
        return -1;
    if (!match.captured(1).isEmpty() && minutes >= 60)
        return -1;
    return hours * 3600 + minutes * 60 + secs;
}

double MediaCutter::parseProgress(const QByteArray &output)
{
    static const QRegularExpression time(QStringLiteral("time=\\s*(-?)(\\d+):(\\d{2}):(\\d{2}(?:\\.\\d+)?)"));
    static const QRegularExpression outTime(QStringLiteral("out_time_us=(\\d+)"));
    const QString text = QString::fromUtf8(output);
    double result = -1;
    qsizetype lastPosition = -1;
    for (QRegularExpressionMatchIterator it = time.globalMatch(text); it.hasNext();) {
        const QRegularExpressionMatch match = it.next();
        // "time=N/A" doesn't match; a negative time counts as 0.
        result = match.captured(1).isEmpty()
            ? match.captured(2).toDouble() * 3600 + match.captured(3).toDouble() * 60 + match.captured(4).toDouble()
            : 0;
        lastPosition = match.capturedStart();
    }
    for (QRegularExpressionMatchIterator it = outTime.globalMatch(text); it.hasNext();) {
        const QRegularExpressionMatch match = it.next();
        if (match.capturedStart() > lastPosition) {
            result = match.captured(1).toDouble() / 1e6;
            lastPosition = match.capturedStart();
        }
    }
    return result;
}

bool MediaCutter::start(const Job &job)
{
    if (isRunning())
        return false;
    const QString ffmpeg = ffmpegPath();
    if (ffmpeg.isEmpty())
        return false;
    m_job = job;
    m_log.clear();

    auto *process = new QProcess(this);
    m_process = process;
    process->setProcessChannelMode(QProcess::SeparateChannels);
    connect(process, &QProcess::readyReadStandardError, this, [this, process] {
        const QByteArray chunk = process->readAllStandardError();
        m_log = (m_log + chunk).right(kLogTail);
        const double done = parseProgress(chunk);
        const double length = m_job.end - m_job.start;
        if (done >= 0 && length > 0)
            Q_EMIT progress(std::clamp(done / length, 0.0, 1.0));
    });
    connect(process, &QProcess::finished, this, &MediaCutter::onFinished);
    connect(process, &QProcess::errorOccurred, this, [this, process](QProcess::ProcessError error) {
        if (error != QProcess::FailedToStart || m_process != process)
            return;
        m_process = nullptr;
        process->deleteLater();
        Q_EMIT finished(false, m_job.output, tr("Could not start ffmpeg: %1").arg(process->errorString()));
    });
    process->start(ffmpeg, arguments(job));
    return true;
}

void MediaCutter::onFinished(int exitCode, QProcess::ExitStatus status)
{
    QProcess *process = m_process.data();
    if (!process)
        return; // cancelled
    m_process = nullptr;
    // Take what is left of stderr now, so that no progress report follows the end.
    process->disconnect(this);
    m_log = (m_log + process->readAllStandardError()).right(kLogTail);
    process->deleteLater();
    const QFileInfo output(m_job.output);
    if (status == QProcess::NormalExit && exitCode == 0 && output.exists() && output.size() > 0) {
        Q_EMIT progress(1.0);
        Q_EMIT finished(true, m_job.output, {});
        return;
    }
    // ffmpeg's last lines say what went wrong.
    QStringList lines = QString::fromUtf8(m_log).split(QRegularExpression(QStringLiteral("[\\r\\n]+")), Qt::SkipEmptyParts);
    lines.removeIf([](const QString &line) { return line.trimmed().startsWith(QLatin1String("frame=")) || line.contains(QLatin1String("time=")); });
    const QString detail = lines.mid(std::max<qsizetype>(0, lines.size() - 3)).join(QLatin1Char('\n')).trimmed();
    QFile::remove(m_job.output);
    Q_EMIT finished(false, m_job.output,
                    detail.isEmpty() ? tr("ffmpeg failed (exit code %1).").arg(exitCode) : detail);
}

void MediaCutter::cancel()
{
    QProcess *process = m_process.data();
    if (!process)
        return;
    m_process = nullptr;
    process->disconnect(this);
    process->kill();
    process->waitForFinished(3000);
    process->deleteLater();
    QFile::remove(m_job.output);
}

bool MediaCutter::isRunning() const
{
    return !m_process.isNull();
}
