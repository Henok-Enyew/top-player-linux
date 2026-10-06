#include "WindowPin.h"

#include <QGuiApplication>
#include <QWindow>

#ifdef TOPPLAYER_HAVE_XCB
#include <QtGui/qguiapplication_platform.h>
#include <xcb/xcb.h>

#include <cstdlib>
#include <cstring>
#endif

namespace WindowPin {

#ifdef TOPPLAYER_HAVE_XCB
namespace {

xcb_connection_t *connection()
{
    auto *x11 = qGuiApp ? qGuiApp->nativeInterface<QNativeInterface::QX11Application>() : nullptr;
    return x11 ? x11->connection() : nullptr;
}

xcb_atom_t atom(xcb_connection_t *c, const char *name)
{
    xcb_intern_atom_reply_t *reply =
        xcb_intern_atom_reply(c, xcb_intern_atom(c, 0, static_cast<uint16_t>(std::strlen(name)), name), nullptr);
    const xcb_atom_t result = reply ? reply->atom : xcb_atom_t(XCB_ATOM_NONE);
    std::free(reply);
    return result;
}

xcb_window_t rootOf(xcb_connection_t *c, xcb_window_t window)
{
    xcb_query_tree_reply_t *reply = xcb_query_tree_reply(c, xcb_query_tree(c, window), nullptr);
    const xcb_window_t root = reply ? reply->root : xcb_window_t(XCB_WINDOW_NONE);
    std::free(reply);
    return root;
}

// An EWMH client message to the root window, as the window manager expects.
void sendToWindowManager(xcb_connection_t *c, xcb_window_t root, xcb_window_t window, xcb_atom_t type,
                         uint32_t d0, uint32_t d1, uint32_t d2 = 0)
{
    xcb_client_message_event_t event;
    std::memset(&event, 0, sizeof(event));
    event.response_type = XCB_CLIENT_MESSAGE;
    event.format = 32;
    event.window = window;
    event.type = type;
    event.data.data32[0] = d0;
    event.data.data32[1] = d1;
    event.data.data32[2] = d2;
    event.data.data32[3] = 1; // source: a normal application
    xcb_send_event(c, 0, root, XCB_EVENT_MASK_SUBSTRUCTURE_REDIRECT | XCB_EVENT_MASK_SUBSTRUCTURE_NOTIFY,
                   reinterpret_cast<const char *>(&event));
}

uint32_t currentDesktop(xcb_connection_t *c, xcb_window_t root)
{
    const xcb_atom_t current = atom(c, "_NET_CURRENT_DESKTOP");
    xcb_get_property_reply_t *reply =
        xcb_get_property_reply(c, xcb_get_property(c, 0, root, current, XCB_ATOM_CARDINAL, 0, 1), nullptr);
    uint32_t desktop = 0;
    if (reply && xcb_get_property_value_length(reply) >= 4)
        desktop = *static_cast<uint32_t *>(xcb_get_property_value(reply));
    std::free(reply);
    return desktop;
}

} // namespace

bool isSupported()
{
    return connection() != nullptr;
}

bool setOnAllWorkspaces(QWindow *window, bool on)
{
    xcb_connection_t *c = connection();
    if (!c || !window)
        return false;
    const auto id = static_cast<xcb_window_t>(window->winId());
    const xcb_window_t root = rootOf(c, id);
    if (root == XCB_WINDOW_NONE)
        return false;
    constexpr uint32_t kRemove = 0;
    constexpr uint32_t kAdd = 1;
    sendToWindowManager(c, root, id, atom(c, "_NET_WM_STATE"), on ? kAdd : kRemove, atom(c, "_NET_WM_STATE_STICKY"));
    // Window managers that track desktops by number want 0xFFFFFFFF ("all").
    sendToWindowManager(c, root, id, atom(c, "_NET_WM_DESKTOP"), on ? 0xFFFFFFFFu : currentDesktop(c, root), 1);
    xcb_flush(c);
    return true;
}

#else

bool isSupported()
{
    return false;
}

bool setOnAllWorkspaces(QWindow *, bool)
{
    return false;
}

#endif

} // namespace WindowPin
