#include "HostTools.h"

#include <QDateTime>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QPair>
#include <QProcess>
#include <QRegularExpression>
#include <QStandardPaths>

#include <sys/xattr.h>

namespace {

// How long a check of the system's tools is trusted: installing one while the
// player runs is noticed, without a process start on every keystroke.
constexpr qint64 kCheckValidMs = 5000;

bool hostHasProgram(const QString &name)
{
    static QHash<QString, QPair<qint64, bool>> checked;
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    if (const auto it = checked.constFind(name); it != checked.cend() && now - it->first < kCheckValidMs)
        return it->second;
    QProcess process;
    process.start(QStringLiteral("flatpak-spawn"),
                  {QStringLiteral("--host"), QStringLiteral("sh"), QStringLiteral("-c"),
                   QStringLiteral("command -v \"$1\" >/dev/null"), QStringLiteral("sh"), name});
    const bool found = process.waitForFinished(3000) && process.exitStatus() == QProcess::NormalExit
                       && process.exitCode() == 0;
    if (process.state() != QProcess::NotRunning) {
        process.kill();
        process.waitForFinished(500);
    }
    checked.insert(name, {now, found});
    return found;
}

} // namespace

namespace HostTools {

bool inFlatpak()
{
    return QFileInfo::exists(QStringLiteral("/.flatpak-info"));
}

QString find(const QString &name)
{
    const QString path = QStandardPaths::findExecutable(name);
    if (path.isEmpty() || !inFlatpak())
        return path;
    // The Flatpak's wrapper runs the system's tool: only there if that is.
    return hostHasProgram(name) ? path : QString();
}

QString realPath(const QString &path)
{
    static const QRegularExpression portal(QStringLiteral("^(/run/user/\\d+/doc/[^/]+/[^/]+)(/.*)?$"));
    const QRegularExpressionMatch m = portal.match(path);
    if (!m.hasMatch())
        return path;
    // The portal names the shared file's real place in an extended attribute.
    char buffer[4096];
    const ssize_t size = getxattr(QFile::encodeName(m.captured(1)).constData(), "user.document-portal.host-path",
                                  buffer, sizeof(buffer) - 1);
    if (size <= 0)
        return path;
    const QString host = QFile::decodeName(QByteArray(buffer, int(size)).chopped(buffer[size - 1] == '\0' ? 1 : 0));
    const QString real = host + m.captured(2);
    return QFileInfo::exists(real) ? real : path;
}

bool hostHas(const QString &name)
{
    return inFlatpak() ? hostHasProgram(name) : !QStandardPaths::findExecutable(name).isEmpty();
}

} // namespace HostTools
