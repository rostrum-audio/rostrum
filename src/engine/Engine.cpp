#include "engine/Engine.h"

#include "core/Volume.h"
#include "pw/PwContext.h"

#include <QCoreApplication>
#include <QLoggingCategory>
#include <QTimer>

Q_LOGGING_CATEGORY(lcEngine, "rostrum.engine")

namespace rostrum::engine {

namespace {
constexpr int kCreateRetryMs = 3000;
constexpr int kCreateTimeoutMs = 5000;
constexpr qint64 kSessionGraceMs = 30000;
} // namespace

Engine::Engine(pw::PwContext *pw, QObject *parent)
    : QObject(parent)
    , m_pw(pw)
    , m_scene(defaults::scene())
{
    connect(m_pw, &pw::PwContext::graphChanged, this, &Engine::scheduleReconcile);
    connect(m_pw, &pw::PwContext::stateChanged, this, &Engine::scheduleReconcile);
    connect(m_pw, &pw::PwContext::nodeRemoved, this, [this](uint32_t id) { m_pendingDestroy.remove(id); });
    connect(m_pw, &pw::PwContext::createFailed, this, [this](const QString &msg) {
        m_mixError = msg;
        Q_EMIT mixStateChanged();
    });
}

void Engine::setScene(const Scene &scene)
{
    m_scene = scene;
    m_soloed.clear();
    Q_EMIT soloChanged();
    Q_EMIT levelsChanged();
    Q_EMIT sceneChanged();
    scheduleReconcile();
}

void Engine::createMix()
{
    if (m_mixEnabled && m_mixError.isEmpty()) {
        scheduleReconcile();
        return;
    }
    m_mixEnabled = true;
    m_mixError.clear();
    m_mixRequested.start();
    m_pendingCreate.clear();
    Q_EMIT mixStateChanged();
    scheduleReconcile();
    // Report a plain error if PipeWire silently refuses to create the nodes.
    QTimer::singleShot(kCreateTimeoutMs + 100, this, [this] {
        if (m_mixEnabled && !mixReady() && m_mixError.isEmpty()) {
            m_mixError = QStringLiteral("PipeWire did not create the virtual devices. Check that the "
                                        "PipeWire service is running, then try again.");
            Q_EMIT mixStateChanged();
        }
    });
}

void Engine::rebuildMix()
{
    destroyMix();
    // Give the daemon a moment to drop the old globals before recreating them.
    QTimer::singleShot(500, this, [this] { createMix(); });
}

void Engine::destroyMix()
{
    m_mixEnabled = false;
    m_pendingCreate.clear();
    const auto nodes = m_pw->graph().nodes;
    for (const auto &n : nodes) {
        if (isOwnedNode(n)) {
            destroyOnce(n.id);
        }
    }
    Q_EMIT mixStateChanged();
}

bool Engine::isOwnedNode(const pw::Node &n) const
{
    if (!n.isRostrum() || n.isPlaybackStream() || n.isCaptureStream()) {
        return false;
    }
    return !n.rostrumRole().isEmpty() || n.mediaClass == QLatin1String("Audio/Sink") ||
           n.mediaClass == QLatin1String("Audio/Source/Virtual");
}

void Engine::destroyOnce(uint32_t id)
{
    if (m_pendingDestroy.contains(id)) {
        return;
    }
    m_pendingDestroy.insert(id);
    m_pw->destroyObject(id);
}

bool Engine::mixReady() const
{
    if (m_pw->state() != pw::PwContext::State::Ready) {
        return false;
    }
    const auto specs = desiredNodes(m_scene);
    for (const auto &s : specs) {
        if (!m_pw->graph().nodeByName(s.name)) {
            return false;
        }
    }
    return true;
}

void Engine::scheduleReconcile()
{
    if (m_reconcilePending) {
        return;
    }
    m_reconcilePending = true;
    QTimer::singleShot(0, this, [this] {
        m_reconcilePending = false;
        reconcile();
    });
}

void Engine::reconcile()
{
    if (m_pw->state() != pw::PwContext::State::Ready) {
        if (m_reportedReady) {
            m_reportedReady = false;
            Q_EMIT mixStateChanged();
        }
        return;
    }
    const auto &g = m_pw->graph();
    for (auto it = m_pendingDestroy.begin(); it != m_pendingDestroy.end();) {
        it = (g.nodes.contains(*it) || g.links.contains(*it)) ? std::next(it) : m_pendingDestroy.erase(it);
    }
    reconcileNodes();
    reconcileDevices();
    reconcileLinks();
    reconcileVolumes();
    reconcileRoutes();
    // Compare with the last reported state: the graph changes before this pass runs.
    const bool ready = mixReady();
    if (ready != m_reportedReady) {
        m_reportedReady = ready;
        if (ready) {
            m_mixError.clear();
        }
        Q_EMIT mixStateChanged();
    }
}

void Engine::reconcileNodes()
{
    if (!m_mixEnabled) {
        return;
    }
    const auto &graph = m_pw->graph();
    const auto specs = desiredNodes(m_scene);
    QSet<QString> wanted;
    for (const auto &spec : specs) {
        wanted.insert(spec.name);
        if (graph.nodeByName(spec.name)) {
            m_pendingCreate.remove(spec.name);
            continue;
        }
        auto it = m_pendingCreate.find(spec.name);
        if (it != m_pendingCreate.end() && it->elapsed() < kCreateRetryMs) {
            continue;
        }
        qCInfo(lcEngine) << "creating" << spec.name;
        m_pw->createNullNode(spec.properties());
        QElapsedTimer t;
        t.start();
        m_pendingCreate.insert(spec.name, t);
    }
    // Remove buses that left the scene, and duplicates left by a create race (keep the oldest).
    QHash<QString, uint32_t> keep;
    for (const auto &n : graph.nodes) {
        if (!isOwnedNode(n) || m_pendingDestroy.contains(n.id)) {
            continue;
        }
        if (!wanted.contains(n.name)) {
            if (n.rostrumRole() == QLatin1String("bus")) {
                qCInfo(lcEngine) << "removing" << n.name;
                destroyOnce(n.id);
            }
            continue;
        }
        auto it = keep.find(n.name);
        if (it == keep.end()) {
            keep.insert(n.name, n.id);
        } else {
            const uint32_t drop = std::max(it.value(), n.id);
            it.value() = std::min(it.value(), n.id);
            qCInfo(lcEngine) << "removing duplicate" << n.name << drop;
            destroyOnce(drop);
        }
    }
}

// ---- apps and routing -------------------------------------------------------------------

bool Engine::isOwnStream(const pw::Node &n) const
{
    return n.isRostrum() || n.pid == QCoreApplication::applicationPid() ||
           n.prop("rostrum.internal") == QLatin1String("true");
}

StreamProps Engine::propsOf(const pw::Node &n) const
{
    return {n.appName, n.binary, n.mediaName, n.name};
}

QString Engine::effectiveBus(const StreamProps &props, const AppIdentity &id, AppKey *ruleKey, bool *session) const
{
    *session = false;
    *ruleKey = {};
    if (auto it = m_sessionAssign.constFind(id.key.toString()); it != m_sessionAssign.cend()) {
        *session = true;
        return it.value();
    }
    const int idx = matchRule(m_scene.rules, props);
    if (idx < 0) {
        return {};
    }
    const AppRule &r = m_scene.rules.at(idx);
    *ruleKey = {r.key, r.match};
    return r.busId;
}

QList<AppStream> Engine::appStreams() const
{
    QList<AppStream> out;
    for (const auto &n : m_pw->graph().nodes) {
        if (!n.isPlaybackStream() || isOwnStream(n)) {
            continue;
        }
        AppStream s;
        s.nodeId = n.id;
        s.props = propsOf(n);
        s.identity = identify(s.props);
        s.busId = effectiveBus(s.props, s.identity, &s.ruleKey, &s.sessionOnly);
        if (!m_scene.bus(s.busId)) {
            s.busId.clear();
        }
        s.volume = s.ruleKey.isValid() ? appVolume(s.ruleKey) : appVolume(s.identity.key);
        if (s.ruleKey.isValid()) {
            if (const AppRule *r = m_scene.rule(s.ruleKey.key, s.ruleKey.match); r && !r->label.isEmpty()) {
                s.identity.displayName = r->label;
                s.identity.unnamed = false;
            }
        }
        out.append(s);
    }
    std::sort(out.begin(), out.end(), [](const AppStream &a, const AppStream &b) {
        const int c = a.identity.displayName.compare(b.identity.displayName, Qt::CaseInsensitive);
        return c != 0 ? c < 0 : a.nodeId < b.nodeId;
    });
    return out;
}

void Engine::assignApp(const AppKey &key, const QString &busId, bool always)
{
    const Bus *bus = m_scene.bus(busId);
    if (!key.isValid() || !bus || bus->isInput()) {
        return;
    }
    if (always) {
        m_sessionAssign.remove(key.toString());
        if (AppRule *r = m_scene.rule(key.key, key.match)) {
            r->busId = busId;
        } else {
            AppRule rule;
            rule.match = key.match;
            rule.key = key.key;
            rule.busId = busId;
            rule.volume = m_sessionVolume.take(key.toString());
            if (rule.volume <= 0.0) {
                rule.volume = 1.0;
            }
            rule.lastSeen = QDateTime::currentDateTimeUtc();
            m_scene.rules.append(rule);
        }
        Q_EMIT structureChanged();
    } else {
        m_sessionAssign.insert(key.toString(), busId);
        QElapsedTimer t;
        t.start();
        m_sessionSeen.insert(key.toString(), t);
    }
    Q_EMIT sceneChanged();
    Q_EMIT appsChanged();
    scheduleReconcile();
}

void Engine::unassignApp(const AppKey &key)
{
    bool structural = false;
    m_sessionAssign.remove(key.toString());
    const auto before = m_scene.rules.size();
    m_scene.rules.removeIf([&](const AppRule &r) { return AppKey{r.key, r.match} == key; });
    structural = before != m_scene.rules.size();
    if (structural) {
        Q_EMIT structureChanged();
        Q_EMIT sceneChanged();
    }
    Q_EMIT appsChanged();
    scheduleReconcile();
}

void Engine::unassignStream(uint32_t nodeId)
{
    const pw::Node *n = m_pw->graph().node(nodeId);
    if (!n) {
        return;
    }
    const auto props = propsOf(*n);
    const auto id = identify(props);
    m_sessionAssign.remove(id.key.toString());
    const int idx = matchRule(m_scene.rules, props);
    if (idx >= 0) {
        const AppRule r = m_scene.rules.at(idx);
        unassignApp({r.key, r.match});
        return;
    }
    Q_EMIT appsChanged();
    scheduleReconcile();
}

void Engine::removeRule(const AppKey &key)
{
    unassignApp(key);
}

void Engine::editRule(const AppKey &oldKey, const AppKey &newKey)
{
    AppRule *r = m_scene.rule(oldKey.key, oldKey.match);
    if (!r || !newKey.isValid()) {
        return;
    }
    r->key = newKey.key;
    r->match = newKey.match.trimmed();
    Q_EMIT structureChanged();
    Q_EMIT sceneChanged();
    Q_EMIT appsChanged();
    scheduleReconcile();
}

void Engine::setRuleLabel(const AppKey &key, const QString &label)
{
    AppRule *r = m_scene.rule(key.key, key.match);
    const QString clean = label.trimmed().left(64);
    if (!r || r->label == clean) {
        return;
    }
    r->label = clean;
    Q_EMIT structureChanged();
    Q_EMIT sceneChanged();
    Q_EMIT appsChanged();
}

void Engine::setAppVolume(const AppKey &key, double volume)
{
    volume = std::clamp(volume, 0.0, 1.0);
    if (AppRule *r = m_scene.rule(key.key, key.match)) {
        r->volume = volume;
        Q_EMIT sceneChanged();
    } else {
        m_sessionVolume.insert(key.toString(), volume);
    }
    Q_EMIT appsChanged();
    scheduleReconcile();
}

double Engine::appVolume(const AppKey &key) const
{
    if (const AppRule *r = m_scene.rule(key.key, key.match)) {
        return r->volume;
    }
    return m_sessionVolume.value(key.toString(), 1.0);
}

void Engine::reconcileRoutes()
{
    const auto &graph = m_pw->graph();
    QSet<uint32_t> present;
    QSet<QString> presentKeys;
    bool seenChanged = false;

    for (const auto &n : graph.nodes) {
        if (!n.isPlaybackStream() || isOwnStream(n)) {
            continue;
        }
        present.insert(n.id);
        const auto props = propsOf(n);
        const auto id = identify(props);
        presentKeys.insert(id.key.toString());
        AppKey ruleKey;
        bool session = false;
        const QString busId = effectiveBus(props, id, &ruleKey, &session);

        if (ruleKey.isValid() && !m_seenThisSession.contains(ruleKey.toString())) {
            m_seenThisSession.insert(ruleKey.toString());
            if (AppRule *r = m_scene.rule(ruleKey.key, ruleKey.match)) {
                r->lastSeen = QDateTime::currentDateTimeUtc();
                seenChanged = true;
            }
        }

        const Bus *bus = m_scene.bus(busId);
        const pw::Node *busNode = bus && !bus->isInput() ? graph.nodeByName(bus->nodeName()) : nullptr;
        const QString current = m_pw->metadataValue(n.id, QStringLiteral("target.object"));

        if (busNode && !busNode->serial.isEmpty()) {
            // Set the target explicitly for every matching stream whenever this runs, including
            // right after a bus node appears. Never wait for a WirePlumber rescan.
            // `requested` stops re-sending while the metadata event is in flight, and leaves a
            // stream alone if the user later moves it by hand in another mixer.
            const auto routed = m_routed.constFind(n.id);
            if (current != busNode->serial &&
                (routed == m_routed.cend() || routed->requested != busNode->serial)) {
                if (routed == m_routed.cend()) {
                    const bool currentIsOurs =
                        std::any_of(graph.nodes.cbegin(), graph.nodes.cend(),
                                    [&](const pw::Node &o) { return o.serial == current && isOwnedNode(o); });
                    m_routed.insert(n.id, {busId, currentIsOurs ? QString() : current, QString()});
                }
                m_routed[n.id].busId = busId;
                m_routed[n.id].requested = busNode->serial;
                qCInfo(lcEngine) << "route" << id.displayName << n.id << "->" << bus->nodeName();
                m_pw->setMetadata(n.id, QStringLiteral("target.object"), QStringLiteral("Spa:Id"), busNode->serial);
            }
        } else if (busId.isEmpty() && m_routed.contains(n.id)) {
            const Routed r = m_routed.take(n.id);
            qCInfo(lcEngine) << "unroute" << id.displayName << n.id;
            if (r.previousTarget.isEmpty()) {
                m_pw->clearMetadata(n.id, QStringLiteral("target.object"));
                m_pw->clearMetadata(n.id, QStringLiteral("target.node"));
            } else {
                m_pw->setMetadata(n.id, QStringLiteral("target.object"), QStringLiteral("Spa:Id"), r.previousTarget);
            }
        }

        // Per-app volume offset. Only touch the stream once the user has set something other
        // than 100%, so Rostrum does not fight volumes set in other mixers.
        const double vol = ruleKey.isValid() ? appVolume(ruleKey) : appVolume(id.key);
        const auto applied = m_appliedStreamVolume.constFind(n.id);
        if ((applied == m_appliedStreamVolume.cend() && vol != 1.0) ||
            (applied != m_appliedStreamVolume.cend() && applied.value() != vol)) {
            m_pw->setNodeVolume(n.id, float(volume::faderToLinear(vol)));
            m_appliedStreamVolume.insert(n.id, vol);
        }
    }

    for (auto it = m_routed.begin(); it != m_routed.end();) {
        it = present.contains(it.key()) ? std::next(it) : m_routed.erase(it);
    }
    for (auto it = m_appliedStreamVolume.begin(); it != m_appliedStreamVolume.end();) {
        it = present.contains(it.key()) ? std::next(it) : m_appliedStreamVolume.erase(it);
    }
    // "This launch only" assignments end once the app has been gone for a while.
    for (auto it = m_sessionAssign.begin(); it != m_sessionAssign.end();) {
        auto &seen = m_sessionSeen[it.key()];
        if (presentKeys.contains(it.key()) || !seen.isValid()) {
            seen.start();
            ++it;
        } else if (seen.elapsed() > kSessionGraceMs) {
            m_sessionSeen.remove(it.key());
            it = m_sessionAssign.erase(it);
        } else {
            ++it;
        }
    }
    if (seenChanged) {
        Q_EMIT structureChanged();
    }
    Q_EMIT appsChanged();
}

} // namespace rostrum::engine
