#pragma once

#include <QObject>
#include <QString>
#include <QStringList>

// What every yt-dlp run (downloads, searches, playlists) and mpv's YouTube
// streaming share:
//  - browser cookies, which YouTube asks for when it suspects a bot ("Sign
//    in to confirm you're not a bot"). By default they are used only once
//    YouTube has asked; the browser can also be chosen, or cookies turned off.
//  - a JavaScript runtime for YouTube's challenges: yt-dlp looks for Deno;
//    with only Node installed, it is told to use that.
namespace YtDlpOptions {

// Emits changed() when the cookie setting changes (or cookies start being
// used after a bot check).
class Notifier : public QObject
{
    Q_OBJECT
Q_SIGNALS:
    void changed();
};
Notifier *notifier();

// Values of the cookie setting: Auto (once YouTube asked), None, or a
// browser as yt-dlp names it ("firefox", "chrome", "firefox:/profile/dir").
QString autoCookies();
QString noCookies();
QString cookiesSetting();
void setCookiesSetting(const QString &value);
// The browser whose cookies are passed now, or empty.
QString cookiesBrowser();

// A browser with a profile on this system: yt-dlp's name for it and a label.
struct Browser {
    QString id;
    QString name;
};
// Firefox first (the most reliable cookies on Linux), then the Chromium family.
QList<Browser> detectBrowsers();

// True if yt-dlp's `error` is YouTube's bot check.
bool isBotCheck(const QString &error);
// After a bot check: in Auto mode, cookies are used from now on (and on
// later runs). Returns true if that changed what yt-dlp is given, so that
// trying again may work.
bool noteBotCheck();
// What to tell the user when a bot check failed even so.
QString botCheckAdvice();

// Arguments for yt-dlp, before its own.
QStringList arguments();
// mpv's ytdl-raw-options for the same.
QString mpvRawOptions();

// Asks yt-dlp (in the background) whether it knows --js-runtimes. Until it
// answers, no runtime is named.
void probe();

} // namespace YtDlpOptions
