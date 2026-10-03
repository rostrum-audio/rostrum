// rostrum-probe: connect to the PipeWire core, list sinks, sources and playback streams,
// check the PipeWire and WirePlumber versions, and exit non-zero with a human message
// if the audio stack is missing or too old.

#include "core/Requirements.h"

#include <pipewire/pipewire.h>

#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>

namespace {

struct Node
{
    uint32_t id = 0;
    std::string mediaClass;
    std::string name;
    std::string description;
    std::string appName;
};

struct Probe
{
    pw_main_loop *loop = nullptr;
    pw_core *core = nullptr;
    pw_registry *registry = nullptr;
    spa_hook coreListener{};
    spa_hook registryListener{};
    int syncSeq = 0;
    std::string serverVersion;
    std::string wireplumberVersion;
    bool sawWirePlumber = false;
    bool failed = false;
    std::string error;
    std::map<uint32_t, Node> nodes;
    pw_client *wpClient = nullptr;
    spa_hook wpClientListener{};
    bool secondPass = false;
};

const char *get(const spa_dict *props, const char *key)
{
    const char *v = props ? spa_dict_lookup(props, key) : nullptr;
    return v ? v : "";
}

void onCoreInfo(void *data, const pw_core_info *info)
{
    auto *p = static_cast<Probe *>(data);
    if (info->version) {
        p->serverVersion = info->version;
    }
}

void onCoreDone(void *data, uint32_t id, int seq)
{
    auto *p = static_cast<Probe *>(data);
    if (id != PW_ID_CORE || seq != p->syncSeq) {
        return;
    }
    // The first round trip enumerates globals; a second one collects bound client info.
    if (!p->secondPass && p->wpClient) {
        p->secondPass = true;
        p->syncSeq = pw_core_sync(p->core, PW_ID_CORE, 0);
        return;
    }
    pw_main_loop_quit(p->loop);
}

void onClientInfo(void *data, const pw_client_info *info)
{
    auto *p = static_cast<Probe *>(data);
    const char *v = spa_dict_lookup(info->props, PW_KEY_APP_VERSION);
    if (v) {
        p->wireplumberVersion = v;
    }
}

const pw_client_events kClientEvents = {
    .version = PW_VERSION_CLIENT_EVENTS,
    .info = onClientInfo,
};

void onCoreError(void *data, uint32_t id, int, int res, const char *message)
{
    auto *p = static_cast<Probe *>(data);
    if (id == PW_ID_CORE && res == -EPIPE) {
        p->failed = true;
        p->error = message ? message : "connection lost";
        pw_main_loop_quit(p->loop);
    }
}

const pw_core_events kCoreEvents = {
    .version = PW_VERSION_CORE_EVENTS,
    .info = onCoreInfo,
    .done = onCoreDone,
    .error = onCoreError,
};

void onGlobal(void *data, uint32_t id, uint32_t, const char *type, uint32_t, const spa_dict *props)
{
    auto *p = static_cast<Probe *>(data);
    if (std::strcmp(type, PW_TYPE_INTERFACE_Client) == 0) {
        const std::string app = get(props, PW_KEY_APP_NAME);
        if (app == "WirePlumber" && !p->wpClient) {
            p->sawWirePlumber = true;
            p->wpClient = static_cast<pw_client *>(
                pw_registry_bind(p->registry, id, PW_TYPE_INTERFACE_Client, PW_VERSION_CLIENT, 0));
            pw_client_add_listener(p->wpClient, &p->wpClientListener, &kClientEvents, p);
        }
        return;
    }
    if (std::strcmp(type, PW_TYPE_INTERFACE_Node) != 0) {
        return;
    }
    Node n;
    n.id = id;
    n.mediaClass = get(props, PW_KEY_MEDIA_CLASS);
    n.name = get(props, PW_KEY_NODE_NAME);
    n.description = get(props, PW_KEY_NODE_DESCRIPTION);
    n.appName = get(props, PW_KEY_APP_NAME);
    p->nodes[id] = n;
}

const pw_registry_events kRegistryEvents = {
    .version = PW_VERSION_REGISTRY_EVENTS,
    .global = onGlobal,
};

void printSection(const Probe &p, const char *title, auto &&predicate)
{
    std::printf("%s\n", title);
    int count = 0;
    for (const auto &[id, n] : p.nodes) {
        if (!predicate(n)) {
            continue;
        }
        const std::string &label = !n.description.empty() ? n.description
                                   : !n.appName.empty()    ? n.appName
                                                           : n.name;
        std::printf("  %4u  %-40s  %s\n", id, label.c_str(), n.name.c_str());
        ++count;
    }
    if (count == 0) {
        std::printf("  (none)\n");
    }
}

} // namespace

int main(int argc, char **argv)
{
    pw_init(&argc, &argv);

    Probe p;
    p.loop = pw_main_loop_new(nullptr);
    pw_context *context = pw_context_new(pw_main_loop_get_loop(p.loop), nullptr, 0);
    p.core = pw_context_connect(context, nullptr, 0);
    if (!p.core) {
        std::fprintf(stderr, "PipeWire is not running (%s).\n\n%s\n", std::strerror(errno),
                     rostrum::requirements::installHint().toUtf8().constData());
        pw_context_destroy(context);
        pw_main_loop_destroy(p.loop);
        pw_deinit();
        return 2;
    }

    pw_core_add_listener(p.core, &p.coreListener, &kCoreEvents, &p);
    p.registry = pw_core_get_registry(p.core, PW_VERSION_REGISTRY, 0);
    pw_registry_add_listener(p.registry, &p.registryListener, &kRegistryEvents, &p);
    p.syncSeq = pw_core_sync(p.core, PW_ID_CORE, 0);
    pw_main_loop_run(p.loop);

    int rc = 0;
    if (p.failed) {
        std::fprintf(stderr, "Lost the PipeWire connection: %s\n", p.error.c_str());
        rc = 2;
    } else {
        std::printf("PipeWire server %s (library %s)\n",
                    p.serverVersion.empty() ? "unknown" : p.serverVersion.c_str(),
                    pw_get_library_version());
        std::printf("Session manager: %s %s\n\n", p.sawWirePlumber ? "WirePlumber" : "not WirePlumber",
                    p.wireplumberVersion.c_str());

        printSection(p, "Output devices (sinks):",
                     [](const Node &n) { return n.mediaClass.rfind("Audio/Sink", 0) == 0; });
        printSection(p, "\nInput devices (sources):",
                     [](const Node &n) { return n.mediaClass.rfind("Audio/Source", 0) == 0; });
        printSection(p, "\nPlayback streams:",
                     [](const Node &n) { return n.mediaClass == "Stream/Output/Audio"; });
        printSection(p, "\nCapture streams:",
                     [](const Node &n) { return n.mediaClass == "Stream/Input/Audio"; });

        if (!rostrum::requirements::pipewireOk(QString::fromStdString(p.serverVersion))) {
            std::fprintf(stderr, "\nPipeWire %s is too old.\n\n%s\n", p.serverVersion.c_str(),
                         rostrum::requirements::installHint().toUtf8().constData());
            rc = 3;
        } else if (p.sawWirePlumber && !p.wireplumberVersion.empty() &&
                   !rostrum::requirements::wireplumberOk(QString::fromStdString(p.wireplumberVersion))) {
            std::fprintf(stderr, "\nWirePlumber %s is too old.\n\n%s\n", p.wireplumberVersion.c_str(),
                         rostrum::requirements::installHint().toUtf8().constData());
            rc = 3;
        } else if (!p.sawWirePlumber) {
            std::fprintf(stderr,
                         "\nPipeWire is running but WirePlumber is not the session manager. Rostrum will "
                         "run, but routes may not survive a reboot.\n\n%s\n",
                         rostrum::requirements::installHint().toUtf8().constData());
            rc = 4;
        }
    }

    if (p.wpClient) {
        spa_hook_remove(&p.wpClientListener);
        pw_proxy_destroy(reinterpret_cast<pw_proxy *>(p.wpClient));
    }
    spa_hook_remove(&p.registryListener);
    spa_hook_remove(&p.coreListener);
    pw_proxy_destroy(reinterpret_cast<pw_proxy *>(p.registry));
    pw_core_disconnect(p.core);
    pw_context_destroy(context);
    pw_main_loop_destroy(p.loop);
    pw_deinit();
    return rc;
}
