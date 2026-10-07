#pragma once
#include "obs/ObsClient.h"
#include "obs/RecordingTracks.h"

namespace rostrum::obs {
void fetchRecordingSnapshot(Client *client,
                            std::function<void(const RecordingSnapshot &, const QString &)> done);

// Checks output activity and collection/profile identity before every write. The journal
// persists successful operations, including the scene item ids returned by OBS.
void applyRecordingChanges(Client *client, const RecordingSnapshot &scope,
                           const QList<RecordingChange> &changes,
                           std::function<bool(const QList<RecordingChange> &)> journal,
                           std::function<void(const QList<RecordingChange> &, const QString &)> done);
} // namespace rostrum::obs
