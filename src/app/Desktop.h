#pragma once

#include <QObject>
#include <QPointer>
#include <QtQml/qqmlregistration.h>

class QQmlEngine;
class QJSEngine;
class QQuickWindow;

namespace rostrum::app {

class AppController;
class Hotkeys;
class Tray;

// Everything that talks to the desktop rather than PipeWire: the tray, global shortcuts, the
// autostart file, notifications and on-screen feedback. Offscreen runs (screenshots, tests) skip
// all of it.
class Desktop : public QObject
{
    Q_OBJECT
    QML_ELEMENT
    QML_SINGLETON

    Q_PROPERTY(bool trayAvailable READ trayAvailable NOTIFY trayChanged)
    Q_PROPERTY(bool launchAtLogin READ launchAtLogin WRITE setLaunchAtLogin NOTIFY changed)
    Q_PROPERTY(bool startInTray READ startInTray WRITE setStartInTray NOTIFY changed)
    // Hide the window at launch: start in tray is on, a tray exists and the wizard is done.
    Q_PROPERTY(bool startHidden READ startHidden CONSTANT)
    // "kglobalaccel", "portal" or "none"
    Q_PROPERTY(QString shortcutBackend READ shortcutBackend CONSTANT)

public:
    Desktop(AppController *app, QObject *parent);
    ~Desktop() override;

    static Desktop *instance() { return s_instance; }
    static Desktop *create(QQmlEngine *, QJSEngine *);

    void setWindow(QQuickWindow *window);
    Hotkeys *hotkeys() const { return m_hotkeys; }

    bool trayAvailable() const { return m_tray != nullptr; }
    bool launchAtLogin() const;
    void setLaunchAtLogin(bool on);
    bool startInTray() const;
    void setStartInTray(bool on);
    bool startHidden() const { return m_startHidden; }
    QString shortcutBackend() const;

Q_SIGNALS:
    void trayChanged();
    void changed();

private Q_SLOTS:
    void checkTray();

private:
    void applyHotkeys();
    void notifyHeadphonesLost(const QString &description);
    void showFeedback(const QString &iconName, const QString &text);
    void notifyFeedback(const QString &iconName, const QString &text);

    static Desktop *s_instance;
    AppController *m_app = nullptr;
    Hotkeys *m_hotkeys = nullptr;
    Tray *m_tray = nullptr;
    QPointer<QQuickWindow> m_window;
    bool m_enabled = false;
    bool m_startHidden = false;
    uint m_notificationId = 0;
    uint m_feedbackNotificationId = 0;
};

} // namespace rostrum::app
