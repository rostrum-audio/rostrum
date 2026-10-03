#include "core/Requirements.h"

#include <QRegularExpression>

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

QString installHint()
{
    return QStringLiteral("Rostrum needs PipeWire 1.0+ with WirePlumber 0.5+.\n"
                          "  Kubuntu / Ubuntu / Debian:  sudo apt install pipewire pipewire-pulse wireplumber\n"
                          "  Fedora:                     sudo dnf install pipewire pipewire-pulseaudio wireplumber\n"
                          "  Arch:                       sudo pacman -S pipewire pipewire-pulse wireplumber\n"
                          "Then log out and back in, or run: systemctl --user enable --now pipewire "
                          "pipewire-pulse wireplumber");
}

} // namespace rostrum::requirements
