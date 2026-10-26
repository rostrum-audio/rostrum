#include "obs/ObsCaptures.h"

#include "obs/RecordingTracks.h"
#include "pw/Graph.h"
#include "pw/PwContext.h"

#include <QSet>

#include <cmath>

namespace rostrum::obs {

namespace {

bool isObs(const pw::Node &n, const pw::Graph &g)
{
    if (n.appName == QLatin1String("OBS") || n.binary == QLatin1String("obs") || n.name.startsWith(QLatin1String("OBS"))) {
        return true;
    }
    const auto client = g.clients.constFind(n.clientId.toUInt());
    return client != g.clients.cend() && client->appName == QLatin1String("OBS");
}

// Pulse streams carry the OBS source name in media.name; the PipeWire plugin names its nodes
// "OBS: <source>".
QString sourceName(const pw::Node &n)
{
    if (n.name.startsWith(QLatin1String("OBS: "))) {
        return n.name.mid(5);
    }
    return n.mediaName.isEmpty() ? n.name : n.mediaName;
}

} // namespace

QList<Recording> obsRecordings(const pw::Graph &g)
{
    QList<Recording> out;
    for (const auto &n : g.nodes) {
        if (!n.isCaptureStream() || !isObs(n, g)) {
            continue;
        }
        QSet<uint32_t> seen;
        for (const auto &l : g.links) {
            if (l.inNode != n.id || seen.contains(l.outNode)) {
                continue;
            }
            seen.insert(l.outNode);
            const pw::Node *src = g.node(l.outNode);
            if (!src) {
                continue;
            }
            Recording r;
            r.source = sourceName(n);
            r.device = src->name;
            if (src->name == QLatin1String("rostrum.mic")) {
                r.capture = Capture::RostrumMic;
            } else if (src->name == QLatin1String("rostrum.stream")) {
                r.capture = Capture::RostrumStream;
            } else if (src->name == QLatin1String("rostrum.vod")) {
                r.capture = Capture::RostrumVod;
            } else if (src->name == QLatin1String(kFilteredMicDevice)) {
                r.capture = Capture::Mic;
            } else if (src->isRostrum()) {
                r.capture = Capture::RostrumBus;
            } else if (src->isSource()) {
                r.capture = Capture::Mic;
            } else if (src->isSink()) {
                r.capture = Capture::Output;
            } else if (src->isPlaybackStream()) {
                r.capture = Capture::App;
            } else {
                continue;
            }
            r.what = r.capture == Capture::App && !src->appName.isEmpty() ? src->appName : src->label();
            out << r;
        }
    }
    std::sort(out.begin(), out.end(), [](const Recording &a, const Recording &b) {
        return a.source.localeAwareCompare(b.source) < 0;
    });
    return out;
}

bool assignedRecordingMic(const Recording &recording, const Input &input, quint32 assignedTracks)
{
    const auto device = input.settings.value(QLatin1String("device_id")).toString();
    return input.settingsKnown && input.tracksKnown && input.kind == QLatin1String(kPulseInput) &&
           recording.source == input.name && recording.capture == Capture::Mic &&
           device == QLatin1String(kFilteredMicDevice) && recording.device == device &&
           input.tracks != 0 && !(input.tracks & 3u) && !(input.tracks & ~assignedTracks);
}

CaptureStatus captureStatus(Capture capture, const Input *input)
{
    if (!input || !input->tracksKnown || !input->muteKnown ||
        (input->gainKnown && !std::isfinite(input->gain)))
        return CaptureStatus::Unknown;
    const quint32 t = input->tracks;
    bool wrongTracks = false;
    switch (capture) {
    case Capture::RostrumMic:
        wrongTracks = (t & 3u) != 3u;
        break;
    case Capture::RostrumStream:
        wrongTracks = !(t & 1u) || (t & 2u);
        break;
    case Capture::RostrumVod:
        wrongTracks = t != 2u;
        break;
    default:
        return CaptureStatus::Unknown;
    }
    if (wrongTracks)
        return CaptureStatus::WrongTracks;
    if (input->gainKnown && input->gain <= 0)
        return CaptureStatus::Silent;
    if (input->muted)
        return CaptureStatus::Muted;
    return CaptureStatus::Configured;
}

VodCaptureStatus vodCaptureStatus(const Input *input)
{
    return captureStatus(Capture::RostrumVod, input);
}

QList<CaptureCheck> rostrumCaptureChecks(const QList<Recording> &recordings, const State *state,
                                         const QSet<QString> &recordingDevices)
{
    auto isRostrum = [](Capture c) {
        return c == Capture::RostrumMic || c == Capture::RostrumStream || c == Capture::RostrumVod;
    };
    auto inputOf = [&](const Recording &r) { return state ? state->input(r.source) : nullptr; };
    // Same rule as the readiness evaluator: a mic capture only on tracks 3–6 that the
    // recording slots assigned is not the stream mic.
    auto isolated = [&](const Recording &r, const Input *in) {
        return r.capture == Capture::RostrumMic && in && in->tracksKnown && in->muteKnown &&
               intendedRecording(*in, recordingDevices) && !(in->tracks & 3u) && (in->tracks & 0x3cu);
    };
    QList<CaptureCheck> out(recordings.size());
    for (qsizetype n = 0; n < recordings.size(); ++n) {
        const auto &r = recordings.at(n);
        if (!isRostrum(r.capture))
            continue;
        const Input *in = inputOf(r);
        const bool muted = in && in->muted;
        auto &check = out[n];
        if (isolated(r, in)) {
            if (in->gainKnown && !std::isfinite(in->gain)) {
                check = {CaptureStatus::Unknown, CaptureKind::Warning};
            } else if (in->gainKnown && in->gain <= 0) {
                check = {CaptureStatus::Silent, CaptureKind::Warning};
            } else {
                check = {CaptureStatus::Isolated, muted ? CaptureKind::Muted : CaptureKind::Ok};
            }
            continue;
        }
        check.status = captureStatus(r.capture, in);
        if (check.status == CaptureStatus::Configured) {
            check.kind = CaptureKind::Ok;
        } else if (check.status == CaptureStatus::Muted) {
            check.kind = CaptureKind::Muted;
        } else {
            check.kind = CaptureKind::Warning;
        }
    }
    return out;
}

Facts factsFrom(const pw::PwContext &pw)
{
    Facts f;
    f.defaultSource = pw.defaultSourceName();
    f.defaultSink = pw.defaultSinkName();
    for (const auto &n : pw.graph().nodes) {
        bool ok = false;
        const quint32 serial = n.serial.toUInt(&ok);
        if (ok) {
            f.serials.insert(serial, n.name);
        }
    }
    return f;
}

} // namespace rostrum::obs
