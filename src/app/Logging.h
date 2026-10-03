#pragma once

#include <QString>

namespace rostrum::logging {

// Sends Qt messages to stderr and to $XDG_STATE_HOME/rostrum/rostrum.log, rotating the log
// to rostrum.log.1 when it grows past 1 MiB. Installs a crash handler that appends a backtrace
// to the same file and writes a crash file into crashDir for the crash reporter. Backtraces
// never reach the UI.
void install(const QString &logFile, const QString &crashDir);

// "key: value" lines copied into every crash file (versions, desktop). Values must not contain
// anything personal; the reporter only sends whitelisted keys, sanitized, either way.
void setCrashContext(const QString &key, const QString &value);

// GNU build id of the running executable, lower-case hex; empty if it was linked without one.
QString buildId();

} // namespace rostrum::logging
