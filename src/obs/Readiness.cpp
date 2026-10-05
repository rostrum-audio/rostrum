#include "obs/Readiness.h"

#include <QCoreApplication>
#include <algorithm>
#include <cmath>

namespace rostrum::obs {
namespace {
QString tr(const char *text)
{
    return QCoreApplication::translate("StreamReadiness", text);
}
ReadinessResult route(const pw::Graph *g, const QString &id, const QString &title, const pw::Node *from,
                      const pw::Node *to, bool wanted, bool idleApp = false, bool mono = false)
{
    ReadinessResult r{id, title, {}, ReadinessStatus::Unknown, {}};
    if (!g) {
        r.detail = tr("PipeWire is unavailable; routing was not checked.");
        return r;
    }
    if (!wanted) {
        const bool forbidden =
            from && to && std::any_of(g->links.cbegin(), g->links.cend(), [&](const pw::Link &l) {
                return l.outNode == from->id && l.inNode == to->id;
            });
        r.status = forbidden ? ReadinessStatus::Attention
                             : (from && to ? ReadinessStatus::Excluded : ReadinessStatus::Unknown);
        r.detail = forbidden
                       ? tr("An excluded destination still has a link, regardless of its activation state.")
                       : (from && to ? tr("Excluded by the selected destination; no direct sends exist.")
                                     : tr("Nodes are missing; exclusion could not be checked."));
        return r;
    }
    if (idleApp && from && to) {
        for (const auto &l : g->links)
            if (l.outNode == from->id && l.inNode != to->id) {
                r.status = ReadinessStatus::Attention;
                r.detail = tr(
                    "Application has a link outside its assigned bus and can bypass that bus's exclusions.");
                return r;
            }
    }
    if (idleApp && from && !from->running) {
        // A known error remains a fault even when a player is idle.
        for (const auto &l : g->links)
            if (l.outNode == from->id && l.state == QLatin1String("error")) {
                r.status = ReadinessStatus::Attention;
                r.detail = tr("An idle application's link reports an error.");
                return r;
            }
        r.status = ReadinessStatus::Excluded;
        r.detail = tr("Application is idle; audio delivery has not been verified.");
        return r;
    }
    if (!from || !to) {
        r.status = ReadinessStatus::Attention;
        r.detail = tr("A required routing node is missing.");
        return r;
    }
    const auto outputs = g->outputPorts(from->id), inputs = g->inputPorts(to->id);
    auto complete = [](const pw::Node *n, const QList<pw::Port> &ports) {
        const int expect = n->channels > 0 ? n->channels : n->prop("audio.channels").toInt();
        return !ports.isEmpty() && (expect <= 0 || ports.size() >= expect);
    };
    if (!complete(from, outputs) || !complete(to, inputs)) {
        r.detail = tr("Channel ports are incomplete; routing has not been verified.");
        return r;
    }
    const auto pairs = mono ? pw::monoPorts(outputs, inputs) : pw::matchPorts(outputs, inputs);
    for (const auto &port : outputs)
        if (std::none_of(pairs.cbegin(), pairs.cend(), [&](const auto &pair) {
                return pair.first == port.id;
            })) {
            r.detail = tr("Not all source channels have a resolved destination mapping.");
            return r;
        }
    bool paused = false, unknown = false;
    for (const auto &pair : pairs) {
        bool found = false;
        for (const auto &l : g->links)
            if (l.outPort == pair.first && l.inPort == pair.second) {
                found = true;
                if (l.state == QLatin1String("error") || l.state == QLatin1String("unlinked")) {
                    r.status = ReadinessStatus::Attention;
                    r.detail = tr("A required channel link reports an error or is unlinked.");
                    return r;
                }
                paused = paused || l.state == QLatin1String("paused");
                unknown =
                    unknown || (l.state != QLatin1String("active") && l.state != QLatin1String("paused"));
            }
        if (!found) {
            r.status = ReadinessStatus::Attention;
            r.detail = tr("A required channel link is missing.");
            return r;
        }
    }
    r.status = unknown  ? ReadinessStatus::Unknown
               : paused ? ReadinessStatus::Excluded
                        : ReadinessStatus::Verified;
    r.detail = unknown ? tr("Channel links exist, but their processing state is not verified.")
               : paused
                   ? tr("All channel links exist; processing is paused/idle. No sound test was performed.")
                   : tr("All expected channel links report active. Audible delivery was not tested.");
    return r;
}
} // namespace

QList<ReadinessResult> evaluateReadiness(const ReadinessInput &i)
{
    QList<ReadinessResult> out;
    auto add = [&](const QString &id, const QString &title, ReadinessStatus status, const QString &detail,
                   std::optional<GoLiveProblem> warning = {}) {
        out.append({id, title, detail, status, warning});
    };
    const auto *mic = i.scene.micBus();
    if (mic) {
        const bool muted = i.mutes.mic || !std::isfinite(mic->volume) || mic->volume <= 0;
        const bool excluded = !feedsStream(mic->destination);
        add(QStringLiteral("mic-level"), tr("Effective stream mic"),
            muted      ? ReadinessStatus::Attention
            : excluded ? ReadinessStatus::Excluded
                       : ReadinessStatus::Verified,
            tr("Effective mute: %1; gain position: %2; destination: %3. Live holds and panic are included.")
                    .arg(i.mutes.mic ? tr("on") : tr("off"))
                    .arg(mic->volume)
                    .arg(destinationName(mic->destination)) +
                tr(" Panic: %1; push-to-talk held: %2; push-to-mute held: %3.")
                    .arg(i.panic ? tr("yes") : tr("no"), i.pushToTalk ? tr("yes") : tr("no"),
                         i.pushToMute ? tr("yes") : tr("no")),
            muted || excluded ? std::optional(GoLiveProblem::MicMuted) : std::nullopt);
    }
    bool included = false, reaches = false;
    for (const auto &b : i.scene.buses)
        if (!b.isInput() && feedsStream(b.destination)) {
            included = true;
            reaches = reaches || (!b.muted && std::isfinite(b.volume) && b.volume > 0 &&
                                  (i.soloed.isEmpty() || i.soloed.contains(b.id)));
        }
    const bool streamMuted = i.mutes.stream || !std::isfinite(i.scene.masterStream) ||
                             i.scene.masterStream <= 0 || (included && !reaches);
    add(QStringLiteral("stream-level"), tr("Effective Stream Mix"),
        streamMuted ? ReadinessStatus::Attention
        : !included ? ReadinessStatus::Excluded
                    : ReadinessStatus::Verified,
        tr("Effective master mute: %1; gain position: %2; soloed buses: %3. Silent meters are not a failure.")
            .arg(i.mutes.stream ? tr("on") : tr("off"))
            .arg(i.scene.masterStream)
            .arg(i.soloed.size()),
        streamMuted || !included ? std::optional(GoLiveProblem::StreamMixSilent) : std::nullopt);

    if (i.inspectRouting) {
        auto device = [&](bool microphone) {
            const QString selected = microphone ? i.selectedMic : i.selectedPhones;
            const QString resolved = microphone ? i.resolvedMic : i.resolvedPhones;
            const bool missing = microphone ? i.micMissing : i.phonesMissing;
            const QString detail =
                tr("Selected: %1. Resolved: %2. %3")
                    .arg(selected.isEmpty() ? tr("system default") : selected,
                         resolved.isEmpty() ? tr("none") : resolved,
                         microphone ? (i.micFallback ? tr("Microphone fallback explicitly enabled.")
                                                     : tr("Microphone fallback disabled."))
                                    : tr("Headphones may fall back without changing the saved selection."));
            add(microphone ? QStringLiteral("mic-device") : QStringLiteral("phones-device"),
                microphone ? tr("Microphone device") : tr("Headphone device"),
                !i.graph                        ? ReadinessStatus::Unknown
                : missing || resolved.isEmpty() ? ReadinessStatus::Attention
                                                : ReadinessStatus::Verified,
                detail);
        };
        device(true);
        device(false);
        auto node = [&](const QString &name) {
            return i.graph ? i.graph->nodeByName(name) : nullptr;
        };
        const auto *phones = node(QStringLiteral("rostrum.phones")),
                   *stream = node(QStringLiteral("rostrum.stream"));
        auto reportedLevel = [&](const QString &id, const QString &title, const pw::Node *n,
                                 bool expectedSilent) {
            if (!n || n->volumes.isEmpty()) {
                add(id, title, ReadinessStatus::Unknown,
                    tr("Observed node mute/gain properties are unavailable."));
                return;
            }
            bool zero = true;
            for (float gain : n->volumes) {
                if (!std::isfinite(gain)) {
                    add(id, title, ReadinessStatus::Unknown, tr("Observed gain properties are invalid."));
                    return;
                }
                zero = zero && gain <= 0;
            }
            const bool silent = n->muted || zero;
            add(id, title,
                expectedSilent ? ReadinessStatus::Excluded
                : silent       ? ReadinessStatus::Attention
                               : ReadinessStatus::Verified,
                tr("Observed node mute: %1; all channel gains zero: %2. These are properties, not an audible "
                   "test.")
                    .arg(n->muted ? tr("on") : tr("off"), zero ? tr("yes") : tr("no")));
        };
        reportedLevel(QStringLiteral("stream-observed-level"), tr("Observed Stream Mix level"), stream,
                      streamMuted || !included);
        reportedLevel(QStringLiteral("mic-observed-level"), tr("Observed Rostrum Mic level"),
                      node(QStringLiteral("rostrum.mic")),
                      !mic || i.mutes.mic || mic->volume <= 0 || !feedsStream(mic->destination));
        for (const auto &b : i.scene.buses)
            if (!b.isInput()) {
                const auto *n = node(b.nodeName());
                out << route(i.graph, b.id + QStringLiteral("-phones"), tr("%1 → headphones").arg(b.name), n,
                             phones, feedsPhones(b.destination));
                out << route(i.graph, b.id + QStringLiteral("-stream"), tr("%1 → Stream Mix").arg(b.name), n,
                             stream, feedsStream(b.destination));
                if (b.muted || b.volume <= 0 || (!i.soloed.isEmpty() && !i.soloed.contains(b.id)))
                    add(b.id + QStringLiteral("-level"), tr("%1 level").arg(b.name),
                        ReadinessStatus::Excluded,
                        tr("Muted, at zero gain, or excluded by solo. No audio is expected from this bus."));
            }
        out << route(i.graph, QStringLiteral("phones-output"), tr("Headphones → resolved output"), phones,
                     node(i.resolvedPhones), true, false, i.monoPhones);
        const auto *source = node(i.resolvedMic), *voice = source;
        if (i.filtersWanted) {
            if (!i.filtersActive)
                add(QStringLiteral("mic-filters"), tr("Mic processing"), ReadinessStatus::Unknown,
                    tr("Filters are requested but their active chain is not verified."));
            else {
                voice = node(QStringLiteral("rostrum.micfx"));
                out << route(i.graph, QStringLiteral("mic-filters"), tr("Resolved mic → filters"), source,
                             voice, true);
            }
        }
        if (i.filtersWanted && !i.filtersActive)
            add(QStringLiteral("mic-route"), tr("Mic → Rostrum Mic capture"), ReadinessStatus::Unknown,
                tr("The requested filter chain is not active; the effective microphone path is not "
                   "verified."));
        else
            out << route(i.graph, QStringLiteral("mic-route"), tr("Mic → Rostrum Mic capture"), voice,
                         node(QStringLiteral("rostrum.mic")), true);
        for (const auto &a : i.apps) {
            const auto *n = i.graph ? i.graph->node(a.nodeId) : nullptr;
            const auto *bus = i.scene.bus(a.busId);
            if (bus)
                out << route(i.graph, QStringLiteral("app-%1").arg(a.nodeId),
                             tr("%1 → %2").arg(n ? n->label() : tr("Application"), bus->name), n,
                             node(bus->nodeName()), true, true);
            else
                add(QStringLiteral("app-%1").arg(a.nodeId), n ? n->label() : tr("Application"),
                    ReadinessStatus::Unknown,
                    tr("Application is not assigned to a Rostrum bus; routing is not verified."));
        }
    }

    if (i.legacyRecordings) {
        bool stream = false, micCapture = false;
        for (const auto &r : *i.legacyRecordings) {
            stream = stream || r.capture == Capture::RostrumStream;
            micCapture = micCapture || r.capture == Capture::RostrumMic;
        }
        if (!stream)
            add(QStringLiteral("obs-stream"), tr("OBS Stream Mix"), ReadinessStatus::Attention,
                tr("No Stream Mix capture link observed."), GoLiveProblem::NoStreamMixCapture);
        if (!micCapture)
            add(QStringLiteral("obs-mic"), tr("OBS Mic"), ReadinessStatus::Attention,
                tr("No Rostrum Mic capture link observed."), GoLiveProblem::NoMicCapture);
    } else if (!i.obsFresh || !i.obsState || !i.obsState->scopeKnown || !i.graph) {
        add(QStringLiteral("obs"), tr("OBS capture configuration"), ReadinessStatus::Unknown,
            i.obsError.isEmpty() ? tr("A fresh, supported live OBS snapshot is unavailable. Offline settings "
                                      "and graph links alone cannot verify OBS output.")
                                 : i.obsError);
    } else {
        bool configuredStream = false, configuredMic = false, configuredVod = false, incomplete = false;
        for (const auto &in : i.obsState->inputs) {
            const auto id = QStringLiteral("obs-") + in.name;
            if (in.channel.isEmpty() && !i.obsState->programInputs.contains(in.name)) {
                add(id, in.name, ReadinessStatus::Excluded,
                    tr("Not enabled directly on the current program scene."));
                continue;
            }
            if (!isAudioCaptureKind(in.kind) || !in.settingsKnown || !in.muteKnown || !in.tracksKnown ||
                !in.gainKnown || !std::isfinite(in.gain)) {
                incomplete = true;
                add(id, in.name, ReadinessStatus::Unknown,
                    tr("Input kind or settings, mute, gain, or track observation is "
                       "unsupported/incomplete."));
                continue;
            }
            const bool pulse = in.kind == QLatin1String(kPulseInput) || in.kind == QLatin1String(kPulseOutput);
            const auto device = in.settings.value(QLatin1String("device_id"));
            if (pulse && (!device.isString() || device.toString().isEmpty())) {
                incomplete = true;
                add(id, in.name, ReadinessStatus::Unknown,
                    tr("The configured capture device identity is incomplete or invalid."));
                continue;
            }
            const auto capture = classify(in, i.facts);
            if (capture == Capture::None) {
                incomplete = true;
                add(id, in.name, ReadinessStatus::Unknown,
                    tr("The configured capture target could not be resolved."));
                continue;
            }
            configuredStream = configuredStream || capture == Capture::RostrumStream;
            configuredMic = configuredMic || capture == Capture::RostrumMic;
            configuredVod = configuredVod || capture == Capture::RostrumVod;
            if (in.muted || in.gain <= 0 || in.tracks == 0) {
                const bool intended = capture == Capture::RostrumStream || capture == Capture::RostrumMic ||
                                      capture == Capture::RostrumVod;
                add(id, in.name, intended ? ReadinessStatus::Attention : ReadinessStatus::Excluded,
                    tr("OBS input is muted, at zero gain, or assigned to no audio tracks."));
                continue;
            }
            if (capture != Capture::RostrumStream && capture != Capture::RostrumMic &&
                capture != Capture::RostrumVod) {
                add(id, in.name, ReadinessStatus::Attention,
                    tr("This capture bypasses Rostrum's Stream Mix or mic controls and can include "
                       "intentionally excluded audio."));
                continue;
            }
            const QString target = capture == Capture::RostrumStream ? QStringLiteral("rostrum.stream")
                                 : capture == Capture::RostrumVod    ? QStringLiteral("rostrum.vod")
                                                                     : QStringLiteral("rostrum.mic");
            const pw::Node *recorder = nullptr;
            bool ambiguous = false;
            for (const auto &n : i.graph->nodes)
                if (n.isCaptureStream() &&
                    (n.appName == QLatin1String("OBS") || n.binary == QLatin1String("obs") ||
                     n.name.startsWith(QLatin1String("OBS")) ||
                     i.graph->clients.value(n.clientId.toUInt()).appName == QLatin1String("OBS")) &&
                    (n.name == QStringLiteral("OBS: ") + in.name || n.mediaName == in.name)) {
                    if (recorder)
                        ambiguous = true;
                    recorder = &n;
                }
            if (!recorder || ambiguous) {
                add(id, in.name, ReadinessStatus::Unknown,
                    tr("Configured for %1, but a unique live capture stream was not observed.").arg(target));
                continue;
            }
            bool wrong = false;
            for (const auto &l : i.graph->links)
                if (l.inNode == recorder->id) {
                    const auto *from = i.graph->node(l.outNode);
                    if (!from || from->name != target)
                        wrong = true;
                }
            if (wrong) {
                add(id, in.name, ReadinessStatus::Attention,
                    tr("OBS is configured for %1, but its observed capture links come from a different or "
                       "unresolved target.")
                        .arg(target));
                continue;
            }
            auto r = route(i.graph, id, in.name, i.graph->nodeByName(target), recorder, true);
            r.detail =
                tr("Configured target: %1. ").arg(target) + r.detail +
                tr(" Input track assignments were read; the output's selected track was not verified.");
            out << r;
        }
        if (!configuredStream && !incomplete)
            add(QStringLiteral("obs-stream"), tr("OBS Stream Mix"), ReadinessStatus::Attention,
                tr("No supported enabled input configured for Rostrum Stream Mix."),
                GoLiveProblem::NoStreamMixCapture);
        if (!configuredMic && !incomplete && mic && feedsStream(mic->destination))
            add(QStringLiteral("obs-mic"), tr("OBS Mic"), ReadinessStatus::Attention,
                tr("No supported enabled input configured for Rostrum Mic."), GoLiveProblem::NoMicCapture);
        if (i.obsState->streamService.compare(QLatin1String("Twitch"), Qt::CaseInsensitive) == 0) {
            if (!configuredVod && !incomplete) {
                add(QStringLiteral("obs-vod"), tr("OBS VOD Mix"), ReadinessStatus::Attention,
                    tr("No supported enabled input configured for Rostrum VOD Mix."));
            }
            if (i.obsState->twitchVodTrack == 2) {
                add(QStringLiteral("obs-twitch-vod"), tr("Twitch VOD Track"), ReadinessStatus::Verified,
                    tr("Output → Streaming → Twitch VOD Track is set to track 2."));
            } else {
                add(QStringLiteral("obs-twitch-vod"), tr("Twitch VOD Track"), ReadinessStatus::Attention,
                    tr("Output → Streaming → Twitch VOD Track is not set to track 2."));
            }
        }
    }
    add(QStringLiteral("output-proof"), tr("Recording and audience audio"), ReadinessStatus::Unknown,
        tr("No sound was played or recorded. Output track selection, recording contents and audience audio "
           "were not verified."));
    return out;
}

QList<GoLiveProblem> readinessWarnings(const QList<ReadinessResult> &results)
{
    QList<GoLiveProblem> out;
    for (const auto &r : results)
        if (r.warning && !out.contains(*r.warning))
            out << *r.warning;
    return out;
}
} // namespace rostrum::obs
