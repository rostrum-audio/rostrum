#pragma once

#include "core/Ducking.h"
#include "core/MicFilters.h"

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
inline constexpr const char *kPushToTalk = "push_to_talk";
inline constexpr const char *kPushToMute = "push_to_mute";
inline constexpr const char *kPanicMute = "panic_mute";
inline constexpr const char *kToggleSidetone = "toggle_sidetone";
inline constexpr const char *kMuteHeadphones = "mute_headphones";
inline constexpr const char *kStreamVolumeUp = "stream_volume_up";
inline constexpr const char *kStreamVolumeDown = "stream_volume_down";
inline constexpr const char *kScene5 = "scene_5";
inline constexpr const char *kScene6 = "scene_6";
inline constexpr const char *kScene7 = "scene_7";
inline constexpr const char *kScene8 = "scene_8";
inline constexpr const char *kToggleMicFilters = "toggle_mic_filters";
// Starts a mic check, or stops the one running. Handled by the app, not the engine.
inline constexpr const char *kMicCheck = "mic_check";
// "mute_bus_<bus id>" toggles one playback bus. There is one per bus id found in any scene, so
// these are not in all().
inline constexpr const char *kMuteBusPrefix = "mute_bus_";

enum class Group
{
    Mic,
    Stream,
    Scenes,
    Buses
};

// The fixed actions, in the order Settings lists them. The first three are shown in the wizard.
QStringList all();
QString label(const QString &id);
QString defaultShortcut(const QString &id);
// Acts while the key is held (push to talk, push to mute) instead of on each press.
bool isHold(const QString &id);
Group group(const QString &id);
QString muteBusAction(const QString &busId);
// The bus a "mute_bus_<id>" action toggles; empty for any other id.
QString busOfAction(const QString &id);
// "scene_3" -> 3, the 1-based position in the scene order it loads; 0 for any other id.
int sceneSlot(const QString &id);
// How many scene_<n> actions all() has.
int sceneSlotCount();
QString sceneSlotAction(int slot); // 3 -> "scene_3"
// A fixed action or a well-formed bus mute action.
bool isKnown(const QString &id);
} // namespace actions

// Bumped when first-run setup gains a step that existing users should see once. 1 = devices and
// buses, 2 = startup, crash reports and updates, 3 = OBS.
inline constexpr int kSetupVersion = 3;

// Scene fade lengths offered in Settings; 0 = switch instantly.
inline constexpr int kSceneFadeChoicesMs[] = {0, 150, 300, 600, 1000};
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
    bool closeToTray = true;     // closing the window hides it while a tray is shown
    bool minimizeToTray = false; // minimizing hides it to the tray instead of the task bar
    bool autoSaveScenes = true; // level changes save to the live scene by themselves
    bool confirmSceneSwitch = false; // only asked while auto-save is off
    bool scrollToAdjust = true;
    bool osdFeedback = true; // show mic, panic and scene changes from hotkeys on screen
    int sceneFadeMs = 0; // one of kSceneFadeChoicesMs
    // Mixer
    QString meterSpeed = QStringLiteral("normal"); // "low" or "normal"
    bool showDb = false;
    ducking::Settings ducking;
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
    // OpenDeck: an explicit plugins folder overrides detection until cleared.
    QString openDeckPluginsFolder;
    QString openDeckInstallation; // native or flatpak, when both were detected
    // OBS
    bool obsBackground = true; // follow OBS over obs-websocket on localhost while OBS runs
    bool obsGoLiveWarnings = true;
    QMap<QString, QString> obsSceneMap; // OBS scene name -> Rostrum scene name
    // Advanced
    bool showNodeIds = false;
    // Scenes
    QString defaultScene = QStringLiteral("Live");
    QStringList sceneOrder; // scene names in the user's order; scenes not listed follow by file name
    // Devices (PipeWire node.name; empty = system default)
    QString headphones;
    QString mic;
    bool micFallback = false; // another mic stands in while the saved one is unplugged
    bool monoHeadphones = false;
    // Mic filters
    micfx::Settings micFilters;
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
