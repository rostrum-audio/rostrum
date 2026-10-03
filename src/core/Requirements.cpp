#include "core/Requirements.h"

#include <QRegularExpression>

#include <cerrno>

namespace rostrum::requirements {

Version parseVersion(const QString &text)
{
    static const QRegularExpression re(QStringLiteral(R"((\d+)\.(\d+)(?:\.(\d+))?)"));
    const auto m = re.match(text);
    Version v;
    if (!m.hasMatch()) {
        return v;
    }
    v.major = m.captured(1).toInt();
    v.minor = m.captured(2).toInt();
    v.micro = m.captured(3).isEmpty() ? 0 : m.captured(3).toInt();
    v.valid = true;
    return v;
}

bool atLeast(const Version &v, int major, int minor)
{
    if (!v.valid) {
        return false;
    }
    return v.major > major || (v.major == major && v.minor >= minor);
}

bool pipewireOk(const QString &serverVersion)
{
    return atLeast(parseVersion(serverVersion), kMinPipeWireMajor, kMinPipeWireMinor);
}

bool wireplumberOk(const QString &version)
{
    return atLeast(parseVersion(version), kMinWirePlumberMajor, kMinWirePlumberMinor);
}

QString startCommand()
{
    return QStringLiteral("systemctl --user enable --now pipewire pipewire-pulse wireplumber");
}

QList<InstallCommand> installCommands()
{
    return {
        {QStringLiteral("Kubuntu, Ubuntu, Debian"), QStringLiteral("sudo apt install pipewire pipewire-pulse wireplumber")},
        {QStringLiteral("Fedora"), QStringLiteral("sudo dnf install pipewire pipewire-pulseaudio wireplumber")},
        {QStringLiteral("Arch"), QStringLiteral("sudo pacman -S pipewire pipewire-pulse wireplumber")},
    };
}

QString installHint()
{
    QString out = QStringLiteral("Rostrum needs PipeWire 1.0+ with WirePlumber 0.5+.\n"
                                 "Start it for your session:\n  %1\n"
                                 "If it is not installed, install it, then log out and back in:\n")
                      .arg(startCommand());
    for (const auto &c : installCommands()) {
        out += QStringLiteral("  %1:\n    %2\n").arg(c.distro, c.command);
    }
    return out.trimmed();
}

ConnectProblem connectProblem(int err)
{
    switch (err) {
    case EHOSTDOWN:
    case ENOENT:
        return ConnectProblem::NotRunning;
    case ECONNREFUSED:
        return ConnectProblem::NotAnswering;
    case EACCES:
    case EPERM:
        return ConnectProblem::NotAllowed;
    default:
        return ConnectProblem::Other;
    }
}

} // namespace rostrum::requirements
