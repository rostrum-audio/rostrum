#include "engine/Engine.h"

#include "core/AppFacts.h"
#include "engine/MixPlan.h"
#include "pw/PwContext.h"

#include <QLoggingCategory>

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
// How long removing the filter nodes waits for apps to move back to the mic.
constexpr qint64 kFxReleaseMs = 3000;

bool isTrue(const QString &v)
{
    return v == QLatin1String("true") || v == QLatin1String("1");
}

// A metadata target.object names this node by its name, its object.serial, or its registry id.
bool namesNode(const pw::Node &n, const QString &target)
{
    return !target.isEmpty() &&
           (n.name == target || n.serial == target || QString::number(n.id) == target);
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
        return m_fxPluginError.isEmpty() ? QStringLiteral("This build of Rostrum has no Studio Voice.") : m_fxPluginError;
    }
    if (m_pw->state() != pw::PwContext::State::Ready) {
        return {};
    }
    if (!m_pw->graph().hasFactory(QStringLiteral("spa-node-factory"))) {
        return QStringLiteral("Studio Voice needs PipeWire's spa-node-factory module, which is not loaded.");
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

// Filters are off but apps are still recording the filtered mic: the processed path must stay
// linked until they have moved to the raw mic (or kFxReleaseMs has passed), so reconcileLinks
// agrees with reconcileMicFx about when the release is complete. An unstarted clock counts as
// holding; reconcileMicFx starts it earlier in the same pass.
bool Engine::micFxReleaseHeld() const
{
    if (!m_mixEnabled || micFxWanted()) {
        return false;
    }
    const pw::Node *fx = micFxNode();
    const pw::Node *filtered = filteredNode();
    if (!fx || !filtered || !micStreamsOnFilter({fx, filtered})) {
        return false;
    }
    return !m_fxReleaseClock.isValid() || m_fxReleaseClock.elapsed() < kFxReleaseMs;
}

const pw::Node *Engine::appsMic() const
{
    return resolveDevice(m_pw->graph(), m_micDevice, m_pw->defaultSourceName(), false, true,
                         m_micBtAddress);
}

QString Engine::micMeterNode() const
{
    if (micFxRunning() && resolveSource()) {
        return QString::fromLatin1(kFilteredNode);
    }
    return resolvedSourceName();
}

uint32_t Engine::micFxForceQuantum() const
{
    // Without noise removal the chain costs well under a tenth of a millisecond.
    if (!m_fxDenoise) {
        return 0;
    }
    return micFxQuantum(m_pw->settingValue(QStringLiteral("clock.min-quantum")),
                        m_pw->settingValue(QStringLiteral("clock.max-quantum")));
}

void Engine::reconcileMicFx()
{
    const auto &g = m_pw->graph();
    const QString fxName = QString::fromLatin1(kMicFxNode);
    const QString filteredName = QString::fromLatin1(kFilteredNode);
    const uint32_t forceQuantum = micFxForceQuantum();
    const QString forceQuantumProp = forceQuantum > 0 ? QString::number(forceQuantum) : QString();

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
            // Likewise a node from an older build, or one made before clock.min-quantum or
            // clock.max-quantum changed, forces the wrong cycle or none.
            if (micFxWanted() && n.props.contains(QStringLiteral("factory.name")) &&
                n.prop("node.force-quantum") != forceQuantumProp) {
                qCInfo(lcEngine) << "replacing mic filter node" << n.id << "to force quantum"
                                 << (forceQuantum > 0 ? forceQuantumProp : QStringLiteral("none"));
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
        // With the mix on, rostrum.filtered always stays (it carries the raw mic when filters are
        // off, failed or unavailable) and only rostrum.micfx goes. Apps still on the filtered mic
        // are pointed back at the mic first (reconcileMicRoutes, later in this pass); micfx goes
        // once they have moved, or after kFxReleaseMs. reconcileLinks keeps the processed path
        // linked for exactly that long (micFxReleaseHeld), then hands filtered over to the mic.
        const auto filterNodes = std::as_const(fxNodes) + filteredNodes;
        bool holding = false;
        if (m_mixEnabled && !fxNodes.isEmpty() && micStreamsOnFilter(filterNodes)) {
            if (!m_fxReleaseClock.isValid()) {
                m_fxReleaseClock.start();
                QTimer::singleShot(kFxReleaseMs + 100, this, &Engine::scheduleReconcile);
            }
            holding = m_fxReleaseClock.elapsed() < kFxReleaseMs;
            if (!holding) {
                qCWarning(lcEngine) << "removing the mic filters with apps still on them";
            }
        }
        if (m_mixEnabled && !holding) {
            for (const pw::Node *n : std::as_const(fxNodes)) {
                qCInfo(lcEngine) << "removing" << n->name;
                destroyOnce(n->id);
            }
            m_fxReleaseClock.invalidate();
            m_micReleased.clear();
            if (filteredNodes.isEmpty()) {
                const auto it = m_pendingCreate.find(filteredName);
                if (it == m_pendingCreate.end() || it->elapsed() >= kFxCreateRetryMs) {
                    qCInfo(lcEngine) << "creating" << filteredName;
                    m_pw->createNullNode(filteredMicSpec().properties());
                    QElapsedTimer t;
                    t.start();
                    m_pendingCreate.insert(filteredName, t);
                }
            } else {
                for (qsizetype i = 1; i < filteredNodes.size(); ++i) {
                    qCInfo(lcEngine) << "removing duplicate" << filteredName << filteredNodes.at(i)->id;
                    destroyOnce(filteredNodes.at(i)->id);
                }
            }
        }
        m_pendingCreate.remove(fxName);
        if (!filteredNodes.isEmpty()) {
            m_pendingCreate.remove(filteredName);
        }
        m_fxConfigured = 0;
        m_fxGraphNode = 0;
        m_fxBuildClock.invalidate();
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

    m_fxReleaseClock.invalidate();
    m_micReleased.clear();

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
        create(fxName, [this, forceQuantum] { m_pw->createSpaNode(micFxProperties(forceQuantum)); });
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
            setMicFxState(MicFxState::Failed, QStringLiteral("PipeWire did not set up the Studio Voice node. "
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
    const bool loaded = micfx::graphLoaded(fx.params.keys(), controls);
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
            m_fxBuildClock.start();
        } else {
            m_fxBuildClock.invalidate();
        }
        setMicFxState(MicFxState::Active);
        return;
    }
    // The crash breaker times from when the plugin starts running, not from resent controls.
    if (!fx.running) {
        m_fxBuildClock.invalidate();
    } else if (!m_fxBuildClock.isValid()) {
        m_fxBuildClock.start();
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
            // A graph that loaded without every control came from an older plugin PipeWire already
            // had under this path (a rebuilt plugin in the build tree, say); restarting PipeWire
            // clears it.
            const QStringList keys = fx.params.keys();
            const bool partial = std::any_of(keys.cbegin(), keys.cend(),
                                             [](const QString &k) { return k.startsWith(QLatin1String("rostrum_")); });
            qCWarning(lcEngine) << "mic filter graph did not load into" << fx.id << m_fxPlugin
                                << (partial ? "(some controls missing: an older plugin is still loaded)" : "");
            m_fxFailed = true;
            setMicFxState(MicFxState::Failed, QStringLiteral("PipeWire could not load Studio Voice. "
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
    if (state == pw::PwContext::State::Failed && m_fxGraphNode != 0 && m_fxBuildClock.isValid()) {
        if (m_fxBuildClock.elapsed() < kFxCrashWindowMs) {
            ++m_fxStrikes;
            qCWarning(lcEngine) << "PipeWire went away" << m_fxBuildClock.elapsed()
                                << "ms after the mic filters loaded, strike" << m_fxStrikes;
            if (m_fxStrikes >= kFxCrashStrikes && m_micFx.enabled) {
                const QString reason = QStringLiteral("PipeWire stopped twice right after Studio Voice started, "
                                                      "so Rostrum turned it off.");
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
        m_fxBuildClock.invalidate();
        m_fxGraphControls.clear();
        m_fxMissingClock.invalidate();
        m_fxVerified = false;
        m_fxSent.clear();
        m_fxTimer.stop();
        m_micRouted.clear();
        m_micRecognised.clear();
        m_micFirstSeen.clear();
        m_micReleased.clear();
        m_fxReleaseClock.invalidate();
        m_chatReleaseClock.invalidate();
        m_chatInactiveClock.invalidate();
        setMicFxState(m_micFx.enabled && !m_fxFailed ? MicFxState::Starting : MicFxState::Off, m_fxError);
    }
}

// ---- chat mic ---------------------------------------------------------------------------

void Engine::setChatMicWanted(bool on)
{
    if (m_chatMicWanted == on) {
        return;
    }
    qCInfo(lcEngine) << "chat mic wanted" << on;
    m_chatMicWanted = on;
    scheduleReconcile();
}

bool Engine::chatMicNeeded() const
{
    // The chat sends that feed it are per-mix sends; perMixActive() also needs PipeWire connected.
    return m_mixEnabled && m_chatMicWanted && perMixActive();
}

const pw::Node *Engine::chatMicNode() const
{
    const pw::Node *oldest = nullptr;
    for (const auto &n : m_pw->graph().nodes) {
        if (n.name == QLatin1String(kChatMicNode) && isOwnedNode(n) && !m_pendingDestroy.contains(n.id) &&
            (!oldest || n.id < oldest->id)) {
            oldest = &n;
        }
    }
    return oldest;
}

bool Engine::chatMicActive() const
{
    if (m_pw->state() != pw::PwContext::State::Ready) {
        return false;
    }
    const pw::Node *chat = chatMicNode();
    const pw::Node *filtered = filteredNode();
    if (!chat || !filtered) {
        return false;
    }
    const auto &g = m_pw->graph();
    return !g.inputPorts(chat->id).isEmpty() && !g.outputPorts(chat->id).isEmpty() &&
           linked(filtered->id, chat->id);
}

QStringList Engine::chatBlockedBusNames() const { return busDisplayNames(m_scene, m_chatBlocked); }

QString Engine::chatUnavailable() const
{
    const QString why = perMixUnavailable();
    if (why.isEmpty()) {
        return {};
    }
    return QStringLiteral("Voice chat needs separate Headphones and Stream levels. %1").arg(why);
}

void Engine::updateChatMic()
{
    const bool active = chatMicActive();
    const QString why = chatUnavailable();
    if (active != m_chatActiveReported || why != m_chatReason) {
        m_chatActiveReported = active;
        m_chatReason = why;
        Q_EMIT chatMicChanged();
    }
}

void Engine::updateChatLoop()
{
    QSet<QString> blocked;
    QStringList apps;
    const pw::Node *chat = chatMicNode();
    if (chat && m_mixEnabled) {
        const auto &g = m_pw->graph();
        // Both sides use identify(...).key: the same program gives the same key for its playback
        // and its capture stream.
        QSet<QString> onChat;
        QHash<QString, QString> names;
        for (const auto &n : g.nodes) {
            if (!isMicCapture(n)) {
                continue;
            }
            const QString target = m_pw->metadataValue(n.id, QStringLiteral("target.object"));
            const bool toChat = linked(chat->id, n.id) || namesNode(*chat, target);
            if (toChat) {
                const MicRecognised &r = recogniseCapture(n);
                onChat.insert(r.identity.key.toString());
                names.insert(r.identity.key.toString(), r.identity.displayName);
            }
        }
        if (!onChat.isEmpty()) {
            QMultiHash<QString, QString> byBus;
            for (const auto &n : g.nodes) {
                if (!n.isPlaybackStream() || isOwnStream(n)) {
                    continue;
                }
                const StreamProps props = propsOf(n);
                const AppIdentity id = identify(props);
                const Placement p = place(n, props, id);
                if (m_scene.bus(p.busId)) {
                    byBus.insert(p.busId, id.key.toString());
                }
            }
            blocked = chatLoopBuses(m_scene, byBus, onChat);
            for (const QString &bus : blocked) {
                for (const QString &key : byBus.values(bus)) {
                    if (onChat.contains(key) && !apps.contains(names.value(key))) {
                        apps.append(names.value(key));
                    }
                }
            }
            apps.sort();
        }
    }
    const bool blockedChanged = blocked != m_chatBlocked;
    if (blockedChanged) {
        for (const QString &bus : blocked) {
            if (!m_chatBlocked.contains(bus)) {
                qCInfo(lcEngine) << "voice chat send off for" << bus << "(" << apps << "records the chat mic)";
            }
        }
        m_chatBlocked = blocked;
        scheduleReconcile(); // the volume pass reads m_chatBlocked
    }
    if (apps != m_chatLoopApps || blockedChanged) {
        m_chatLoopApps = apps;
        Q_EMIT chatLoopChanged();
    }
}

void Engine::reconcileChatMic()
{
    // Nodes left by an earlier run stay while the mix is off, like the filter nodes.
    if (!m_mixEnabled) {
        m_chatReleaseClock.invalidate();
        return;
    }
    const auto &g = m_pw->graph();
    const QString name = QString::fromLatin1(kChatMicNode);
    QList<const pw::Node *> nodes;
    for (const auto &n : g.nodes) {
        if (n.name == name && isOwnedNode(n) && !m_pendingDestroy.contains(n.id)) {
            nodes.append(&n);
        }
    }
    if (!nodes.isEmpty()) {
        // One lingering from an earlier run is adopted.
        m_pendingCreate.remove(name);
        std::sort(nodes.begin(), nodes.end(),
                  [](const pw::Node *a, const pw::Node *b) { return a->id < b->id; });
        // Duplicates from a create race: keep the oldest, the one chatMicNode() names.
        for (qsizetype i = 1; i < nodes.size(); ++i) {
            qCInfo(lcEngine) << "removing duplicate" << name << nodes.at(i)->id;
            destroyOnce(nodes.at(i)->id);
        }
        if (chatMicNeeded()) {
            m_chatReleaseClock.invalidate();
            return;
        }
        // No longer wanted. Apps on it are moved off first (reconcileMicRoutes, later in this pass,
        // sends them to the filtered mic or back where they were), and it keeps carrying the voice
        // until they have moved, or kChatMicReleaseMs has passed. An app counts while its target
        // names the chat mic or, moved off, it is still linked from it.
        if (!m_chatReleaseClock.isValid()) {
            m_chatReleaseClock.start();
            QTimer::singleShot(micfx::kChatMicReleaseMs + 100, Qt::PreciseTimer, this,
                               &Engine::scheduleReconcile);
        }
        const QString targetKey = QStringLiteral("target.object");
        const bool appsOn = std::any_of(g.nodes.cbegin(), g.nodes.cend(), [&](const pw::Node &s) {
            if (!isMicCapture(s)) {
                return false;
            }
            const QString target = m_pw->metadataValue(s.id, targetKey);
            return std::any_of(nodes.cbegin(), nodes.cend(), [&](const pw::Node *c) {
                return namesNode(*c, target) || linked(c->id, s.id);
            });
        });
        if (micfx::chatMicReleaseHeld(appsOn, m_chatReleaseClock.elapsed())) {
            return;
        }
        if (appsOn) {
            qCWarning(lcEngine) << "removing the chat mic with apps still on it";
        }
        qCInfo(lcEngine) << "removing" << name;
        destroyOnce(nodes.first()->id);
        m_chatReleaseClock.invalidate();
        return;
    }
    m_chatReleaseClock.invalidate();
    if (!chatMicNeeded()) {
        m_pendingCreate.remove(name);
        return;
    }
    const auto it = m_pendingCreate.constFind(name);
    if (it != m_pendingCreate.cend() && it->elapsed() < micfx::kChatMicCreateRetryMs) {
        return;
    }
    qCInfo(lcEngine) << "creating" << name;
    m_pw->createNullNode(chatMicSpec().properties());
    QElapsedTimer t;
    t.start();
    m_pendingCreate.insert(name, t);
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
    const AppFacts facts = collectFacts(props, n.props, n.pid, m_desktop, [](const QString &) { return true; }, m_procRoot);
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
    // While the chat mic is in use, apps that would record the filtered mic record it instead, with
    // Studio Voice on or off: it carries what rostrum.filtered carries, plus the chat sends. Apps on
    // a Rostrum mic stay on it, or switch, while no voice reaches filtered for a moment (Studio Voice
    // handing its feed over, the mic unplugged: clips still go). Only an app still on the mic waits
    // for the voice, so the move does not cut it off.
    // A WirePlumber restart rebuilds the chat mic's ports and drops the link from filtered, so the
    // node is briefly not in use. Apps already on it keep that target until it is in use again, or
    // for kChatMicReleaseMs, instead of being sent to the raw mic and back. A first move, and a
    // switch from the filtered mic, wait until it is in use.
    const pw::Node *chatNode = chatMicNeeded() ? chatMicNode() : nullptr;
    if (chatNode && chatNode->serial.isEmpty()) {
        chatNode = nullptr;
    }
    const bool chatLive = chatNode && chatMicActive();
    if (chatNode && !chatLive) {
        if (!m_chatInactiveClock.isValid()) {
            m_chatInactiveClock.start();
            QTimer::singleShot(micfx::kChatMicReleaseMs + 100, Qt::PreciseTimer, this, &Engine::scheduleReconcile);
        }
    } else {
        m_chatInactiveClock.invalidate();
    }
    const bool chatHold = chatNode && !chatLive && m_chatInactiveClock.isValid() &&
                           m_chatInactiveClock.elapsed() < micfx::kChatMicReleaseMs;
    const pw::Node *chat = chatLive ? chatNode : nullptr;
    const pw::Node *feedInto = filteredNode();
    const bool chatHasVoice = chat && feedInto && mic &&
                              (linked(mic->id, feedInto->id) ||
                               (fx && linked(mic->id, fx->id) && linked(fx->id, feedInto->id)));
    const QString targetKey = QStringLiteral("target.object");
    const auto names = [](const pw::Node *n, const QString &target) { return n && namesNode(*n, target); };
    const auto namesChatMic = [&](const QString &target) {
        return std::any_of(g.nodes.cbegin(), g.nodes.cend(), [&](const pw::Node &c) {
            return c.name == QLatin1String(kChatMicNode) && namesNode(c, target);
        });
    };

    QSet<uint32_t> present;
    for (const auto &n : g.nodes) {
        if (!isMicCapture(n)) {
            continue;
        }
        present.insert(n.id);
        checkNodeSerial(n);
        QElapsedTimer &firstSeen = m_micFirstSeen[n.id];
        if (!firstSeen.isValid()) {
            firstSeen.start();
        }
        const MicRecognised &r = recogniseCapture(n);
        const micfx::AppChoice choice = micfx::appChoice(m_micFx, r.identity.key.toString());
        const bool wantsChat = micfx::useChatMic(m_micFx, choice, r.excluded);
        const bool toChat = chat && wantsChat;
        const bool toFiltered = filtered && micfx::useFiltered(m_micFx, choice, r.excluded);
        const pw::Node *target = toChat ? chat : (toFiltered ? filtered : nullptr);
        const QString current = m_pw->metadataValue(n.id, targetKey);
        auto routed = m_micRouted.find(n.id);
        const bool alreadyOnChat = (routed != m_micRouted.end() && routed->chat) || namesChatMic(current);
        if (wantsChat && chatHold) {
            // The metadata was cleared with WirePlumber. Ask for the chat mic again when the app
            // was already on it and nothing else has chosen a target. An app still on the filtered
            // mic, or not moved yet, stays where it is until the chat mic is in use.
            const bool reset = routed != m_micRouted.end() && routed->requested.isEmpty();
            const bool ours = current.isEmpty() || namesChatMic(current) || (reset && names(anyFiltered, current));
            if (alreadyOnChat && current != chatNode->serial && ours &&
                m_pw->setMetadata(n.id, targetKey, QStringLiteral("Spa:Id"), chatNode->serial)) {
                if (routed == m_micRouted.end()) {
                    MicRouted moved{QString(), chatNode->serial, {}, 0, true};
                    moved.since.start();
                    m_micRouted.insert(n.id, moved);
                } else {
                    routed->requested = chatNode->serial;
                    routed->chat = true;
                    routed->since.start();
                }
            }
            continue;
        }

        if (target) {
            if (routed != m_micRouted.end()) {
                // Once the metadata was reset (WirePlumber restarted), WirePlumber may put the app back
                // on a Rostrum mic itself before Rostrum asks again: that is still Rostrum's move, so
                // the chat mic can hand it back later. (The filtered mic alone is left as it was.)
                const bool reset = routed->requested.isEmpty();
                if (toChat && reset && current == target->serial) {
                    routed->requested = target->serial;
                    routed->chat = true;
                    continue;
                }
                const bool onOther = toChat ? names(anyFiltered, current) : namesChatMic(current);
                // Re-sent after the target is recreated, or switched between the filtered and the
                // chat mic, keeping where it was before the first move; left alone if the user moved it.
                if (current != target->serial && routed->requested != target->serial &&
                    (current == routed->requested || current.isEmpty() || (reset && onOther))) {
                    if (m_pw->setMetadata(n.id, targetKey, QStringLiteral("Spa:Id"), target->serial)) {
                        if (toChat && !routed->chat) {
                            qCInfo(lcEngine) << "chat mic for" << r.identity.displayName << n.id;
                        } else if (!toChat && routed->chat) {
                            qCInfo(lcEngine) << "filtered mic for" << r.identity.displayName << n.id
                                             << "(chat mic off)";
                        }
                        routed->requested = target->serial;
                        routed->chat = toChat;
                        routed->since.start();
                        routed->reclaims = 0;
                    }
                } else if (current != target->serial && !current.isEmpty() &&
                           routed->requested == target->serial &&
                           // Just switched: the other Rostrum mic is the old target, not another program's.
                           !onOther && routed->since.isValid() &&
                           routed->since.elapsed() < kReclaimWindowMs && routed->reclaims < kMaxReclaims &&
                           m_pw->setMetadata(n.id, targetKey, QStringLiteral("Spa:Id"), target->serial)) {
                    ++routed->reclaims;
                    qCInfo(lcEngine) << "reclaim" << r.identity.displayName << n.id << "from" << describeTarget(current)
                                     << (toChat ? "-> chat mic" : "-> filtered mic")
                                     << "(moved by another program as it started)";
                }
                continue;
            }
            if (current == target->serial) {
                // Moved by an earlier run that quit or crashed.
                m_micRouted.insert(n.id, {QString(), target->serial, {}, 0, toChat});
                continue;
            }
            // A stream on the other Rostrum mic is Rostrum's to switch: an earlier run moved it there,
            // or WirePlumber put the app back where one did.
            const bool onOther = toChat ? names(anyFiltered, current) : namesChatMic(current);
            if (!onOther) {
                if (toChat && !chatHasVoice) {
                    continue;
                }
                // Only streams recording the mic apps use; a stream that picked another source keeps
                // it. Easy Effects moves every new recording stream to its own source, which is not a
                // choice.
                const bool takenByEffects =
                    isEffectsNode(nodeNamed(current)) && firstSeen.elapsed() < kReclaimWindowMs;
                if (!takenByEffects &&
                    (!linked(mic->id, n.id) || (!current.isEmpty() && !names(mic, current)))) {
                    continue;
                }
            }
            if (!m_pw->setMetadata(n.id, targetKey, QStringLiteral("Spa:Id"), target->serial)) {
                continue; // no metadata yet: the next pass asks again
            }
            if (toChat) {
                qCInfo(lcEngine) << "chat mic for" << r.identity.displayName << n.id;
            } else if (onOther) {
                qCInfo(lcEngine) << "filtered mic for" << r.identity.displayName << n.id << "(chat mic off)";
            } else {
                qCInfo(lcEngine) << "filter mic for" << r.identity.displayName << n.id;
            }
            MicRouted moved{onOther ? QString() : current, target->serial, {}, 0, toChat};
            moved.since.start();
            m_micRouted.insert(n.id, moved);
        } else if (routed != m_micRouted.end()) {
            const MicRouted was = m_micRouted.take(n.id);
            if (!current.isEmpty() && current != was.requested) {
                continue; // moved by the user since
            }
            qCInfo(lcEngine) << "raw mic for" << r.identity.displayName << n.id;
            m_micReleased.insert(n.id);
            if (was.previousTarget.isEmpty()) {
                m_pw->clearMetadata(n.id, targetKey);
                m_pw->clearMetadata(n.id, QStringLiteral("target.node"));
            } else {
                m_pw->setMetadata(n.id, targetKey, QStringLiteral("Spa:Id"), was.previousTarget);
            }
        } else if ((((m_mixEnabled && !micFxWanted()) || !micfx::useFiltered(m_micFx, choice, r.excluded)) &&
                    names(anyFiltered, current)) ||
                   namesChatMic(current)) {
            // WirePlumber remembers where a stream was moved and puts the app back there when it
            // next starts. An app set to raw since goes back to the mic, and clearing the target
            // makes WirePlumber forget it; so does every app while the mix runs without filters
            // (turned off, failed or unavailable), so it records the mic directly instead of a
            // remembered target on rostrum.filtered (which stays, carrying the raw mic). An app that
            // picked the filtered mic itself sets target.object in its own properties instead,
            // which WirePlumber leaves alone. A remembered target on rostrum.chatmic is cleared
            // whenever the app is not to record it (the chat mic is not in use, or the app is set to
            // the plain mic), so the next pass can place it.
            qCInfo(lcEngine) << "raw mic for" << r.identity.displayName << n.id << "(remembered by WirePlumber)";
            m_micReleased.insert(n.id);
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
    for (auto it = m_micReleased.begin(); it != m_micReleased.end();) {
        it = present.contains(*it) ? std::next(it) : m_micReleased.erase(it);
    }
}

bool Engine::micStreamsOnFilter(const QList<const pw::Node *> &filterNodes) const
{
    const QString targetKey = QStringLiteral("target.object");
    for (const auto &n : m_pw->graph().nodes) {
        if (!isMicCapture(n)) {
            continue;
        }
        const QString target = m_pw->metadataValue(n.id, targetKey);
        for (const pw::Node *f : filterNodes) {
            // Rostrum's (or WirePlumber's remembered) target still names it, or Rostrum pointed
            // the stream back and WirePlumber has not relinked it yet.
            if ((!target.isEmpty() &&
                 (target == f->serial || target == f->name || target == QString::number(f->id))) ||
                (m_micReleased.contains(n.id) && linked(f->id, n.id))) {
                return true;
            }
        }
    }
    return false;
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
    const pw::Node *chat = g.nodeByName(QString::fromLatin1(kChatMicNode));
    const bool fxRuns = micFxRunning();
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
        // The chat mic carries the filtered voice while Studio Voice runs, else the plain one.
        const bool onChat = (chat && linked(chat->id, n.id)) || (movedHere && routed->chat);
        const bool fromFiltered = linkedFiltered || (movedHere && !routed->chat) || (onChat && fxRuns);
        const pw::Node *effects = nullptr;
        if (!fromFiltered && !fromMic && !onChat) {
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
