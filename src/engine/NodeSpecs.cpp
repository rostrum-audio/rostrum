#include "engine/NodeSpecs.h"

namespace rostrum::engine {

QString roleName(NodeRole role)
{
    switch (role) {
    case NodeRole::Bus:
        return QStringLiteral("bus");
    case NodeRole::Phones:
        return QStringLiteral("phones");
    case NodeRole::Stream:
        return QStringLiteral("stream");
    case NodeRole::Mic:
        return QStringLiteral("mic");
    case NodeRole::Sidetone:
        return QStringLiteral("sidetone");
    case NodeRole::MicFx:
        return QStringLiteral("micfx");
    case NodeRole::Filtered:
        return QStringLiteral("filtered");
    }
    return {};
}

QMap<QString, QString> NodeSpec::properties() const
{
    QMap<QString, QString> p;
    p.insert(QStringLiteral("node.name"), name);
    p.insert(QStringLiteral("node.description"), description);
    p.insert(QStringLiteral("media.class"), source ? QStringLiteral("Audio/Source/Virtual")
                                                   : QStringLiteral("Audio/Sink"));
    p.insert(QStringLiteral("audio.position"), mono ? QStringLiteral("[ MONO ]") : QStringLiteral("[ FL FR ]"));
    p.insert(QStringLiteral("audio.channels"), mono ? QStringLiteral("1") : QStringLiteral("2"));
    // Volume and mute on a virtual sink must also apply to what its monitor carries.
    p.insert(QStringLiteral("monitor.channel-volumes"), QStringLiteral("true"));
    // The node lives in the daemon, so audio keeps flowing if Rostrum quits or crashes.
    p.insert(QStringLiteral("object.linger"), QStringLiteral("true"));
    p.insert(QStringLiteral("node.virtual"), QStringLiteral("true"));
    // Never let WirePlumber pick a Rostrum node as the system default device.
    p.insert(QStringLiteral("priority.session"), QStringLiteral("0"));
    p.insert(QStringLiteral("priority.driver"), QStringLiteral("0"));
    p.insert(QStringLiteral("device.icon-name"), source ? QStringLiteral("audio-input-microphone")
                                                        : QStringLiteral("audio-card"));
    p.insert(QStringLiteral("rostrum.role"), roleName(role));
    if (!busId.isEmpty()) {
        p.insert(QStringLiteral("rostrum.bus"), busId);
    }
    return p;
}

bool needsRename(const NodeSpec &spec, const QString &currentDescription)
{
    return (spec.role == NodeRole::Phones || spec.role == NodeRole::Sidetone) &&
           currentDescription != spec.description;
}

QList<NodeSpec> desiredNodes(const Scene &scene)
{
    QList<NodeSpec> out;
    out.append({QString::fromLatin1(kPhonesNode), QStringLiteral("Rostrum Headphones Mix"), NodeRole::Phones});
    out.append({QString::fromLatin1(kStreamNode), QString::fromLatin1(kStreamDescription), NodeRole::Stream});
    out.append({QString::fromLatin1(kMicNode), QString::fromLatin1(kMicDescription), NodeRole::Mic, QString(),
                true, true});
    out.append({QString::fromLatin1(kSidetoneNode), QStringLiteral("Rostrum Sidetone"), NodeRole::Sidetone,
                QString(), true, false});
    for (const auto &b : scene.buses) {
        if (b.isInput()) {
            continue;
        }
        out.append({b.nodeName(), QStringLiteral("Rostrum %1").arg(b.name), NodeRole::Bus, b.id});
    }
    return out;
}

NodeSpec filteredMicSpec()
{
    return {QString::fromLatin1(kFilteredNode), QString::fromLatin1(kFilteredDescription), NodeRole::Filtered,
            QString(), true, true};
}

QMap<QString, QString> micFxProperties()
{
    return {
        {QStringLiteral("factory.name"), QStringLiteral("audio.convert")},
        {QStringLiteral("node.name"), QString::fromLatin1(kMicFxNode)},
        {QStringLiteral("node.description"), QStringLiteral("Rostrum Mic Filters")},
        {QStringLiteral("audio.channels"), QStringLiteral("1")},
        {QStringLiteral("audio.position"), QStringLiteral("[ MONO ]")},
        {QStringLiteral("object.linger"), QStringLiteral("true")},
        {QStringLiteral("node.virtual"), QStringLiteral("true")},
        {QStringLiteral("rostrum.role"), roleName(NodeRole::MicFx)},
    };
}

} // namespace rostrum::engine
