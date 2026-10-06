#pragma once

#include "PlaylistOps.h"

#include <optional>

// The queue saved between runs in ~/.config/top-player/last_playlist.json,
// and the settings that control whether it is restored.
namespace PlaylistSession {

struct State {
    QList<PlaylistOps::Entry> entries;
    int current = -1;     // entry that was playing (or last played), -1 if none
    double position = 0;  // seconds into `current`
};

// ~/.config/top-player (follows $XDG_CONFIG_HOME).
QString configDir();
// Copies the settings of versions before 1.0 (~/.config/potplayer-linux)
// into configDir(), unless that already exists. Call once at startup.
void migrateLegacyConfig();
QString sessionFile();

bool save(const State &state, const QString &path = sessionFile());
std::optional<State> load(const QString &path = sessionFile());

// Restore the last queue on startup (default on). Turning it off also stops
// saving it, and deletes the saved queue.
bool rememberPlaylist();
void setRememberPlaylist(bool enabled);
// Reopen the last entry where it was left, paused (default on).
bool resumePlayback();
void setResumePlayback(bool enabled);
// Width of the playlist drawer as the user last sized it, or `defaultWidth`.
int drawerWidth(int defaultWidth);
void setDrawerWidth(int width);
// Run through XWayland (Qt's xcb platform) in a Wayland session, so the mini
// player can stay above other apps and on every workspace (default off).
bool x11Mode();
void setX11Mode(bool enabled);

} // namespace PlaylistSession
