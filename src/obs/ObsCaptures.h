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
};

QList<Recording> obsRecordings(const pw::Graph &graph);

Facts factsFrom(const pw::PwContext &pw);

} // namespace rostrum::obs
