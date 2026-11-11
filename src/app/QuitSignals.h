#pragma once

#include <QObject>

namespace rostrum::app {

// SIGTERM (logout, `systemctl --user stop`, `kill`), SIGINT (Ctrl+C) and SIGHUP (the terminal
// closed) quit through QCoreApplication::quit(), so the normal quit path runs: holds and transient
// levels are released and every app Rostrum muted is unmuted before it exits. A second signal
// while that runs ends the process at once.
void installQuitSignals(QObject *parent);

} // namespace rostrum::app
