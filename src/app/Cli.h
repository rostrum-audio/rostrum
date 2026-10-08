#pragma once

#include "core/Remote.h"

#include <QList>
#include <QStringList>
#include <optional>

class QCommandLineParser;

namespace rostrum::app {

class DBusControl;

// rostrum --scene, --action, --mute-mic, --set-volume, --list-… With Rostrum running, these go to
// it over D-Bus and this process exits. Without it, lists are read from the files on disk and
// control options start Rostrum and are applied once its scenes are loaded. --no-start instead
// exits with status 2 and never constructs the mixer.
namespace cli {

inline constexpr int kExitOk = 0;
inline constexpr int kExitRefused = 1;     // bad arguments, or Rostrum said no (unknown scene, bus or action)
inline constexpr int kExitUnreachable = 2; // Rostrum is absent (--no-start) or did not answer

struct Request
{
    QString scene;
    std::optional<bool> micMuted;
    bool toggleMic = false;
    QList<remote::VolumeArg> volumes;
    QStringList actions;
    bool listScenes = false;
    bool listActions = false;
    bool listBuses = false;
    QStringList errors; // bad arguments, in words

    bool hasControls() const;
    bool hasQueries() const;
};

// False when the arguments only print and exit (help, version, lists), so those work without a
// display. Decided before QApplication exists, so it looks at argv itself.
bool needsGui(int argc, char **argv);
void addOptions(QCommandLineParser &parser);
Request parse(const QCommandLineParser &parser);
bool instanceRunning();
// Sends the request to the running instance and prints the answers. Returns the exit status.
int forward(const Request &request);
// Prints the lists from the scene files, for when Rostrum is not running.
int answerOffline(const Request &request);
// Applies the control options in this process, which is the running instance.
void apply(const Request &request, DBusControl &control);

// Hidden options a restart gives the new copy: wait for the old process to exit first, and stay in
// the tray if the window was hidden.
inline constexpr char kRestartAfter[] = "restart-after";
inline constexpr char kStartHidden[] = "start-hidden";
// The program to start again: the AppImage itself rather than its mount, and the installed path
// even after a reinstall replaced the running binary.
QString restartProgram();
// Arguments for the new copy of this process.
QStringList restartArguments(bool hidden);
// Waits until `pid` has exited, at most `timeoutMs`. False if it is still running.
bool waitForExit(qint64 pid, int timeoutMs);

} // namespace cli
} // namespace rostrum::app
