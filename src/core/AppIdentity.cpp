#include "core/AppIdentity.h"

#include <QHash>

namespace rostrum {

QString AppKey::toString() const
{
    return matchKeyName(key) + QLatin1Char(':') + match;
}

AppKey AppKey::fromString(const QString &s)
{
    const auto colon = s.indexOf(QLatin1Char(':'));
    AppKey k;
    if (colon <= 0) {
        k.match = s;
        return k;
    }
    k.key = matchKeyFromString(s.left(colon)).value_or(MatchKey::Name);
    k.match = s.mid(colon + 1);
    return k;
}

QString cleanBinary(const QString &binary)
{
    QString b = binary.trimmed();
    if (b.endsWith(QLatin1String(" (deleted)"))) {
        b.chop(int(qstrlen(" (deleted)")));
    }
    const auto slash = b.lastIndexOf(QLatin1Char('/'));
    if (slash >= 0) {
        b = b.mid(slash + 1);
    }
    return b;
}

bool isGenericAppName(const QString &name)
{
    const QString n = name.trimmed();
    if (n.isEmpty()) {
        return true;
    }
    static const QStringList exact = {
        QStringLiteral("WEBRTC VoiceEngine"), QStringLiteral("Chromium"),       QStringLiteral("Chromium input"),
        QStringLiteral("Chrome"),             QStringLiteral("audio stream"),   QStringLiteral("AudioStream"),
        QStringLiteral("playback"),           QStringLiteral("Playback"),       QStringLiteral("SDL Application"),
        QStringLiteral("OpenAL Soft"),        QStringLiteral("ALSA plug-in"),   QStringLiteral("PipeWire ALSA"),
        QStringLiteral("wine-preloader"),     QStringLiteral("wine64-preloader"),
    };
    if (exact.contains(n, Qt::CaseInsensitive)) {
        return true;
    }
    return n.startsWith(QLatin1String("ALSA plug-in [")) || n.startsWith(QLatin1String("PipeWire ALSA ["));
}

namespace {

QString prettyBinary(const QString &binary)
{
    static const QHash<QString, QString> known = {
        {QStringLiteral("discord"), QStringLiteral("Discord")},
        {QStringLiteral("discordcanary"), QStringLiteral("Discord Canary")},
        {QStringLiteral("discordptb"), QStringLiteral("Discord PTB")},
        {QStringLiteral("vesktop"), QStringLiteral("Vesktop")},
        {QStringLiteral("electron"), QStringLiteral("Electron app")},
        {QStringLiteral("firefox"), QStringLiteral("Firefox")},
        {QStringLiteral("firefox-bin"), QStringLiteral("Firefox")},
        {QStringLiteral("chrome"), QStringLiteral("Google Chrome")},
        {QStringLiteral("chromium"), QStringLiteral("Chromium")},
        {QStringLiteral("brave"), QStringLiteral("Brave")},
        {QStringLiteral("spotify"), QStringLiteral("Spotify")},
        {QStringLiteral("steam"), QStringLiteral("Steam")},
        {QStringLiteral("obs"), QStringLiteral("OBS Studio")},
    };
    const QString lower = binary.toLower();
    if (auto it = known.constFind(lower); it != known.cend()) {
        return it.value();
    }
    QString out = binary;
    if (!out.isEmpty()) {
        out[0] = out.at(0).toUpper();
    }
    return out;
}

} // namespace

AppIdentity identify(const StreamProps &props)
{
    AppIdentity id;
    id.binary = cleanBinary(props.binary);
    const QString name = props.appName.trimmed();
    if (!isGenericAppName(name)) {
        id.displayName = name;
        id.key = {MatchKey::Name, name};
        return id;
    }
    if (!id.binary.isEmpty()) {
        id.displayName = prettyBinary(id.binary);
        id.key = {MatchKey::Binary, id.binary};
        id.unnamed = name.isEmpty();
        return id;
    }
    id.unnamed = true;
    id.displayName = !props.mediaName.isEmpty() ? props.mediaName
                     : !name.isEmpty()          ? name
                                                : props.nodeName;
    id.key = {MatchKey::Name, !name.isEmpty() ? name : id.displayName};
    return id;
}

bool ruleMatches(const AppRule &rule, const StreamProps &props)
{
    if (rule.match.isEmpty()) {
        return false;
    }
    if (rule.key == MatchKey::Name) {
        return rule.match.compare(props.appName.trimmed(), Qt::CaseInsensitive) == 0;
    }
    return rule.match.compare(cleanBinary(props.binary), Qt::CaseInsensitive) == 0;
}

int matchRule(const QList<AppRule> &rules, const StreamProps &props)
{
    for (int i = 0; i < rules.size(); ++i) {
        if (rules.at(i).key == MatchKey::Name && ruleMatches(rules.at(i), props)) {
            return i;
        }
    }
    for (int i = 0; i < rules.size(); ++i) {
        if (rules.at(i).key == MatchKey::Binary && ruleMatches(rules.at(i), props)) {
            return i;
        }
    }
    return -1;
}

} // namespace rostrum
