#pragma once

#include "core/AppClassifier.h"
#include "core/AppIdentity.h"
#include "core/DesktopEntries.h"
#include "core/Ducking.h"
#include "core/Model.h"
#include "engine/NodeSpecs.h"
#include "pw/MeterBank.h"

#include <QElapsedTimer>
#include <QHash>
#include <QObject>
#include <QSet>
#include <QTimer>

#include <optional>

namespace rostrum::pw {
class PwContext;
struct Graph;
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
    bool automatic = false;   // placed by what Rostrum recognised, not by the user
    Classification detected;  // what Rostrum recognised, whether or not it placed it
    QString detectedName;     // e.g. the Steam game's name, empty if none
    bool skipped = false;     // the user took it off its automatic bus
    QStringList iconNames;    // theme icon names or absolute paths, best first
    double volume = 1.0;
    bool muted = false;
};

// The device Rostrum links for headphones (sink) or the mic: the saved node.name if present,
// otherwise the system default, otherwise the highest priority.session. With `fallback` off, a
// saved device that is missing resolves to nothing. An empty `saved` always follows the default.
// Rostrum nodes are never candidates.
const pw::Node *resolveDevice(const pw::Graph &graph, const QString &saved, const QString &systemDefault,
                              bool sink, bool fallback);

// Owns the desired Rostrum graph for the current scene and reconciles PipeWire toward it
// whenever either side changes. The engine never deletes the scene because a device vanished.
class Engine : public QObject
{
    Q_OBJECT
public:
    explicit Engine(pw::PwContext *pw, QObject *parent = nullptr);

    pw::PwContext *pw() const { return m_pw; }

    const Scene &scene() const { return m_scene; }
    // fade: ramp bus and master levels over sceneFadeMs() instead of jumping. The scene takes the
    // new values at once; the ramp is session-only and never saved. The mic is never faded.
    void setScene(const Scene &scene, bool fade = false);
    void setSceneFadeMs(int ms);
    int sceneFadeMs() const { return m_sceneFadeMs; }
    bool fading() const { return !m_fades.isEmpty(); }
    // The fader position being sent to a bus or master node mid-fade, if it is fading.
    std::optional<double> fadingPosition(const QString &nodeName) const;

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
    void setBusBalance(const QString &id, double balance); // playback buses; -1 left .. 1 right
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

    // Holds from hotkeys and remote control are session-only too: they change what is sent to
    // PipeWire, never the scene. An explicit mic or Stream choice ends a hold that contradicts it.
    void setPushToTalk(bool held); // mic live while held
    void setPushToMute(bool held); // mic muted while held; wins over push to talk
    void setPanic(bool on);        // mic and Stream master muted until turned off, across scene switches
    bool pushToTalk() const { return m_pushToTalk; }
    bool pushToMute() const { return m_pushToMute; }
    bool panic() const { return m_panicMic || m_panicStream; }
    bool effectiveMicMuted() const;
    bool effectiveStreamMuted() const;
    // Drops every hold and sends the scene's own mutes now, so quitting leaves the mix as saved.
    void releaseHolds();

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
    // Off by default: while the saved mic is unplugged, rostrum.mic stays unlinked and muted.
    // On: another mic stands in, the way headphones fall back. Never rewrites the saved mic.
    void setMicFallback(bool on);
    bool micFallback() const { return m_micFallback; }
    bool micSilenced() const;        // the saved mic is missing and nothing stands in for it
    QString missingMicLabel() const; // its description from when it was last seen, or its node.name
    // Both headphone channels carry L+R at half level, by linking each side into both. Off by default.
    void setMonoHeadphones(bool on);
    bool monoHeadphones() const { return m_monoHeadphones; }

    // Auto-ducking: while the trigger is heard, the target buses are turned down. Applied as a
    // multiplier when volumes are sent; the scene never changes. Its meter streams exist only
    // while ducking is on (a mic trigger keeps the desktop's mic indicator lit).
    void setDucking(const ducking::Settings &settings);
    const ducking::Settings &ducking() const { return m_ducking; }
    bool isDucked(const QString &busId) const; // a target bus, turned down right now
    bool isDuckTarget(const QString &busId) const;
    // On quit: ends ducking and any scene fade, sending every bus its scene level, so nothing is
    // left turned down. Returns whether anything was sent.
    bool releaseTransientLevels();

    // Apps
    QList<AppStream> appStreams() const;
    // Icons for a saved rule's app while it is not running, from the app menu, best first.
    QStringList ruleIconCandidates(const AppRule &rule) const;
    // always = true writes a rule into the scene; false places the app for this launch only.
    void assignApp(const AppKey &key, const QString &busId, bool always);
    void unassignApp(const AppKey &key);
    void unassignStream(uint32_t nodeId);
    void setAppVolume(const AppKey &key, double volume);
    double appVolume(const AppKey &key) const;
    // Saved in the app's rule like its volume; for apps without a rule, until Rostrum quits.
    void setAppMuted(const AppKey &key, bool muted);
    bool appMuted(const AppKey &key) const;
    // On quit: unmutes the streams Rostrum muted, so quitting never leaves an app silent. Saved
    // mutes apply again at the next start. Returns whether anything was sent.
    bool releaseAppMutes();
    void removeRule(const AppKey &key);
    void editRule(const AppKey &oldKey, const AppKey &newKey);
    void setRuleLabel(const AppKey &key, const QString &label);

    // Automatic assignment: recognised apps go to the bus whose autoCategory matches, below
    // "this launch only" choices and rules. Taking an app off its automatic bus skips it from
    // then on, until the user assigns it again or forgets the skips.
    void setAutoAssign(bool on);
    bool autoAssign() const { return m_autoAssign; }
    void setAutoSkip(const QStringList &keys);
    QStringList autoSkip() const;
    void forgetAutoSkip();
    void setBusAutoCategory(const QString &busId, AppCategory category);

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
    void micLost(const QString &description);
    void micRestored();
    void autoSkipChanged();
    void duckedChanged();

protected:
    void scheduleReconcile();
    virtual void reconcile();
    void reconcileNodes();
    void reconcileRoutes();
    void reconcileLinks();
    void reconcileVolumes();
    void reconcileDevices();
    bool phonesCrossLinked() const; // headphone links that carry one side into the other
    struct Level
    {
        double position = 0.0; // 0 when muted
        double balance = 0.0;
        bool operator==(const Level &) const = default;
    };
    QHash<QString, Level> fadeLevels(const Scene &scene, bool withSolo) const; // by node name
    std::optional<Level> fadingLevel(const QString &nodeName) const;
    void cancelFade(const QString &nodeName);
    void fadeTick();
    void reconcileDucking();
    void duckTick();
    QString duckTriggerBus() const; // the voice bus that triggers, if the trigger includes it
    void levelChanged();
    const pw::Node *resolveSink() const;
    const pw::Node *resolveSource() const;
    bool isOwnStream(const pw::Node &n) const;
    StreamProps propsOf(const pw::Node &n) const;
    struct Placement
    {
        QString busId;
        AppKey ruleKey;
        bool session = false;
        bool automatic = false;
    };
    struct Recognised
    {
        QString signature; // the props it was computed from; a change recomputes it
        AppFacts facts;
        Classification detected;
        QString skipKey; // "steam:<id>" for Steam games, else the app key
    };
    const Recognised &recognise(const pw::Node &n, const StreamProps &props, const AppIdentity &id) const;
    Placement place(const pw::Node &n, const StreamProps &props, const AppIdentity &id) const;
    bool isRostrumTarget(const QString &target) const;
    void skipAuto(const AppKey &key, bool skip);

    struct Routed
    {
        QString busId;
        QString previousTarget; // metadata target.object before Rostrum moved it, empty if none
        QString requested;      // the bus serial Rostrum last asked for
    };
    QHash<uint32_t, Routed> m_routed;
    QHash<QString, QString> m_sessionAssign;    // AppKey string -> bus id
    QHash<QString, double> m_sessionVolume;      // AppKey string -> volume for apps without a rule
    QHash<QString, bool> m_sessionMuted;         // AppKey string -> mute for apps without a rule
    QHash<QString, QElapsedTimer> m_sessionSeen; // AppKey string -> last time a stream was present
    QHash<uint32_t, double> m_appliedStreamVolume;
    QHash<uint32_t, bool> m_appliedStreamMute;
    QSet<QString> m_seenThisSession;
    bool m_autoAssign = true;
    QSet<QString> m_autoSkip; // lower-case skip keys
    mutable QHash<uint32_t, Recognised> m_recognised;
    mutable DesktopIndex m_desktop;

    QSet<QString> m_soloed;
    bool m_pushToTalk = false;
    bool m_pushToMute = false;
    bool m_panicMic = false;
    bool m_panicStream = false;
    void holdsChanged();
    QString m_headphones;
    QString m_micDevice;
    QString m_lastSinkDescription;
    bool m_headphonesWereMissing = false;
    QString m_lastSourceDescription;
    bool m_micWasMissing = false;
    bool m_micFallback = false;
    bool m_monoHeadphones = false;
    bool m_phonesCrossLinkWanted = false;
    QString m_devicesSignature;
    QHash<QString, QElapsedTimer> m_pendingLinks; // "out:in" port pairs being created
    struct SentVolume
    {
        QList<float> volumes; // linear, one per channel or one for all
        bool mute = false;
        QElapsedTimer when;
    };
    QHash<uint32_t, SentVolume> m_sentVolume;
    struct Fade
    {
        Level from;
        Level to;
    };
    QHash<QString, Fade> m_fades; // node name -> ramp; all share one clock
    QElapsedTimer m_fadeClock;
    int m_fadeLength = 0;
    int m_sceneFadeMs = 0;
    QTimer m_fadeTimer;
    ducking::Settings m_ducking;
    ducking::Envelope m_duckEnvelope;
    pw::MeterBank m_duckMeters;
    QTimer m_duckTimer;
    QElapsedTimer m_duckClock;
    QString m_duckMicNode;   // hardware mic being metered, empty when the mic is not live
    QString m_duckVoiceNode; // voice bus being metered
    double m_duckGainSent = 1.0;
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
