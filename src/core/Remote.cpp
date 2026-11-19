#include "core/Remote.h"

#include "core/Model.h"

#include <cmath>

namespace rostrum::remote {

double maxPosition(const QString &busId)
{
    return busId == QLatin1String(kMicBusId) ? 1.5 : 1.0;
}

std::optional<VolumeArg> parseVolumeArg(const QString &text)
{
    const qsizetype eq = text.indexOf(QLatin1Char('='));
    if (eq <= 0) {
        return std::nullopt;
    }
    VolumeArg out;
    out.busId = text.left(eq).trimmed();
    QString value = text.mid(eq + 1).trimmed();
    const bool percent = value.endsWith(QLatin1Char('%'));
    if (percent) {
        value.chop(1);
    }
    bool ok = false;
    out.position = value.toDouble(&ok);
    if (!ok || out.busId.isEmpty() || !std::isfinite(out.position)) {
        return std::nullopt;
    }
    if (percent) {
        out.position /= 100.0;
    }
    return out;
}

QString resolveBusId(const Scene &scene, const QString &text)
{
    const QString t = text.trimmed();
    for (const char *master : {kStreamId, kPhonesId}) {
        if (t.compare(QLatin1String(master), Qt::CaseInsensitive) == 0) {
            return QString::fromLatin1(master);
        }
    }
    for (const auto &b : scene.buses) {
        if (b.id.compare(t, Qt::CaseInsensitive) == 0) {
            return b.id;
        }
    }
    for (const auto &b : scene.buses) {
        if (b.name.compare(t, Qt::CaseInsensitive) == 0) {
            return b.id;
        }
    }
    return {};
}

MixTarget splitMixTarget(const QString &text)
{
    const qsizetype dot = text.lastIndexOf(QLatin1Char('.'));
    if (dot > 0) {
        const QString suffix = text.mid(dot + 1);
        for (const Mix m : {Mix::Phones, Mix::Stream}) {
            if (suffix.compare(mixName(m), Qt::CaseInsensitive) == 0) {
                return {text.left(dot), m};
            }
        }
    }
    return {text, std::nullopt};
}

VolumeTarget resolveVolumeTarget(const Scene &scene, const QString &text)
{
    if (const QString whole = resolveBusId(scene, text); !whole.isEmpty()) {
        return {whole, std::nullopt, {}};
    }
    const MixTarget t = splitMixTarget(text.trimmed());
    const QString bus = t.mix ? resolveBusId(scene, t.bus) : QString();
    if (bus.isEmpty()) {
        return {{}, std::nullopt, QString::fromLatin1(kErrorNoSuchBus)};
    }
    const bool oneLevel = bus == QLatin1String(kStreamId) || bus == QLatin1String(kPhonesId) ||
                          bus == QLatin1String(kMicBusId);
    return {bus, t.mix, oneLevel ? QString::fromLatin1(kErrorInvalidValue) : QString()};
}

} // namespace rostrum::remote
