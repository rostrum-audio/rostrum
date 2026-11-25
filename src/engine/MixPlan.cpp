#include "engine/MixPlan.h"

#include "engine/NodeSpecs.h"
#include "pw/Graph.h"

#include <algorithm>

namespace rostrum::engine {

namespace {

QMap<QString, QString> sendBase(const Bus &bus, const QString &name, const QString &destination, const QString &mix)
{
    return {
        {QStringLiteral("factory.name"), QStringLiteral("audio.convert")},
        // Both sides are DSP ports in the graph clock; see micFxProperties() for why output.
        {QStringLiteral("convert.direction"), QStringLiteral("output")},
        {QStringLiteral("node.name"), name},
        {QStringLiteral("node.description"), QStringLiteral("Rostrum %1 → %2").arg(bus.name, destination)},
        {QStringLiteral("audio.channels"), QStringLiteral("2")},
        {QStringLiteral("audio.position"), QStringLiteral("[ FL FR ]")},
        {QStringLiteral("object.linger"), QStringLiteral("true")},
        {QStringLiteral("node.virtual"), QStringLiteral("true")},
        {QStringLiteral("rostrum.role"), roleName(NodeRole::Send)},
        {QStringLiteral("rostrum.bus"), bus.id},
        {QStringLiteral("rostrum.mix"), mix},
    };
}

bool hasChat(const Bus &b)
{
    return b.chat && chatAllowed(b);
}

} // namespace

QMap<QString, QString> sendProperties(const Bus &bus, Mix mix)
{
    return sendBase(bus, sendNodeName(bus.id, mix),
                    mix == Mix::Phones ? QStringLiteral("Headphones") : QStringLiteral("Stream"), mixName(mix));
}

QMap<QString, QString> chatSendProperties(const Bus &bus)
{
    return sendBase(bus, chatSendNodeName(bus.id), QStringLiteral("Voice chat"), QStringLiteral("chat"));
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
        if (hasChat(b)) {
            out.append({chatSendNodeName(b.id), b.id, Mix::Phones, true});
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
            if (hasChat(b)) {
                out.append({bus, chatSendNodeName(b.id)});
                out.append({chatSendNodeName(b.id), QString::fromLatin1(kChatMicNode)});
            }
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

QHash<QString, NodeLevel> chatSendLevels(const Scene &scene, const QSet<QString> &blockedBuses)
{
    QHash<QString, NodeLevel> out;
    for (const auto &b : scene.buses) {
        if (b.isInput() || !hasChat(b)) {
            continue;
        }
        out.insert(chatSendNodeName(b.id), {b.chatVolume, 0.0, b.chatMuted || blockedBuses.contains(b.id), 0.5});
    }
    return out;
}

QStringList busDisplayNames(const Scene &scene, const QSet<QString> &busIds)
{
    QStringList names;
    for (const QString &id : busIds) {
        if (const Bus *b = scene.bus(id)) {
            names << b->name;
        }
    }
    names.sort();
    return names;
}

QSet<QString> chatLoopBuses(const Scene &scene, const QMultiHash<QString, QString> &appKeysByBus,
                            const QSet<QString> &appKeysOnChatMic)
{
    QSet<QString> out;
    for (const auto &b : scene.buses) {
        if (b.isInput() || !hasChat(b)) {
            continue;
        }
        const auto keys = appKeysByBus.values(b.id);
        if (std::any_of(keys.cbegin(), keys.cend(), [&](const QString &k) { return appKeysOnChatMic.contains(k); })) {
            out.insert(b.id);
        }
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
