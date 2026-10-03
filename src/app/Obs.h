#pragma once

#include "obs/ObsClient.h"
#include "obs/ObsConfig.h"
#include "obs/ObsModel.h"
#include "obs/ObsPlan.h"

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
class Obs : public QObject
{
    Q_OBJECT
    QML_ELEMENT
    QML_SINGLETON

    // Set by the page while it is on screen; nothing polls or connects otherwise.
    Q_PROPERTY(bool active READ active WRITE setActive NOTIFY activeChanged)
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

public:
    Obs(AppController *app, QObject *parent);
    ~Obs() override;

    static Obs *create(QQmlEngine *, QJSEngine *);

    bool active() const { return m_active; }
    void setActive(bool active);
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

    Q_INVOKABLE void refresh();
    Q_INVOKABLE void setPlanItemEnabled(int index, bool enabled);
    Q_INVOKABLE void apply();
    Q_INVOKABLE void undo();

Q_SIGNALS:
    void activeChanged();
    void changed();
    void planChanged();
    void recordingsChanged();

private:
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

    static Obs *s_instance;
    AppController *m_app = nullptr;
    obs::Client m_client;
    QTimer m_poll;
    QTimer m_refetch;
    QTimer m_graphDebounce;
    bool m_active = false;
    bool m_busy = false;
    QString m_stateName = QStringLiteral("notInstalled");
    std::optional<obs::Install> m_install;
    obs::WebSocketConfig m_ws;
    QString m_rejectedPassword;
    QString m_offlineStamp;
    bool m_running = false;
    obs::State m_state;
    bool m_haveState = false;
    obs::Plan m_plan;
    bool m_havePlan = false;
    QVariantList m_recordings;
};

} // namespace rostrum::app
