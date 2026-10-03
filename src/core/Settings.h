#pragma once

#include <QMap>
#include <QString>
#include <QStringList>

namespace rostrum {

// Global actions that can be bound to shortcuts. Ids are stable; they are written to settings.toml
// and registered with KGlobalAccel.
namespace actions {
inline constexpr const char *kMuteMic = "mute_mic";
inline constexpr const char *kMuteStream = "mute_stream";
inline constexpr const char *kPrevScene = "previous_scene";
inline constexpr const char *kNextScene = "next_scene";
inline constexpr const char *kScene1 = "scene_1";
inline constexpr const char *kScene2 = "scene_2";
inline constexpr const char *kScene3 = "scene_3";
inline constexpr const char *kScene4 = "scene_4";
QStringList all();
QString label(const QString &id);
QString defaultShortcut(const QString &id);
// "scene_3" -> 3, the position in the scene order it loads; 0 for any other action.
int sceneSlot(const QString &id);
int sceneSlotCount();
QString sceneSlotAction(int slot); // 3 -> "scene_3"
} // namespace actions

// Bumped when first-run setup gains a step that existing users should see once. 1 = devices and
// buses, 2 = startup, crash reports and updates.
inline constexpr int kSetupVersion = 2;
inline constexpr int kCompactMinWidth = 420;
inline constexpr int kCompactMinHeight = 220;

namespace crashmode {
inline constexpr const char *kSend = "send";
inline constexpr const char *kAsk = "ask";
inline constexpr const char *kNever = "never";
} // namespace crashmode

struct Settings
{
    // General
    bool wizardDone = false;
    int setupVersion = 0;
    bool launchAtLogin = false;
    bool startInTray = false;
    bool autoSaveScenes = true; // level changes save to the live scene by themselves
    bool confirmSceneSwitch = false; // only asked while auto-save is off
    bool scrollToAdjust = true;
    // Mixer
    QString meterSpeed = QStringLiteral("normal"); // "low" or "normal"
    bool showDb = false;
    // Apps
    bool autoAssign = true;  // place recognised apps on the bus for their kind
    QStringList autoSkip;    // app keys the user took off their automatic bus
    // Privacy: what happens to a crash report on the next start
    QString crashReports = QString::fromLatin1(crashmode::kAsk);
    // Updates
    bool checkUpdates = true;
    bool installUpdates = true; // only where Rostrum can replace itself (AppImage)
    QString skippedVersion;
    qint64 lastUpdateCheck = 0; // seconds since the epoch
    // Advanced
    bool showNodeIds = false;
    // Scenes
    QString defaultScene = QStringLiteral("Live");
    QStringList sceneOrder; // scene names in the user's order; scenes not listed follow by file name
    // Devices (PipeWire node.name; empty = system default)
    QString headphones;
    QString mic;
    // Shortcuts: action id -> portable key sequence ("Meta+Alt+M"); empty = unbound
    QMap<QString, QString> hotkeys;
    // Window
    int windowWidth = 1100;
    int windowHeight = 680;
    QString lastPage = QStringLiteral("mixer");
    bool sidebarCollapsed = false;
    bool compactWindow = false; // the small always-handy mixer instead of the full window
    int compactWidth = 460;
    int compactHeight = 320;
    bool keepOnTop = false; // compact window only

    bool operator==(const Settings &) const = default;
};

Settings defaultSettings();
QString serializeSettings(const Settings &settings);
Settings parseSettings(const QString &text, QString *error = nullptr);

Settings loadSettings(const QString &path, QString *error = nullptr);
bool saveSettings(const QString &path, const Settings &settings, QString *error = nullptr);

} // namespace rostrum
