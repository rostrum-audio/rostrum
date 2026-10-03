#pragma once

#include <QHash>
#include <QList>
#include <QMap>
#include <QString>
#include <QVariant>

#include <algorithm>
#include <cstdint>

namespace rostrum::pw {

struct Node
{
    uint32_t id = 0;
    QString serial;
    QString name;
    QString description;
    QString nick;
    QString mediaClass;
    QString appName;
    QString binary;
    QString mediaName;
    QString clientId;
    qint64 pid = 0;
    QMap<QString, QString> props; // full properties once node info arrives
    int channels = 0;             // from the Props param, 0 = unknown
    bool muted = false;
    QList<float> volumes;
    QVariantMap params; // the Props "params" struct (audioconvert settings, filter-graph controls)
    bool running = false; // processing audio, for nodes Rostrum binds

    QString prop(const char *key) const { return props.value(QString::fromUtf8(key)); }
    QString label() const;

    bool isSink() const { return mediaClass.startsWith(QLatin1String("Audio/Sink")); }
    bool isSource() const { return mediaClass.startsWith(QLatin1String("Audio/Source")); }
    bool isPlaybackStream() const { return mediaClass == QLatin1String("Stream/Output/Audio"); }
    bool isCaptureStream() const { return mediaClass == QLatin1String("Stream/Input/Audio"); }
    bool isRostrum() const { return name.startsWith(QLatin1String("rostrum.")); }
    QString rostrumRole() const { return prop("rostrum.role"); }
};

struct Port
{
    uint32_t id = 0;
    uint32_t nodeId = 0;
    bool output = false;
    bool monitor = false;
    QString channel;
    QString name;
};

struct Link
{
    uint32_t id = 0;
    uint32_t outNode = 0;
    uint32_t outPort = 0;
    uint32_t inNode = 0;
    uint32_t inPort = 0;
};

struct Client
{
    uint32_t id = 0;
    QString appName;
    QString version;
};

struct Graph
{
    QHash<uint32_t, Node> nodes;
    QHash<uint32_t, Port> ports;
    QHash<uint32_t, Link> links;
    QHash<uint32_t, Client> clients;
    QHash<uint32_t, QString> factories; // global id -> factory.name

    bool hasFactory(const QString &name) const { return std::find(factories.cbegin(), factories.cend(), name) != factories.cend(); }
    const Node *node(uint32_t id) const;
    const Node *nodeByName(const QString &name) const;
    // Ports carrying the node's audio out: monitor ports for sinks, capture ports otherwise.
    QList<Port> outputPorts(uint32_t nodeId) const;
    QList<Port> inputPorts(uint32_t nodeId) const;
    bool hasLink(uint32_t outPort, uint32_t inPort) const;
};

// Channel-matched port pairs for linking `out` ports into `in` ports.
// Matches by audio.channel, spreads a mono output to every input, sums every output into a
// mono input, and falls back to index order.
QList<QPair<uint32_t, uint32_t>> matchPorts(const QList<Port> &out, const QList<Port> &in);

// Mono downmix by links: every output into both front inputs (FL and FR), which PipeWire sums.
// Anything without two outputs and both front inputs gets matchPorts.
QList<QPair<uint32_t, uint32_t>> monoPorts(const QList<Port> &out, const QList<Port> &in);

} // namespace rostrum::pw
