#include "engine/MixPlan.h"

#include "engine/NodeSpecs.h"
#include "pw/Graph.h"

#include <algorithm>

namespace rostrum::engine {

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

QHash<QString, NodeLevel> busNodeLevels(const Scene &scene, const QSet<QString> &soloed,
                                        const QSet<QString> &duckTargets, double duckGain, bool perMix)
{
    QHash<QString, NodeLevel> out;
    for (const auto &b : scene.buses) {
        if (b.isInput()) {
            continue;
        }
        const bool dimmed = !soloed.isEmpty() && !soloed.contains(b.id);
        const double gain = duckTargets.contains(b.id) ? duckGain : 1.0;
        if (!perMix) {
            out.insert(b.nodeName(), {b.volume, b.balance, b.muted || dimmed, gain});
            continue;
        }
        out.insert(b.nodeName(), {1.0, b.balance, b.muted, gain});
        out.insert(sendNodeName(b.id, Mix::Phones),
                   {b.phonesVolume, 0.0, !feedsPhones(b.destination) || dimmed, 1.0});
        out.insert(sendNodeName(b.id, Mix::Stream), {b.volume, 0.0, !feedsStream(b.destination), 1.0});
    }
    return out;
}

QHash<QString, FadeLevel> busFadeLevels(const Scene &scene, const QSet<QString> &soloed, bool perMix)
{
    QHash<QString, FadeLevel> out;
    const auto levels = busNodeLevels(scene, soloed, {}, 1.0, perMix);
    for (auto it = levels.cbegin(); it != levels.cend(); ++it) {
        out.insert(it.key(), {it->mute ? 0.0 : it->position, it->balance});
    }
    return out;
}

double voiceTriggerGain(const Bus &voice, bool dimmedBySolo, bool perMix)
{
    if (!perMix) {
        return 1.0;
    }
    return std::max(mixGain(voice, Mix::Stream, dimmedBySolo), mixGain(voice, Mix::Phones, dimmedBySolo));
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
