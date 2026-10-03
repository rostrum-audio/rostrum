#include "engine/Engine.h"

#include "pw/PwContext.h"

#include <QLoggingCategory>
#include <QTimer>

Q_LOGGING_CATEGORY(lcEngine, "rostrum.engine")

namespace rostrum::engine {

namespace {
constexpr int kCreateRetryMs = 3000;
constexpr int kCreateTimeoutMs = 5000;
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
        return;
    }
    const bool wasReady = mixReady();
    reconcileNodes();
    if (wasReady != mixReady()) {
        if (mixReady()) {
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

} // namespace rostrum::engine
