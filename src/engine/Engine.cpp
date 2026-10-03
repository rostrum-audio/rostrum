#include "engine/Engine.h"

#include "core/AppFacts.h"
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
constexpr int kFadeStepMs = 16;
constexpr int kDuckStepMs = 20;
} // namespace

Engine::Engine(pw::PwContext *pw, QObject *parent)
    : QObject(parent)
    , m_duckMeters(pw)
    , m_pw(pw)
    , m_scene(defaults::scene())
{
    connect(m_pw, &pw::PwContext::graphChanged, this, &Engine::scheduleReconcile);
    connect(m_pw, &pw::PwContext::stateChanged, this, &Engine::scheduleReconcile);
    connect(m_pw, &pw::PwContext::stateChanged, this, &Engine::micFxConnectionChanged);
    connect(m_pw, &pw::PwContext::nodeRemoved, this, [this](uint32_t id) { m_pendingDestroy.remove(id); });
    connect(m_pw, &pw::PwContext::createFailed, this, [this](const QString &msg, const QString &nodeName) {
        // The mix works without mic filters, so their failure is theirs alone.
        if (nodeName == QLatin1String(kMicFxNode) || nodeName == QLatin1String(kFilteredNode)) {
            qCWarning(lcEngine) << "could not create" << nodeName << msg;
            m_fxFailed = true;
            setMicFxState(MicFxState::Failed, QStringLiteral("PipeWire could not create the mic filters (%1). "
                                                             "Your mic is used without filters.").arg(msg));
            scheduleReconcile();
            return;
        }
        m_mixError = msg;
        Q_EMIT mixStateChanged();
    });
    m_fxTimer.setSingleShot(true);
    connect(&m_fxTimer, &QTimer::timeout, this, &Engine::scheduleReconcile);
    m_fadeTimer.setInterval(kFadeStepMs);
    m_fadeTimer.setTimerType(Qt::PreciseTimer);
    connect(&m_fadeTimer, &QTimer::timeout, this, &Engine::fadeTick);
    m_duckTimer.setInterval(kDuckStepMs);
    m_duckTimer.setTimerType(Qt::PreciseTimer);
    connect(&m_duckTimer, &QTimer::timeout, this, &Engine::duckTick);
}

void Engine::setScene(const Scene &scene, bool fade)
{
    QHash<QString, Fade> fades;
    if (fade && m_sceneFadeMs > 0) {
        // A switch mid-fade continues from where the last one got to.
        const auto from = fadeLevels(m_scene, true);
        const auto to = fadeLevels(scene, false);
        for (auto it = to.cbegin(); it != to.cend(); ++it) {
            Level start = fadingLevel(it.key()).value_or(from.value(it.key(), Level{0.0, it->balance}));
            if (start != *it) {
                fades.insert(it.key(), {start, *it});
            }
        }
    }
    m_fades = fades;
    if (m_fades.isEmpty()) {
        m_fadeTimer.stop();
    } else {
        m_fadeLength = m_sceneFadeMs;
        m_fadeClock.start();
        m_fadeTimer.start();
    }
    m_scene = scene;
    m_soloed.clear();
    Q_EMIT soloChanged();
    Q_EMIT levelsChanged();
    Q_EMIT sceneChanged();
    scheduleReconcile();
}

void Engine::setSceneFadeMs(int ms)
{
    m_sceneFadeMs = std::clamp(ms, 0, 5000);
}

QHash<QString, Engine::Level> Engine::fadeLevels(const Scene &scene, bool withSolo) const
{
    QHash<QString, Level> levels;
    for (const auto &b : scene.buses) {
        if (b.isInput()) {
            continue;
        }
        const bool off = b.muted || (withSolo && dimmedBySolo(b.id));
        levels.insert(b.nodeName(), {off ? 0.0 : b.volume, b.balance});
    }
    levels.insert(QString::fromLatin1(kPhonesNode),
                  {scene.masterPhonesMuted ? 0.0 : scene.masterPhones, 0.0});
    levels.insert(QString::fromLatin1(kStreamNode),
                  {scene.masterStreamMuted ? 0.0 : scene.masterStream, 0.0});
    return levels;
}

std::optional<Engine::Level> Engine::fadingLevel(const QString &nodeName) const
{
    const auto it = m_fades.constFind(nodeName);
    if (it == m_fades.cend()) {
        return std::nullopt;
    }
    const double t = m_fadeLength > 0 ? double(m_fadeClock.elapsed()) / m_fadeLength : 1.0;
    return Level{volume::fadePosition(it->from.position, it->to.position, t),
                 volume::fadePosition(it->from.balance, it->to.balance, t)};
}

std::optional<double> Engine::fadingPosition(const QString &nodeName) const
{
    const auto level = fadingLevel(nodeName);
    return level ? std::optional<double>(level->position) : std::nullopt;
}

void Engine::cancelFade(const QString &nodeName)
{
    if (m_fades.remove(nodeName) && m_fades.isEmpty()) {
        m_fadeTimer.stop();
    }
}

void Engine::fadeTick()
{
    if (m_fadeClock.elapsed() >= m_fadeLength) {
        m_fades.clear();
        m_fadeTimer.stop();
    }
    if (m_pw->state() == pw::PwContext::State::Ready) {
        reconcileVolumes();
    }
}

void Engine::restoreScene(const Scene &scene)
{
    const Scene before = m_scene;
    // A running scene fade would otherwise carry on toward the levels being undone.
    m_fades.clear();
    m_fadeTimer.stop();
    m_scene = scene;
    const qsizetype soloed = m_soloed.size();
    m_soloed.removeIf([&](const QString &id) { return !m_scene.bus(id); });
    for (auto it = m_sessionAssign.begin(); it != m_sessionAssign.end();) {
        it = m_scene.bus(it.value()) ? std::next(it) : m_sessionAssign.erase(it);
    }
    if (m_soloed.size() != soloed) {
        Q_EMIT soloChanged();
    }
    if (mergeStructure(before, m_scene) != before) {
        Q_EMIT structureChanged();
    }
    if (before.rules != m_scene.rules || before.buses.size() != m_scene.buses.size()) {
        Q_EMIT appsChanged();
    }
    levelChanged();
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
    reconcileMicFx();
    reconcileDevices();
    reconcileLinks();
    reconcileDucking();
    reconcileVolumes();
    reconcileRoutes();
    reconcileMicRoutes();
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
        if (const pw::Node *n = graph.nodeByName(spec.name)) {
            m_pendingCreate.remove(spec.name);
            if (isOwnedNode(*n) && !m_pendingDestroy.contains(n->id) && needsRename(spec, n->description)) {
                qCInfo(lcEngine) << "recreating" << spec.name << "as" << spec.description;
                destroyOnce(n->id);
            }
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

bool Engine::isRostrumTarget(const QString &target) const
{
    if (target.startsWith(QLatin1String("rostrum."))) {
        return true;
    }
    const auto &nodes = m_pw->graph().nodes;
    return std::any_of(nodes.cbegin(), nodes.cend(), [&](const pw::Node &o) {
        return o.isRostrum() && (o.serial == target || QString::number(o.id) == target);
    });
}

// Naming the headphones or the default output is no choice: Java's OpenAL and some SDL builds
// pass the default device's name, and Rostrum's buses end up there anyway.
bool Engine::isPlainTarget(const QString &target) const
{
    const auto names = [&target](const pw::Node *n) {
        return n && (n->name == target || n->serial == target || QString::number(n->id) == target);
    };
    return isRostrumTarget(target) || names(resolveSink()) ||
           names(m_pw->graph().nodeByName(m_pw->defaultSinkName()));
}

const pw::Node *Engine::divertedNode(const pw::Node &stream, uint32_t expected, bool output) const
{
    const auto &g = m_pw->graph();
    const pw::Node *other = nullptr;
    for (const auto &l : g.links) {
        const uint32_t self = output ? l.outNode : l.inNode;
        const uint32_t peer = output ? l.inNode : l.outNode;
        if (self != stream.id) {
            continue;
        }
        if (peer == expected) {
            return nullptr;
        }
        if (!other) {
            other = g.node(peer);
        }
    }
    return other;
}

QString Engine::describeTarget(const QString &target) const
{
    const pw::Node *n = nodeNamed(target);
    return n ? n->label() : target;
}

const Engine::Recognised &Engine::recognise(const pw::Node &n, const StreamProps &props, const AppIdentity &id) const
{
    const QString signature =
        QStringList{props.appName, props.binary, QString::number(n.pid), n.prop("media.role"),
                    n.prop("application.icon-name"), n.prop("application.id"), n.prop("pipewire.access.portal.app_id"),
                    n.prop("target.object"), n.prop("node.target"), n.prop("node.dont-move"),
                    m_headphones, m_pw->defaultSinkName()}
            .join(QChar(u'\x1f'));
    if (auto it = m_recognised.constFind(n.id); it != m_recognised.cend() && it->signature == signature) {
        return *it;
    }
    Recognised r;
    r.signature = signature;
    r.facts = collectFacts(props, n.props, n.pid, m_desktop, [this](const QString &t) { return isPlainTarget(t); });
    r.detected = classify(r.facts);
    r.skipKey = (r.facts.steamAppId.isEmpty() ? id.key.toString() : QStringLiteral("steam:") + r.facts.steamAppId).toLower();
    qCInfo(lcEngine) << "recognised" << id.displayName << n.id << "as"
                     << (r.detected.excluded ? QStringLiteral("excluded") : categoryName(r.detected.category))
                     << "evidence" << int(r.detected.evidence);
    return *m_recognised.insert(n.id, r);
}

Engine::Placement Engine::place(const pw::Node &n, const StreamProps &props, const AppIdentity &id) const
{
    Placement p;
    if (auto it = m_sessionAssign.constFind(id.key.toString()); it != m_sessionAssign.cend()) {
        p.session = true;
        p.busId = it.value();
        return p;
    }
    if (const int idx = matchRule(m_scene.rules, props); idx >= 0) {
        const AppRule &r = m_scene.rules.at(idx);
        p.ruleKey = {r.key, r.match};
        p.busId = r.busId;
        return p;
    }
    if (!m_autoAssign) {
        return p;
    }
    const Recognised &r = recognise(n, props, id);
    if (r.detected.excluded || m_autoSkip.contains(r.skipKey)) {
        return p;
    }
    if (const Bus *bus = m_scene.busFor(r.detected.category)) {
        p.busId = bus->id;
        p.automatic = true;
    }
    return p;
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
        const Placement p = place(n, s.props, s.identity);
        s.busId = m_scene.bus(p.busId) ? p.busId : QString();
        s.ruleKey = p.ruleKey;
        s.sessionOnly = p.session;
        s.automatic = p.automatic && !s.busId.isEmpty();
        const Recognised &r = recognise(n, s.props, s.identity);
        s.detected = r.detected;
        s.detectedName = r.facts.steamGameName;
        s.skipped = m_autoSkip.contains(r.skipKey);
        s.iconNames = iconCandidates(r.facts);
        if (s.identity.unnamed && !s.detectedName.isEmpty()) {
            s.identity.displayName = s.detectedName;
            s.identity.unnamed = false;
        }
        if (const Bus *bus = m_scene.bus(s.busId); bus && !bus->isInput()) {
            const pw::Node *busNode = m_pw->graph().nodeByName(bus->nodeName());
            const QString current = m_pw->metadataValue(n.id, QStringLiteral("target.object"));
            // Rostrum's own move shows in the metadata before the links follow, so a stream
            // being moved onto its bus is not reported.
            if (busNode && current != busNode->serial) {
                if (const pw::Node *other = divertedNode(n, busNode->id, true)) {
                    s.divertedTo = other->label();
                }
            }
        }
        s.volume = s.ruleKey.isValid() ? appVolume(s.ruleKey) : appVolume(s.identity.key);
        s.muted = s.ruleKey.isValid() ? appMuted(s.ruleKey) : appMuted(s.identity.key);
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

QStringList Engine::ruleIconCandidates(const AppRule &rule) const
{
    QStringList out;
    const QString match = rule.key == MatchKey::Binary ? cleanBinary(rule.match) : rule.match;
    if (const auto entry = m_desktop.find({match, rule.label})) {
        out << entry->icon << entry->id;
    }
    out << match.toLower();
    out.removeAll(QString());
    out.removeDuplicates();
    return out;
}

void Engine::assignApp(const AppKey &key, const QString &busId, bool always)
{
    const Bus *bus = m_scene.bus(busId);
    if (!key.isValid() || !bus || bus->isInput()) {
        return;
    }
    skipAuto(key, false);
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
            rule.muted = m_sessionMuted.take(key.toString());
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
    // Taking an app off its bus must not let the automatic tier put it straight back.
    skipAuto(key, true);
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
    skipAuto(id.key, true);
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

void Engine::reclaimStream(uint32_t nodeId)
{
    if (auto it = m_routed.find(nodeId); it != m_routed.end()) {
        it->requested.clear();
    }
    scheduleReconcile();
}

void Engine::removeRule(const AppKey &key)
{
    // Deleting a rule is tidying up, not "keep it off": the app may fall back to its automatic bus.
    const auto before = m_scene.rules.size();
    m_scene.rules.removeIf([&](const AppRule &r) { return AppKey{r.key, r.match} == key; });
    if (before != m_scene.rules.size()) {
        Q_EMIT structureChanged();
        Q_EMIT sceneChanged();
    }
    Q_EMIT appsChanged();
    scheduleReconcile();
}

void Engine::skipAuto(const AppKey &key, bool skip)
{
    bool changed = false;
    if (!skip) {
        changed = m_autoSkip.remove(key.toString().toLower());
    }
    for (const auto &n : m_pw->graph().nodes) {
        if (!n.isPlaybackStream() || isOwnStream(n)) {
            continue;
        }
        const auto props = propsOf(n);
        const auto id = identify(props);
        const int idx = matchRule(m_scene.rules, props);
        const bool same = id.key == key || (idx >= 0 && AppKey{m_scene.rules.at(idx).key, m_scene.rules.at(idx).match} == key);
        if (!same) {
            continue;
        }
        const Recognised &r = recognise(n, props, id);
        if (skip && (r.detected.excluded || r.detected.category == AppCategory::None)) {
            continue;
        }
        if (!skip) {
            changed |= m_autoSkip.remove(r.skipKey);
        } else if (!m_autoSkip.contains(r.skipKey)) {
            m_autoSkip.insert(r.skipKey);
            changed = true;
        }
    }
    if (changed) {
        Q_EMIT autoSkipChanged();
    }
}

void Engine::setAutoAssign(bool on)
{
    if (m_autoAssign == on) {
        return;
    }
    m_autoAssign = on;
    Q_EMIT appsChanged();
    scheduleReconcile();
}

void Engine::setAutoSkip(const QStringList &keys)
{
    QSet<QString> next;
    for (const auto &k : keys) {
        next.insert(k.toLower());
    }
    if (next == m_autoSkip) {
        return;
    }
    m_autoSkip = next;
    Q_EMIT appsChanged();
    scheduleReconcile();
}

QStringList Engine::autoSkip() const
{
    QStringList out(m_autoSkip.cbegin(), m_autoSkip.cend());
    out.sort();
    return out;
}

void Engine::forgetAutoSkip()
{
    if (m_autoSkip.isEmpty()) {
        return;
    }
    m_autoSkip.clear();
    Q_EMIT autoSkipChanged();
    Q_EMIT appsChanged();
    scheduleReconcile();
}

void Engine::setBusAutoCategory(const QString &busId, AppCategory category)
{
    Bus *target = m_scene.bus(busId);
    if (!target || target->isInput() || target->autoCategory == category) {
        return;
    }
    for (auto &b : m_scene.buses) {
        if (category != AppCategory::None && b.autoCategory == category) {
            b.autoCategory = AppCategory::None;
        }
    }
    target->autoCategory = category;
    Q_EMIT structureChanged();
    Q_EMIT sceneChanged();
    Q_EMIT appsChanged();
    scheduleReconcile();
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

void Engine::setAppMuted(const AppKey &key, bool muted)
{
    if (AppRule *r = m_scene.rule(key.key, key.match)) {
        if (r->muted == muted) {
            return;
        }
        r->muted = muted;
        Q_EMIT sceneChanged();
    } else if (muted) {
        m_sessionMuted.insert(key.toString(), true);
    } else {
        m_sessionMuted.remove(key.toString());
    }
    Q_EMIT appsChanged();
    scheduleReconcile();
}

bool Engine::appMuted(const AppKey &key) const
{
    if (const AppRule *r = m_scene.rule(key.key, key.match)) {
        return r->muted;
    }
    return m_sessionMuted.value(key.toString(), false);
}

bool Engine::releaseAppMutes()
{
    bool sent = false;
    for (auto it = m_appliedStreamMute.cbegin(); it != m_appliedStreamMute.cend(); ++it) {
        if (it.value() && m_pw->graph().node(it.key())) {
            m_pw->setNodeMute(it.key(), false);
            sent = true;
        }
    }
    m_appliedStreamMute.clear();
    return sent;
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
        const Placement placement = place(n, props, id);
        const QString &busId = placement.busId;
        const AppKey &ruleKey = placement.ruleKey;

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
            auto routed = m_routed.find(n.id);
            if (current != busNode->serial &&
                (routed == m_routed.end() || routed->requested != busNode->serial)) {
                if (routed == m_routed.end()) {
                    const bool currentIsOurs =
                        std::any_of(graph.nodes.cbegin(), graph.nodes.cend(),
                                    [&](const pw::Node &o) { return o.serial == current && isOwnedNode(o); });
                    routed = m_routed.insert(n.id, {busId, currentIsOurs ? QString() : current, QString()});
                }
                routed->busId = busId;
                routed->requested = busNode->serial;
                routed->since.start();
                routed->reclaims = 0;
                qCInfo(lcEngine) << "route" << id.displayName << n.id << "->" << bus->nodeName()
                                 << (placement.automatic ? "(automatic)" : "");
                m_pw->setMetadata(n.id, QStringLiteral("target.object"), QStringLiteral("Spa:Id"), busNode->serial);
            } else if (current != busNode->serial && !current.isEmpty() && routed != m_routed.end() &&
                       routed->since.isValid() && routed->since.elapsed() < kReclaimWindowMs &&
                       routed->reclaims < kMaxReclaims) {
                ++routed->reclaims;
                qCInfo(lcEngine) << "reclaim" << id.displayName << n.id << "from" << describeTarget(current)
                                 << "->" << bus->nodeName() << "(moved by another program as it started)";
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
        // Per-app mute, the same way: a stream is only touched once the user has muted the app.
        const bool muted = ruleKey.isValid() ? appMuted(ruleKey) : appMuted(id.key);
        const auto appliedMute = m_appliedStreamMute.constFind(n.id);
        if ((appliedMute == m_appliedStreamMute.cend() && muted) ||
            (appliedMute != m_appliedStreamMute.cend() && appliedMute.value() != muted)) {
            qCInfo(lcEngine) << (muted ? "mute" : "unmute") << id.displayName << n.id;
            m_pw->setNodeMute(n.id, muted);
            m_appliedStreamMute.insert(n.id, muted);
        }
    }

    for (auto it = m_routed.begin(); it != m_routed.end();) {
        it = present.contains(it.key()) ? std::next(it) : m_routed.erase(it);
    }
    for (auto it = m_appliedStreamVolume.begin(); it != m_appliedStreamVolume.end();) {
        it = present.contains(it.key()) ? std::next(it) : m_appliedStreamVolume.erase(it);
    }
    for (auto it = m_appliedStreamMute.begin(); it != m_appliedStreamMute.end();) {
        it = present.contains(it.key()) ? std::next(it) : m_appliedStreamMute.erase(it);
    }
    for (auto it = m_recognised.begin(); it != m_recognised.end();) {
        it = present.contains(it.key()) ? std::next(it) : m_recognised.erase(it);
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
