#include "pw/PwContext.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QLoggingCategory>
#include <QTimer>

#include <pipewire/extensions/metadata.h>
#include <pipewire/pipewire.h>
#include <spa/param/audio/format-utils.h>
#include <spa/param/audio/raw.h>
#include <spa/param/port-config.h>
#include <spa/param/props.h>
#include <spa/pod/builder.h>
#include <spa/pod/iter.h>
#include <spa/pod/parser.h>
#include <spa/utils/result.h>

#include <cstring>
#include <list>
#include <map>
#include <unordered_map>

Q_LOGGING_CATEGORY(lcPw, "rostrum.pw")

namespace rostrum::pw {

namespace {

QMap<QString, QString> toMap(const spa_dict *dict)
{
    QMap<QString, QString> out;
    if (!dict) {
        return out;
    }
    const spa_dict_item *item;
    spa_dict_for_each(item, dict)
    {
        out.insert(QString::fromUtf8(item->key), QString::fromUtf8(item->value ? item->value : ""));
    }
    return out;
}

void fillNode(Node &n, const QMap<QString, QString> &p)
{
    auto v = [&](const char *k) { return p.value(QString::fromUtf8(k)); };
    n.props.insert(p);
    if (p.contains(QStringLiteral(PW_KEY_OBJECT_SERIAL))) {
        n.serial = v(PW_KEY_OBJECT_SERIAL);
    }
    n.name = v(PW_KEY_NODE_NAME);
    n.description = v(PW_KEY_NODE_DESCRIPTION);
    n.nick = v(PW_KEY_NODE_NICK);
    n.mediaClass = v(PW_KEY_MEDIA_CLASS);
    n.appName = v(PW_KEY_APP_NAME);
    n.binary = v(PW_KEY_APP_PROCESS_BINARY);
    n.mediaName = v(PW_KEY_MEDIA_NAME);
    n.clientId = v(PW_KEY_CLIENT_ID);
    n.pid = v(PW_KEY_APP_PROCESS_ID).toLongLong();
}

bool wantsProps(const QMap<QString, QString> &p)
{
    const QString cls = p.value(QStringLiteral(PW_KEY_MEDIA_CLASS));
    const QString name = p.value(QStringLiteral(PW_KEY_NODE_NAME));
    return cls == QLatin1String("Stream/Output/Audio") || name.startsWith(QLatin1String("rostrum."));
}

} // namespace

struct BoundNode
{
    PwContext::Impl *impl = nullptr;
    uint32_t id = 0;
    pw_node *proxy = nullptr;
    spa_hook listener{};
    bool subscribed = false;
    // audioconvert reports its own settings as Props 0 and each filter graph's controls as Props 1
    // and up, and does not always resend them all together.
    std::map<uint32_t, QVariantMap> paramsByIndex;
};

struct CreatedProxy
{
    PwContext::Impl *impl = nullptr;
    pw_proxy *proxy = nullptr;
    spa_hook listener{};
    uint32_t globalId = SPA_ID_INVALID;
    QString nodeName;
};

struct PwContext::Impl
{
    PwContext *q = nullptr;

    // Loop-thread state, guarded by the thread-loop lock.
    pw_thread_loop *loop = nullptr;
    pw_context *context = nullptr;
    pw_core *core = nullptr;
    pw_registry *registry = nullptr;
    spa_hook coreListener{};
    spa_hook registryListener{};
    pw_metadata *metadata = nullptr;
    uint32_t metadataId = SPA_ID_INVALID;
    spa_hook metadataListener{};
    pw_client *wpClient = nullptr;
    spa_hook wpClientListener{};
    std::unordered_map<uint32_t, std::unique_ptr<BoundNode>> boundNodes;
    std::list<std::unique_ptr<CreatedProxy>> created;
    int initialSeq = -1;
    int roundtripSeq = -1;
    bool roundtripDone = false;

    // Qt-thread state.
    State state = State::Idle;
    QString error;
    int errorCode = 0;
    QString serverVersion;
    QString wireplumberVersion;
    bool hasWirePlumber = false;
    Graph graph;
    QHash<uint32_t, QHash<QString, QString>> metadataValues;
    QString defaultSink;
    QString defaultSource;
    bool changePending = false;

    template<typename F>
    void post(F &&f)
    {
        QMetaObject::invokeMethod(q, std::forward<F>(f), Qt::QueuedConnection);
    }

    void teardownLocked();
};

// ---- loop-thread callbacks --------------------------------------------------------------

namespace {

void onNodeInfo(void *data, const pw_node_info *info)
{
    auto *b = static_cast<BoundNode *>(data);
    auto *impl = b->impl;
    if (info->change_mask & PW_NODE_CHANGE_MASK_STATE) {
        const uint32_t id = b->id;
        const bool running = info->state == PW_NODE_STATE_RUNNING;
        impl->post([impl, id, running] {
            auto it = impl->graph.nodes.find(id);
            if (it != impl->graph.nodes.end() && it->running != running) {
                it->running = running;
                impl->q->scheduleChanged();
            }
        });
    }
    if (!(info->change_mask & PW_NODE_CHANGE_MASK_PROPS)) {
        return;
    }
    const auto props = toMap(info->props);
    if (!b->subscribed && wantsProps(props)) {
        uint32_t ids[] = {SPA_PARAM_Props};
        pw_node_subscribe_params(b->proxy, ids, 1);
        b->subscribed = true;
    }
    const uint32_t id = b->id;
    impl->post([impl, id, props] {
        auto it = impl->graph.nodes.find(id);
        if (it == impl->graph.nodes.end()) {
            return;
        }
        fillNode(it.value(), props);
        Q_EMIT impl->q->nodeAdded(id);
        impl->q->scheduleChanged();
    });
}

void onNodeParam(void *data, int, uint32_t id, uint32_t index, uint32_t, const spa_pod *param)
{
    auto *b = static_cast<BoundNode *>(data);
    if (id != SPA_PARAM_Props || !param || !spa_pod_is_object(param)) {
        return;
    }
    QList<float> volumes;
    bool mute = false;
    bool haveVolumes = false;
    int mapChannels = 0;
    QVariantMap params;
    bool haveParams = false;
    const auto *obj = reinterpret_cast<const spa_pod_object *>(param);
    const spa_pod_prop *prop;
    SPA_POD_OBJECT_FOREACH(obj, prop)
    {
        if (prop->key == SPA_PROP_params && spa_pod_is_struct(&prop->value)) {
            haveParams = true;
            spa_pod_parser prs;
            spa_pod_frame f;
            spa_pod_parser_pod(&prs, &prop->value);
            if (spa_pod_parser_push_struct(&prs, &f) < 0) {
                continue;
            }
            const char *name = nullptr;
            spa_pod *value = nullptr;
            while (spa_pod_parser_get_string(&prs, &name) >= 0 && spa_pod_parser_get_pod(&prs, &value) >= 0) {
                float fl;
                double db;
                int32_t i32;
                int64_t i64;
                bool bl;
                const char *str;
                QVariant v;
                if (spa_pod_get_bool(value, &bl) >= 0) {
                    v = bl;
                } else if (spa_pod_get_float(value, &fl) >= 0) {
                    v = double(fl);
                } else if (spa_pod_get_double(value, &db) >= 0) {
                    v = db;
                } else if (spa_pod_get_int(value, &i32) >= 0) {
                    v = i32;
                } else if (spa_pod_get_long(value, &i64) >= 0) {
                    v = qint64(i64);
                } else if (spa_pod_get_string(value, &str) >= 0) {
                    v = QString::fromUtf8(str);
                }
                params.insert(QString::fromUtf8(name), v);
            }
        } else if (prop->key == SPA_PROP_channelVolumes) {
            float vols[SPA_AUDIO_MAX_CHANNELS];
            const uint32_t n = spa_pod_copy_array(&prop->value, SPA_TYPE_Float, vols, SPA_AUDIO_MAX_CHANNELS);
            for (uint32_t i = 0; i < n; ++i) {
                volumes.append(vols[i]);
            }
            haveVolumes = n > 0;
        } else if (prop->key == SPA_PROP_channelMap) {
            uint32_t map[SPA_AUDIO_MAX_CHANNELS];
            mapChannels = int(spa_pod_copy_array(&prop->value, SPA_TYPE_Id, map, SPA_AUDIO_MAX_CHANNELS));
        } else if (prop->key == SPA_PROP_mute) {
            spa_pod_get_bool(&prop->value, &mute);
        }
    }
    if (!haveVolumes && !haveParams) {
        return;
    }
    if (haveParams) {
        b->paramsByIndex[index] = params;
        params.clear();
        for (const auto &[i, p] : b->paramsByIndex) {
            params.insert(p);
        }
    }
    auto *impl = b->impl;
    const uint32_t nodeId = b->id;
    impl->post([impl, nodeId, volumes, mute, haveVolumes, mapChannels, params, haveParams] {
        auto it = impl->graph.nodes.find(nodeId);
        if (it == impl->graph.nodes.end()) {
            return;
        }
        if (haveVolumes) {
            // audioconvert keeps whatever number of volumes it was last sent, so a stale write
            // (a restored state, an older layout) can disagree with the real channel layout.
            it->channels = mapChannels > 0 ? mapChannels : int(volumes.size());
            it->volumes = volumes;
            it->muted = mute;
        }
        if (haveParams) {
            it->params = params;
        }
        impl->q->scheduleChanged();
    });
}

const pw_node_events kNodeEvents = {
    .version = PW_VERSION_NODE_EVENTS,
    .info = onNodeInfo,
    .param = onNodeParam,
};

void onClientInfo(void *data, const pw_client_info *info)
{
    auto *impl = static_cast<PwContext::Impl *>(data);
    const char *v = spa_dict_lookup(info->props, PW_KEY_APP_VERSION);
    if (!v) {
        return;
    }
    const QString version = QString::fromUtf8(v);
    impl->post([impl, version] {
        impl->wireplumberVersion = version;
        Q_EMIT impl->q->stateChanged();
    });
}

const pw_client_events kClientEvents = {
    .version = PW_VERSION_CLIENT_EVENTS,
    .info = onClientInfo,
};

QString jsonName(const char *value)
{
    if (!value) {
        return {};
    }
    const auto doc = QJsonDocument::fromJson(QByteArray(value));
    return doc.object().value(QStringLiteral("name")).toString();
}

int onMetadataProperty(void *data, uint32_t subject, const char *key, const char *, const char *value)
{
    auto *impl = static_cast<PwContext::Impl *>(data);
    const QString k = key ? QString::fromUtf8(key) : QString();
    const QString v = value ? QString::fromUtf8(value) : QString();
    const bool isNull = value == nullptr;
    QString defaultName;
    if (key && (std::strcmp(key, "default.audio.sink") == 0 || std::strcmp(key, "default.audio.source") == 0)) {
        defaultName = jsonName(value);
    }
    impl->post([impl, subject, k, v, isNull, defaultName] {
        if (k.isEmpty()) {
            impl->metadataValues.remove(subject);
        } else if (isNull) {
            impl->metadataValues[subject].remove(k);
        } else {
            impl->metadataValues[subject].insert(k, v);
        }
        if (k == QLatin1String("default.audio.sink")) {
            impl->defaultSink = defaultName;
            Q_EMIT impl->q->defaultsChanged();
        } else if (k == QLatin1String("default.audio.source")) {
            impl->defaultSource = defaultName;
            Q_EMIT impl->q->defaultsChanged();
        }
        Q_EMIT impl->q->metadataChanged(subject, k);
        impl->q->scheduleChanged();
    });
    return 0;
}

const pw_metadata_events kMetadataEvents = {
    .version = PW_VERSION_METADATA_EVENTS,
    .property = onMetadataProperty,
};

void onCreatedBound(void *data, uint32_t globalId)
{
    static_cast<CreatedProxy *>(data)->globalId = globalId;
}

void onCreatedRemoved(void *data)
{
    pw_proxy_destroy(static_cast<CreatedProxy *>(data)->proxy);
}

void onCreatedDestroy(void *data)
{
    auto *c = static_cast<CreatedProxy *>(data);
    spa_hook_remove(&c->listener);
    auto &list = c->impl->created;
    list.remove_if([c](const std::unique_ptr<CreatedProxy> &p) { return p.get() == c; });
}

void onCreatedError(void *data, int, int res, const char *message)
{
    auto *created = static_cast<CreatedProxy *>(data);
    auto *impl = created->impl;
    const QString msg = QStringLiteral("%1 %2").arg(QString::fromUtf8(spa_strerror(res)),
                                                    QString::fromUtf8(message ? message : ""));
    const QString nodeName = created->nodeName;
    qCWarning(lcPw) << "create failed:" << nodeName << msg;
    impl->post([impl, msg, nodeName] { Q_EMIT impl->q->createFailed(msg.trimmed(), nodeName); });
}

const pw_proxy_events kCreatedEvents = {
    .version = PW_VERSION_PROXY_EVENTS,
    .destroy = onCreatedDestroy,
    .bound = onCreatedBound,
    .removed = onCreatedRemoved,
    .error = onCreatedError,
};

void onGlobal(void *data, uint32_t id, uint32_t, const char *type, uint32_t, const spa_dict *dict)
{
    auto *impl = static_cast<PwContext::Impl *>(data);
    const auto props = toMap(dict);

    if (std::strcmp(type, PW_TYPE_INTERFACE_Node) == 0) {
        auto b = std::make_unique<BoundNode>();
        b->impl = impl;
        b->id = id;
        b->proxy = static_cast<pw_node *>(pw_registry_bind(impl->registry, id, type, PW_VERSION_NODE, 0));
        if (b->proxy) {
            pw_node_add_listener(b->proxy, &b->listener, &kNodeEvents, b.get());
            impl->boundNodes[id] = std::move(b);
        }
        impl->post([impl, id, props] {
            Node n;
            n.id = id;
            fillNode(n, props);
            impl->graph.nodes.insert(id, n);
            Q_EMIT impl->q->nodeAdded(id);
            impl->q->scheduleChanged();
        });
    } else if (std::strcmp(type, PW_TYPE_INTERFACE_Port) == 0) {
        Port p;
        p.id = id;
        p.nodeId = props.value(QStringLiteral(PW_KEY_NODE_ID)).toUInt();
        p.output = props.value(QStringLiteral(PW_KEY_PORT_DIRECTION)) == QLatin1String("out");
        p.monitor = props.value(QStringLiteral(PW_KEY_PORT_MONITOR)) == QLatin1String("true");
        p.channel = props.value(QStringLiteral(PW_KEY_AUDIO_CHANNEL));
        p.name = props.value(QStringLiteral(PW_KEY_PORT_NAME));
        impl->post([impl, p] {
            impl->graph.ports.insert(p.id, p);
            impl->q->scheduleChanged();
        });
    } else if (std::strcmp(type, PW_TYPE_INTERFACE_Link) == 0) {
        Link l;
        l.id = id;
        l.outNode = props.value(QStringLiteral(PW_KEY_LINK_OUTPUT_NODE)).toUInt();
        l.outPort = props.value(QStringLiteral(PW_KEY_LINK_OUTPUT_PORT)).toUInt();
        l.inNode = props.value(QStringLiteral(PW_KEY_LINK_INPUT_NODE)).toUInt();
        l.inPort = props.value(QStringLiteral(PW_KEY_LINK_INPUT_PORT)).toUInt();
        impl->post([impl, l] {
            impl->graph.links.insert(l.id, l);
            impl->q->scheduleChanged();
        });
    } else if (std::strcmp(type, PW_TYPE_INTERFACE_Client) == 0) {
        Client c;
        c.id = id;
        c.appName = props.value(QStringLiteral(PW_KEY_APP_NAME));
        if (c.appName == QLatin1String("WirePlumber") && !impl->wpClient) {
            impl->wpClient = static_cast<pw_client *>(
                pw_registry_bind(impl->registry, id, type, PW_VERSION_CLIENT, 0));
            if (impl->wpClient) {
                pw_client_add_listener(impl->wpClient, &impl->wpClientListener, &kClientEvents, impl);
            }
        }
        impl->post([impl, c] {
            impl->graph.clients.insert(c.id, c);
            if (c.appName == QLatin1String("WirePlumber")) {
                impl->hasWirePlumber = true;
                Q_EMIT impl->q->stateChanged();
            }
        });
    } else if (std::strcmp(type, PW_TYPE_INTERFACE_Factory) == 0) {
        const QString name = props.value(QStringLiteral(PW_KEY_FACTORY_NAME));
        impl->post([impl, id, name] {
            impl->graph.factories.insert(id, name);
            impl->q->scheduleChanged();
        });
    } else if (std::strcmp(type, PW_TYPE_INTERFACE_Metadata) == 0) {
        if (props.value(QStringLiteral(PW_KEY_METADATA_NAME)) == QLatin1String("default") && !impl->metadata) {
            impl->metadata = static_cast<pw_metadata *>(
                pw_registry_bind(impl->registry, id, type, PW_VERSION_METADATA, 0));
            impl->metadataId = id;
            if (impl->metadata) {
                pw_metadata_add_listener(impl->metadata, &impl->metadataListener, &kMetadataEvents, impl);
            }
        }
    }
}

void onGlobalRemove(void *data, uint32_t id)
{
    auto *impl = static_cast<PwContext::Impl *>(data);
    if (auto it = impl->boundNodes.find(id); it != impl->boundNodes.end()) {
        spa_hook_remove(&it->second->listener);
        pw_proxy_destroy(reinterpret_cast<pw_proxy *>(it->second->proxy));
        impl->boundNodes.erase(it);
    }
    if (id == impl->metadataId && impl->metadata) {
        spa_hook_remove(&impl->metadataListener);
        pw_proxy_destroy(reinterpret_cast<pw_proxy *>(impl->metadata));
        impl->metadata = nullptr;
        impl->metadataId = SPA_ID_INVALID;
    }
    impl->post([impl, id] {
        auto &g = impl->graph;
        if (auto it = g.nodes.find(id); it != g.nodes.end()) {
            const QString name = it->name;
            g.nodes.erase(it);
            impl->metadataValues.remove(id);
            Q_EMIT impl->q->nodeRemoved(id, name);
        }
        g.ports.remove(id);
        g.links.remove(id);
        g.clients.remove(id);
        g.factories.remove(id);
        impl->q->scheduleChanged();
    });
}

const pw_registry_events kRegistryEvents = {
    .version = PW_VERSION_REGISTRY_EVENTS,
    .global = onGlobal,
    .global_remove = onGlobalRemove,
};

void onCoreInfo(void *data, const pw_core_info *info)
{
    auto *impl = static_cast<PwContext::Impl *>(data);
    if (!info->version) {
        return;
    }
    const QString version = QString::fromUtf8(info->version);
    impl->post([impl, version] { impl->serverVersion = version; });
}

void onCoreDone(void *data, uint32_t id, int seq)
{
    auto *impl = static_cast<PwContext::Impl *>(data);
    if (id == PW_ID_CORE && seq == impl->roundtripSeq) {
        impl->roundtripDone = true;
        pw_thread_loop_signal(impl->loop, false);
    }
    if (id == PW_ID_CORE && seq == impl->initialSeq) {
        impl->post([impl] {
            impl->state = PwContext::State::Ready;
            Q_EMIT impl->q->stateChanged();
            impl->q->scheduleChanged();
        });
    }
}

void onCoreError(void *data, uint32_t id, int seq, int res, const char *message)
{
    auto *impl = static_cast<PwContext::Impl *>(data);
    const QString msg = QString::fromUtf8(message ? message : "");
    if (id == PW_ID_CORE && res == -EPIPE) {
        impl->post([impl, msg] {
            impl->state = PwContext::State::Failed;
            impl->error = QStringLiteral("Lost the connection to PipeWire (%1).").arg(msg);
            Q_EMIT impl->q->stateChanged();
        });
        return;
    }
    // ENOENT is routine: an object vanished between our mirror update and the request.
    if (res == -ENOENT) {
        qCDebug(lcPw) << "core error on object" << id << "seq" << seq << spa_strerror(res) << msg;
        return;
    }
    qCWarning(lcPw) << "core error on object" << id << "seq" << seq << spa_strerror(res) << msg;
}

const pw_core_events kCoreEvents = {
    .version = PW_VERSION_CORE_EVENTS,
    .info = onCoreInfo,
    .done = onCoreDone,
    .error = onCoreError,
};

} // namespace

void PwContext::Impl::teardownLocked()
{
    for (auto &[id, b] : boundNodes) {
        spa_hook_remove(&b->listener);
        pw_proxy_destroy(reinterpret_cast<pw_proxy *>(b->proxy));
    }
    boundNodes.clear();
    while (!created.empty()) {
        // Lingering objects stay in the daemon; only the local proxy goes away.
        pw_proxy_destroy(created.front()->proxy);
    }
    if (metadata) {
        spa_hook_remove(&metadataListener);
        pw_proxy_destroy(reinterpret_cast<pw_proxy *>(metadata));
        metadata = nullptr;
    }
    if (wpClient) {
        spa_hook_remove(&wpClientListener);
        pw_proxy_destroy(reinterpret_cast<pw_proxy *>(wpClient));
        wpClient = nullptr;
    }
    if (registry) {
        spa_hook_remove(&registryListener);
        pw_proxy_destroy(reinterpret_cast<pw_proxy *>(registry));
        registry = nullptr;
    }
    if (core) {
        spa_hook_remove(&coreListener);
        pw_core_disconnect(core);
        core = nullptr;
    }
}

// ---- Qt-thread API ----------------------------------------------------------------------

PwContext::PwContext(QObject *parent)
    : QObject(parent)
    , d(std::make_unique<Impl>())
{
    d->q = this;
    pw_init(nullptr, nullptr);
}

PwContext::~PwContext()
{
    stop();
    pw_deinit();
}

bool PwContext::start()
{
    stop();
    d->graph = {};
    d->metadataValues.clear();
    d->error.clear();
    d->errorCode = 0;
    d->state = State::Connecting;
    Q_EMIT stateChanged();

    d->loop = pw_thread_loop_new("rostrum-pw", nullptr);
    d->context = pw_context_new(pw_thread_loop_get_loop(d->loop), nullptr, 0);
    if (!d->context) {
        d->error = QStringLiteral("Could not create a PipeWire context.");
        d->state = State::Failed;
        Q_EMIT stateChanged();
        return false;
    }
    pw_thread_loop_lock(d->loop);
    if (pw_thread_loop_start(d->loop) < 0) {
        pw_thread_loop_unlock(d->loop);
        d->error = QStringLiteral("Could not start the PipeWire thread.");
        d->state = State::Failed;
        Q_EMIT stateChanged();
        return false;
    }
    auto *props = pw_properties_new(PW_KEY_APP_NAME, "Rostrum", PW_KEY_APP_ID, ROSTRUM_APP_ID, nullptr);
    d->core = pw_context_connect(d->context, props, 0);
    if (!d->core) {
        const int err = errno;
        pw_thread_loop_unlock(d->loop);
        d->error = QStringLiteral("PipeWire is not running (%1).").arg(QString::fromUtf8(std::strerror(err)));
        d->errorCode = err;
        stop();
        d->state = State::Failed;
        Q_EMIT stateChanged();
        return false;
    }
    pw_core_add_listener(d->core, &d->coreListener, &kCoreEvents, d.get());
    d->registry = pw_core_get_registry(d->core, PW_VERSION_REGISTRY, 0);
    pw_registry_add_listener(d->registry, &d->registryListener, &kRegistryEvents, d.get());
    d->initialSeq = pw_core_sync(d->core, PW_ID_CORE, 0);
    pw_thread_loop_unlock(d->loop);
    return true;
}

void PwContext::stop()
{
    if (!d->loop) {
        return;
    }
    Q_EMIT aboutToStop();
    pw_thread_loop_lock(d->loop);
    d->teardownLocked();
    pw_thread_loop_unlock(d->loop);
    pw_thread_loop_stop(d->loop);
    if (d->context) {
        pw_context_destroy(d->context);
        d->context = nullptr;
    }
    pw_thread_loop_destroy(d->loop);
    d->loop = nullptr;
    d->hasWirePlumber = false;
    if (d->state != State::Failed) {
        d->state = State::Idle;
    }
}

PwContext::State PwContext::state() const { return d->state; }
QString PwContext::errorString() const { return d->error; }
int PwContext::errorCode() const { return d->errorCode; }
QString PwContext::serverVersion() const { return d->serverVersion; }
QString PwContext::libraryVersion() const { return QString::fromUtf8(pw_get_library_version()); }
QString PwContext::wireplumberVersion() const { return d->wireplumberVersion; }
bool PwContext::hasWirePlumber() const { return d->hasWirePlumber; }
const Graph &PwContext::graph() const { return d->graph; }
QString PwContext::defaultSinkName() const { return d->defaultSink; }
QString PwContext::defaultSourceName() const { return d->defaultSource; }

QString PwContext::metadataValue(uint32_t subject, const QString &key) const
{
    return d->metadataValues.value(subject).value(key);
}

pw_thread_loop *PwContext::threadLoop() const { return d->loop; }
pw_core *PwContext::core() const { return d->core; }

void PwContext::scheduleChanged()
{
    if (d->changePending) {
        return;
    }
    d->changePending = true;
    QTimer::singleShot(0, this, [this] {
        d->changePending = false;
        Q_EMIT graphChanged();
    });
}

void PwContext::createObject(const char *factory, const char *type, uint32_t version,
                             const QMap<QString, QString> &props)
{
    if (!d->core) {
        return;
    }
    pw_thread_loop_lock(d->loop);
    auto *p = pw_properties_new(nullptr, nullptr);
    for (auto it = props.cbegin(); it != props.cend(); ++it) {
        pw_properties_set(p, it.key().toUtf8().constData(), it.value().toUtf8().constData());
    }
    auto c = std::make_unique<CreatedProxy>();
    c->impl = d.get();
    if (std::strcmp(type, PW_TYPE_INTERFACE_Node) == 0) {
        c->nodeName = props.value(QStringLiteral(PW_KEY_NODE_NAME));
    }
    c->proxy = static_cast<pw_proxy *>(pw_core_create_object(d->core, factory, type, version, &p->dict, 0));
    pw_properties_free(p);
    if (c->proxy) {
        pw_proxy_add_listener(c->proxy, &c->listener, &kCreatedEvents, c.get());
        d->created.push_back(std::move(c));
    }
    pw_thread_loop_unlock(d->loop);
}

void PwContext::createNullNode(const QMap<QString, QString> &props)
{
    auto p = props;
    p.insert(QStringLiteral(PW_KEY_FACTORY_NAME), QStringLiteral("support.null-audio-sink"));
    createObject("adapter", PW_TYPE_INTERFACE_Node, PW_VERSION_NODE, p);
}

void PwContext::createSpaNode(const QMap<QString, QString> &props)
{
    createObject("spa-node-factory", PW_TYPE_INTERFACE_Node, PW_VERSION_NODE, props);
}

void PwContext::setPortConfig(uint32_t nodeId, bool output, int channels)
{
    if (!d->loop) {
        return;
    }
    spa_audio_info_raw info{};
    info.format = SPA_AUDIO_FORMAT_F32P;
    if (channels <= 1) {
        info.channels = 1;
        info.position[0] = SPA_AUDIO_CHANNEL_MONO;
    } else {
        info.channels = 2;
        info.position[0] = SPA_AUDIO_CHANNEL_FL;
        info.position[1] = SPA_AUDIO_CHANNEL_FR;
    }
    pw_thread_loop_lock(d->loop);
    if (auto it = d->boundNodes.find(nodeId); it != d->boundNodes.end()) {
        uint8_t buffer[1024];
        spa_pod_builder b = SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));
        spa_pod_frame f;
        spa_pod_builder_push_object(&b, &f, SPA_TYPE_OBJECT_ParamPortConfig, SPA_PARAM_PortConfig);
        spa_pod_builder_add(&b, SPA_PARAM_PORT_CONFIG_direction,
                            SPA_POD_Id(output ? SPA_DIRECTION_OUTPUT : SPA_DIRECTION_INPUT),
                            SPA_PARAM_PORT_CONFIG_mode, SPA_POD_Id(SPA_PARAM_PORT_CONFIG_MODE_dsp), 0);
        spa_pod_builder_prop(&b, SPA_PARAM_PORT_CONFIG_format, 0);
        spa_format_audio_raw_build(&b, SPA_PARAM_Format, &info);
        auto *pod = static_cast<spa_pod *>(spa_pod_builder_pop(&b, &f));
        pw_node_set_param(it->second->proxy, SPA_PARAM_PortConfig, 0, pod);
    }
    pw_thread_loop_unlock(d->loop);
}

void PwContext::setNodeParams(uint32_t nodeId, const QList<QPair<QString, QVariant>> &params)
{
    if (!d->loop || params.isEmpty()) {
        return;
    }
    QList<QByteArray> keys;
    QList<QByteArray> strings;
    qsizetype size = 1024;
    for (const auto &[key, value] : params) {
        keys << key.toUtf8();
        size += keys.last().size() + 32;
        if (value.typeId() == QMetaType::QString) {
            strings << value.toString().toUtf8();
            size += strings.last().size() + 32;
        }
    }
    std::vector<uint8_t> buffer(size_t(size) * 2);
    spa_pod_builder b = SPA_POD_BUILDER_INIT(buffer.data(), uint32_t(buffer.size()));
    spa_pod_frame f[2];
    spa_pod_builder_push_object(&b, &f[0], SPA_TYPE_OBJECT_Props, SPA_PARAM_Props);
    spa_pod_builder_prop(&b, SPA_PROP_params, 0);
    spa_pod_builder_push_struct(&b, &f[1]);
    qsizetype nextString = 0;
    for (qsizetype i = 0; i < params.size(); ++i) {
        const QVariant &value = params[i].second;
        spa_pod_builder_string(&b, keys[i].constData());
        switch (value.typeId()) {
        case QMetaType::Bool:
            spa_pod_builder_bool(&b, value.toBool());
            break;
        case QMetaType::QString:
            spa_pod_builder_string(&b, strings[nextString++].constData());
            break;
        default:
            spa_pod_builder_float(&b, value.toFloat());
            break;
        }
    }
    spa_pod_builder_pop(&b, &f[1]);
    auto *pod = static_cast<spa_pod *>(spa_pod_builder_pop(&b, &f[0]));
    if (!pod) {
        qCWarning(lcPw) << "params for node" << nodeId << "did not fit";
        return;
    }
    pw_thread_loop_lock(d->loop);
    if (auto it = d->boundNodes.find(nodeId); it != d->boundNodes.end()) {
        pw_node_set_param(it->second->proxy, SPA_PARAM_Props, 0, pod);
    }
    pw_thread_loop_unlock(d->loop);
}

bool PwContext::roundtrip(int timeoutMs)
{
    if (!d->core || !d->loop) {
        return false;
    }
    pw_thread_loop_lock(d->loop);
    d->roundtripDone = false;
    d->roundtripSeq = pw_core_sync(d->core, PW_ID_CORE, 0);
    timespec deadline{};
    pw_thread_loop_get_time(d->loop, &deadline, int64_t(timeoutMs) * SPA_NSEC_PER_MSEC);
    while (!d->roundtripDone) {
        if (pw_thread_loop_timed_wait_full(d->loop, &deadline) != 0) {
            break;
        }
    }
    const bool done = d->roundtripDone;
    d->roundtripSeq = -1;
    pw_thread_loop_unlock(d->loop);
    return done;
}

void PwContext::destroyObject(uint32_t id)
{
    if (!d->registry) {
        return;
    }
    pw_thread_loop_lock(d->loop);
    pw_registry_destroy(d->registry, id);
    pw_thread_loop_unlock(d->loop);
}

void PwContext::createLink(uint32_t outPort, uint32_t inPort)
{
    createObject("link-factory", PW_TYPE_INTERFACE_Link, PW_VERSION_LINK,
                 {{QStringLiteral(PW_KEY_OBJECT_LINGER), QStringLiteral("true")},
                  {QStringLiteral(PW_KEY_LINK_OUTPUT_PORT), QString::number(outPort)},
                  {QStringLiteral(PW_KEY_LINK_INPUT_PORT), QString::number(inPort)}});
}

void PwContext::setNodeVolume(uint32_t nodeId, float linear, bool mute)
{
    sendProps(nodeId, {linear}, mute ? 1 : 0);
}

void PwContext::setNodeVolumes(uint32_t nodeId, const QList<float> &perChannel, bool mute)
{
    sendProps(nodeId, perChannel, mute ? 1 : 0);
}

void PwContext::setNodeVolume(uint32_t nodeId, float linear)
{
    sendProps(nodeId, {linear}, -1);
}

void PwContext::setNodeMute(uint32_t nodeId, bool mute)
{
    sendProps(nodeId, {}, mute ? 1 : 0);
}

void PwContext::sendProps(uint32_t nodeId, const QList<float> &volumes, int mute)
{
    const Node *n = d->graph.node(nodeId);
    if (!n || !d->loop || (volumes.isEmpty() && mute < 0)) {
        return;
    }
    int channels = n->channels;
    if (channels <= 0) {
        channels = n->prop("audio.channels").toInt();
    }
    if (channels <= 0) {
        channels = 2;
    }
    channels = std::min(channels, int(SPA_AUDIO_MAX_CHANNELS));
    float vols[SPA_AUDIO_MAX_CHANNELS];
    for (int i = 0; i < channels && !volumes.isEmpty(); ++i) {
        vols[i] = volumes.value(i, volumes.last());
    }

    pw_thread_loop_lock(d->loop);
    auto it = d->boundNodes.find(nodeId);
    if (it != d->boundNodes.end()) {
        uint8_t buffer[1024];
        spa_pod_builder b = SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));
        spa_pod_frame f;
        spa_pod_builder_push_object(&b, &f, SPA_TYPE_OBJECT_Props, SPA_PARAM_Props);
        if (!volumes.isEmpty()) {
            spa_pod_builder_prop(&b, SPA_PROP_channelVolumes, 0);
            spa_pod_builder_array(&b, sizeof(float), SPA_TYPE_Float, uint32_t(channels), vols);
        }
        if (mute >= 0) {
            spa_pod_builder_prop(&b, SPA_PROP_mute, 0);
            spa_pod_builder_bool(&b, mute == 1);
        }
        auto *pod = static_cast<spa_pod *>(spa_pod_builder_pop(&b, &f));
        pw_node_set_param(it->second->proxy, SPA_PARAM_Props, 0, pod);
    }
    pw_thread_loop_unlock(d->loop);
}

void PwContext::setMetadata(uint32_t subject, const QString &key, const QString &type, const QString &value)
{
    if (!d->loop) {
        return;
    }
    pw_thread_loop_lock(d->loop);
    if (d->metadata) {
        pw_metadata_set_property(d->metadata, subject, key.toUtf8().constData(), type.toUtf8().constData(),
                                 value.toUtf8().constData());
    }
    pw_thread_loop_unlock(d->loop);
}

void PwContext::clearMetadata(uint32_t subject, const QString &key)
{
    if (!d->loop) {
        return;
    }
    pw_thread_loop_lock(d->loop);
    if (d->metadata) {
        pw_metadata_set_property(d->metadata, subject, key.toUtf8().constData(), nullptr, nullptr);
    }
    pw_thread_loop_unlock(d->loop);
}

} // namespace rostrum::pw
