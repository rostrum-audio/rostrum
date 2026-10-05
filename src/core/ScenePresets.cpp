#include "core/ScenePresets.h"

#include <QMap>

namespace rostrum::presets {

namespace {

struct Level
{
    Destination destination = Destination::Both;
    double volume = 1.0; // fader position; 0.8 is about -6 dB, 0.5 about -18 dB
    bool muted = false;
    bool vod = true;
};

struct Preset
{
    bool micMuted = false;
    QMap<AppCategory, Level> buses;
};

Level defaultLevel(AppCategory category)
{
    return {category == AppCategory::Music ? Destination::Stream : Destination::Both, 1.0, false,
            category != AppCategory::Music};
}

QMap<QString, Preset> table()
{
    using enum AppCategory;
    using enum Destination;
    return {
        {QString::fromLatin1(kGaming),
         {false,
          {{Game, {Both, 1.0}}, {Voice, {Both, 0.85}}, {Music, {Stream, 0.5, false, false}}, {Alerts, {Both, 0.8}},
           {Desktop, {Both, 0.8}}}}},
        {QString::fromLatin1(kChatting),
         {false,
          {{Game, {Both, 1.0, true}}, {Voice, {Both, 0.85}}, {Music, {Both, 0.55, false, false}}, {Alerts, {Both, 0.8}},
           {Desktop, {Both, 0.9}}}}},
        {QString::fromLatin1(kMusic),
         {false,
          {{Game, {Both, 1.0, true}}, {Voice, {Phones, 0.8}}, {Music, {Both, 1.0, false, false}}, {Alerts, {Both, 0.6}},
           {Desktop, {Phones, 0.8}}}}},
        {QString::fromLatin1(kPodcast),
         {false,
          {{Game, {Both, 1.0, true}}, {Voice, {Both, 1.0}}, {Music, {Stream, 0.5, true, false}},
           {Alerts, {Both, 0.8, true}}, {Desktop, {Phones, 0.8}}}}},
        {QString::fromLatin1(kBreak),
         {true,
          {{Game, {Both, 1.0, true}}, {Voice, {Phones, 0.85}}, {Music, {Both, 0.8, false, false}}, {Alerts, {Both, 0.8}},
           {Desktop, {Phones, 0.8}}}}},
    };
}

} // namespace

QStringList ids()
{
    return {QString::fromLatin1(kGaming), QString::fromLatin1(kChatting), QString::fromLatin1(kMusic),
            QString::fromLatin1(kPodcast), QString::fromLatin1(kBreak)};
}

Scene apply(const QString &id, const Scene &base)
{
    const auto presets = table();
    if (!presets.contains(id)) {
        return base;
    }
    const Preset &preset = presets.value(id);
    Scene out = base;
    out.masterPhones = 1.0;
    out.masterPhonesMuted = false;
    out.masterStream = 1.0;
    out.masterStreamMuted = false;
    for (Bus &bus : out.buses) {
        if (bus.isInput()) {
            bus.muted = preset.micMuted;
            continue;
        }
        if (bus.autoCategory == AppCategory::None) {
            continue;
        }
        const Level level = preset.buses.value(bus.autoCategory, defaultLevel(bus.autoCategory));
        bus.destination = level.destination;
        bus.vod = level.vod;
        bus.volume = level.volume;
        bus.muted = level.muted;
    }
    return out;
}

} // namespace rostrum::presets
