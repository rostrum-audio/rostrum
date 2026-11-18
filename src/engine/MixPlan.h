#pragma once

// What the mix graph should look like for a scene: the send nodes after each playback bus, the
// links between Rostrum's nodes, and the level of each node. Pure functions, so the Engine only
// applies them and the tests can check them without a daemon.

#include "core/Model.h"

#include <QList>
#include <QMap>
#include <QSet>
#include <QString>

#include <cstdint>

namespace rostrum::pw {
struct Graph;
}

namespace rostrum::engine {

// "rostrum.<bus>.phones" or "rostrum.<bus>.stream": the bus's level in that mix.
QString sendNodeName(const QString &busId, Mix mix);

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

// A new audio.convert node starts in convert mode with channel-less ports; only after PortConfig
// does it expose the DSP ports, each with an audio.channel.
bool hasDspPorts(const pw::Graph &g, uint32_t node);

// The send has no DSP ports yet and this run has not configured it. A send that lingered from an
// earlier run keeps its ports, and configuring it again would drop its audio for a moment.
bool sendNeedsPortConfig(const pw::Graph &g, uint32_t id, const QSet<uint32_t> &configured);

} // namespace rostrum::engine
