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
    bool chat = false; // ".chat": the voice chat level
};
// "game.stream" -> {"game", Stream}; "Game.Phones" -> {"Game", Phones}; anything without a
// ".phones" or ".stream" suffix after a non-empty bus part is the whole text with no mix. A
// ".chat" suffix sets `chat` instead (the voice chat level), with no mix.
MixTarget splitMixTarget(const QString &text);

struct VolumeTarget
{
    QString busId;
    std::optional<Mix> mix; // one level of a playback bus; none for both levels, the mic or a master
    bool chat = false;      // the voice chat level of a playback bus that may have one
    QString error;          // kErrorNoSuchBus or kErrorInvalidValue (a mix on the mic or a master)
};
// What SetBusVolume and --set-volume set. The whole text as a bus wins, so a bus can be called
// "Chat.Stream"; otherwise a mix suffix is split off and the rest resolved.
VolumeTarget resolveVolumeTarget(const Scene &scene, const QString &text);

// A side of a playback bus for the deck keys: one mix, both levels together, or the voice chat send.
enum class Side { Stream, Phones, Both, Chat };
// "stream", "phones", "both" or "chat", in any case. Anything else is no side.
std::optional<Side> parseSide(const QString &text);
QString sideName(Side side); // "stream", "phones", "both" or "chat"

// A playback bus plays in a mix when it is not muted and its destination includes that mix.
bool playsIn(Destination d, bool muted, Mix mix);

struct InMix
{
    Destination destination = Destination::Both;
    bool muted = false;
    bool operator==(const InMix &) const = default;
};
// ToggleBusInMix: a bus that plays in `mix` comes off it (destination Both becomes the other mix
// only; a bus only in `mix` is muted instead). One that does not comes back on. A muted bus
// comes back on `mix` alone, since it was silent on the other side too. Whether the bus plays in
// the other mix never changes.
InMix toggledInMix(Destination d, bool muted, Mix mix);

struct ChatState
{
    bool chat = false;
    bool muted = false;
    bool operator==(const ChatState &) const = default;
};
// ToggleBusInMix(bus, "chat"): the first press sets the destination up, unmuted. After that the
// press flips the live mute only, so no app ever moves.
ChatState toggledChat(bool chat, bool muted);

enum class StepProblem { None, NoSuchBus, BadSide, OneLevelBus, BadDelta, NoPerMixLevels };
struct StepTarget
{
    QString busId;
    Side side = Side::Both;
    StepProblem problem = StepProblem::None;
};
// StepBusVolume's arguments. The mic and the masters have one level and take only "both". delta
// must be finite and within -1..1. Without per-mix levels only "both" can move. "chat" needs a
// bus that may have a voice chat send (else BadSide) and moves only that level.
StepTarget resolveStep(const Scene &scene, const QString &busText, const QString &sideText, double delta,
                       bool perMixLevels);

enum class InMixProblem { None, NoSuchBus, BadSide, NotPlaybackBus };
struct InMixTarget
{
    QString busId;
    Mix mix = Mix::Stream;
    bool chat = false; // "chat": the voice chat destination; `mix` is not used
    InMixProblem problem = InMixProblem::None;
};
// ToggleBusInMix's arguments: a playback bus and "stream", "phones" or "chat" (BadSide when the
// bus may not go to voice chat).
InMixTarget resolveInMix(const Scene &scene, const QString &busText, const QString &sideText);

} // namespace rostrum::remote
