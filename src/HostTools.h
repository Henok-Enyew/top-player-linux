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

} // namespace HostTools
