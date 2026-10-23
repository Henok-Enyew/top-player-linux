#pragma once

#include "MemoryUsage.h"

#include <QByteArray>
#include <QStringList>
#include <QVariantMap>

#include <mpv/client.h>
#include <mpv/render.h>

#include <deque>
#include <thread>
#include <vector>

namespace MpvDetail {

// Runs `run` with `args` as a null-terminated argv of UTF-8 strings.
template<typename Run>
int withArgv(const QStringList &args, Run run)
{
    std::vector<QByteArray> storage;
    storage.reserve(args.size());
    std::vector<const char *> argv;
    argv.reserve(args.size() + 1);
    for (const QString &arg : args) {
        storage.push_back(arg.toUtf8());
        argv.push_back(storage.back().constData());
    }
    argv.push_back(nullptr);
    return run(argv.data());
}

// Builds an mpv_node tree from strings and maps of them. The nodes point
// into the builder, so it must outlive their use.
class NodeBuilder
{
public:
    mpv_node build(const QVariant &value)
    {
        mpv_node node{};
        if (value.typeId() != QMetaType::QVariantMap) {
            node.format = MPV_FORMAT_STRING;
            node.u.string = string(value.toString());
            return node;
        }
        const QVariantMap map = value.toMap();
        std::vector<mpv_node> &values = m_values.emplace_back(map.size());
        std::vector<char *> &keys = m_keys.emplace_back();
        int i = 0;
        for (auto it = map.cbegin(); it != map.cend(); ++it, ++i) {
            keys.push_back(string(it.key()));
            values[i] = build(it.value());
        }
        mpv_node_list &list = m_lists.emplace_back();
        list.num = static_cast<int>(map.size());
        list.values = values.data();
        list.keys = keys.data();
        node.format = MPV_FORMAT_NODE_MAP;
        node.u.list = &list;
        return node;
    }

private:
    char *string(const QString &text) { return m_strings.emplace_back(text.toUtf8()).data(); }

    // Deques keep their elements in place as they grow.
    std::deque<QByteArray> m_strings;
    std::deque<std::vector<mpv_node>> m_values;
    std::deque<std::vector<char *>> m_keys;
    std::deque<mpv_node_list> m_lists;
};

} // namespace MpvDetail

// Runs an mpv command asynchronously, e.g. {"seek", "5", "relative"}.
// `reply` comes back as the reply_userdata of its MPV_EVENT_COMMAND_REPLY.
// Returns mpv's error code: the command did not run if it is negative, e.g.
// MPV_ERROR_EVENT_QUEUE_FULL while about a thousand replies are pending.
inline int mpvCommandAsync(mpv_handle *mpv, const QStringList &args, uint64_t reply = 0)
{
    return MpvDetail::withArgv(args, [mpv, reply](const char **argv) { return mpv_command_async(mpv, reply, argv); });
}

// Runs an mpv command with named arguments asynchronously, e.g.
// {name: "loadfile", url: "...", options: {referrer: "..."}}. Values are
// strings or maps of strings. Returns mpv's error code like mpvCommandAsync().
inline int mpvCommandNodeAsync(mpv_handle *mpv, const QVariantMap &args, uint64_t reply = 0)
{
    MpvDetail::NodeBuilder builder;
    mpv_node node = builder.build(args);
    // mpv parses the command before returning, so the nodes may go away then.
    return mpv_command_node_async(mpv, reply, &node);
}

// Frees a helper player (and its render context, if any) in a thread of its
// own: one that hangs on a file (a mount that stopped answering) must not
// freeze the window while it shuts down. Its wakeup and render callbacks
// are cleared first, so nothing reaches the caller afterwards.
inline void destroyMpvDetached(mpv_handle *mpv, mpv_render_context *renderCtx = nullptr)
{
    if (!mpv)
        return;
    mpv_set_wakeup_callback(mpv, nullptr, nullptr);
    if (renderCtx)
        mpv_render_context_set_update_callback(renderCtx, nullptr, nullptr);
    std::thread([mpv, renderCtx] {
        if (renderCtx)
            mpv_render_context_free(renderCtx);
        mpv_terminate_destroy(mpv);
        MemoryUsage::releaseFreed();
    }).detach();
}
