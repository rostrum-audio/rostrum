#include "pw/Graph.h"

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

} // namespace rostrum::pw
