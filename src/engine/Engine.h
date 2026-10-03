#pragma once

#include "core/AppIdentity.h"
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

struct AppStream
{
    uint32_t nodeId = 0;
    StreamProps props;
    AppIdentity identity;
    QString busId;          // effective bus, empty if unassigned
    AppKey ruleKey;         // the rule that placed it, if any
    bool sessionOnly = false; // placed by a "this launch only" assignment
    double volume = 1.0;
};

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

    // Apps
    QList<AppStream> appStreams() const;
    // always = true writes a rule into the scene; false places the app for this launch only.
    void assignApp(const AppKey &key, const QString &busId, bool always);
    void unassignApp(const AppKey &key);
    void unassignStream(uint32_t nodeId);
    void setAppVolume(const AppKey &key, double volume);
    double appVolume(const AppKey &key) const;
    void removeRule(const AppKey &key);
    void editRule(const AppKey &oldKey, const AppKey &newKey);

Q_SIGNALS:
    void sceneChanged();
    void structureChanged(); // bus list, names, colors or rules changed: persist without saving faders
    void mixStateChanged();
    void appsChanged();

protected:
    void scheduleReconcile();
    virtual void reconcile();
    void reconcileNodes();
    void reconcileRoutes();
    bool isOwnStream(const pw::Node &n) const;
    StreamProps propsOf(const pw::Node &n) const;
    QString effectiveBus(const StreamProps &props, const AppIdentity &id, AppKey *ruleKey, bool *session) const;

    struct Routed
    {
        QString busId;
        QString previousTarget; // metadata target.object before Rostrum moved it, empty if none
        QString requested;      // the bus serial Rostrum last asked for
    };
    QHash<uint32_t, Routed> m_routed;
    QHash<QString, QString> m_sessionAssign;    // AppKey string -> bus id
    QHash<QString, double> m_sessionVolume;      // AppKey string -> volume for apps without a rule
    QHash<QString, QElapsedTimer> m_sessionSeen; // AppKey string -> last time a stream was present
    QHash<uint32_t, double> m_appliedStreamVolume;
    QSet<QString> m_seenThisSession;
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
