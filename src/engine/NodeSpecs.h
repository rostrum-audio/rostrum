#pragma once

#include "core/Model.h"

#include <QMap>
#include <QString>

#include <cstdint>

namespace rostrum::engine {

inline constexpr const char *kPhonesNode = "rostrum.phones";
inline constexpr const char *kStreamNode = "rostrum.stream";
inline constexpr const char *kVodNode = "rostrum.vod";
inline constexpr const char *kMicNode = "rostrum.mic";
inline constexpr const char *kSidetoneNode = "rostrum.sidetone";
// Mic filters: the processing node (an audioconvert running Rostrum's filter graph) and the
// virtual mic that apps record instead of the hardware mic.
inline constexpr const char *kMicFxNode = "rostrum.micfx";
inline constexpr const char *kFilteredNode = "rostrum.filtered";

inline constexpr const char *kStreamDescription = "Rostrum Stream Mix";
inline constexpr const char *kVodDescription = "Rostrum VOD Mix";
inline constexpr const char *kMicDescription = "Rostrum Mic";
inline constexpr const char *kFilteredDescription = "Rostrum Filtered Mic";

enum class NodeRole { Bus, Phones, Stream, Vod, Mic, Sidetone, MicFx, Filtered };

struct NodeSpec
{
    QString name;
    QString description;
    NodeRole role = NodeRole::Bus;
    QString busId;
    bool mono = false;
    bool source = false;

    QMap<QString, QString> properties() const;
};

QString roleName(NodeRole role);

// A node keeps the description it was created with, and nodes outlive Rostrum. Only Rostrum links
// to the headphones mix and sidetone, so those two can be recreated to take a new name without
// moving an app or an OBS source.
bool needsRename(const NodeSpec &spec, const QString &currentDescription);

// Every virtual node Rostrum owns for this scene, in creation order. The mic filter nodes are
// not among them: they come and go with the filters and never hold up the mix.
QList<NodeSpec> desiredNodes(const Scene &scene);

NodeSpec filteredMicSpec();

// The graph cycle, in samples, that rostrum.micfx forces while it runs noise removal. RNNoise runs
// in the PipeWire daemon's realtime thread, and on a busy desktop one of its 10 ms frames can take
// several milliseconds (its model is about 1 MB and falls out of the CPU cache between frames).
// When an app such as a voice chat asks for 256 samples (5.3 ms), those frames overrun the cycle,
// and every device in the graph, headphones included, crackles. PipeWire has no per-node minimum,
// so the node forces this cycle, raised to the user's clock.min-quantum and capped at their
// clock.max-quantum. A clock.force-quantum in the settings metadata still overrides it.
inline constexpr uint32_t kMicFxQuantum = 512;
// minQuantum, maxQuantum: the "settings" metadata values, empty when not set.
uint32_t micFxQuantum(const QString &minQuantum, const QString &maxQuantum);

// For spa-node-factory: a bare mono audioconvert. It has no media.class, so the session manager
// leaves it alone and it appears in no device list. forceQuantum: node.force-quantum, 0 for none.
QMap<QString, QString> micFxProperties(uint32_t forceQuantum);

} // namespace rostrum::engine
