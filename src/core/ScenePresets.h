#pragma once

#include "core/Model.h"

#include <QStringList>

namespace rostrum::presets {

inline constexpr const char *kGaming = "gaming";
inline constexpr const char *kChatting = "chatting";
inline constexpr const char *kMusic = "music";
inline constexpr const char *kPodcast = "podcast";
inline constexpr const char *kBreak = "break";

// In menu order.
QStringList ids();

// A new scene with the bus list, names, colors and app rules of `base` and the preset's levels.
// Buses are matched by what they receive automatically, so renamed buses still get the right
// level; buses that receive nothing automatically keep their levels from `base`. Mic gain and
// sidetone are calibrated to the user's hardware and are kept too. Unknown ids return `base`.
Scene apply(const QString &id, const Scene &base);

} // namespace rostrum::presets
