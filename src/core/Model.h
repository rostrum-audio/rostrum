#pragma once

#include <QDateTime>
#include <QList>
#include <QString>
#include <QStringList>

#include <optional>

namespace rostrum {

inline constexpr int kMaxBuses = 12;
inline constexpr const char *kMicBusId = "mic";
inline constexpr const char *kDefaultSceneName = "Live";

enum class Destination { Phones, Stream, Both };
enum class BusKind { Input, Playback };
enum class MatchKey { Name, Binary };
// What kind of app a bus receives automatically. None = only apps the user puts there.
enum class AppCategory { None, Game, Voice, Music, Alerts, Desktop };

QString destinationName(Destination d);
std::optional<Destination> destinationFromString(const QString &s);
QString matchKeyName(MatchKey k);
std::optional<MatchKey> matchKeyFromString(const QString &s);
QString categoryName(AppCategory c);
std::optional<AppCategory> categoryFromString(const QString &s);

inline bool feedsPhones(Destination d) { return d == Destination::Phones || d == Destination::Both; }
inline bool feedsStream(Destination d) { return d == Destination::Stream || d == Destination::Both; }

struct Bus
{
    QString id;    // stable; the PipeWire node is "rostrum.<id>"
    QString name;  // user-visible, renamable
    QString color; // "#rrggbb", user data
    BusKind kind = BusKind::Playback;
    double volume = 1.0; // fader position, 0..1 for playback, 0..1.5 for mic gain (1.0 = unity / 0 dB)
    bool muted = false;
    double balance = 0.0; // -1 left .. 1 right; playback buses only
    Destination destination = Destination::Both;
    bool vod = true;                              // Twitch VOD destination (playback buses)
    AppCategory autoCategory = AppCategory::None; // at most one bus per category in a scene

    bool isInput() const { return kind == BusKind::Input; }
    QString nodeName() const;
    bool operator==(const Bus &) const = default;
};

inline bool feedsVod(const Bus &b) { return feedsStream(b.destination) && b.vod; }

struct AppRule
{
    QString match; // compared case-insensitively against the key below
    MatchKey key = MatchKey::Name;
    QString busId;
    double volume = 1.0; // per-app offset on top of the bus fader
    bool muted = false;  // per-app mute, saved like the volume
    QString label;       // user's name for an app that reports none; empty = use the app's own
    QStringList iconNames; // icon candidates; bookkeeping, survives when the app stops
    QDateTime lastSeen;  // bookkeeping only; ignored by operator== so it never marks a scene dirty

    bool operator==(const AppRule &o) const
    {
        return match == o.match && key == o.key && busId == o.busId && volume == o.volume &&
               muted == o.muted && label == o.label;
    }
};

// A scene is everything a streamer recalls with one click.
// HARD RULE: solo is session-only state owned by the engine. It must never be added to Bus or
// Scene, and it must never be written to TOML. Saving while soloed saves the user's own mutes.
struct Scene
{
    QString name = QString::fromLatin1(kDefaultSceneName);
    double masterPhones = 1.0;
    bool masterPhonesMuted = false;
    double masterStream = 1.0;
    bool masterStreamMuted = false;
    double sidetoneVolume = 0.0; // sidetone is on when the mic destination includes Phones
    QList<Bus> buses;
    QList<AppRule> rules;

    Bus *bus(const QString &id);
    const Bus *bus(const QString &id) const;
    Bus *micBus() { return bus(QString::fromLatin1(kMicBusId)); }
    const Bus *micBus() const { return bus(QString::fromLatin1(kMicBusId)); }
    QList<Bus> playbackBuses() const;
    const Bus *busFor(AppCategory category) const; // the playback bus that receives it, if any

    AppRule *rule(MatchKey key, const QString &match);
    const AppRule *rule(MatchKey key, const QString &match) const;
    void removeRulesForBus(const QString &busId);

    // "6 buses, Music → Stream, mic muted"
    QString summary() const;

    bool operator==(const Scene &) const = default;
};

namespace defaults {

struct Swatch
{
    const char *name;
    const char *hex;
};

// 12 swatches, ordered so the default buses take the first six.
const QList<Swatch> &palette();
Scene scene(const QString &name = QString::fromLatin1(kDefaultSceneName));
Destination destinationFor(const QString &busId);
bool vodFor(const QString &busId);
AppCategory autoCategoryFor(const QString &busId); // what the default buses receive

} // namespace defaults

// Lower-case slug used for bus ids and scene file names, unique within `taken`.
QString makeSlug(const QString &name, const QStringList &taken, const QString &fallback = QStringLiteral("bus"));

// Keep the saved levels (faders, mutes, destinations, masters) but take the structure (bus list,
// names, colors, rules) from `current`. Used so renames and rules persist without saving faders.
Scene mergeStructure(const Scene &saved, const Scene &current);

} // namespace rostrum
