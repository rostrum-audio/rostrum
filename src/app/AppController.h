#pragma once

#include "core/Settings.h"
#include "engine/Engine.h"
#include "engine/SceneManager.h"
#include "pw/PwContext.h"

#include <QObject>
#include <QTimer>
#include <QtQml/qqmlregistration.h>

class QQmlEngine;
class QJSEngine;

namespace rostrum::app {

// The one object QML talks to for app-wide state: PipeWire health, devices, scenes, the mic
// button and window state. Pages get their own models in later slices.
class AppController : public QObject
{
    Q_OBJECT
    QML_NAMED_ELEMENT(App)
    QML_SINGLETON

    // "connecting", "ok", "degraded" or "missing"
    Q_PROPERTY(QString pipewireState READ pipewireState NOTIFY statusChanged)
    Q_PROPERTY(QString pipewireStatusText READ pipewireStatusText NOTIFY statusChanged)
    Q_PROPERTY(QString pipewireDetail READ pipewireDetail NOTIFY statusChanged)
    Q_PROPERTY(QString installHint READ installHint CONSTANT)
    Q_PROPERTY(bool connected READ connected NOTIFY statusChanged)
    Q_PROPERTY(bool hasWirePlumber READ hasWirePlumber NOTIFY statusChanged)
    Q_PROPERTY(QString pipewireVersion READ pipewireVersion NOTIFY statusChanged)
    Q_PROPERTY(QString wireplumberVersion READ wireplumberVersion NOTIFY statusChanged)
    Q_PROPERTY(QString version READ version CONSTANT)

    Q_PROPERTY(QString headphonesText READ headphonesText NOTIFY devicesChanged)
    Q_PROPERTY(QString micText READ micText NOTIFY devicesChanged)
    Q_PROPERTY(bool headphonesMissing READ headphonesMissing NOTIFY devicesChanged)
    Q_PROPERTY(bool micMissing READ micMissing NOTIFY devicesChanged)
    Q_PROPERTY(bool hasMic READ hasMic NOTIFY devicesChanged)

    Q_PROPERTY(bool micMuted READ micMuted WRITE setMicMuted NOTIFY levelsChanged)
    Q_PROPERTY(double micGain READ micGain WRITE setMicGain NOTIFY levelsChanged)
    Q_PROPERTY(bool sidetoneEnabled READ sidetoneEnabled WRITE setSidetoneEnabled NOTIFY levelsChanged)
    Q_PROPERTY(double sidetoneVolume READ sidetoneVolume WRITE setSidetoneVolume NOTIFY levelsChanged)

    Q_PROPERTY(QStringList sceneNames READ sceneNames NOTIFY scenesChanged)
    Q_PROPERTY(QString currentScene READ currentScene NOTIFY scenesChanged)
    Q_PROPERTY(QString defaultScene READ defaultScene NOTIFY scenesChanged)
    Q_PROPERTY(bool sceneDirty READ sceneDirty NOTIFY scenesChanged)

    Q_PROPERTY(bool mixEnabled READ mixEnabled NOTIFY mixChanged)
    Q_PROPERTY(bool mixReady READ mixReady NOTIFY mixChanged)
    Q_PROPERTY(bool mixBusy READ mixBusy NOTIFY mixChanged)
    Q_PROPERTY(QString mixError READ mixError NOTIFY mixChanged)

    Q_PROPERTY(QString lastPage READ lastPage WRITE setLastPage NOTIFY windowStateChanged)
    Q_PROPERTY(bool sidebarCollapsed READ sidebarCollapsed WRITE setSidebarCollapsed NOTIFY windowStateChanged)
    Q_PROPERTY(int windowWidth READ windowWidth WRITE setWindowWidth NOTIFY windowStateChanged)
    Q_PROPERTY(int windowHeight READ windowHeight WRITE setWindowHeight NOTIFY windowStateChanged)
    Q_PROPERTY(bool confirmSceneSwitch READ confirmSceneSwitch NOTIFY settingsChanged)
    Q_PROPERTY(bool wizardDone READ wizardDone NOTIFY settingsChanged)
    // Bus id the Apps page filters to when opened from a strip's "+N"; empty = all.
    Q_PROPERTY(QString appsFilter READ appsFilter WRITE setAppsFilter NOTIFY appsFilterChanged)

public:
    // No default argument: QML must get the one instance through create(), never construct it.
    explicit AppController(QObject *parent);
    ~AppController() override;

    static AppController *instance() { return s_instance; }
    static AppController *create(QQmlEngine *, QJSEngine *);

    // Loads settings and scenes, then connects to PipeWire.
    void start();

    pw::PwContext *pw() { return &m_pw; }
    engine::Engine *engine() { return &m_engine; }
    engine::SceneManager *scenes() { return &m_scenes; }
    Settings &settings() { return m_settings; }
    void saveSettingsSoon();

    QString pipewireState() const { return m_status; }
    QString pipewireStatusText() const;
    QString pipewireDetail() const { return m_detail; }
    QString installHint() const;
    bool connected() const;
    bool hasWirePlumber() const;
    QString pipewireVersion() const;
    QString wireplumberVersion() const;
    QString version() const;

    QString headphonesText() const;
    QString micText() const;
    bool headphonesMissing() const;
    bool micMissing() const;
    bool hasMic() const;

    bool micMuted() const;
    void setMicMuted(bool muted);
    double micGain() const;
    void setMicGain(double position);
    bool sidetoneEnabled() const;
    void setSidetoneEnabled(bool on);
    double sidetoneVolume() const;
    void setSidetoneVolume(double position);

    QStringList sceneNames() const;
    QString currentScene() const;
    QString defaultScene() const;
    bool sceneDirty() const;

    bool mixEnabled() const;
    bool mixReady() const;
    bool mixBusy() const;
    QString mixError() const;

    QString lastPage() const { return m_settings.lastPage; }
    void setLastPage(const QString &page);
    bool sidebarCollapsed() const { return m_settings.sidebarCollapsed; }
    void setSidebarCollapsed(bool collapsed);
    int windowWidth() const { return m_settings.windowWidth; }
    void setWindowWidth(int w);
    int windowHeight() const { return m_settings.windowHeight; }
    void setWindowHeight(int h);
    bool confirmSceneSwitch() const { return m_settings.confirmSceneSwitch; }
    bool wizardDone() const { return m_settings.wizardDone; }
    QString appsFilter() const { return m_appsFilter; }
    void setAppsFilter(const QString &busId)
    {
        if (busId != m_appsFilter) {
            m_appsFilter = busId;
            Q_EMIT appsFilterChanged();
        }
    }

    Q_INVOKABLE void retry();
    Q_INVOKABLE void toggleMicMute();
    Q_INVOKABLE bool switchScene(const QString &name);
    Q_INVOKABLE bool saveScene();
    Q_INVOKABLE void createMix();
    // Wizard: finish once the mix exists; skip creates the mix with defaults and finishes too.
    Q_INVOKABLE void finishWizard();
    Q_INVOKABLE void skipWizard();
    Q_INVOKABLE void copyToClipboard(const QString &text, const QString &toastText = QString());
    Q_INVOKABLE void saveSettingsNow();
    Q_INVOKABLE void quit();

Q_SIGNALS:
    void statusChanged();
    void devicesChanged();
    void levelsChanged();
    void scenesChanged();
    void mixChanged();
    void windowStateChanged();
    void settingsChanged();
    void appsFilterChanged();
    void toast(const QString &message);
    void raiseRequested();
    void headphonesLost(const QString &description);

private:
    void updateStatus();
    void onConnectionChanged();
    QString describeNode(const QString &nodeName) const;

    static AppController *s_instance;

    pw::PwContext m_pw;
    engine::Engine m_engine;
    engine::SceneManager m_scenes;
    Settings m_settings;
    QString m_status = QStringLiteral("connecting");
    QString m_detail;
    bool m_versionRefused = false;
    bool m_ruleExportOn = false;
    QString m_appsFilter;
    QTimer m_saveTimer;
    QTimer m_retryTimer;
};

} // namespace rostrum::app
