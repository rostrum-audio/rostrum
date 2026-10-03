#pragma once

#include <QString>
#include <optional>

namespace rostrum::remote {

// The D-Bus control interface (data/dev.getrostrum.Rostrum1.xml). The service name is the app id,
// owned by the running instance through KDBusService. KDBusService itself holds the object at
// /dev/getrostrum/Rostrum, so the interface lives one level below.
inline constexpr const char *kPath = "/dev/getrostrum/Rostrum/Control";
inline constexpr const char *kInterface = "dev.getrostrum.Rostrum1";
inline constexpr const char *kErrorUnknownAction = "dev.getrostrum.Rostrum1.Error.UnknownAction";
inline constexpr const char *kErrorNoSuchScene = "dev.getrostrum.Rostrum1.Error.NoSuchScene";
inline constexpr const char *kErrorNoSuchBus = "dev.getrostrum.Rostrum1.Error.NoSuchBus";
inline constexpr const char *kErrorInvalidValue = "dev.getrostrum.Rostrum1.Error.InvalidValue";
inline constexpr const char *kErrorHoldAction = "dev.getrostrum.Rostrum1.Error.HoldAction";

// Ids that name the masters where a bus id is expected. Neither can be a bus id (SceneToml).
inline constexpr const char *kStreamId = "stream";
inline constexpr const char *kPhonesId = "phones";

// Highest fader position for an id: mic gain goes to 150 %, everything else to 100 %.
double maxPosition(const QString &busId);

struct VolumeArg
{
    QString busId;
    double position = 0.0;
};

// "game=0.5" or "game=50%" from --set-volume. The range is checked by whoever applies it.
std::optional<VolumeArg> parseVolumeArg(const QString &text);

} // namespace rostrum::remote
