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
    enum class Type {
        SetDevice,    // point an existing PulseAudio input at a Rostrum device
        Unmute,       // an input Rostrum now relies on
        CreateInput,  // live: a new input added to `scenes`
        CreateGlobal, // offline: a new global audio device on `channel`
        Mute,         // an input that would double audio or bypass Rostrum
    };
    enum class Role { Mic, Stream, Conflict };

    Type type = Type::Mute;
    Role role = Role::Conflict;
    Capture capture = Capture::None; // for Mute: what the input records
    QString input;                   // existing input, or the name to create
    QString kind;                    // CreateInput / CreateGlobal
    QString device;                  // SetDevice / CreateInput / CreateGlobal
    QString channel;                 // CreateGlobal
    QStringList scenes;              // CreateInput
    quint32 tracks = 0;              // CreateInput / CreateGlobal; 0 = OBS default
    bool enabled = true;             // the user can untick conflicts in the preview
};

struct Plan
{
    Mode mode = Mode::Live;
    QList<Action> actions;

    bool isEmpty() const { return actions.isEmpty(); }
    QList<Action> enabledActions() const;
};

Plan makePlan(const State &state, const Facts &facts, Mode mode);

// How to put back what a plan changed. Ops run in reverse order.
struct UndoOp
{
    enum class Type { RestoreDevice, RestoreMute, RemoveInput, RemoveGlobal };
    Type type = Type::RestoreMute;
    QString input;
    QString device;  // RestoreDevice
    bool muted = false; // RestoreMute
    QString channel; // RemoveGlobal
};

struct Undo
{
    QString configDir;
    QList<UndoOp> ops;

    bool isEmpty() const { return ops.isEmpty(); }
    QJsonObject toJson() const;
    static Undo fromJson(const QJsonObject &json);
};

// The undo op for one action, given the state before it ran.
UndoOp undoFor(const Action &action, const State &before);

} // namespace rostrum::obs
