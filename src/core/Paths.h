#pragma once

#include <QString>

namespace rostrum::paths {

// All paths honour the XDG base directory variables and fall back to the spec defaults.
QString configHome();
QString stateHome();
QString dataHome();

QString configDir();   // $XDG_CONFIG_HOME/rostrum
QString scenesDir();   // $XDG_CONFIG_HOME/rostrum/scenes
QString settingsFile(); // $XDG_CONFIG_HOME/rostrum/settings.toml
QString trashDir();     // $XDG_CONFIG_HOME/rostrum/trash (deleted scenes, purged after 30 days)
QString backupsDir();   // $XDG_CONFIG_HOME/rostrum/backups
QString stateDir();    // $XDG_STATE_HOME/rostrum
QString logFile();     // $XDG_STATE_HOME/rostrum/rostrum.log
QString crashDir();    // $XDG_STATE_HOME/rostrum/crashes
QString sentryDir();   // $XDG_STATE_HOME/rostrum/sentry
QString dspDir();      // $XDG_DATA_HOME/rostrum/dsp (AppImage copies of the mic filter plugin)
QString autostartFile();

QString pipewirePulseFragment(); // ~/.config/pipewire/pipewire-pulse.conf.d/50-rostrum.conf
QString pipewireClientFragment(); // ~/.config/pipewire/client.conf.d/50-rostrum.conf

} // namespace rostrum::paths
