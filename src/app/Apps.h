#pragma once

#include "app/MeterBallistics.h"
#include "app/RowsModel.h"
#include "engine/Engine.h"
#include "pw/MeterBank.h"

#include <QElapsedTimer>
#include <QObject>
#include <QTimer>
#include <QVariantList>
#include <QtQml/qqmlregistration.h>

class QQmlEngine;
class QJSEngine;

namespace rostrum::app {

class AppController;

// The Apps page: running audio apps (one row per app, however many streams it has), the saved
// rules of the live scene, per-app meters while the page is visible, and the assign actions.
class Apps : public QObject
{
    Q_OBJECT
    QML_ELEMENT
    QML_SINGLETON

    // Rows: {key, name, icon, binary, matchKey, busId, busName, busColor, volume, muted, always,
    //        unnamed, nodeIds, automatic, tool, detail}. detail explains an automatic placement, or why an
    //        app Rostrum recognised was left where it is; empty for the user's own choices.
    Q_PROPERTY(QVariantList running READ running NOTIFY changed)
    // Rows: {key, match, matchKey, label, icon, busId, busName, busColor, lastSeen, running}
    Q_PROPERTY(QVariantList rules READ rules NOTIFY changed)
    // The same rows as models, for views: rows update in place while their keys stay the same.
    Q_PROPERTY(rostrum::app::RowsModel *runningModel READ runningModel CONSTANT)
    Q_PROPERTY(rostrum::app::RowsModel *rulesModel READ rulesModel CONSTANT)
    // Playback buses an app can go to: {id, name, color}
    Q_PROPERTY(QVariantList buses READ buses NOTIFY changed)
    // First running app that reports no usable name, or empty: {key, binary}
    Q_PROPERTY(QVariantMap unnamed READ unnamed NOTIFY changed)
    Q_PROPERTY(bool metersActive READ metersActive WRITE setMetersActive NOTIFY metersActiveChanged)
    // app key -> 0..1 meter fraction
    Q_PROPERTY(QVariantMap levels READ levels NOTIFY levelsChanged)

public:
    Apps(AppController *app, QObject *parent);
    ~Apps() override;

    static Apps *create(QQmlEngine *, QJSEngine *);

    // "Steam game", "Its app menu entry lists it as a music player", ... Empty if Rostrum does
    // not recognise the app.
    static QString reason(const engine::AppStream &app);
    // The first candidate the icon theme has, as an icon name, or a file URL for a path. Empty if
    // none; views then show a generic app icon.
    static QString iconFor(const QStringList &candidates);

    QVariantList running() const { return m_running; }
    QVariantList rules() const { return m_rules; }
    RowsModel *runningModel() { return &m_runningModel; }
    RowsModel *rulesModel() { return &m_rulesModel; }
    QVariantList buses() const { return m_buses; }
    QVariantMap unnamed() const { return m_unnamed; }
    bool metersActive() const { return m_metersActive; }
    void setMetersActive(bool active);
    QVariantMap levels() const { return m_levels; }

    // "Always" is on when assigning; turning it off keeps the app on the bus for this launch only.
    Q_INVOKABLE void assign(const QString &key, const QString &busId);
    Q_INVOKABLE void setAlways(const QString &key, bool always);
    Q_INVOKABLE void unassign(const QString &key);
    Q_INVOKABLE void setVolume(const QString &key, double volume);
    Q_INVOKABLE void setMuted(const QString &key, bool muted);
    Q_INVOKABLE void removeRule(const QString &key);
    // matchKey: "name" or "binary". Returns false if the match is empty or already has a rule.
    Q_INVOKABLE bool editMatch(const QString &key, const QString &match, const QString &matchKey);
    // For apps that report no name: assign by binary and remember the user's name for it.
    Q_INVOKABLE void nameApp(const QString &key, const QString &label, const QString &busId);

Q_SIGNALS:
    void changed();
    void metersActiveChanged();
    void levelsChanged();

private:
    void rebuild();
    void tick();

    static Apps *s_instance;
    AppController *m_app = nullptr;
    pw::MeterBank m_meters;
    QTimer m_timer;
    QTimer m_relativeTimeTimer;
    QElapsedTimer m_clock;
    qint64 m_lastTick = 0;
    bool m_metersActive = false;
    QVariantList m_running;
    QVariantList m_rules;
    RowsModel m_runningModel;
    RowsModel m_rulesModel;
    QVariantList m_buses;
    QVariantMap m_unnamed;
    QHash<QString, QStringList> m_meterTargets; // app key -> "#id" targets
    QHash<QString, meters::State> m_state;
    QVariantMap m_levels;
};

} // namespace rostrum::app
