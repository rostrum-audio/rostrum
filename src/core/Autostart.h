#pragma once

#include <QString>

namespace rostrum::autostart {

// Quotes one Exec argument per the Desktop Entry spec, e.g. a build path with a space.
QString execQuote(const QString &arg);

// The XDG autostart entry. `exec` is already quoted; it should end in --autostart so the app
// can tell a login start from a menu start.
QString entry(const QString &exec, const QString &comment);

} // namespace rostrum::autostart
