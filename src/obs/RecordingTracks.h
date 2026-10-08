#pragma once

#include "core/Model.h"
#include "obs/ObsPlan.h"

#include <QJsonArray>
#include <QMap>

namespace rostrum::obs {

using RecordingAssignments = QMap<int, QString>; // tracks 3–6 -> bus id; absent = keep OBS, empty = clear

struct RecordingSnapshot
{
    State state;
    QString collection, profile, outputMode;
    QString recordingType = QStringLiteral("Standard");
    QMap<int, QString> trackNames;
    quint32 recordingTracks = 0;
    bool streaming = false, recording = false, known = false;
    QMap<QString, QJsonArray> sceneItems; // scenes and groups; enabled nesting is followed
};

struct RecordingChange
{
    QString method;
    QJsonObject data;
    QString undoMethod;
    QJsonObject undoData;
};

struct RecordingPlan
{
    QList<RecordingChange> changes;
    QStringList problems; // stable codes, translated by the app
    QMap<QString, quint32> inputTracks;
    quint32 recordingTracks = 0;
};

RecordingAssignments recordingSuggestion(const Scene &scene, const RecordingSnapshot &before);
QSet<QString> recordingDevices(const Scene &scene, const RecordingAssignments &assignments,
                               bool filteredMic = false);
bool intendedRecording(const Input &input, const QSet<QString> &devices);
RecordingPlan recordingPlan(const RecordingSnapshot &before, const Scene &scene,
                            const RecordingAssignments &assignments, bool filteredMic = false);
RecordingPlan recordingUndo(const RecordingPlan &plan);
QJsonArray recordingChangesJson(const QList<RecordingChange> &changes);
QList<RecordingChange> recordingChangesFromJson(const QJsonArray &json);
QByteArray recordingFingerprint(const RecordingSnapshot &snapshot);
QJsonObject recordingTracksJson(quint32 mask);

} // namespace rostrum::obs
