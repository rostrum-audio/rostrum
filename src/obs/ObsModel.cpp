#include "obs/ObsModel.h"

#include <QJsonArray>

namespace rostrum::obs {

const Input *State::input(const QString &name) const
{
    for (const auto &i : inputs) {
        if (i.name == name) {
            return &i;
        }
    }
    return nullptr;
}

const Input *State::channel(const QString &ch) const
{
    for (const auto &i : inputs) {
        if (!ch.isEmpty() && i.channel == ch) {
            return &i;
        }
    }
    return nullptr;
}

QString State::freeChannel(const QString &family) const
{
    const int count = family == QLatin1String("desktop") ? 2 : 4;
    for (int n = 1; n <= count; ++n) {
        const QString ch = family + QString::number(n);
        if (!channel(ch)) {
            return ch;
        }
    }
    return {};
}

bool isAudioCaptureKind(const QString &kind)
{
    return kind == QLatin1String(kPulseInput) || kind == QLatin1String(kPulseOutput) ||
           kind == QLatin1String(kPwInput) || kind == QLatin1String(kPwOutput) || kind == QLatin1String(kPwApp);
}

namespace {

constexpr quint32 kPwDefaultTarget = 0xFFFFFFFF;

// `device` is a PulseAudio source name: a node.name, or "<sink>.monitor".
Capture classifyDevice(const QString &device)
{
    if (device == QLatin1String(kMicDevice)) {
        return Capture::RostrumMic;
    }
    if (device == QLatin1String(kStreamDevice)) {
        return Capture::RostrumStream;
    }
    if (device == QLatin1String(kFilteredMicDevice)) {
        return Capture::Mic;
    }
    if (device.startsWith(QLatin1String("rostrum."))) {
        return Capture::RostrumBus;
    }
    return device.endsWith(QLatin1String(".monitor")) ? Capture::Output : Capture::Mic;
}

QString monitorOf(const QString &sink)
{
    return sink.isEmpty() ? QStringLiteral("default.monitor") : sink + QLatin1String(".monitor");
}

} // namespace

Capture classify(const Input &input, const Facts &facts)
{
    const QString &kind = input.kind;
    const bool output = kind == QLatin1String(kPulseOutput) || kind == QLatin1String(kPwOutput);

    if (kind == QLatin1String(kPulseInput) || kind == QLatin1String(kPulseOutput)) {
        QString device = input.settings.value(QLatin1String("device_id")).toString(QStringLiteral("default"));
        if (device == QLatin1String("default")) {
            device = output ? monitorOf(facts.defaultSink) : facts.defaultSource;
        }
        return classifyDevice(device);
    }
    if (kind == QLatin1String(kPwInput) || kind == QLatin1String(kPwOutput)) {
        QString node = input.settings.value(QLatin1String("TargetName")).toString();
        if (node.isEmpty()) {
            const auto id = quint32(input.settings.value(QLatin1String("TargetId")).toDouble(kPwDefaultTarget));
            node = id == kPwDefaultTarget ? (output ? facts.defaultSink : facts.defaultSource) : facts.serials.value(id);
            if (node.isEmpty() && id != kPwDefaultTarget) {
                return Capture::None;
            }
        }
        return classifyDevice(output ? monitorOf(node) : node);
    }
    if (kind == QLatin1String(kPwApp)) {
        const bool hasTarget = !input.settings.value(QLatin1String("TargetName")).toString().isEmpty() ||
                               !input.settings.value(QLatin1String("apps")).toArray().isEmpty();
        return hasTarget ? Capture::App : Capture::None;
    }
    return Capture::None;
}

} // namespace rostrum::obs
