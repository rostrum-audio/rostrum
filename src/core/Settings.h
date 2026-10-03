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
} // namespace actions

struct Settings
{
    // General
    bool wizardDone = false;
    bool launchAtLogin = false;
    bool startInTray = false;
    bool confirmSceneSwitch = false;
    bool scrollToAdjust = true;
    // Mixer
    QString meterSpeed = QStringLiteral("normal"); // "low" or "normal"
    bool showDb = false;
    // Advanced
    bool showNodeIds = false;
    // Scenes
    QString defaultScene = QStringLiteral("Live");
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

    bool operator==(const Settings &) const = default;
};

Settings defaultSettings();
QString serializeSettings(const Settings &settings);
Settings parseSettings(const QString &text, QString *error = nullptr);

Settings loadSettings(const QString &path, QString *error = nullptr);
bool saveSettings(const QString &path, const Settings &settings, QString *error = nullptr);

} // namespace rostrum
