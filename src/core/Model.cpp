#include "core/Model.h"

#include <QHash>
#include <QRegularExpression>

namespace rostrum {

QString destinationName(Destination d)
{
    switch (d) {
    case Destination::Phones:
        return QStringLiteral("phones");
    case Destination::Stream:
        return QStringLiteral("stream");
    case Destination::Both:
        return QStringLiteral("both");
    }
    return QStringLiteral("both");
}

std::optional<Destination> destinationFromString(const QString &s)
{
    const QString v = s.trimmed().toLower();
    if (v == QLatin1String("phones") || v == QLatin1String("headphones")) {
        return Destination::Phones;
    }
    if (v == QLatin1String("stream")) {
        return Destination::Stream;
    }
    if (v == QLatin1String("both")) {
        return Destination::Both;
    }
    return std::nullopt;
}

QString matchKeyName(MatchKey k)
{
    return k == MatchKey::Binary ? QStringLiteral("binary") : QStringLiteral("name");
}

std::optional<MatchKey> matchKeyFromString(const QString &s)
{
    const QString v = s.trimmed().toLower();
    if (v == QLatin1String("name")) {
        return MatchKey::Name;
    }
    if (v == QLatin1String("binary")) {
        return MatchKey::Binary;
    }
    return std::nullopt;
}

QString Bus::nodeName() const
{
    return QStringLiteral("rostrum.") + id;
}

QString categoryName(AppCategory c)
{
    switch (c) {
    case AppCategory::Game:
        return QStringLiteral("game");
    case AppCategory::Voice:
        return QStringLiteral("voice");
    case AppCategory::Music:
        return QStringLiteral("music");
    case AppCategory::Alerts:
        return QStringLiteral("alerts");
    case AppCategory::Desktop:
        return QStringLiteral("desktop");
    case AppCategory::None:
        break;
    }
    return QStringLiteral("none");
}

std::optional<AppCategory> categoryFromString(const QString &s)
{
    static const QHash<QString, AppCategory> map = {
        {QStringLiteral("none"), AppCategory::None},   {QStringLiteral("game"), AppCategory::Game},
        {QStringLiteral("voice"), AppCategory::Voice}, {QStringLiteral("music"), AppCategory::Music},
        {QStringLiteral("alerts"), AppCategory::Alerts}, {QStringLiteral("desktop"), AppCategory::Desktop},
    };
    if (auto it = map.constFind(s.trimmed().toLower()); it != map.cend()) {
        return it.value();
    }
    return std::nullopt;
}

Bus *Scene::bus(const QString &id)
{
    for (auto &b : buses) {
        if (b.id == id) {
            return &b;
        }
    }
    return nullptr;
}

const Bus *Scene::bus(const QString &id) const
{
    return const_cast<Scene *>(this)->bus(id);
}

QList<Bus> Scene::playbackBuses() const
{
    QList<Bus> out;
    for (const auto &b : buses) {
        if (!b.isInput()) {
            out.append(b);
        }
    }
    return out;
}

const Bus *Scene::busFor(AppCategory category) const
{
    if (category == AppCategory::None) {
        return nullptr;
    }
    for (const auto &b : buses) {
        if (!b.isInput() && b.autoCategory == category) {
            return &b;
        }
    }
    return nullptr;
}

AppRule *Scene::rule(MatchKey key, const QString &match)
{
    for (auto &r : rules) {
        if (r.key == key && r.match.compare(match, Qt::CaseInsensitive) == 0) {
            return &r;
        }
    }
    return nullptr;
}

const AppRule *Scene::rule(MatchKey key, const QString &match) const
{
    return const_cast<Scene *>(this)->rule(key, match);
}

void Scene::removeRulesForBus(const QString &busId)
{
    rules.removeIf([&](const AppRule &r) { return r.busId == busId; });
}

QString Scene::summary() const
{
    QStringList parts;
    parts << QStringLiteral("%1 buses").arg(buses.size());
    for (const auto &b : buses) {
        if (b.isInput() || b.destination == Destination::Both) {
            continue;
        }
        parts << QStringLiteral("%1 → %2").arg(b.name, b.destination == Destination::Stream
                                                           ? QStringLiteral("Stream")
                                                           : QStringLiteral("Headphones"));
    }
    if (const Bus *mic = micBus(); mic && mic->muted) {
        parts << QStringLiteral("mic muted");
    }
    if (masterStreamMuted) {
        parts << QStringLiteral("stream muted");
    }
    return parts.join(QStringLiteral(", "));
}

namespace defaults {

const QList<Swatch> &palette()
{
    static const QList<Swatch> p = {
        {"Red", "#da4453"},    {"Green", "#27ae60"}, {"Blue", "#3daee9"},  {"Purple", "#9b59b6"},
        {"Amber", "#f39c1f"},  {"Gray", "#7f8c8d"},  {"Teal", "#16a085"},  {"Pink", "#e93a9a"},
        {"Orange", "#f67400"}, {"Lime", "#a3c13a"},  {"Indigo", "#5254b4"}, {"Brown", "#a0694b"},
    };
    return p;
}

Destination destinationFor(const QString &busId)
{
    if (busId == QLatin1String("music") || busId == QLatin1String(kMicBusId)) {
        return Destination::Stream;
    }
    return Destination::Both;
}

AppCategory autoCategoryFor(const QString &busId)
{
    return busId == QLatin1String(kMicBusId) ? AppCategory::None
                                              : categoryFromString(busId).value_or(AppCategory::None);
}

Scene scene(const QString &name)
{
    struct Def
    {
        const char *id;
        const char *name;
    };
    static const Def defs[] = {
        {"mic", "Mic"},     {"game", "Game"},     {"voice", "Voice"},
        {"music", "Music"}, {"alerts", "Alerts"}, {"desktop", "Desktop"},
    };
    Scene s;
    s.name = name;
    int i = 0;
    for (const auto &d : defs) {
        Bus b;
        b.id = QString::fromLatin1(d.id);
        b.name = QString::fromLatin1(d.name);
        b.color = QString::fromLatin1(palette().at(i++).hex);
        b.kind = b.id == QLatin1String(kMicBusId) ? BusKind::Input : BusKind::Playback;
        b.destination = destinationFor(b.id);
        b.autoCategory = autoCategoryFor(b.id);
        s.buses.append(b);
    }
    return s;
}

} // namespace defaults

QString makeSlug(const QString &name, const QStringList &taken, const QString &fallback)
{
    static const QRegularExpression nonWord(QStringLiteral("[^a-z0-9]+"));
    QString base = name.normalized(QString::NormalizationForm_KD).toLower();
    base.replace(nonWord, QStringLiteral("-"));
    while (base.startsWith(QLatin1Char('-'))) {
        base.remove(0, 1);
    }
    while (base.endsWith(QLatin1Char('-'))) {
        base.chop(1);
    }
    if (base.isEmpty()) {
        base = fallback;
    }
    base = base.left(28); // room for a "-NN" suffix inside the 32-character id limit
    while (base.endsWith(QLatin1Char('-'))) {
        base.chop(1);
    }
    QString candidate = base;
    for (int n = 2; taken.contains(candidate, Qt::CaseInsensitive); ++n) {
        candidate = QStringLiteral("%1-%2").arg(base).arg(n);
    }
    return candidate;
}

Scene mergeStructure(const Scene &saved, const Scene &current)
{
    Scene out = current;
    out.masterPhones = saved.masterPhones;
    out.masterPhonesMuted = saved.masterPhonesMuted;
    out.masterStream = saved.masterStream;
    out.masterStreamMuted = saved.masterStreamMuted;
    out.sidetoneVolume = saved.sidetoneVolume;
    for (auto &b : out.buses) {
        if (const Bus *s = saved.bus(b.id)) {
            b.volume = s->volume;
            b.muted = s->muted;
            b.destination = s->destination;
        }
    }
    for (auto &r : out.rules) {
        if (const AppRule *s = saved.rule(r.key, r.match)) {
            r.volume = s->volume;
            r.muted = s->muted;
        }
    }
    return out;
}

} // namespace rostrum
