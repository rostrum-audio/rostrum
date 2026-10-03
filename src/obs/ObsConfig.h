#pragma once

#include <QList>
#include <QString>

#include <optional>

namespace rostrum::obs {

// Where an OBS install keeps its configuration. Rostrum only reads it, except for the scene
// collection while OBS is closed.
struct Install
{
    QString configDir; // .../obs-studio
    QString flavor;    // "native", "flatpak" or "snap"
};

// Installs whose config directory exists, newest first. `home` is overridable for tests.
QList<Install> findInstalls(const QString &home = QString());
// True when an OBS binary is installed even if it has never been run.
bool obsBinaryInstalled(const QString &home = QString());

struct WebSocketConfig
{
    bool found = false;
    bool enabled = false;
    quint16 port = 4455;
    bool authRequired = true;
    QString password;
};

// obs-websocket 5 settings: plugin_config/obs-websocket/config.json (OBS 30+), or the
// [OBSWebSocket] group of global.ini (OBS 28 and 29).
WebSocketConfig readWebSocketConfig(const QString &configDir);

// The active scene collection file, from user.ini (OBS 31+) or global.ini. Empty if unknown.
QString sceneCollectionFile(const QString &configDir);

// Whether an OBS process runs for this user (native or sandboxed: the process is "obs").
bool isObsRunning();

} // namespace rostrum::obs
