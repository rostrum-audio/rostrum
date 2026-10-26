#pragma once

#include <QString>

namespace rostrum::autostart {

// Quotes one Exec argument per the Desktop Entry spec. If forceQuote is true,
// wraps the string in quotes even without reserved characters (e.g. for AppImage paths).
QString execQuote(const QString &arg, bool forceQuote = false);

// Resolves the stable executable path to launch on login.
// Rejects temporary /tmp/.mount_ AppImage mount paths, mount argv[0], and build-tree paths.
// For AppImages, returns the AppImage path.
// For installed binaries (e.g. ~/.local/bin/rostrum or /usr/bin/rostrum), returns the absolute path.
QString launcherPath(const QString &self, const QString &appImage = QString());

// Produces the Exec= command line (quoted launcher + " --autostart"), or empty if `launcher`
// cannot be represented as a resolvable Desktop Entry executable path.
QString execLine(const QString &launcher);

// The XDG autostart entry. `exec` is already quoted; it should end in --autostart so the app
// can tell a login start from a menu start. `tryExec` is the raw launcher executable path
// (escaped as a Desktop Entry `string` value when written).
QString entry(const QString &exec, const QString &comment, const QString &tryExec = QString());

// True when `existing`'s [Desktop Entry] group has the exact Exec, TryExec, and StartupWMClass
// values expected for `launcher`.
bool isCurrent(const QString &existing, const QString &launcher);

} // namespace rostrum::autostart
