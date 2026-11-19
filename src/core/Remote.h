#pragma once

#include "core/Model.h"

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

// A bus id or name (case-insensitive, id first), or kStreamId / kPhonesId for the masters. Empty if
// nothing matches.
QString resolveBusId(const Scene &scene, const QString &text);

struct MixTarget
{
    QString bus;
    std::optional<Mix> mix;
};
// "game.stream" -> {"game", Stream}; "Game.Phones" -> {"Game", Phones}; anything without a
// ".phones" or ".stream" suffix after a non-empty bus part is the whole text with no mix.
MixTarget splitMixTarget(const QString &text);

struct VolumeTarget
{
    QString busId;
    std::optional<Mix> mix; // one level of a playback bus; none for both levels, the mic or a master
    QString error;          // kErrorNoSuchBus or kErrorInvalidValue (a mix on the mic or a master)
};
// What SetBusVolume and --set-volume set. The whole text as a bus wins, so a bus can be called
// "Chat.Stream"; otherwise a mix suffix is split off and the rest resolved.
VolumeTarget resolveVolumeTarget(const Scene &scene, const QString &text);

} // namespace rostrum::remote
