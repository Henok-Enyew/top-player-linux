#include "FileShare.h"
#include "HostTools.h"
#include "Icons.h"
#include "MediaFiles.h"

#include <QApplication>
#include <QClipboard>
#include <QCoreApplication>
#include <QDesktopServices>
#include <QFileInfo>
#include <QMenu>
#include <QMimeData>
#include <QProcess>
#include <QRegularExpression>
#include <QUrl>
#ifdef TOPPLAYER_HAVE_DBUS
#include <QDBusConnection>
#include <QDBusMessage>
#endif

namespace {

QString tr(const char *text, const char *comment = nullptr, int n = -1)
{
    return QCoreApplication::translate("FileShare", text, comment, n);
}

// Starts the first desktop program the script finds, detached, and exits 0;
// 127 if none is installed. "launch" leaves the program running after the
// player quits.
const char kLaunch[] = "launch() { setsid \"$@\" >/dev/null 2>&1 </dev/null & exit 0; }\n";

// Telegram Desktop as a package, a Flatpak (handed the file through the
// document portal) or a Snap. -sendpath opens its chat picker with the file.
const char kTelegramScript[] =
    "for app in telegram-desktop Telegram telegram; do\n"
    "  command -v \"$app\" >/dev/null 2>&1 && launch \"$app\" -sendpath \"$1\"\n"
    "done\n"
    "if command -v flatpak >/dev/null 2>&1 && flatpak info org.telegram.desktop >/dev/null 2>&1; then\n"
    "  launch flatpak run --file-forwarding org.telegram.desktop -sendpath @@ \"$1\" @@\n"
    "fi\n"
    "if command -v snap >/dev/null 2>&1 && snap list telegram-desktop >/dev/null 2>&1; then\n"
    "  launch snap run telegram-desktop -sendpath \"$1\"\n"
    "fi\n"
    "exit 127\n";

// Every file as an --attach argument.
const char kEmailScript[] =
    "command -v xdg-email >/dev/null 2>&1 || exit 127\n"
    "count=$#\n"
    "for f in \"$@\"; do set -- \"$@\" --attach \"$f\"; done\n"
    "shift \"$count\"\n"
    "launch xdg-email \"$@\"\n";

// Runs `script` with `args` on the system (outside the Flatpak's sandbox, if
// in it); `done` gets its exit code (127: nothing to start).
void runOnHost(const char *script, const QStringList &args, std::function<void(int)> done)
{
    const QString body = QString::fromLatin1(kLaunch) + QString::fromLatin1(script);
    QStringList arguments{QStringLiteral("-c"), body, QStringLiteral("sh")};
    arguments += args;
    QString program = QStringLiteral("sh");
    if (HostTools::inFlatpak()) {
        arguments.prepend(program);
        arguments.prepend(QStringLiteral("--host"));
        program = QStringLiteral("flatpak-spawn");
    }
    auto *process = new QProcess(qApp);
    QObject::connect(process, &QProcess::finished, process, [process, done](int exitCode, QProcess::ExitStatus status) {
        process->deleteLater();
        done(status == QProcess::NormalExit ? exitCode : 127);
    });
    QObject::connect(process, &QProcess::errorOccurred, process, [process, done](QProcess::ProcessError error) {
        if (error != QProcess::FailedToStart)
            return;
        process->deleteLater();
        done(127);
    });
    process->start(program, arguments);
}

QStringList fileUrls(const QStringList &files)
{
    QStringList urls;
    for (const QString &file : files)
        urls.append(QUrl::fromLocalFile(QFileInfo(file).absoluteFilePath()).toString(QUrl::FullyEncoded));
    return urls;
}

QString encoded(const QString &text)
{
    return QString::fromLatin1(QUrl::toPercentEncoding(text));
}

} // namespace

namespace FileShare {

QString localFile(const QString &entry)
{
    const QString path = MediaFiles::localPath(entry);
    return !path.isEmpty() && QFileInfo(path).isFile() ? QFileInfo(path).absoluteFilePath() : QString();
}

QString webLink(const QString &entry)
{
    QString link = entry.trimmed();
    if (link.startsWith(QLatin1String("ytdl://"), Qt::CaseInsensitive))
        link = link.mid(7);
    // A song found by name: the same search on YouTube's site.
    static const QRegularExpression search(QStringLiteral("^ytsearch\\d*:(.+)$"));
    if (const QRegularExpressionMatch m = search.match(link); m.hasMatch())
        return QStringLiteral("https://www.youtube.com/results?search_query=") + encoded(m.captured(1));
    const QUrl url(link);
    const QString scheme = url.scheme().toLower();
    return (scheme == QLatin1String("http") || scheme == QLatin1String("https")) && !url.host().isEmpty() ? link : QString();
}

void showInFileManager(const QStringList &files)
{
    if (files.isEmpty())
        return;
#ifdef TOPPLAYER_HAVE_DBUS
    // The freedesktop file manager interface opens the folder with the files
    // selected (Nautilus, Dolphin, Nemo, Thunar, Caja, ...).
    QDBusMessage call = QDBusMessage::createMethodCall(QStringLiteral("org.freedesktop.FileManager1"),
                                                       QStringLiteral("/org/freedesktop/FileManager1"),
                                                       QStringLiteral("org.freedesktop.FileManager1"),
                                                       QStringLiteral("ShowItems"));
    call << fileUrls(files) << QString();
    const QDBusMessage reply = QDBusConnection::sessionBus().call(call, QDBus::Block, 3000);
    if (reply.type() == QDBusMessage::ReplyMessage)
        return;
#endif
    // Otherwise the folder, in whatever opens folders.
    QDesktopServices::openUrl(QUrl::fromLocalFile(QFileInfo(files.first()).absolutePath()));
}

void sendToTelegram(const QString &file, const Notify &notify)
{
    runOnHost(kTelegramScript, {file}, [notify, file](int code) {
        if (code == 127 && notify)
            notify(tr("Telegram Desktop is not installed"), tr("Copy the file and paste it in a chat instead"));
        else if (notify)
            notify(tr("Sharing on Telegram:"), QFileInfo(file).fileName());
    });
}

void email(const QStringList &files, const Notify &notify)
{
    runOnHost(kEmailScript, files, [notify](int code) {
        if (code == 127 && notify)
            notify(tr("No email app found"), tr("xdg-email is missing"));
    });
}

void copyFiles(const QStringList &files)
{
    auto *mime = new QMimeData;
    QList<QUrl> urls;
    for (const QString &file : files)
        urls.append(QUrl::fromLocalFile(QFileInfo(file).absoluteFilePath()));
    mime->setUrls(urls);
    // GNOME's file managers and many apps paste files from this.
    mime->setData(QStringLiteral("x-special/gnome-copied-files"),
                  (QStringLiteral("copy\n") + fileUrls(files).join(QLatin1Char('\n'))).toUtf8());
    mime->setText(files.join(QLatin1Char('\n')));
    QApplication::clipboard()->setMimeData(mime);
}

QString shareLinkUrl(const QString &site, const QString &link, const QString &title)
{
    const QString url = encoded(link);
    const QString text = encoded(title);
    if (site == QLatin1String("telegram"))
        return QStringLiteral("https://t.me/share/url?url=%1&text=%2").arg(url, text);
    if (site == QLatin1String("whatsapp"))
        return QStringLiteral("https://wa.me/?text=%1").arg(encoded(title.isEmpty() ? link : title + QLatin1Char(' ') + link));
    if (site == QLatin1String("x"))
        return QStringLiteral("https://x.com/intent/tweet?url=%1&text=%2").arg(url, text);
    if (site == QLatin1String("facebook"))
        return QStringLiteral("https://www.facebook.com/sharer/sharer.php?u=%1").arg(url);
    if (site == QLatin1String("reddit"))
        return QStringLiteral("https://www.reddit.com/submit?url=%1&title=%2").arg(url, text);
    if (site == QLatin1String("email"))
        return QStringLiteral("mailto:?subject=%1&body=%2").arg(text, url);
    return {};
}

void shareLink(const QString &site, const QString &link, const QString &title)
{
    const QString url = shareLinkUrl(site, link, title);
    if (!url.isEmpty())
        QDesktopServices::openUrl(QUrl::fromEncoded(url.toUtf8()));
}

void addActions(QMenu *menu, const QStringList &entries, const QStringList &titles, const Notify &notify)
{
    QStringList files;
    for (const QString &entry : entries) {
        if (const QString file = localFile(entry); !file.isEmpty() && !files.contains(file))
            files.append(file);
    }
    // Links are shared one at a time: the sites take one.
    const QString link = entries.size() == 1 ? webLink(entries.first()) : QString();
    const QString title = titles.value(0);
    if (files.isEmpty() && link.isEmpty())
        return;

    if (!files.isEmpty()) {
        QAction *show = menu->addAction(skinIcon(IconType::Folder), tr("Show in File Manager"), menu,
                                        [files] { showInFileManager(files); });
        show->setObjectName(QStringLiteral("ShowInFileManagerAction"));
    }
    QMenu *share = menu->addMenu(skinIcon(IconType::Url), tr("Share"));
    share->setObjectName(QStringLiteral("ShareMenu"));
    if (!files.isEmpty()) {
        QAction *telegram = share->addAction(tr("Telegram"), share, [files, notify] { sendToTelegram(files.first(), notify); });
        telegram->setEnabled(files.size() == 1);
        telegram->setToolTip(files.size() == 1 ? tr("Pick a chat in Telegram Desktop to send the file to")
                                               : tr("Telegram takes one file at a time"));
        share->addAction(tr("Email..."), share, [files, notify] { email(files, notify); });
        share->addAction(files.size() == 1 ? tr("Copy File (Paste in Any Chat or Upload)")
                                           : tr("Copy %n Files (Paste in Any Chat or Upload)", nullptr, int(files.size())),
                         share, [files, notify] {
                             copyFiles(files);
                             if (notify)
                                 notify(tr("Copied"), files.size() == 1 ? QFileInfo(files.first()).fileName()
                                                                        : tr("%n files", nullptr, int(files.size())));
                         });
        share->addAction(tr("Copy Path"), share, [files] {
            QApplication::clipboard()->setText(files.join(QLatin1Char('\n')));
        });
    }
    if (!link.isEmpty()) {
        if (!files.isEmpty())
            share->addSeparator();
        const QList<QPair<QString, QString>> sites{
            {QStringLiteral("telegram"), tr("Telegram")}, {QStringLiteral("whatsapp"), tr("WhatsApp")},
            {QStringLiteral("x"), tr("X (Twitter)")},      {QStringLiteral("facebook"), tr("Facebook")},
            {QStringLiteral("reddit"), tr("Reddit")},      {QStringLiteral("email"), tr("Email")},
        };
        for (const auto &[site, name] : sites)
            share->addAction(tr("%1 (Link)").arg(name), share, [site = site, link, title] { shareLink(site, link, title); });
        share->addAction(tr("Copy Link"), share, [link, notify] {
            QApplication::clipboard()->setText(link);
            if (notify)
                notify(tr("Link Copied"), link.left(80));
        });
    }
}

} // namespace FileShare
