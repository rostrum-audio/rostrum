#pragma once

#include "core/Model.h"

#include <QMap>
#include <QString>

namespace rostrum::engine {

inline constexpr const char *kPhonesNode = "rostrum.phones";
inline constexpr const char *kStreamNode = "rostrum.stream";
inline constexpr const char *kMicNode = "rostrum.mic";
inline constexpr const char *kSidetoneNode = "rostrum.sidetone";

inline constexpr const char *kStreamDescription = "Rostrum Stream Mix";
inline constexpr const char *kMicDescription = "Rostrum Mic";

enum class NodeRole { Bus, Phones, Stream, Mic, Sidetone };

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

// Every virtual node Rostrum owns for this scene, in creation order.
QList<NodeSpec> desiredNodes(const Scene &scene);

} // namespace rostrum::engine
