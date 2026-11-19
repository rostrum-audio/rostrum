#pragma once

// What the mix graph should look like for a scene: the send nodes after each playback bus, the
// links between Rostrum's nodes, and the level of each node. Pure functions, so the Engine only
// applies them and the tests can check them without a daemon.

#include "core/Model.h"

#include <QHash>
#include <QList>
#include <QMap>
#include <QSet>
#include <QString>

#include <cstdint>

namespace rostrum::pw {
struct Graph;
}

namespace rostrum::engine {

using rostrum::sendNodeName; // core/Model.h, shared with stream readiness

// For spa-node-factory: a bare stereo audio.convert, like rostrum.micfx. No media.class, so the
// session manager leaves it alone and it appears in no device list. It lingers like every node.
QMap<QString, QString> sendProperties(const Bus &bus, Mix mix);

struct SendSpec
{
    QString name;
    QString busId;
    Mix mix = Mix::Phones;
};
// Two per playback bus (Headphones, then Stream), in bus order. The mic has none.
QList<SendSpec> desiredSends(const Scene &scene);

// A send of a playback bus that is in the scene.
bool sendWanted(const Scene &scene, const QString &nodeName);

struct NodeLink
{
    QString from;
    QString to;
    bool operator==(const NodeLink &) const = default;
};
// Links between the playback buses and the masters, by node name. perMix: every bus into both its
// sends, the Headphones send into rostrum.phones when the bus feeds headphones, the Stream send
// into rostrum.stream when it feeds the stream and into rostrum.vod when it also feeds the VOD.
// Otherwise the bus links straight into the masters, one level for both.
QList<NodeLink> busLinks(const Scene &scene, bool perMix);

struct NodeLevel
{
    double position = 1.0; // fader position, 0..1
    double balance = 0.0;
    bool mute = false;
    double gain = 1.0; // session-only multiplier (ducking)
};
// The level of every playback bus node and, with perMix, of its sends, by node name. perMix: the
// bus node is at unity with the bus's balance, mute and ducking; the Headphones send carries the
// Headphones level and is muted where the bus does not reach headphones or is dimmed by solo; the
// Stream send carries the Stream level and is muted where the bus does not reach the stream.
// Single level: the bus node carries the Stream level, muted when the bus is muted or dimmed.
QHash<QString, NodeLevel> busNodeLevels(const Scene &scene, const QSet<QString> &soloed,
                                        const QSet<QString> &duckTargets, double duckGain, bool perMix);

struct FadeLevel
{
    double position = 0.0; // 0 when muted
    double balance = 0.0;
    bool operator==(const FadeLevel &) const = default;
};
// busNodeLevels without ducking, as scene fades see them: a muted node is at 0.
QHash<QString, FadeLevel> busFadeLevels(const Scene &scene, const QSet<QString> &soloed, bool perMix);

// What the Voice bus meter is multiplied by for the ducking trigger. perMix: the louder of the
// levels for the mixes the bus reaches (the meter is before both faders). Single level: 1, as the
// bus monitor is already after the fader and mute.
double voiceTriggerGain(const Bus &voice, bool dimmedBySolo, bool perMix);

// A new audio.convert node starts in convert mode with channel-less ports; only after PortConfig
// does it expose the DSP ports, each with an audio.channel.
bool hasDspPorts(const pw::Graph &g, uint32_t node);

// The send has no DSP ports yet and this run has not configured it. A send that lingered from an
// earlier run keeps its ports, and configuring it again would drop its audio for a moment.
bool sendNeedsPortConfig(const pw::Graph &g, uint32_t id, const QSet<uint32_t> &configured);

} // namespace rostrum::engine
