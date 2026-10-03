#pragma once

#include <QDBusConnection>
#include <QDBusContext>
#include <QHash>
#include <QList>
#include <QMetaType>
#include <QObject>
#include <QSet>
#include <QStringList>
#include <QVariantMap>

class QDBusArgument;
class QDBusServiceWatcher;

namespace rostrum {
struct Scene;
}

namespace rostrum::app {

class AppController;

struct BusInfo
{
    QString id;
    QString name;
    double position = 0.0;
    bool muted = false;
};
using BusInfoList = QList<BusInfo>;

struct ActionInfo
{
    QString id;
    QString label;
};
using ActionInfoList = QList<ActionInfo>;

QDBusArgument &operator<<(QDBusArgument &arg, const BusInfo &b);
const QDBusArgument &operator>>(const QDBusArgument &arg, BusInfo &b);
QDBusArgument &operator<<(QDBusArgument &arg, const ActionInfo &a);
const QDBusArgument &operator>>(const QDBusArgument &arg, ActionInfo &a);

// dev.getrostrum.Rostrum1 at /dev/getrostrum/Rostrum/Control, described in data/dev.getrostrum.Rostrum1.xml.
// The command line and scripts (Stream Deck, KDE Connect, keyboard daemons) drive the running
// instance through it. Calls never open a dialog: a scene switch happens at once, and with
// auto-save off unsaved fader moves are dropped, as the Switch button in the dialog would.
class DBusControl : public QObject, protected QDBusContext
{
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "dev.getrostrum.Rostrum1")
    Q_PROPERTY(bool MicMuted READ micMuted)
    Q_PROPERTY(bool StreamMuted READ streamMuted)
    Q_PROPERTY(bool Panic READ panic)
    Q_PROPERTY(QString CurrentScene READ currentScene)
    Q_PROPERTY(bool Connected READ connected)

public:
    // An error to send back, or to print when the command line runs in this process.
    struct Error
    {
        QString name; // empty = no error
        QString message;
        explicit operator bool() const { return !name.isEmpty(); }
    };

    explicit DBusControl(AppController *app, QObject *parent = nullptr);
    ~DBusControl() override;

    bool registerOn(QDBusConnection bus);

    bool micMuted() const;
    bool streamMuted() const;
    bool panic() const;
    QString currentScene() const;
    bool connected() const;

    Error triggerAction(const QString &id);
    Error switchScene(const QString &name);
    Error setBusVolume(const QString &busId, double position);
    Error setBusMuted(const QString &busId, bool muted);
    Error toggleBusMuted(const QString &busId);
    void setMicMuted(bool muted);
    void toggleMicMute();
    // Stream and Headphones masters, the mic, then the playback buses, as saved in `scene`.
    static BusInfoList busesOf(const Scene &scene);
    BusInfoList buses() const; // the live scene, with holds and panic applied
    ActionInfoList actionList() const;

public Q_SLOTS:
    void TriggerAction(const QString &id);
    void PressAction(const QString &id);
    void ReleaseAction(const QString &id);
    void SwitchScene(const QString &name);
    void SetBusVolume(const QString &busId, double position);
    void SetBusMuted(const QString &busId, bool muted);
    void ToggleBusMuted(const QString &busId);
    void SetMicMuted(bool muted);
    void ToggleMicMute();
    QStringList ListScenes();
    rostrum::app::BusInfoList ListBuses();
    rostrum::app::ActionInfoList ListActions();

private:
    void reply(const Error &e);
    QString resolveBus(const QString &text) const;
    Error checkKnown(const QString &id) const;
    void releaseHoldsOf(const QString &service);
    void publishChanges();

    AppController *m_app = nullptr;
    QDBusConnection m_bus;
    QDBusServiceWatcher *m_watcher = nullptr;
    QHash<QString, QSet<QString>> m_holds; // caller's unique name -> hold actions it pressed
    QVariantMap m_published;
};

} // namespace rostrum::app

Q_DECLARE_METATYPE(rostrum::app::BusInfo)
Q_DECLARE_METATYPE(rostrum::app::ActionInfo)
