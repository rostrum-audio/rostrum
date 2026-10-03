#pragma once

#include "core/AppIdentity.h"
#include "core/Model.h"

#include <QString>
#include <QStringList>

namespace rostrum {

// Everything Rostrum knows about the app behind a playback stream, from PipeWire, from the
// process and from the app's desktop entry. Gathering it is AppFacts.h; judging it is classify().
struct AppFacts
{
    StreamProps props;
    QString mediaRole;     // media.role: "Music", "Game", "Communication", ...
    QString appId;         // Flatpak id, Snap name or application.id, any case
    QString iconName;      // application.icon-name
    QString steamAppId;    // SteamAppId / SteamGameId of the process, empty if none or "0"
    QString steamGameName; // from the Steam library, when the app id is known
    QString desktopId;     // the matching .desktop entry, if any
    QStringList desktopCategories;
    bool dontMove = false;       // node.dont-move: the app asked to stay where it is
    bool ownOutputChoice = false; // the app picked a device itself (target.object outside Rostrum)
};

// Why classify() decided, strongest first. The app layer turns this into a sentence.
enum class Evidence {
    None,
    Catalog,      // a well-known app
    Steam,        // started by Steam as a game
    Wine,         // a Windows program under Wine or Proton
    MediaRole,    // the app says what it plays
    DesktopEntry, // the app menu lists it as a game, chat app, music player, ...
    GameEngine,   // plays through an audio library games use
    OwnOutput,    // excluded: the app picked its output device itself
};

struct Classification
{
    AppCategory category = AppCategory::None;
    Evidence evidence = Evidence::None;
    // Never placed automatically, whatever it is: OBS (its monitor would loop into the stream),
    // audio tools, screen readers, and apps that chose their own output.
    bool excluded = false;

    bool operator==(const Classification &) const = default;
};

Classification classify(const AppFacts &facts);

} // namespace rostrum
