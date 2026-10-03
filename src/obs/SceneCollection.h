#pragma once

#include "obs/ObsModel.h"
#include "obs/ObsPlan.h"

#include <QJsonObject>

namespace rostrum::obs {

// OBS's scene collection JSON (basic/scenes/<name>.json). Only edit it while OBS is closed:
// OBS rewrites the whole file when it saves.

// "desktop1" -> "DesktopAudioDevice1", "mic2" -> "AuxAudioDevice2"; empty if unknown.
QString globalKey(const QString &channel);

State stateFromCollection(const QJsonObject &doc);

// Applies the actions OBS can take offline. CreateInput is skipped: offline plans use
// CreateGlobal instead.
QJsonObject applyToCollection(QJsonObject doc, const QList<Action> &actions);

QJsonObject undoInCollection(QJsonObject doc, const Undo &undo);

} // namespace rostrum::obs
