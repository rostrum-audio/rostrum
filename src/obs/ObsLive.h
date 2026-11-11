#pragma once

#include "obs/ObsClient.h"
#include "obs/ObsModel.h"
#include "obs/ObsPlan.h"

#include <functional>

namespace rostrum::obs {

// Reads the inputs Rostrum cares about and the scene list. `error` is empty on success.
void fetchState(Client *client, std::function<void(const State &state, const QString &error)> done,
                bool includeUnsupported = false);

// A read-only live readiness snapshot; nested scenes/groups remain unsupported.
void fetchReadinessState(Client *client, std::function<void(const State &, const QString &)> done);

// Runs the actions in order and stops at the first one OBS refuses. `applied` holds the undo
// ops for everything that did happen, so a partial run can still be undone.
void applyPlan(Client *client, const QList<Action> &actions, const State &before,
               std::function<void(const QList<UndoOp> &applied, const QString &error)> done);

// Both first check that OBS is still on the scene collection and profile they were planned for
// (applyPlan: `before`'s; applyUndo: `scope`'s, the profile only when given).

struct UndoResult
{
    QList<qsizetype> settled; // ops (indices into the undo) OBS answered: put back, or refused
    int failed = 0;           // requests OBS refused
    QString lastError;
    QString stopped; // why the other ops were not tried (OBS disconnected or switched); else empty
};

// Best effort while OBS stays connected and on `scope`: every op is tried, newest first. Ops
// not settled were not tried and belong in the undo record still.
void applyUndo(Client *client, const Undo &undo, const UndoScope &scope,
               std::function<void(const UndoResult &)> done);

} // namespace rostrum::obs
