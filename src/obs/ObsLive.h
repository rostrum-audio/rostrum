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

// Best effort: every op is tried. `failed` counts the ones OBS refused.
void applyUndo(Client *client, const Undo &undo, std::function<void(int failed, const QString &lastError)> done);

} // namespace rostrum::obs
