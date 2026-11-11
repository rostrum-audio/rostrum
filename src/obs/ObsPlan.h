#pragma once

#include "obs/ObsModel.h"

#include <QJsonObject>
#include <QList>
#include <QString>
#include <QStringList>

namespace rostrum::obs {

// Live: OBS is running and reachable over obs-websocket. It cannot turn on a global audio
// device, so a missing Stream Mix is added to every scene. Offline: OBS is closed and its scene
// collection file is edited, so Rostrum's devices become OBS's global Desktop Audio and Mic/Aux.
enum class Mode { Live, Offline };

struct Action
{
    enum class Type
    {
        SetDevice,         // point an existing PulseAudio input at a Rostrum device
        Unmute,            // an input Rostrum now relies on
        CreateInput,       // a new input added to `scenes`
        CreateGlobal,      // offline: a new global audio device on `channel`
        Mute,              // an input that would double audio or bypass Rostrum
        SetTwitchVodTrack, // live: make OBS send track 2 to Twitch as the VOD track (`profile`)
    };
    enum class Role { Mic, Stream, Vod, Conflict };

    Type type = Type::Mute;
    Role role = Role::Conflict;
    Capture capture = Capture::None; // for Mute: what the input records
    QString input;                   // existing input, or the name to create
    QString kind;                    // CreateInput / CreateGlobal
    QString device;                  // SetDevice / CreateInput / CreateGlobal
    QString channel;                 // CreateGlobal
    QStringList scenes;              // CreateInput
    quint32 tracks = 0;              // CreateInput / CreateGlobal / SetDevice; 0 = keep/default
    QList<ProfileParameter> profile; // SetTwitchVodTrack: the profile values written
    bool enabled = true;             // the user can untick conflicts in the preview
};

struct Plan
{
    Mode mode = Mode::Live;
    QList<Action> actions;
    // What Rostrum could not read: audio inputs missing their settings, mute or tracks, and
    // (live) the streaming settings. What a plan would change depends on all of them, so an
    // incomplete plan has no actions and does not mean OBS is set up.
    QStringList unreadInputs;
    bool streamSettingsUnread = false;
    // Which scene collection (live: and profile) the state came from. Applying checks OBS is
    // still on them, and undo is kept for them.
    bool scopeUnread = false;

    bool incomplete() const { return !unreadInputs.isEmpty() || streamSettingsUnread || scopeUnread; }
    bool isEmpty() const { return actions.isEmpty(); }
    QList<Action> enabledActions() const;
};

Plan makePlan(const State &state, const Facts &facts, Mode mode, const QSet<QString> &recordingDevices = {});

// How to put back what a plan changed. Ops run in reverse order.
struct UndoOp
{
    enum class Type
    {
        RestoreDevice,
        RestoreMute,
        RemoveInput,
        RemoveGlobal,
        RestoreProfile
    };
    Type type = Type::RestoreMute;
    QString input;
    QString device;  // RestoreDevice
    bool muted = false; // RestoreMute
    QString channel; // RemoveGlobal
    quint32 tracks = 0;              // RestoreDevice
    bool tracksSet = false;          // RestoreDevice: put `tracks` back, which may be none at all
    QList<ProfileParameter> profile; // RestoreProfile: the values before, never removed
    QString configDir;               // the OBS install it was made in
    // The scene collection it was made in, or for RestoreProfile the profile. Empty for an op
    // from Rostrum 0.2.0, which did not record it.
    QString scope;
};

// Where undo runs: an install, and OBS's current scene collection and profile. Offline the
// profile is empty, and profile values wait for a live undo.
struct UndoScope
{
    QString configDir, collection, profile;
};

// Every change Set Up OBS made and has not undone, oldest first.
struct Undo
{
    QList<UndoOp> ops;

    bool isEmpty() const { return ops.isEmpty(); }
    // The ops undo may put back in `scope`, as indices, oldest first. Ops from Rostrum 0.2.0
    // carry no scope: their inputs are put back only if every one of them exists in `now`.
    QList<qsizetype> runnable(const UndoScope &scope, const State &now) const;
    Undo only(const QList<qsizetype> &indices) const;
    Undo without(const QList<qsizetype> &indices) const;
    QJsonObject toJson() const;                    // format 2
    static Undo fromJson(const QJsonObject &json); // format 1 too
};

// The undo op for one action, given the state before it ran.
UndoOp undoFor(const Action &action, const State &before);

} // namespace rostrum::obs
