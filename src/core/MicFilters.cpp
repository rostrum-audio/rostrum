#include "core/MicFilters.h"

#include <QMap>

#include <algorithm>
#include <cmath>

namespace rostrum::micfx {

QString scopeName(Scope s)
{
    return s == Scope::StreamOnly ? QStringLiteral("stream") : QStringLiteral("all");
}

std::optional<Scope> scopeFromString(const QString &s)
{
    if (s == QLatin1String("all")) {
        return Scope::AllApps;
    }
    if (s == QLatin1String("stream")) {
        return Scope::StreamOnly;
    }
    return std::nullopt;
}

QString moduleName(Module m)
{
    switch (m) {
    case Module::Highpass:
        return QStringLiteral("highpass");
    case Module::Denoise:
        return QStringLiteral("denoise");
    case Module::Gate:
        return QStringLiteral("gate");
    case Module::Eq:
        return QStringLiteral("eq");
    case Module::Compressor:
        return QStringLiteral("compressor");
    case Module::Limiter:
        return QStringLiteral("limiter");
    }
    return {};
}

QString moduleNode(Module m)
{
    return QStringLiteral("rostrum_") + moduleName(m);
}

const QList<Param> &params()
{
    using S = Settings;
    static const QList<Param> list = {
        {Module::Highpass, "frequency", "Frequency", &S::highpassHz, 40, 200, 5},
        {Module::Denoise, "strength", "Strength", &S::denoiseStrength, 0, 100, 5, 0.01},
        {Module::Denoise, "voice_threshold", "Voice threshold", &S::voiceThreshold, 0, 95, 5, 0.01},
        {Module::Gate, "threshold", "Threshold", &S::gateThresholdDb, -80, -20, 1},
        {Module::Gate, "range", "Range", &S::gateRangeDb, -80, 0, 1},
        {Module::Gate, "attack", "Attack", &S::gateAttackMs, 0.5, 50, 0.5},
        {Module::Gate, "hold", "Hold", &S::gateHoldMs, 0, 1000, 10},
        {Module::Gate, "release", "Release", &S::gateReleaseMs, 20, 2000, 10},
        {Module::Eq, "low_gain", "Low gain", &S::lowGainDb, -12, 12, 0.5},
        {Module::Eq, "low_frequency", "Low frequency", &S::lowHz, 40, 400, 5},
        {Module::Eq, "mud_gain", "Mud gain", &S::mudGainDb, -12, 6, 0.5},
        {Module::Eq, "mud_frequency", "Mud frequency", &S::mudHz, 150, 1000, 10},
        {Module::Eq, "presence_gain", "Presence gain", &S::presenceGainDb, -6, 12, 0.5},
        {Module::Eq, "presence_frequency", "Presence frequency", &S::presenceHz, 1500, 8000, 100},
        {Module::Eq, "air_gain", "Air gain", &S::airGainDb, -6, 12, 0.5},
        {Module::Eq, "air_frequency", "Air frequency", &S::airHz, 5000, 16000, 250},
        {Module::Compressor, "threshold", "Threshold", &S::compThresholdDb, -60, 0, 1},
        {Module::Compressor, "ratio", "Ratio", &S::compRatio, 1, 20, 0.1},
        {Module::Compressor, "knee", "Knee", &S::compKneeDb, 0, 24, 1},
        {Module::Compressor, "attack", "Attack", &S::compAttackMs, 0.5, 200, 0.5},
        {Module::Compressor, "release", "Release", &S::compReleaseMs, 10, 2000, 10},
        {Module::Compressor, "makeup", "Makeup", &S::compMakeupDb, 0, 24, 0.5},
        {Module::Limiter, "ceiling", "Ceiling", &S::limiterCeilingDb, -12, 0, 0.1},
        {Module::Limiter, "release", "Release", &S::limiterReleaseMs, 10, 1000, 5},
    };
    return list;
}

const QList<Switch> &switches()
{
    using S = Settings;
    static const QList<Switch> list = {
        {Module::Highpass, "enabled", "Enabled", &S::highpass},
        {Module::Highpass, "steep", "Steep", &S::highpassSteep},
        {Module::Denoise, "enabled", "Enabled", &S::denoise},
        {Module::Gate, "enabled", "Enabled", &S::gate},
        {Module::Eq, "enabled", "Enabled", &S::eq},
        {Module::Compressor, "enabled", "Enabled", &S::compressor},
        {Module::Limiter, "enabled", "Enabled", &S::limiter},
    };
    return list;
}

const Param *findParam(const QString &module, const QString &key)
{
    for (const Param &p : params()) {
        if (moduleName(p.module) == module && key == QLatin1String(p.key)) {
            return &p;
        }
    }
    return nullptr;
}

const Switch *findSwitch(const QString &module, const QString &key)
{
    for (const Switch &sw : switches()) {
        if (moduleName(sw.module) == module && key == QLatin1String(sw.key)) {
            return &sw;
        }
    }
    return nullptr;
}

namespace {

double snap(const Param &p, double v)
{
    if (!std::isfinite(v)) {
        v = Settings{}.*p.field;
    }
    v = std::clamp(v, p.min, p.max);
    v = p.min + std::round((v - p.min) / p.step) * p.step;
    return std::round(std::clamp(v, p.min, p.max) * 10000.0) / 10000.0;
}

QStringList tidy(const QStringList &keys)
{
    QStringList out;
    for (const QString &key : keys) {
        const QString k = key.trimmed();
        if (!k.isEmpty() && !out.contains(k)) {
            out << k;
        }
    }
    return out;
}

Settings presetValues(const QString &id)
{
    Settings s; // the defaults are the streaming preset
    if (id == QLatin1String("light")) {
        s.denoiseStrength = 70;
        s.eq = false;
        s.lowGainDb = 0;
        s.mudGainDb = 0;
        s.presenceGainDb = 0;
        s.airGainDb = 0;
        s.compThresholdDb = -18;
        s.compRatio = 2;
        s.compAttackMs = 10;
        s.compMakeupDb = 2;
    } else if (id == QLatin1String("noisy")) {
        s.highpassHz = 100;
        s.highpassSteep = true;
        s.voiceThreshold = 50;
        s.gate = true;
        s.gateThresholdDb = -45;
        s.gateRangeDb = -30;
    } else if (id == QLatin1String("broadcast")) {
        s.highpassHz = 90;
        s.highpassSteep = true;
        s.gate = true;
        s.gateRangeDb = -15;
        s.lowGainDb = 2;
        s.mudGainDb = -3;
        s.mudHz = 350;
        s.presenceGainDb = 3;
        s.presenceHz = 4500;
        s.airGainDb = 2;
        s.airHz = 11000;
        s.compThresholdDb = -24;
        s.compRatio = 4;
        s.compAttackMs = 5;
        s.compReleaseMs = 120;
        s.compMakeupDb = 7;
    }
    return s;
}

bool moduleOn(const Settings &s, Module m)
{
    for (const Switch &sw : switches()) {
        if (sw.module == m && QLatin1String(sw.key) == QLatin1String("enabled")) {
            return s.*sw.field;
        }
    }
    return false;
}

QString quoted(const QString &s)
{
    QString out = s;
    out.replace(QLatin1Char('\\'), QLatin1String("\\\\"));
    out.replace(QLatin1Char('"'), QLatin1String("\\\""));
    return QLatin1Char('"') + out + QLatin1Char('"');
}

QList<Module> chain(bool denoise)
{
    QList<Module> out;
    for (const Module m : kModules) {
        if (m != Module::Denoise || denoise) {
            out << m;
        }
    }
    return out;
}

} // namespace

Settings sanitize(Settings s)
{
    for (const Param &p : params()) {
        s.*p.field = snap(p, s.*p.field);
    }
    s.filteredApps = tidy(s.filteredApps);
    s.rawApps = tidy(s.rawApps);
    for (const QString &key : std::as_const(s.filteredApps)) {
        s.rawApps.removeAll(key);
    }
    return s;
}

QStringList presetIds()
{
    return {QStringLiteral("light"), QStringLiteral("streaming"), QStringLiteral("noisy"),
            QStringLiteral("broadcast")};
}

QString presetLabel(const QString &id)
{
    static const QMap<QString, QString> labels = {
        {QStringLiteral("light"), QStringLiteral("Light")},
        {QStringLiteral("streaming"), QStringLiteral("Streaming")},
        {QStringLiteral("noisy"), QStringLiteral("Noisy room")},
        {QStringLiteral("broadcast"), QStringLiteral("Broadcast")},
        {QStringLiteral("custom"), QStringLiteral("Custom")},
    };
    return labels.value(id, id);
}

Settings applyPreset(Settings s, const QString &id)
{
    if (!presetIds().contains(id)) {
        return s;
    }
    const Settings preset = presetValues(id);
    for (const Param &p : params()) {
        s.*p.field = preset.*p.field;
    }
    for (const Switch &sw : switches()) {
        s.*sw.field = preset.*sw.field;
    }
    return s;
}

QString matchingPreset(const Settings &s)
{
    for (const QString &id : presetIds()) {
        const Settings preset = presetValues(id);
        bool same = true;
        for (const Switch &sw : switches()) {
            // A switched-off module's details do not change how the preset sounds.
            if (QLatin1String(sw.key) != QLatin1String("enabled") && !moduleOn(preset, sw.module)) {
                continue;
            }
            same = same && s.*sw.field == preset.*sw.field;
        }
        for (const Param &p : params()) {
            if (moduleOn(preset, p.module)) {
                same = same && std::abs(s.*p.field - preset.*p.field) < p.step / 2;
            }
        }
        if (same) {
            return id;
        }
    }
    return QStringLiteral("custom");
}

AppChoice appChoice(const Settings &s, const QString &appKey)
{
    if (s.filteredApps.contains(appKey)) {
        return AppChoice::Filtered;
    }
    if (s.rawApps.contains(appKey)) {
        return AppChoice::Raw;
    }
    return AppChoice::Default;
}

Settings setAppChoice(Settings s, const QString &appKey, AppChoice choice)
{
    if (appKey.isEmpty()) {
        return s;
    }
    s.filteredApps.removeAll(appKey);
    s.rawApps.removeAll(appKey);
    if (choice == AppChoice::Filtered) {
        s.filteredApps << appKey;
    } else if (choice == AppChoice::Raw) {
        s.rawApps << appKey;
    }
    return s;
}

bool useFiltered(const Settings &s, AppChoice choice, bool excludedByDefault)
{
    if (!s.enabled) {
        return false;
    }
    switch (choice) {
    case AppChoice::Filtered:
        return true;
    case AppChoice::Raw:
        return false;
    case AppChoice::Default:
        break;
    }
    return s.scope == Scope::AllApps && !excludedByDefault;
}

QList<Control> controls(const Settings &s, bool denoise)
{
    QList<Control> out;
    for (const Module m : chain(denoise)) {
        const QString node = moduleNode(m) + QLatin1Char(':');
        for (const Switch &sw : switches()) {
            if (sw.module == m) {
                out << Control{node + QLatin1String(sw.port), s.*sw.field ? 1.0f : 0.0f};
            }
        }
        for (const Param &p : params()) {
            if (p.module == m) {
                out << Control{node + QLatin1String(p.port), float(s.*p.field * p.scale)};
            }
        }
    }
    return out;
}

QString graphJson(const Settings &s, const QString &pluginPath, bool denoise)
{
    const QList<Module> modules = chain(denoise);
    const QList<Control> values = controls(s, denoise);
    QStringList nodes;
    for (const Module m : modules) {
        const QString node = moduleNode(m);
        QStringList control;
        for (const Control &c : values) {
            if (c.key.startsWith(node + QLatin1Char(':'))) {
                control << quoted(c.key.mid(node.size() + 1)) + QStringLiteral(" = ") +
                               QString::number(double(c.value), 'g', 7);
            }
        }
        nodes << QStringLiteral("{ type = ladspa name = %1 plugin = %2 label = %1 control = { %3 } }")
                     .arg(node, quoted(pluginPath), control.join(QLatin1Char(' ')));
    }
    QStringList links;
    for (qsizetype i = 1; i < modules.size(); ++i) {
        links << QStringLiteral("{ output = \"%1:Out\" input = \"%2:In\" }")
                     .arg(moduleNode(modules[i - 1]), moduleNode(modules[i]));
    }
    return QStringLiteral("{ nodes = [ %1 ] links = [ %2 ] inputs = [ \"%3:In\" ] outputs = [ \"%4:Out\" ] }")
        .arg(nodes.join(QLatin1Char(' ')), links.join(QLatin1Char(' ')), moduleNode(modules.first()),
             moduleNode(modules.last()));
}

bool graphLoaded(const QStringList &propKeys)
{
    const QString last = moduleNode(Module::Limiter) + QStringLiteral(":Enabled");
    return propKeys.contains(last);
}

} // namespace rostrum::micfx
