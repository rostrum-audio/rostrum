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
        if (suffix.compare(QLatin1String("chat"), Qt::CaseInsensitive) == 0) {
            return {text.left(dot), std::nullopt, true};
        }
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
        return {whole, std::nullopt, false, {}};
    }
    const MixTarget t = splitMixTarget(text.trimmed());
    const QString bus = t.mix || t.chat ? resolveBusId(scene, t.bus) : QString();
    if (bus.isEmpty()) {
        return {{}, std::nullopt, false, QString::fromLatin1(kErrorNoSuchBus)};
    }
    bool bad = bus == QLatin1String(kStreamId) || bus == QLatin1String(kPhonesId) ||
               bus == QLatin1String(kMicBusId);
    if (t.chat && !bad) {
        const Bus *b = scene.bus(bus);
        bad = !b || !chatAllowed(*b);
    }
    return {bus, t.mix, t.chat, bad ? QString::fromLatin1(kErrorInvalidValue) : QString()};
}

std::optional<Side> parseSide(const QString &text)
{
    if (text.compare(QLatin1String("stream"), Qt::CaseInsensitive) == 0) {
        return Side::Stream;
    }
    if (text.compare(QLatin1String("phones"), Qt::CaseInsensitive) == 0) {
        return Side::Phones;
    }
    if (text.compare(QLatin1String("both"), Qt::CaseInsensitive) == 0) {
        return Side::Both;
    }
    if (text.compare(QLatin1String("chat"), Qt::CaseInsensitive) == 0) {
        return Side::Chat;
    }
    return std::nullopt;
}

QString sideName(Side side)
{
    switch (side) {
    case Side::Stream:
        return QStringLiteral("stream");
    case Side::Phones:
        return QStringLiteral("phones");
    case Side::Chat:
        return QStringLiteral("chat");
    case Side::Both:
        break;
    }
    return QStringLiteral("both");
}

bool playsIn(Destination d, bool muted, Mix mix)
{
    return !muted && (mix == Mix::Phones ? feedsPhones(d) : feedsStream(d));
}

InMix toggledInMix(Destination d, bool muted, Mix mix)
{
    const Destination only = mix == Mix::Phones ? Destination::Phones : Destination::Stream;
    const Destination otherOnly = mix == Mix::Phones ? Destination::Stream : Destination::Phones;
    if (playsIn(d, muted, mix)) {
        // Off this mix. A bus only here would play nowhere, so it is muted instead.
        return d == Destination::Both ? InMix{otherOnly, false} : InMix{d, true};
    }
    if (muted) {
        return {only, false}; // silent on the other side before, so still silent there
    }
    return {Destination::Both, false}; // it was on the other mix only
}

ChatState toggledChat(bool chat, bool muted)
{
    return chat ? ChatState{true, !muted} : ChatState{true, false};
}

namespace {

bool hasOneLevel(const QString &busId)
{
    return busId == QLatin1String(kMicBusId) || busId == QLatin1String(kStreamId) ||
           busId == QLatin1String(kPhonesId);
}

} // namespace

StepTarget resolveStep(const Scene &scene, const QString &busText, const QString &sideText, double delta,
                       bool perMixLevels)
{
    StepTarget out;
    out.busId = resolveBusId(scene, busText);
    if (out.busId.isEmpty()) {
        out.problem = StepProblem::NoSuchBus;
        return out;
    }
    const auto side = parseSide(sideText);
    if (!side) {
        out.problem = StepProblem::BadSide;
        return out;
    }
    out.side = *side;
    if (out.side != Side::Both && hasOneLevel(out.busId)) {
        out.problem = StepProblem::OneLevelBus;
    } else if (out.side == Side::Chat && !chatAllowed(*scene.bus(out.busId))) {
        out.problem = StepProblem::BadSide;
    } else if (!std::isfinite(delta) || delta < -1.0 || delta > 1.0) {
        out.problem = StepProblem::BadDelta;
    } else if (out.side != Side::Both && !perMixLevels) {
        out.problem = StepProblem::NoPerMixLevels;
    }
    return out;
}

InMixTarget resolveInMix(const Scene &scene, const QString &busText, const QString &sideText)
{
    InMixTarget out;
    out.busId = resolveBusId(scene, busText);
    if (out.busId.isEmpty()) {
        out.problem = InMixProblem::NoSuchBus;
        return out;
    }
    const auto side = parseSide(sideText);
    if (!side || *side == Side::Both) {
        out.problem = InMixProblem::BadSide;
        return out;
    }
    out.chat = *side == Side::Chat;
    out.mix = *side == Side::Phones ? Mix::Phones : Mix::Stream;
    if (hasOneLevel(out.busId)) {
        out.problem = InMixProblem::NotPlaybackBus;
    } else if (out.chat && !chatAllowed(*scene.bus(out.busId))) {
        out.problem = InMixProblem::BadSide;
    }
    return out;
}

} // namespace rostrum::remote
