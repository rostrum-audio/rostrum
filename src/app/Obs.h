#pragma once

#include "obs/ObsClient.h"
#include "obs/ObsConfig.h"
#include "obs/ObsModel.h"
#include "obs/ObsPlan.h"
#include "obs/ObsStatus.h"

#include <QObject>
#include <QTimer>
#include <QVariantList>
#include <QtQml/qqmlregistration.h>

#include <optional>

class QQmlEngine;
class QJSEngine;

namespace rostrum::app {

class AppController;

// The OBS page: what OBS records (from PipeWire, always), and a one-click setup. With OBS
// running it goes through obs-websocket, using the password from OBS's own settings; with OBS
// closed it edits the scene collection file. Every change is recorded so it can be undone.
// With "Follow OBS" on, the same connection stays open while OBS runs, for the LIVE and REC
// badges, go-live warnings and scene mapping. That part only reads from OBS.
class Obs : public QObject
{
    Q_OBJECT
    QML_ELEMENT
    QML_SINGLETON

    // Set by the page while it is on screen; nothing polls or connects otherwise, unless
    // "Follow OBS" is on and OBS is running.
    Q_PROPERTY(bool active READ pageActive WRITE setActive NOTIFY activeChanged)
    // Set by the setup wizard's OBS step, which needs the same status and plan as the page.
    Q_PROPERTY(bool wizardActive READ wizardActive WRITE setWizardActive NOTIFY activeChanged)
    // "notInstalled", "neverRun", "closed", "noWebSocket", "connecting", "connected",
    // "authFailed" or "failed"
    Q_PROPERTY(QString state READ state NOTIFY changed)
    Q_PROPERTY(QString summary READ summary NOTIFY changed)
    Q_PROPERTY(QString detail READ detail NOTIFY changed)
    Q_PROPERTY(bool setUp READ setUp NOTIFY changed)
    // Mic and Stream Mix are in place; only sources that double audio are left to mute.
    Q_PROPERTY(bool onlyDoubling READ onlyDoubling NOTIFY changed)
    Q_PROPERTY(bool canApply READ canApply NOTIFY changed)
    Q_PROPERTY(bool canUndo READ canUndo NOTIFY changed)
    Q_PROPERTY(bool busy READ busy NOTIFY changed)
    // Rows: {source, text, kind}; kind is "ok", "warning" or "muted"
    Q_PROPERTY(QVariantList recordings READ recordings NOTIFY recordingsChanged)
    // Rows: {index, title, detail, optional, enabled}
    Q_PROPERTY(QVariantList planItems READ planItems NOTIFY planChanged)

    Q_PROPERTY(bool background READ background WRITE setBackground NOTIFY preferencesChanged)
    Q_PROPERTY(bool goLiveWarnings READ goLiveWarnings WRITE setGoLiveWarnings NOTIFY preferencesChanged)

    Q_PROPERTY(bool streaming READ streaming NOTIFY liveChanged)
    Q_PROPERTY(bool recording READ recording NOTIFY liveChanged)
    Q_PROPERTY(bool recordPaused READ recordPaused NOTIFY liveChanged)
    // ms since the epoch; QML works out the elapsed time
    Q_PROPERTY(double streamStartMs READ streamStartMs NOTIFY liveChanged)
    Q_PROPERTY(double recordStartMs READ recordStartMs NOTIFY liveChanged)
    Q_PROPERTY(double recordPausedElapsedMs READ recordPausedElapsedMs NOTIFY liveChanged)
    Q_PROPERTY(QString programScene READ programScene NOTIFY liveChanged)

    // Rows: {obsScene, rostrumScene, present, onProgram}; rostrumScene is empty for "No change".
    // present: OBS has the scene now (or, while OBS is closed, its scene collection does).
    Q_PROPERTY(QVariantList sceneMap READ sceneMap NOTIFY sceneMapChanged)
    // Problems found when the stream started that are still there: "micMuted", "streamSilent",
    // "noStreamCapture", "noMicCapture".
    Q_PROPERTY(QStringList warnings READ warnings NOTIFY warningsChanged)
    Q_PROPERTY(QString warningText READ warningText NOTIFY warningsChanged)

public:
    Obs(AppController *app, QObject *parent);
    ~Obs() override;

    static Obs *instance() { return s_instance; }
    static Obs *create(QQmlEngine *, QJSEngine *);

    bool pageActive() const { return m_pageActive; }
    void setActive(bool active);
    bool wizardActive() const { return m_wizardActive; }
    void setWizardActive(bool active);
    QString state() const { return m_stateName; }
    QString summary() const;
    QString detail() const;
    bool setUp() const { return m_havePlan && m_plan.isEmpty(); }
    bool onlyDoubling() const;
    bool canApply() const;
    bool canUndo() const;
    bool busy() const { return m_busy; }
    QVariantList recordings() const { return m_recordings; }
    QVariantList planItems() const;

    bool background() const;
    void setBackground(bool on);
    bool goLiveWarnings() const;
    void setGoLiveWarnings(bool on);

    bool streaming() const { return m_live.streaming(); }
    bool recording() const { return m_live.recording(); }
    bool recordPaused() const { return m_live.recordPaused(); }
    double streamStartMs() const { return double(m_live.streamStartMs()); }
    double recordStartMs() const { return double(m_live.recordStartMs()); }
    double recordPausedElapsedMs() const { return double(m_live.recordPausedElapsedMs()); }
    QString programScene() const { return m_live.programScene(); }

    QVariantList sceneMap() const;
    QStringList warnings() const;
    QString warningText() const;

    Q_INVOKABLE void refresh();
    Q_INVOKABLE void setPlanItemEnabled(int index, bool enabled);
    Q_INVOKABLE void apply();
    Q_INVOKABLE void undo();
    // An empty `rostrumScene` removes the mapping.
    Q_INVOKABLE void setSceneMapping(const QString &obsScene, const QString &rostrumScene);
    Q_INVOKABLE void dismissWarnings();

Q_SIGNALS:
    void activeChanged();
    void changed();
    void planChanged();
    void recordingsChanged();
    void preferencesChanged();
    void liveChanged();
    void sceneMapChanged();
    void warningsChanged();

private:
    void updateActive();
    void updatePolling();
    bool backgroundAllowed() const;
    void poll();
    void openClient();
    void setStateName(const QString &name);
    void fetchLive();
    // Re-reads the scene collection when it changed on disk, or always with `force`.
    void loadOffline(bool force = false);
    void setPlan(const obs::State &state, obs::Mode mode);
    void clearPlan();
    void rebuildRecordings();
    void finish(bool ok, const QString &message);
    obs::Undo loadUndo() const;
    void saveUndo(const obs::Undo &undo) const;
    void appendUndo(const QList<obs::UndoOp> &ops);
    QList<obs::GoLiveProblem> currentProblems() const;
    void checkGoLive();
    void refreshWarnings();
    void followProgramScene(const QString &obsScene);

    static Obs *s_instance;
    AppController *m_app = nullptr;
    obs::Client m_client;
    obs::LiveStatus m_live{&m_client};
    QTimer m_poll;
    QTimer m_refetch;
    QTimer m_graphDebounce;
    bool m_pageActive = false;
    bool m_wizardActive = false;
    bool m_active = false;
    bool m_offscreen = false;
    bool m_busy = false;
    QString m_stateName = QStringLiteral("notInstalled");
    std::optional<obs::Install> m_install;
    obs::WebSocketConfig m_ws;
    QString m_rejectedPassword;
    int m_failures = 0;
    qint64 m_retryAt = 0;
    QString m_offlineStamp;
    bool m_running = false;
    obs::State m_state;
    bool m_haveState = false;
    obs::Plan m_plan;
    bool m_havePlan = false;
    QVariantList m_recordings;
    QList<obs::GoLiveProblem> m_problems;
};

} // namespace rostrum::app
