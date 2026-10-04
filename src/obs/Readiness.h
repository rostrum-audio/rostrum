#pragma once
#include "core/Model.h"
#include "obs/ObsModel.h"
#include "obs/ObsStatus.h"
#include "pw/Graph.h"

#include <optional>

namespace rostrum::obs {
enum class ReadinessStatus
{
    Verified,
    Attention,
    Excluded,
    Unknown
};
struct ReadinessResult
{
    QString id, title, detail;
    ReadinessStatus status = ReadinessStatus::Unknown;
    std::optional<GoLiveProblem> warning;
};
struct ReadinessApp
{
    uint32_t nodeId;
    QString busId;
};
struct ReadinessInput
{
    Scene scene;
    QSet<QString> soloed;
    EffectiveMutes mutes;
    bool panic = false, pushToTalk = false, pushToMute = false;
    bool inspectRouting = true;
    const pw::Graph *graph = nullptr; // null means disconnected / unavailable
    QString selectedMic, resolvedMic, selectedPhones, resolvedPhones;
    bool micMissing = false, phonesMissing = false, micFallback = false;
    bool filtersWanted = false, filtersActive = false, monoPhones = false;
    QList<ReadinessApp> apps;
    const State *obsState = nullptr; // live, complete scope; never an offline collection
    bool obsFresh = false;
    QString obsError;
    Facts facts;
    const QList<Recording> *legacyRecordings = nullptr;
};
QList<ReadinessResult> evaluateReadiness(const ReadinessInput &input);
QList<GoLiveProblem> readinessWarnings(const QList<ReadinessResult> &results);
} // namespace rostrum::obs
