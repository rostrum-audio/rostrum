#pragma once

#include "core/Model.h"
#include "engine/NodeSpecs.h"

#include <QElapsedTimer>
#include <QHash>
#include <QObject>
#include <QSet>

namespace rostrum::pw {
class PwContext;
struct Node;
}

namespace rostrum::engine {

// Owns the desired Rostrum graph for the current scene and reconciles PipeWire toward it
// whenever either side changes. The engine never deletes the scene because a device vanished.
class Engine : public QObject
{
    Q_OBJECT
public:
    explicit Engine(pw::PwContext *pw, QObject *parent = nullptr);

    pw::PwContext *pw() const { return m_pw; }

    const Scene &scene() const { return m_scene; }
    void setScene(const Scene &scene);

    // Node creation is opt-in: the wizard (or a completed first run) turns it on.
    bool mixEnabled() const { return m_mixEnabled; }
    void createMix();
    void rebuildMix();
    void destroyMix(); // removes every node Rostrum created; used by rebuild and teardown
    bool mixReady() const; // every desired node exists in the graph
    QString mixError() const { return m_mixError; }

Q_SIGNALS:
    void sceneChanged();
    void mixStateChanged();

protected:
    void scheduleReconcile();
    virtual void reconcile();
    void reconcileNodes();
    bool isOwnedNode(const pw::Node &n) const;
    void destroyOnce(uint32_t id);

    pw::PwContext *m_pw = nullptr;
    Scene m_scene;
    bool m_mixEnabled = false;
    QString m_mixError;
    bool m_reconcilePending = false;
    QHash<QString, QElapsedTimer> m_pendingCreate;
    QSet<uint32_t> m_pendingDestroy;
    QElapsedTimer m_mixRequested;
};

} // namespace rostrum::engine
