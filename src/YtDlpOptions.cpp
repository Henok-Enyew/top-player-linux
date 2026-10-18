#include "YtDlpOptions.h"
#include "HostTools.h"
#include "MediaDownloader.h"
#include "PlaylistSession.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QProcess>
#include <QSettings>

#include <algorithm>

namespace {

QString settingsFile()
{
    return PlaylistSession::configDir() + QStringLiteral("/settings.ini");
}

// The JavaScript runtime to name with --js-runtimes, once probe() found
// that yt-dlp takes it and Deno (its default) is missing.
QString s_jsRuntime;
bool s_probed = false;

// Run on the system (where yt-dlp runs): prints "js-runtimes" if yt-dlp
// knows the option, then the runtimes installed.
const char kProbeScript[] =
    "\"$1\" --help 2>/dev/null | grep -q -- --js-runtimes && echo js-runtimes\n"
    "for r in deno node bun; do command -v \"$r\" >/dev/null 2>&1 && echo \"$r\"; done\n";

QString jsRuntime()
{
    return s_jsRuntime;
}

} // namespace

namespace YtDlpOptions {

Notifier *notifier()
{
    static Notifier *instance = new Notifier;
    return instance;
}

QString autoCookies()
{
    return QStringLiteral("auto");
}

QString noCookies()
{
    return QStringLiteral("none");
}

QString cookiesSetting()
{
    return QSettings(settingsFile(), QSettings::IniFormat).value(QStringLiteral("youtube/cookies"), autoCookies()).toString();
}

void setCookiesSetting(const QString &value)
{
    if (value == cookiesSetting())
        return;
    QSettings(settingsFile(), QSettings::IniFormat).setValue(QStringLiteral("youtube/cookies"), value);
    Q_EMIT notifier()->changed();
}

QString cookiesBrowser()
{
    const QString setting = cookiesSetting();
    if (setting == noCookies())
        return {};
    if (setting != autoCookies())
        return setting;
    if (!QSettings(settingsFile(), QSettings::IniFormat).value(QStringLiteral("youtube/botCheckSeen")).toBool())
        return {};
    const QList<Browser> browsers = detectBrowsers();
    return browsers.isEmpty() ? QString() : browsers.first().id;
}

QList<Browser> detectBrowsers()
{
    const QString home = QDir::homePath();
    struct Candidate {
        const char *id;
        const char *name;
        const char *path;   // under the home folder
        const char *suffix; // appended to a Flatpak's path for yt-dlp (its profile)
        bool flatpak;
    };
    // Packaged browsers keep their profiles where yt-dlp looks by itself;
    // Flatpak ones are handed their folder.
    static const Candidate candidates[] = {
        {"firefox", "Firefox", ".mozilla/firefox", "", false},
        {"firefox", "Firefox", ".config/mozilla/firefox", "", false},
        {"firefox", "Firefox (Flatpak)", ".var/app/org.mozilla.firefox/.mozilla/firefox", "", true},
        {"firefox", "Firefox (Flatpak)", ".var/app/org.mozilla.firefox/config/mozilla/firefox", "", true},
        {"chrome", "Google Chrome", ".config/google-chrome", "", false},
        {"chrome", "Google Chrome (Flatpak)", ".var/app/com.google.Chrome/config/google-chrome", "/Default", true},
        {"chromium", "Chromium", ".config/chromium", "", false},
        {"chromium", "Chromium (Flatpak)", ".var/app/org.chromium.Chromium/config/chromium", "/Default", true},
        {"brave", "Brave", ".config/BraveSoftware/Brave-Browser", "", false},
        {"brave", "Brave (Flatpak)", ".var/app/com.brave.Browser/config/BraveSoftware/Brave-Browser", "/Default", true},
        {"edge", "Microsoft Edge", ".config/microsoft-edge", "", false},
        {"vivaldi", "Vivaldi", ".config/vivaldi", "", false},
        {"opera", "Opera", ".config/opera", "", false},
    };
    QList<Browser> found;
    for (const Candidate &candidate : candidates) {
        const QString dir = home + QLatin1Char('/') + QString::fromLatin1(candidate.path);
        if (!QFileInfo(dir).isDir())
            continue;
        Browser browser;
        browser.id = QString::fromLatin1(candidate.id);
        if (candidate.flatpak)
            browser.id += QLatin1Char(':') + dir + QString::fromLatin1(candidate.suffix);
        browser.name = QString::fromLatin1(candidate.name);
        if (std::none_of(found.cbegin(), found.cend(), [&](const Browser &b) { return b.id == browser.id; }))
            found.append(browser);
    }
    return found;
}

bool isBotCheck(const QString &error)
{
    return error.contains(QLatin1String("not a bot"), Qt::CaseInsensitive)
        || error.contains(QLatin1String("Sign in to confirm"), Qt::CaseInsensitive);
}

bool noteBotCheck()
{
    if (cookiesSetting() != autoCookies())
        return false;
    QSettings settings(settingsFile(), QSettings::IniFormat);
    if (settings.value(QStringLiteral("youtube/botCheckSeen")).toBool())
        return false;
    settings.setValue(QStringLiteral("youtube/botCheckSeen"), true);
    settings.sync();
    if (cookiesBrowser().isEmpty())
        return false;
    Q_EMIT notifier()->changed();
    return true;
}

QString botCheckAdvice()
{
    const QString browser = cookiesBrowser();
    QString advice = browser.isEmpty()
        ? QCoreApplication::translate("YtDlpOptions",
                                      "YouTube wants to confirm you're not a bot. Sign in to YouTube in Firefox or "
                                      "Chrome, then pick that browser under Tools › YouTube Sign-In.")
        : QCoreApplication::translate("YtDlpOptions",
                                      "YouTube wants to confirm you're not a bot, even with your browser's cookies. "
                                      "Make sure you're signed in to YouTube in that browser.");
    advice += QLatin1Char(' ')
              + QCoreApplication::translate("YtDlpOptions",
                                            "An up-to-date yt-dlp also helps: sudo dnf upgrade --refresh yt-dlp, or "
                                            "pip install -U \"yt-dlp[default]\".");
    return advice;
}

QStringList arguments()
{
    QStringList args;
    if (const QString browser = cookiesBrowser(); !browser.isEmpty())
        args << QStringLiteral("--cookies-from-browser") << browser;
    if (const QString runtime = jsRuntime(); !runtime.isEmpty())
        args << QStringLiteral("--js-runtimes") << runtime;
    return args;
}

QString mpvRawOptions()
{
    QStringList options;
    if (const QString browser = cookiesBrowser(); !browser.isEmpty())
        options << QStringLiteral("cookies-from-browser=") + browser;
    if (const QString runtime = jsRuntime(); !runtime.isEmpty())
        options << QStringLiteral("js-runtimes=") + runtime;
    // mpv separates the options with commas; a path with one can't be passed.
    options.erase(std::remove_if(options.begin(), options.end(),
                                 [](const QString &option) { return option.count(QLatin1Char(',')) > 0; }),
                  options.end());
    return options.join(QLatin1Char(','));
}

void probe()
{
    if (s_probed || MediaDownloader::executable().isEmpty())
        return;
    s_probed = true;
    // In the Flatpak, yt-dlp is the system's: ask the system.
    QString program = QStringLiteral("sh");
    QStringList args{QStringLiteral("-c"), QString::fromLatin1(kProbeScript), QStringLiteral("sh")};
    if (HostTools::inFlatpak()) {
        args << QStringLiteral("yt-dlp");
        args.prepend(program);
        args.prepend(QStringLiteral("--host"));
        program = QStringLiteral("flatpak-spawn");
    } else {
        args << MediaDownloader::executable();
    }
    auto *process = new QProcess(notifier());
    QObject::connect(process, &QProcess::finished, process, [process] {
        process->deleteLater();
        const QStringList lines = QString::fromUtf8(process->readAllStandardOutput()).split(QLatin1Char('\n'), Qt::SkipEmptyParts);
        if (!lines.contains(QLatin1String("js-runtimes")) || lines.contains(QLatin1String("deno")))
            return;
        for (const char *runtime : {"node", "bun"}) {
            if (lines.contains(QLatin1String(runtime))) {
                s_jsRuntime = QString::fromLatin1(runtime);
                Q_EMIT notifier()->changed();
                return;
            }
        }
    });
    QObject::connect(process, &QProcess::errorOccurred, process, [process](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart)
            process->deleteLater();
    });
    process->start(program, args);
}

} // namespace YtDlpOptions
