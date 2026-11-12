#include "pw/Graph.h"

#include <pipewire/keys.h>

#include <QMap>

#include <algorithm>

namespace rostrum::pw {

QString Node::label() const
{
    if (!description.isEmpty()) {
        return description;
    }
    if (!nick.isEmpty()) {
        return nick;
    }
    if (!appName.isEmpty()) {
        return appName;
    }
    return name;
}

const Node *Graph::node(uint32_t id) const
{
    auto it = nodes.constFind(id);
    return it == nodes.cend() ? nullptr : &it.value();
}

const Node *Graph::nodeByName(const QString &name) const
{
    if (name.isEmpty()) {
        return nullptr;
    }
    for (const auto &n : nodes) {
        if (n.name == name) {
            return &n;
        }
    }
    return nullptr;
}

namespace {

QList<Port> sortedPorts(QList<Port> ports)
{
    std::sort(ports.begin(), ports.end(), [](const Port &a, const Port &b) { return a.id < b.id; });
    return ports;
}

} // namespace

QList<Port> Graph::outputPorts(uint32_t nodeId) const
{
    const Node *n = node(nodeId);
    const bool sink = n && n->isSink();
    QList<Port> plain;
    QList<Port> monitors;
    for (const auto &p : ports) {
        if (p.nodeId == nodeId && p.output) {
            (p.monitor ? monitors : plain).append(p);
        }
    }
    // Sinks carry their audio out on monitor ports. Virtual sources built from a null sink also
    // flag their capture ports as monitors, so fall back to those when there is nothing else.
    if (sink) {
        return sortedPorts(monitors);
    }
    return sortedPorts(plain.isEmpty() ? monitors : plain);
}

QList<Port> Graph::inputPorts(uint32_t nodeId) const
{
    QList<Port> out;
    for (const auto &p : ports) {
        if (p.nodeId == nodeId && !p.output) {
            out.append(p);
        }
    }
    return sortedPorts(out);
}

bool Graph::hasLink(uint32_t outPort, uint32_t inPort) const
{
    return std::any_of(links.cbegin(), links.cend(),
                       [&](const Link &l) { return l.outPort == outPort && l.inPort == inPort; });
}

QList<QPair<uint32_t, uint32_t>> matchPorts(const QList<Port> &out, const QList<Port> &in)
{
    QList<QPair<uint32_t, uint32_t>> pairs;
    if (out.isEmpty() || in.isEmpty()) {
        return pairs;
    }
    if (out.size() == 1) {
        for (const auto &i : in) {
            pairs.append({out.first().id, i.id});
        }
        return pairs;
    }
    if (in.size() == 1) {
        // Many USB interfaces expose a mono mic as stereo with signal on one side only, so every
        // channel is summed into a mono input. A true dual-mono feed comes out 6 dB hotter.
        for (const auto &o : out) {
            pairs.append({o.id, in.first().id});
        }
        return pairs;
    }
    bool matchedByName = false;
    for (const auto &o : out) {
        for (const auto &i : in) {
            if (!o.channel.isEmpty() && o.channel == i.channel) {
                pairs.append({o.id, i.id});
                matchedByName = true;
            }
        }
    }
    if (matchedByName) {
        return pairs;
    }
    const auto n = std::min(out.size(), in.size());
    for (qsizetype k = 0; k < n; ++k) {
        pairs.append({out.at(k).id, in.at(k).id});
    }
    return pairs;
}

QList<QPair<uint32_t, uint32_t>> monoPorts(const QList<Port> &out, const QList<Port> &in)
{
    QList<Port> front;
    for (const auto &i : in) {
        if (i.channel == QLatin1String("FL") || i.channel == QLatin1String("FR")) {
            front.append(i);
        }
    }
    if (out.size() < 2 || front.size() != 2) {
        return matchPorts(out, in);
    }
    QList<QPair<uint32_t, uint32_t>> pairs;
    for (const auto &o : out) {
        for (const auto &i : front) {
            pairs.append({o.id, i.id});
        }
    }
    return pairs;
}

QMap<QString, QString> linkCreateProps(const Graph &g, uint32_t outPort, uint32_t inPort)
{
    QMap<QString, QString> props;
    props.insert(QStringLiteral(PW_KEY_OBJECT_LINGER), QStringLiteral("true"));
    const Port out = g.ports.value(outPort);
    const Port in = g.ports.value(inPort);
    const Node *outNode = out.id != 0 ? g.node(out.nodeId) : nullptr;
    const Node *inNode = in.id != 0 ? g.node(in.nodeId) : nullptr;
    // Registry IDs are reused after WirePlumber restarts. A reconcile can still hold a
    // pre-removal port ID that already names a different port on the daemon. Node + port
    // names stay with the role we meant to link; the daemon resolves them to the live IDs.
    if (outNode && !outNode->name.isEmpty() && !out.name.isEmpty() && inNode &&
        !inNode->name.isEmpty() && !in.name.isEmpty()) {
        props.insert(QStringLiteral(PW_KEY_LINK_OUTPUT_NODE), outNode->name);
        props.insert(QStringLiteral(PW_KEY_LINK_OUTPUT_PORT), out.name);
        props.insert(QStringLiteral(PW_KEY_LINK_INPUT_NODE), inNode->name);
        props.insert(QStringLiteral(PW_KEY_LINK_INPUT_PORT), in.name);
        return props;
    }
    props.insert(QStringLiteral(PW_KEY_LINK_OUTPUT_PORT), QString::number(outPort));
    props.insert(QStringLiteral(PW_KEY_LINK_INPUT_PORT), QString::number(inPort));
    return props;
}

} // namespace rostrum::pw
