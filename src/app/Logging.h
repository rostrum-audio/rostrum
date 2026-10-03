#pragma once

#include <QString>

namespace rostrum::logging {

// Sends Qt messages to stderr and to $XDG_STATE_HOME/rostrum/rostrum.log, rotating the log
// to rostrum.log.1 when it grows past 1 MiB. Installs a crash handler that appends a backtrace
// to the same file. Backtraces never reach the UI.
void install(const QString &logFile);

} // namespace rostrum::logging
