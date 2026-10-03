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

    // Levels: these change the live scene and make it dirty until saved.
    void setBusVolume(const QString &id, double volume);
    void setBusMuted(const QString &id, bool muted);
    void setBusDestination(const QString &id, Destination d);
    void setMasterPhones(double volume);
    void setMasterPhonesMuted(bool muted);
    void setMasterStream(double volume);
    void setMasterStreamMuted(bool muted);
    void setMicMuted(bool muted);
    bool micMuted() const;
    void setSidetoneEnabled(bool on); // the mic destination's Phones half
    bool sidetoneEnabled() const;
    void setSidetoneVolume(double volume);

    // Solo is session-only. It is applied as an effective mute and never stored in the scene.
    void setSolo(const QString &id, bool soloed);
    bool isSoloed(const QString &id) const { return m_soloed.contains(id); }
    bool anySolo() const { return !m_soloed.isEmpty(); }
    bool dimmedBySolo(const QString &id) const;
    void clearSolo();

    // Structure: persisted right away by the app without saving fader moves.
    QString addBus(const QString &name, const QString &color = QString());
    bool removeBus(const QString &id);
    void renameBus(const QString &id, const QString &name);
    void recolorBus(const QString &id, const QString &color);
    QString duplicateBus(const QString &id);
    bool canAddBus() const { return m_scene.buses.size() < kMaxBuses; }

    // Devices. Empty name = follow the system default.
    void setHeadphoneDevice(const QString &nodeName);
    void setMicDevice(const QString &nodeName);
    QString headphoneDevice() const { return m_headphones; }
    QString micDevice() const { return m_micDevice; }
    QString resolvedSinkName() const;
    QString resolvedSourceName() const;
    bool headphonesMissing() const;
    bool micMissing() const;
    bool hasMic() const { return !resolvedSourceName().isEmpty(); }

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
    void setRuleLabel(const AppKey &key, const QString &label);

Q_SIGNALS:
    void sceneChanged();
    void structureChanged(); // bus list, names, colors or rules changed: persist without saving faders
    void mixStateChanged();
    void appsChanged();
    void levelsChanged();
    void soloChanged();
    void devicesChanged();
    void headphonesLost(const QString &description);
    void headphonesRestored();

protected:
    void scheduleReconcile();
    virtual void reconcile();
    void reconcileNodes();
    void reconcileRoutes();
    void reconcileLinks();
    void reconcileVolumes();
    void reconcileDevices();
    void levelChanged();
    const pw::Node *resolveSink() const;
    const pw::Node *resolveSource() const;
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

    QSet<QString> m_soloed;
    QString m_headphones;
    QString m_micDevice;
    QString m_lastSinkDescription;
    bool m_headphonesWereMissing = false;
    QString m_devicesSignature;
    QHash<QString, QElapsedTimer> m_pendingLinks; // "out:in" port pairs being created
    struct SentVolume
    {
        float linear = -1.0f;
        bool mute = false;
        QElapsedTimer when;
    };
    QHash<uint32_t, SentVolume> m_sentVolume;
    bool isOwnedNode(const pw::Node &n) const;
    void destroyOnce(uint32_t id);

    pw::PwContext *m_pw = nullptr;
    Scene m_scene;
    bool m_mixEnabled = false;
    bool m_reportedReady = false;
    QString m_mixError;
    bool m_reconcilePending = false;
    QHash<QString, QElapsedTimer> m_pendingCreate;
    QSet<uint32_t> m_pendingDestroy;
    QElapsedTimer m_mixRequested;
};

} // namespace rostrum::engine
