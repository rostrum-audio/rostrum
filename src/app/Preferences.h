#pragma once

#include <QKeySequence>
#include <QObject>
#include <QUrl>
#include <QVariantList>
#include <QtQml/qqmlregistration.h>

class QQmlEngine;
class QJSEngine;

namespace rostrum::app {

class AppController;

// The Settings page's view of settings.toml, plus the Advanced actions.
class Preferences : public QObject
{
    Q_OBJECT
    QML_ELEMENT
    QML_SINGLETON

    Q_PROPERTY(bool autoSaveScenes READ autoSaveScenes WRITE setAutoSaveScenes NOTIFY changed)
    Q_PROPERTY(bool confirmSceneSwitch READ confirmSceneSwitch WRITE setConfirmSceneSwitch NOTIFY changed)
    Q_PROPERTY(bool scrollToAdjust READ scrollToAdjust WRITE setScrollToAdjust NOTIFY changed)
    Q_PROPERTY(bool osdFeedback READ osdFeedback WRITE setOsdFeedback NOTIFY changed)
    Q_PROPERTY(bool lowMeterSpeed READ lowMeterSpeed WRITE setLowMeterSpeed NOTIFY changed)
    Q_PROPERTY(bool showDb READ showDb WRITE setShowDb NOTIFY changed)
    Q_PROPERTY(bool showNodeIds READ showNodeIds WRITE setShowNodeIds NOTIFY changed)
    Q_PROPERTY(bool autoAssign READ autoAssign WRITE setAutoAssign NOTIFY changed)
    // Apps the user took off their automatic bus
    Q_PROPERTY(int skippedApps READ skippedApps NOTIFY changed)
    // Rows: {id, label, description, group, hold, shortcut, defaultShortcut, global, problem}.
    // group: "mic", "stream", "scenes" or "buses". hold: acts while the key is held. global: the
    // desktop delivers the shortcut while Rostrum is not focused. problem: why a bound shortcut is
    // not global.
    Q_PROPERTY(QVariantList hotkeys READ hotkeys NOTIFY changed)
    Q_PROPERTY(QString configFolder READ configFolder CONSTANT)
    Q_PROPERTY(QString logFile READ logFile CONSTANT)
    Q_PROPERTY(QStringList ruleFiles READ ruleFiles CONSTANT)
    // file:// URL of the installed README, or empty if it is not installed.
    Q_PROPERTY(QUrl readmeUrl READ readmeUrl CONSTANT)

public:
    Preferences(AppController *app, QObject *parent);
    ~Preferences() override;

    static Preferences *create(QQmlEngine *, QJSEngine *);
    // busName: for a bus mute action, the name to show; empty looks it up in the running app.
    static QString actionLabel(const QString &id, const QString &busName = QString());
    static QString actionDescription(const QString &id);

    bool autoSaveScenes() const;
    void setAutoSaveScenes(bool on);
    bool confirmSceneSwitch() const;
    void setConfirmSceneSwitch(bool on);
    bool scrollToAdjust() const;
    void setScrollToAdjust(bool on);
    bool osdFeedback() const;
    void setOsdFeedback(bool on);
    bool lowMeterSpeed() const;
    void setLowMeterSpeed(bool on);
    bool showDb() const;
    void setShowDb(bool on);
    bool showNodeIds() const;
    void setShowNodeIds(bool on);
    bool autoAssign() const;
    void setAutoAssign(bool on);
    int skippedApps() const;
    QVariantList hotkeys() const;
    QString configFolder() const;
    QString logFile() const;
    QStringList ruleFiles() const;
    QUrl readmeUrl() const;

    // Portable key sequence ("Meta+Alt+M"); empty clears the binding.
    Q_INVOKABLE void setHotkey(const QString &actionId, const QString &sequence);
    Q_INVOKABLE void setHotkeySequence(const QString &actionId, const QKeySequence &sequence);
    Q_INVOKABLE void resetHotkey(const QString &actionId);
    // The action already using this sequence, or empty.
    Q_INVOKABLE QString hotkeyConflict(const QString &actionId, const QString &sequence) const;
    Q_INVOKABLE void openConfigFolder();
    Q_INVOKABLE void forgetSkippedApps();
    Q_INVOKABLE void rebuildMix();
    Q_INVOKABLE void exportRulesNow();

Q_SIGNALS:
    void changed();

private:
    template<typename T>
    void update(T &field, const T &value);

    static Preferences *s_instance;
    AppController *m_app = nullptr;
};

} // namespace rostrum::app
