#include "engine/MixPlan.h"

#include "engine/NodeSpecs.h"
#include "pw/Graph.h"

#include <algorithm>

namespace rostrum::engine {

QString sendNodeName(const QString &busId, Mix mix)
{
    return QStringLiteral("rostrum.%1.%2").arg(busId, mixName(mix));
}

QMap<QString, QString> sendProperties(const Bus &bus, Mix mix)
{
    return {
        {QStringLiteral("factory.name"), QStringLiteral("audio.convert")},
        // Both sides are DSP ports in the graph clock; see micFxProperties() for why output.
        {QStringLiteral("convert.direction"), QStringLiteral("output")},
        {QStringLiteral("node.name"), sendNodeName(bus.id, mix)},
        {QStringLiteral("node.description"),
         QStringLiteral("Rostrum %1 → %2").arg(bus.name, mix == Mix::Phones ? QStringLiteral("Headphones")
                                                                              : QStringLiteral("Stream"))},
        {QStringLiteral("audio.channels"), QStringLiteral("2")},
        {QStringLiteral("audio.position"), QStringLiteral("[ FL FR ]")},
        {QStringLiteral("object.linger"), QStringLiteral("true")},
        {QStringLiteral("node.virtual"), QStringLiteral("true")},
        {QStringLiteral("rostrum.role"), roleName(NodeRole::Send)},
        {QStringLiteral("rostrum.bus"), bus.id},
        {QStringLiteral("rostrum.mix"), mixName(mix)},
    };
}

QList<SendSpec> desiredSends(const Scene &scene)
{
    QList<SendSpec> out;
    for (const auto &b : scene.buses) {
        if (b.isInput()) {
            continue;
        }
        for (const Mix m : {Mix::Phones, Mix::Stream}) {
            out.append({sendNodeName(b.id, m), b.id, m});
        }
    }
    return out;
}

bool sendWanted(const Scene &scene, const QString &nodeName)
{
    const auto sends = desiredSends(scene);
    return std::any_of(sends.cbegin(), sends.cend(), [&](const SendSpec &s) { return s.name == nodeName; });
}

QList<NodeLink> busLinks(const Scene &scene, bool perMix)
{
    const QString phones = QString::fromLatin1(kPhonesNode);
    const QString stream = QString::fromLatin1(kStreamNode);
    const QString vod = QString::fromLatin1(kVodNode);
    QList<NodeLink> out;
    for (const auto &b : scene.buses) {
        if (b.isInput()) {
            continue;
        }
        const QString bus = b.nodeName();
        const QString toPhones = perMix ? sendNodeName(b.id, Mix::Phones) : bus;
        const QString toStream = perMix ? sendNodeName(b.id, Mix::Stream) : bus;
        if (perMix) {
            out.append({bus, toPhones});
            out.append({bus, toStream});
        }
        if (feedsPhones(b.destination)) {
            out.append({toPhones, phones});
        }
        if (feedsStream(b.destination)) {
            out.append({toStream, stream});
        }
        if (feedsVod(b)) {
            out.append({toStream, vod});
        }
    }
    return out;
}

bool hasDspPorts(const pw::Graph &g, uint32_t node)
{
    const auto dsp = [](const QList<pw::Port> &ports) {
        return !ports.isEmpty()
            && std::all_of(ports.cbegin(), ports.cend(), [](const pw::Port &p) { return !p.channel.isEmpty(); });
    };
    return dsp(g.inputPorts(node)) && dsp(g.outputPorts(node));
}

bool sendNeedsPortConfig(const pw::Graph &g, uint32_t id, const QSet<uint32_t> &configured)
{
    return !configured.contains(id) && !hasDspPorts(g, id);
}

} // namespace rostrum::engine
