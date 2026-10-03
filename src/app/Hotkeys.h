#pragma once

#include <QDBusObjectPath>
#include <QHash>
#include <QMap>
#include <QObject>
#include <QVariantMap>

class QAction;

namespace rostrum::app {

// Global shortcuts. On Plasma they go through KGlobalAccel, so they show up in System Settings →
// Shortcuts; elsewhere through the XDG GlobalShortcuts portal. Wayland never lets an app grab
// keys itself, so when neither is there only the in-window shortcuts work.
class Hotkeys : public QObject
{
    Q_OBJECT
public:
    enum class Backend
    {
        None,
        KGlobalAccel,
        Portal
    };

    explicit Hotkeys(QObject *parent = nullptr);
    ~Hotkeys() override;

    Backend backend() const { return m_backend; }
    // action id -> portable key sequence ("Meta+Alt+M"); empty = unbound.
    void apply(const QMap<QString, QString> &bindings, const QMap<QString, QString> &labels);

    // True when the desktop delivers this action's shortcut even while Rostrum is not focused.
    bool isGlobal(const QString &id) const { return m_global.value(id); }
    // Why a bound shortcut is not global, in words for the Settings page; empty if it is.
    QString problem(const QString &id) const { return m_problem.value(id); }

Q_SIGNALS:
    void triggered(const QString &id);
    void statusChanged();
    // The user rebound an action in System Settings.
    void changedExternally(const QString &id, const QString &portableSequence);

private Q_SLOTS:
    void onPortalResponse(uint response, const QVariantMap &results);
    void onPortalActivated(const QDBusObjectPath &session, const QString &id, qulonglong timestamp,
                           const QVariantMap &options);

private:
    void applyKGlobalAccel();
    void applyPortal();
    void portalCreateSession();
    void portalBind();
    QString portalRequestPath(const QString &token) const;
    void setStatus(const QString &id, bool global, const QString &problem);

    Backend m_backend = Backend::None;
    bool m_disabled = false;
    QMap<QString, QString> m_bindings;
    QMap<QString, QString> m_labels;
    QHash<QString, QAction *> m_actions;
    QHash<QString, bool> m_global;
    QHash<QString, QString> m_problem;
    bool m_applying = false;

    enum class PortalStep
    {
        Idle,
        CreatingSession,
        Binding
    };
    PortalStep m_portalStep = PortalStep::Idle;
    QString m_portalSession;
    QString m_portalRequest;
    bool m_portalRebind = false;
    int m_tokenCounter = 0;
};

} // namespace rostrum::app
