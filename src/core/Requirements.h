#pragma once

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

// Plain-language text naming the packages to install on Kubuntu, Fedora and Arch.
QString installHint();

} // namespace rostrum::requirements
