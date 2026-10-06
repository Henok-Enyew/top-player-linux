#pragma once

class QWindow;

// Keeping a window on every workspace (virtual desktop), as the mini player
// does. X11 window managers honor _NET_WM_STATE_STICKY; Wayland gives
// clients no way to ask, so there the compositor's own window menu
// ("Always on Visible Workspace", "On All Desktops") does it.
namespace WindowPin {

// True if setOnAllWorkspaces() can work here (an X11 session).
bool isSupported();
// Shows `window` on every workspace, or only on the current one again.
// Call once the window is shown. Returns false if unsupported.
bool setOnAllWorkspaces(QWindow *window, bool on);

} // namespace WindowPin
