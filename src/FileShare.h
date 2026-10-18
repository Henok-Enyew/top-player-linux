#pragma once

#include <QString>
#include <QStringList>

#include <functional>

class QMenu;
class QWidget;

// Show in File Manager and Share, for playlist entries and the file that
// plays. Local files go to Telegram Desktop (its chat picker, with the file
// attached), to an email, or to the clipboard as files to paste into any app
// (WhatsApp, Discord, a browser's upload); online entries share their link
// on Telegram, WhatsApp, X, Facebook, Reddit or by email.
//
// Desktop programs are started on the system, also from the Flatpak.
namespace FileShare {

// Feedback for the OSD, e.g. ("Copied", "song.mp3").
using Notify = std::function<void(const QString &label, const QString &value)>;

// The local file of a playlist entry (a path or file:// URL), or empty.
QString localFile(const QString &entry);
// The web page to share for an online entry, or empty: its URL, or a
// YouTube search for a song found by name ("ytdl://ytsearch1:...").
QString webLink(const QString &entry);

// Opens the file manager with `files` selected (their folder, if it can't select).
void showInFileManager(const QStringList &files);
// Starts Telegram Desktop's "send to" for `file`. Reports through `notify`
// if Telegram Desktop isn't installed.
void sendToTelegram(const QString &file, const Notify &notify);
// Opens a new email with `files` attached.
void email(const QStringList &files, const Notify &notify);
// Puts `files` on the clipboard as files (pasted into a chat, they are sent).
void copyFiles(const QStringList &files);
// Opens the sharing page of `site` ("telegram", "whatsapp", "x",
// "facebook", "reddit", "email") for `link`, titled `title`.
void shareLink(const QString &site, const QString &link, const QString &title);
QString shareLinkUrl(const QString &site, const QString &link, const QString &title);

// Adds Show in File Manager and a Share submenu for `entries` (playlist
// entries: paths or URLs, with their display `titles`) to `menu`.
void addActions(QMenu *menu, const QStringList &entries, const QStringList &titles, const Notify &notify);

} // namespace FileShare
