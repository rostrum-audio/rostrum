#pragma once

#include <QList>
#include <QString>

namespace rostrum::requirements {

inline constexpr int kMinPipeWireMajor = 1;
inline constexpr int kMinPipeWireMinor = 0;
inline constexpr int kMinWirePlumberMajor = 0;
inline constexpr int kMinWirePlumberMinor = 5;

struct Version
{
    int major = 0;
    int minor = 0;
    int micro = 0;
    bool valid = false;
};

// Parses "1.6.2", "0.5.13", "1.0.0-rc1" or "Linked with libpipewire 1.6.2".
Version parseVersion(const QString &text);
bool atLeast(const Version &v, int major, int minor);

bool pipewireOk(const QString &serverVersion);
bool wireplumberOk(const QString &version);

// Starts the audio services for the user session. Usually all that is missing on Kubuntu.
QString startCommand();

struct InstallCommand
{
    QString distro;
    QString command;
};
// The packages to install on Kubuntu, Fedora and Arch.
QList<InstallCommand> installCommands();

// The same commands as plain text for a terminal.
QString installHint();

enum class ConnectProblem { NotRunning, NotAnswering, NotAllowed, Other };
// What a failed connect means, from its errno. libpipewire reports a missing socket as EHOSTDOWN.
ConnectProblem connectProblem(int err);

} // namespace rostrum::requirements
