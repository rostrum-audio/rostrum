#include "engine/Engine.h"

#include "core/AppFacts.h"
#include "pw/PwContext.h"

#include <QLoggingCategory>
#include <QVersionNumber>

#include <algorithm>
#include <cmath>

Q_DECLARE_LOGGING_CATEGORY(lcEngine)

namespace rostrum::engine {

namespace {
constexpr qint64 kFxCreateRetryMs = 3000;
constexpr qint64 kFxLoadTimeoutMs = 4000;
constexpr int kFxRecheckMs = 250;
constexpr qint64 kFxCrashWindowMs = 10000;
constexpr int kFxCrashStrikes = 2;
constexpr qint64 kFxControlResendMs = 1000;

// A new audio.convert node starts in convert mode with one channel-less port per side; only after
// PortConfig does it expose the mono DSP ports the filter graph runs on.
bool hasDspPorts(const pw::Graph &g, uint32_t node)
{
    const auto dsp = [](const QList<pw::Port> &ports) {
        return !ports.isEmpty()
            && std::all_of(ports.cbegin(), ports.cend(), [](const pw::Port &p) { return !p.channel.isEmpty(); });
    };
    return dsp(g.inputPorts(node)) && dsp(g.outputPorts(node));
}

bool isTrue(const QString &v)
{
    return v == QLatin1String("true") || v == QLatin1String("1");
}

// OBS records the mic itself and has its own filters; Rostrum's OBS plan decides what it hears.
bool isObs(const pw::Node &n, const pw::Graph &g)
{
    if (n.appName == QLatin1String("OBS") || n.binary == QLatin1String("obs") || n.name.startsWith(QLatin1String("OBS"))) {
        return true;
    }
    const auto client = g.clients.constFind(n.clientId.toUInt());
    return client != g.clients.cend() && client->appName == QLatin1String("OBS");
}

const char *stateName(Engine::MicFxState s)
{
    switch (s) {
    case Engine::MicFxState::Off: return "off";
    case Engine::MicFxState::Starting: return "starting";
    case Engine::MicFxState::Active: return "active";
    case Engine::MicFxState::Failed: return "failed";
    }
    return "?";
}
} // namespace

// ---- settings ---------------------------------------------------------------------------

void Engine::setMicFilters(const micfx::Settings &settings)
{
    const micfx::Settings s = micfx::sanitize(settings);
    if (s == m_micFx) {
        return;
    }
    if (s.enabled != m_micFx.enabled) {
        qCInfo(lcEngine) << "mic filters" << s.enabled;
        m_fxFailed = false;
        m_fxStrikes = 0;
        m_fxError.clear();
    }
    m_micFx = s;
    Q_EMIT micFiltersChanged();
    Q_EMIT appsChanged();
    scheduleReconcile();
}

void Engine::setMicFilterPlugin(const QString &path, bool denoise, const QString &error)
{
    if (path == m_fxPlugin && denoise == m_fxDenoise && error == m_fxPluginError) {
        return;
    }
    qCInfo(lcEngine) << "mic filter plugin" << (path.isEmpty() ? error : path) << "denoise" << denoise;
    m_fxPlugin = path;
    m_fxDenoise = denoise && !path.isEmpty();
    m_fxPluginError = error;
    m_fxFailed = false;
    m_fxError.clear();
    Q_EMIT micFiltersStateChanged();
    scheduleReconcile();
}

QString Engine::micFiltersUnavailable() const
{
    if (m_fxPlugin.isEmpty()) {
        return m_fxPluginError.isEmpty() ? QStringLiteral("This build of Rostrum has no mic filters.") : m_fxPluginError;
    }
    if (m_pw->state() != pw::PwContext::State::Ready) {
        return {};
    }
    const QVersionNumber version = QVersionNumber::fromString(m_pw->serverVersion());
    if (!version.isNull() && version < QVersionNumber(1, 4, 0)) {
        return QStringLiteral("Mic filters need PipeWire 1.4 or newer. This system runs PipeWire %1.")
            .arg(m_pw->serverVersion());
    }
    if (!m_pw->graph().hasFactory(QStringLiteral("spa-node-factory"))) {
        return QStringLiteral("Mic filters need PipeWire's spa-node-factory module, which is not loaded.");
    }
    return {};
}

Engine::MicFxState Engine::micFiltersState() const
{
    return m_fxState;
}

void Engine::setMicFxState(MicFxState state, const QString &error)
{
    if (state == m_fxState && error == m_fxError) {
        return;
    }
    if (state != m_fxState) {
        qCInfo(lcEngine) << "mic filters" << stateName(state) << error;
    }
    m_fxState = state;
    m_fxError = error;
    Q_EMIT micFiltersStateChanged();
}

// ---- nodes ------------------------------------------------------------------------------

bool Engine::micFxWanted() const
{
    return m_mixEnabled && m_micFx.enabled && !m_fxFailed && m_pw->state() == pw::PwContext::State::Ready &&
           micFiltersUnavailable().isEmpty();
}

const pw::Node *Engine::micFxNode() const
{
    const auto &g = m_pw->graph();
    const pw::Node *n = g.nodeByName(QString::fromLatin1(kMicFxNode));
    if (!n || !isOwnedNode(*n) || m_pendingDestroy.contains(n->id)) {
        return nullptr;
    }
    return hasDspPorts(g, n->id) ? n : nullptr;
}

const pw::Node *Engine::filteredNode() const
{
    if (!micFxNode()) {
        return nullptr;
    }
    const pw::Node *n = m_pw->graph().nodeByName(QString::fromLatin1(kFilteredNode));
    return n && isOwnedNode(*n) && !m_pendingDestroy.contains(n->id) ? n : nullptr;
}

// Once this run sent its graph. A node lingering from the last run is reloaded in the same pass
// that links are made, so a restart never switches to the raw mic and back.
bool Engine::micFxRunning() const
{
    const pw::Node *fx = micFxNode();
    return micFxWanted() && fx && filteredNode() && m_fxGraphNode == fx->id;
}

const pw::Node *Engine::appsMic() const
{
    return resolveDevice(m_pw->graph(), m_micDevice, m_pw->defaultSourceName(), false, true);
}

QString Engine::micMeterNode() const
{
    if (micFxRunning() && resolveSource()) {
        return QString::fromLatin1(kFilteredNode);
    }
    return resolvedSourceName();
}

void Engine::reconcileMicFx()
{
    const auto &g = m_pw->graph();
    const QString fxName = QString::fromLatin1(kMicFxNode);
    const QString filteredName = QString::fromLatin1(kFilteredNode);

    QList<const pw::Node *> fxNodes;
    QList<const pw::Node *> filteredNodes;
    for (const auto &n : g.nodes) {
        if (!isOwnedNode(n) || m_pendingDestroy.contains(n.id)) {
            continue;
        }
        if (n.name == fxName) {
            // Creation properties cannot be changed by reloading the LADSPA graph.
            // Upgrade a persistent pre-merge converter, keeping the public virtual
            // microphones intact. factory.name is available in full node info;
            // don't judge an incomplete initial registry observation.
            if (micFxWanted() && n.props.contains(QStringLiteral("factory.name")) &&
                n.prop("convert.direction") != QLatin1String("output")) {
                qCInfo(lcEngine) << "replacing legacy mic filter converter" << n.id;
                destroyOnce(n.id);
                continue;
            }
            fxNodes.append(&n);
        } else if (n.name == filteredName) {
            filteredNodes.append(&n);
        }
    }
    const auto byId = [](const pw::Node *a, const pw::Node *b) { return a->id < b->id; };
    std::sort(fxNodes.begin(), fxNodes.end(), byId);
    std::sort(filteredNodes.begin(), filteredNodes.end(), byId);

    if (!micFxWanted()) {
        // Nodes left by an earlier run stay while the mix is off: nothing else is linked either.
        if (m_mixEnabled) {
            for (const pw::Node *n : std::as_const(fxNodes) + filteredNodes) {
                qCInfo(lcEngine) << "removing" << n->name;
                destroyOnce(n->id);
            }
        }
        m_pendingCreate.remove(fxName);
        m_pendingCreate.remove(filteredName);
        m_fxConfigured = 0;
        m_fxGraphNode = 0;
        m_fxGraphControls.clear();
        m_fxMissingClock.invalidate();
        m_fxVerified = false;
        m_fxSent.clear();
        m_fxTimer.stop();
        if (m_fxFailed) {
            setMicFxState(MicFxState::Failed, m_fxError);
        } else if (m_micFx.enabled && m_mixEnabled) {
            setMicFxState(MicFxState::Off, micFiltersUnavailable());
        } else {
            setMicFxState(MicFxState::Off, m_micFx.enabled ? QString() : m_fxError);
        }
        return;
    }

    // Duplicates from a create race: keep the oldest.
    for (qsizetype i = 1; i < fxNodes.size(); ++i) {
        qCInfo(lcEngine) << "removing duplicate" << fxName << fxNodes.at(i)->id;
        destroyOnce(fxNodes.at(i)->id);
    }
    for (qsizetype i = 1; i < filteredNodes.size(); ++i) {
        qCInfo(lcEngine) << "removing duplicate" << filteredName << filteredNodes.at(i)->id;
        destroyOnce(filteredNodes.at(i)->id);
    }

    const auto create = [this](const QString &name, auto &&send) {
        auto it = m_pendingCreate.find(name);
        if (it != m_pendingCreate.end() && it->elapsed() < kFxCreateRetryMs) {
            return;
        }
        qCInfo(lcEngine) << "creating" << name;
        send();
        QElapsedTimer t;
        t.start();
        m_pendingCreate.insert(name, t);
    };
    if (fxNodes.isEmpty()) {
        create(fxName, [this] { m_pw->createSpaNode(micFxProperties()); });
    } else {
        m_pendingCreate.remove(fxName);
    }
    if (filteredNodes.isEmpty()) {
        create(filteredName, [this] { m_pw->createNullNode(filteredMicSpec().properties()); });
    } else {
        m_pendingCreate.remove(filteredName);
    }
    if (fxNodes.isEmpty()) {
        setMicFxState(MicFxState::Starting);
        return;
    }

    const pw::Node &fx = *fxNodes.first();
    if (!hasDspPorts(g, fx.id)) {
        if (m_fxConfigured != fx.id) {
            m_pw->setPortConfig(fx.id, false, 1);
            m_pw->setPortConfig(fx.id, true, 1);
            m_fxConfigured = fx.id;
            m_fxGraphClock.start();
        } else if (m_fxGraphClock.isValid() && m_fxGraphClock.elapsed() > kFxLoadTimeoutMs) {
            qCWarning(lcEngine) << "mic filter node" << fx.id << "has no ports";
            m_fxFailed = true;
            setMicFxState(MicFxState::Failed, QStringLiteral("PipeWire did not set up the mic filter node. "
                                                             "Your mic is used without filters."));
            scheduleReconcile();
            return;
        }
        m_fxTimer.start(kFxRecheckMs);
        setMicFxState(MicFxState::Starting);
        return;
    }

    // The graph is sent once per session per node, so a node lingering from an earlier run picks
    // up this run's plugin. Afterwards only the controls change, without a glitch.
    // audioconvert builds the graph, and reports its controls, only while the node runs; a mic
    // nobody records leaves it idle. Audio passes unfiltered if the graph cannot load, so the path
    // is used at once and judged only once audio flows.
    const QString layout = micfx::graphJson(micfx::Settings{}, m_fxPlugin, m_fxDenoise);
    const QList<micfx::Control> controls = micfx::controls(m_micFx, m_fxDenoise);
    const bool loaded = micfx::graphLoaded(fx.params.keys());
    if (!fx.running || loaded) {
        m_fxMissingClock.invalidate();
    } else if (!m_fxMissingClock.isValid()) {
        m_fxMissingClock.start();
    }
    // Controls cannot be set on a graph that is not built yet; an idle node takes a fresh graph.
    const bool idleChange = m_fxGraphNode == fx.id && !loaded && !fx.running && controls != m_fxGraphControls;
    if (m_fxGraphNode != fx.id || m_fxGraphJson != layout || idleChange) {
        if (!idleChange) {
            qCInfo(lcEngine) << "loading mic filter graph into" << fx.id << m_fxPlugin;
        }
        m_pw->setNodeParams(fx.id, {{QString::fromLatin1(micfx::kGraphKey),
                                     micfx::graphJson(m_micFx, m_fxPlugin, m_fxDenoise)}});
        m_fxGraphNode = fx.id;
        m_fxGraphJson = layout;
        m_fxGraphControls = controls;
        m_fxGraphClock.start();
        m_fxVerified = false;
        m_fxSent.clear();
        // The graph carries the current values, so they count as sent.
        for (const auto &c : controls) {
            SentControl &sent = m_fxSent[c.key];
            sent.value = c.value;
            sent.when.start();
        }
        if (fx.running) {
            m_fxTimer.start(kFxRecheckMs);
        }
        setMicFxState(MicFxState::Active);
        return;
    }
    if (loaded) {
        if (!m_fxVerified) {
            qCInfo(lcEngine) << "mic filter graph running in" << fx.id;
        }
        m_fxVerified = true;
        m_fxTimer.stop();
        applyMicFxControls(fx);
        setMicFxState(MicFxState::Active);
        return;
    }
    if (fx.running) {
        const qint64 since = std::min(m_fxGraphClock.elapsed(), m_fxMissingClock.elapsed());
        if (since > kFxLoadTimeoutMs && m_fxVerified) {
            // Something replaced or dropped the graph after it ran: send it again, keeping the
            // links and app moves in place.
            qCInfo(lcEngine) << "mic filter graph gone from" << fx.id;
            m_fxGraphJson.clear();
            scheduleReconcile();
            return;
        }
        if (since > kFxLoadTimeoutMs) {
            qCWarning(lcEngine) << "mic filter graph did not load into" << fx.id << m_fxPlugin;
            m_fxFailed = true;
            setMicFxState(MicFxState::Failed, QStringLiteral("PipeWire could not load the mic filters. "
                                                             "Your mic is used without filters."));
            scheduleReconcile();
            return;
        }
        // PipeWire caches the Props it reports and audioconvert only refreshes them when a control
        // is set, not when the graph starts with the node. Setting the controls brings a running
        // graph into view; without a graph audioconvert ignores them.
        applyMicFxControls(fx);
        m_fxTimer.start(kFxRecheckMs);
    }
    setMicFxState(MicFxState::Active);
}

// Sends the controls that differ from what the node reports. Another tool changing them is
// overruled at most once per second, like volumes.
void Engine::applyMicFxControls(const pw::Node &fx)
{
    QList<QPair<QString, QVariant>> send;
    for (const auto &c : micfx::controls(m_micFx, m_fxDenoise)) {
        const QVariant reported = fx.params.value(c.key);
        if (reported.isValid() && std::abs(reported.toDouble() - double(c.value)) < 1e-4) {
            continue;
        }
        SentControl &sent = m_fxSent[c.key];
        if (sent.when.isValid() && sent.value == c.value && sent.when.elapsed() < kFxControlResendMs) {
            continue;
        }
        sent.value = c.value;
        sent.when.start();
        send.append({c.key, double(c.value)});
    }
    if (!send.isEmpty()) {
        m_pw->setNodeParams(fx.id, send);
    }
}

// PipeWire going away right after a graph load, twice, points at the graph crashing the daemon.
// Turning the filters off keeps the user's audio working; they can switch them on again.
void Engine::micFxConnectionChanged()
{
    const auto state = m_pw->state();
    if (state == pw::PwContext::State::Failed && m_fxGraphNode != 0 && m_fxGraphClock.isValid()) {
        if (m_fxGraphClock.elapsed() < kFxCrashWindowMs) {
            ++m_fxStrikes;
            qCWarning(lcEngine) << "PipeWire went away" << m_fxGraphClock.elapsed()
                                << "ms after the mic filters loaded, strike" << m_fxStrikes;
            if (m_fxStrikes >= kFxCrashStrikes && m_micFx.enabled) {
                const QString reason = QStringLiteral("PipeWire stopped twice right after the mic filters started, "
                                                      "so Rostrum turned them off.");
                m_micFx.enabled = false;
                m_fxStrikes = 0;
                m_fxError = reason;
                Q_EMIT micFiltersChanged();
                Q_EMIT micFiltersTripped(reason);
            }
        } else {
            m_fxStrikes = 0;
        }
    }
    if (state != pw::PwContext::State::Ready) {
        m_fxConfigured = 0;
        m_fxGraphNode = 0;
        m_fxGraphClock.invalidate();
        m_fxGraphControls.clear();
        m_fxMissingClock.invalidate();
        m_fxVerified = false;
        m_fxSent.clear();
        m_fxTimer.stop();
        m_micRouted.clear();
        m_micRecognised.clear();
        m_micFirstSeen.clear();
        setMicFxState(m_micFx.enabled && !m_fxFailed ? MicFxState::Starting : MicFxState::Off, m_fxError);
    }
}

// ---- apps -------------------------------------------------------------------------------

bool Engine::linked(uint32_t outNode, uint32_t inNode) const
{
    const auto &links = m_pw->graph().links;
    return std::any_of(links.cbegin(), links.cend(),
                       [&](const pw::Link &l) { return l.outNode == outNode && l.inNode == inNode; });
}

bool Engine::isMicCapture(const pw::Node &n) const
{
    if (!n.isCaptureStream() || isOwnStream(n)) {
        return false;
    }
    if (isTrue(n.prop("stream.monitor")) || isTrue(n.prop("node.dont-move")) ||
        isTrue(n.prop("stream.capture.sink"))) {
        return false;
    }
    return !isObs(n, m_pw->graph());
}

const Engine::MicRecognised &Engine::recogniseCapture(const pw::Node &n) const
{
    const StreamProps props = propsOf(n);
    const QString signature =
        QStringList{props.appName, props.binary, QString::number(n.pid), n.prop("media.role"),
                    n.prop("application.icon-name"), n.prop("application.id"), n.prop("pipewire.access.portal.app_id")}
            .join(QChar(u'\x1f'));
    if (auto it = m_micRecognised.constFind(n.id); it != m_micRecognised.cend() && it->signature == signature) {
        return *it;
    }
    MicRecognised r;
    r.signature = signature;
    r.identity = identify(props);
    // Where it records from is no reason to leave it unfiltered, unlike where a player plays to.
    const AppFacts facts = collectFacts(props, n.props, n.pid, m_desktop, [](const QString &) { return true; });
    r.excluded = classify(facts).excluded;
    r.iconNames = iconCandidates(facts);
    if (r.identity.unnamed && !facts.steamGameName.isEmpty()) {
        r.identity.displayName = facts.steamGameName;
        r.identity.unnamed = false;
    }
    return *m_micRecognised.insert(n.id, r);
}

void Engine::reconcileMicRoutes()
{
    const auto &g = m_pw->graph();
    const pw::Node *fx = micFxNode();
    const pw::Node *filtered = micFxRunning() ? filteredNode() : nullptr;
    const pw::Node *anyFiltered = g.nodeByName(QString::fromLatin1(kFilteredNode));
    const pw::Node *mic = appsMic();
    // Moving apps before the filtered mic carries sound would cut them off for a moment.
    if (filtered && (!mic || !linked(mic->id, fx->id) || !linked(fx->id, filtered->id) || filtered->serial.isEmpty())) {
        filtered = nullptr;
    }
    const QString targetKey = QStringLiteral("target.object");
    const auto names = [](const pw::Node *n, const QString &target) {
        return n && (n->name == target || n->serial == target || QString::number(n->id) == target);
    };

    QSet<uint32_t> present;
    for (const auto &n : g.nodes) {
        if (!isMicCapture(n)) {
            continue;
        }
        present.insert(n.id);
        QElapsedTimer &firstSeen = m_micFirstSeen[n.id];
        if (!firstSeen.isValid()) {
            firstSeen.start();
        }
        const MicRecognised &r = recogniseCapture(n);
        const micfx::AppChoice choice = micfx::appChoice(m_micFx, r.identity.key.toString());
        const bool want = filtered && micfx::useFiltered(m_micFx, choice, r.excluded);
        const QString current = m_pw->metadataValue(n.id, targetKey);
        auto routed = m_micRouted.find(n.id);

        if (want) {
            if (routed != m_micRouted.end()) {
                // Re-sent after the filtered mic is recreated; left alone if the user moved it.
                if (current != filtered->serial && routed->requested != filtered->serial &&
                    (current == routed->requested || current.isEmpty())) {
                    routed->requested = filtered->serial;
                    routed->since.start();
                    routed->reclaims = 0;
                    m_pw->setMetadata(n.id, targetKey, QStringLiteral("Spa:Id"), filtered->serial);
                } else if (current != filtered->serial && !current.isEmpty() && routed->requested == filtered->serial &&
                           routed->since.isValid() && routed->since.elapsed() < kReclaimWindowMs &&
                           routed->reclaims < kMaxReclaims) {
                    ++routed->reclaims;
                    qCInfo(lcEngine) << "reclaim" << r.identity.displayName << n.id << "from" << describeTarget(current)
                                     << "-> filtered mic (moved by another program as it started)";
                    m_pw->setMetadata(n.id, targetKey, QStringLiteral("Spa:Id"), filtered->serial);
                }
                continue;
            }
            if (current == filtered->serial) {
                // Moved by an earlier run that quit or crashed.
                m_micRouted.insert(n.id, {QString(), filtered->serial, {}, 0});
                continue;
            }
            // Only streams recording the mic apps use; a stream that picked another source keeps it.
            // Easy Effects moves every new recording stream to its own source, which is not a choice.
            const bool takenByEffects = isEffectsNode(nodeNamed(current)) && firstSeen.elapsed() < kReclaimWindowMs;
            if (!takenByEffects && (!linked(mic->id, n.id) || (!current.isEmpty() && !names(mic, current)))) {
                continue;
            }
            qCInfo(lcEngine) << "filter mic for" << r.identity.displayName << n.id;
            MicRouted moved{current, filtered->serial, {}, 0};
            moved.since.start();
            m_micRouted.insert(n.id, moved);
            m_pw->setMetadata(n.id, targetKey, QStringLiteral("Spa:Id"), filtered->serial);
        } else if (routed != m_micRouted.end()) {
            const MicRouted was = m_micRouted.take(n.id);
            if (!current.isEmpty() && current != was.requested) {
                continue; // moved by the user since
            }
            qCInfo(lcEngine) << "raw mic for" << r.identity.displayName << n.id;
            if (was.previousTarget.isEmpty()) {
                m_pw->clearMetadata(n.id, targetKey);
                m_pw->clearMetadata(n.id, QStringLiteral("target.node"));
            } else {
                m_pw->setMetadata(n.id, targetKey, QStringLiteral("Spa:Id"), was.previousTarget);
            }
        } else if (!micfx::useFiltered(m_micFx, choice, r.excluded) && names(anyFiltered, current)) {
            // WirePlumber remembers where a stream was moved and puts the app back there when it
            // next starts. An app set to raw since goes back to the mic, and clearing the target
            // makes WirePlumber forget it. An app that picked the filtered mic itself sets
            // target.object in its own properties instead, which WirePlumber leaves alone.
            qCInfo(lcEngine) << "raw mic for" << r.identity.displayName << n.id << "(remembered by WirePlumber)";
            m_pw->clearMetadata(n.id, targetKey);
            m_pw->clearMetadata(n.id, QStringLiteral("target.node"));
        }
    }
    for (auto it = m_micRouted.begin(); it != m_micRouted.end();) {
        it = present.contains(it.key()) ? std::next(it) : m_micRouted.erase(it);
    }
    for (auto it = m_micRecognised.begin(); it != m_micRecognised.end();) {
        it = present.contains(it.key()) ? std::next(it) : m_micRecognised.erase(it);
    }
    for (auto it = m_micFirstSeen.begin(); it != m_micFirstSeen.end();) {
        it = present.contains(it.key()) ? std::next(it) : m_micFirstSeen.erase(it);
    }
}

const pw::Node *Engine::nodeNamed(const QString &target) const
{
    if (target.isEmpty()) {
        return nullptr;
    }
    for (const auto &n : m_pw->graph().nodes) {
        if (n.serial == target || n.name == target) {
            return &n;
        }
    }
    return nullptr;
}

bool Engine::isEffectsNode(const pw::Node *n)
{
    return n && (n->prop("application.id") == QLatin1String("com.github.wwmm.easyeffects") ||
                 n->name == QLatin1String("easyeffects_sink") || n->name == QLatin1String("easyeffects_source"));
}

QList<MicApp> Engine::micApps() const
{
    const auto &g = m_pw->graph();
    const pw::Node *mic = appsMic();
    const pw::Node *filtered = g.nodeByName(QString::fromLatin1(kFilteredNode));
    QHash<QString, MicApp> byKey;
    for (const auto &n : g.nodes) {
        if (!isMicCapture(n)) {
            continue;
        }
        const bool fromMic = mic && linked(mic->id, n.id);
        const bool linkedFiltered = filtered && linked(filtered->id, n.id);
        // Before its links appear, a stream Rostrum just moved counts as filtered.
        const auto routed = m_micRouted.constFind(n.id);
        const bool movedHere = routed != m_micRouted.cend() &&
                               m_pw->metadataValue(n.id, QStringLiteral("target.object")) == routed->requested;
        const bool fromFiltered = linkedFiltered || movedHere;
        const pw::Node *effects = nullptr;
        if (!fromFiltered && !fromMic) {
            // Shown only when Easy Effects took it from the mic, not for streams recording other things.
            effects = divertedNode(n, 0, false);
            if (!isEffectsNode(effects)) {
                continue;
            }
        }
        const MicRecognised &r = recogniseCapture(n);
        const QString key = r.identity.key.toString();
        auto it = byKey.find(key);
        if (it == byKey.end()) {
            MicApp a;
            a.nodeId = n.id;
            a.identity = r.identity;
            a.iconNames = r.iconNames;
            a.choice = micfx::appChoice(m_micFx, key);
            a.excludedByDefault = r.excluded;
            a.filtered = fromFiltered;
            a.recordsFrom = effects ? effects->label() : QString();
            byKey.insert(key, a);
        } else {
            it->filtered = it->filtered || fromFiltered;
            if (effects && it->recordsFrom.isEmpty() && !it->filtered) {
                it->recordsFrom = effects->label();
            }
        }
    }
    QList<MicApp> out = byKey.values();
    std::sort(out.begin(), out.end(), [](const MicApp &a, const MicApp &b) {
        const int c = a.identity.displayName.compare(b.identity.displayName, Qt::CaseInsensitive);
        return c != 0 ? c < 0 : a.nodeId < b.nodeId;
    });
    return out;
}

void Engine::setMicAppChoice(const AppKey &key, micfx::AppChoice choice)
{
    const micfx::Settings s = micfx::setAppChoice(m_micFx, key.toString(), choice);
    if (s == m_micFx) {
        return;
    }
    qCInfo(lcEngine) << "mic filter choice" << key.toString() << int(choice);
    m_micFx = s;
    Q_EMIT micFiltersChanged();
    Q_EMIT appsChanged();
    scheduleReconcile();
}

} // namespace rostrum::engine
