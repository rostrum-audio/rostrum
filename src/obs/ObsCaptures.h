#pragma once

#include "obs/ObsModel.h"

#include <QList>
#include <QString>

namespace rostrum::pw {
class PwContext;
struct Graph;
} // namespace rostrum::pw

namespace rostrum::obs {

// What OBS records right now, read from the PipeWire graph. Works without obs-websocket.
struct Recording
{
    QString source; // the OBS source name
    QString what;   // the node it records, as people know it
    Capture capture = Capture::None;
    QString device; // observed node.name; never inferred from its human-readable label
};

QList<Recording> obsRecordings(const pw::Graph &graph);
// Whether this observed filtered mic belongs exclusively to the saved mic recording slots.
bool assignedRecordingMic(const Recording &recording, const Input &input, quint32 assignedTracks);

// Configuration checks for an observed Rostrum Mic, Stream Mix or VOD Mix capture, not a
// recording sound test. Required tracks follow Set Up OBS: Mic on 1 and 2 (recording tracks 3–6
// may be added), Stream Mix on 1 and never on the VOD's track 2, VOD Mix on 2 only. Volume counts
// only when it was read.
enum class CaptureStatus
{
    Unknown,
    Configured,
    WrongTracks,
    Muted,
    Silent,
    Isolated // an assigned recording-only mic on tracks 3–6
};
using VodCaptureStatus = CaptureStatus;
CaptureStatus captureStatus(Capture capture, const Input *input);
VodCaptureStatus vodCaptureStatus(const Input *input);

enum class CaptureKind
{
    Ok,
    Warning,
    Muted
};
struct CaptureCheck
{
    CaptureStatus status = CaptureStatus::Unknown;
    CaptureKind kind = CaptureKind::Warning;
};
// One check per recording, for Rostrum Mic, Stream Mix and VOD Mix rows; other rows get the
// defaults and are judged by the caller. `state` is null when OBS's inputs are not known. A mute
// does not hide another problem, unless another unmuted capture records the same device: then
// the muted one is a resolved duplicate.
QList<CaptureCheck> rostrumCaptureChecks(const QList<Recording> &recordings, const State *state,
                                         const QSet<QString> &recordingDevices);

Facts factsFrom(const pw::PwContext &pw);

} // namespace rostrum::obs
