#pragma once

#include <QString>

// Finds the command-line tools the player runs (yt-dlp, ffmpeg).
//
// In the Flatpak these are small wrappers in /app/bin that run the system's
// own tool through flatpak-spawn --host, so they keep up with the
// distribution's updates (yt-dlp needs that often). A wrapper only counts if
// the tool is installed on the system.
namespace HostTools {

// True when running inside a Flatpak sandbox.
bool inFlatpak();
// The path of `name` to run, or empty if it isn't available.
QString find(const QString &name);
// True if the system (outside the Flatpak, if in it) has a program `name`,
// e.g. one that yt-dlp runs itself.
bool hostHas(const QString &name);
// The real path of a file or folder picked through the Flatpak's file chooser
// portal, which hands out /run/user/<uid>/doc/<id>/<name> paths: those work
// only while the portal keeps them, and mean nothing to the user. Other
// paths come back as they are.
QString realPath(const QString &path);

} // namespace HostTools
