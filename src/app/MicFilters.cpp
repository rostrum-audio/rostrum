#include "app/MicFilters.h"

#include "app/AppController.h"
#include "app/Apps.h"

#include <KLocalizedString>

#include <QJSEngine>
#include <QLocale>

namespace rostrum::app {

namespace {

using micfx::Module;

struct ModuleText
{
    Module module;
    KLocalizedString label;
    KLocalizedString description;
};

const QList<ModuleText> &moduleTexts()
{
    static const QList<ModuleText> texts = {
        {Module::Highpass, ki18nc("@title mic filter", "Rumble filter"),
         ki18n("Cuts hum, desk thumps and handling noise below your voice.")},
        {Module::Denoise, ki18nc("@title mic filter", "Noise removal"),
         ki18n("Removes fans, keyboards, traffic and room noise while you talk. Adds 10 ms of delay.")},
        {Module::Gate, ki18nc("@title mic filter", "Noise gate"),
         ki18n("Turns the mic down between phrases. Noise removal usually does this better; the gate "
               "helps with noise it cannot remove.")},
        {Module::Eq, ki18nc("@title mic filter", "Tone"),
         ki18n("A gentle EQ: less boom and mud, more clarity and air.")},
        {Module::Compressor, ki18nc("@title mic filter", "Compressor"),
         ki18n("Evens out loud and quiet speech, so your voice sits at a steady level.")},
        {Module::Limiter, ki18nc("@title mic filter", "Limiter"),
         ki18n("A safety net: shouts and laughs never go above the ceiling, so they do not clip.")},
    };
    return texts;
}

struct ControlText
{
    const char *module;
    const char *key;
    KLocalizedString label;
    KLocalizedString description; // empty for none
    bool advanced = false;
};

const QList<ControlText> &controlTexts()
{
    static const QList<ControlText> texts = {
        {"highpass", "frequency", ki18nc("@label:slider rumble filter", "Cut below"), KLocalizedString()},
        {"highpass", "steep", ki18nc("@option:check rumble filter", "Steeper cut"),
         ki18n("Cuts harder below the frequency, for strong hum or a boomy room.")},
        {"denoise", "strength", ki18nc("@label:slider noise removal", "Strength"),
         ki18n("Lower lets a little background through, which can sound more natural.")},
        {"denoise", "voice_threshold", ki18nc("@label:slider noise removal", "Mute when not talking"),
         ki18n("Silences the mic while noise removal is less sure than this that you are talking. "
               "Off keeps everything it lets through."),
         true},
        {"gate", "threshold", ki18nc("@label:slider noise gate", "Opens above"), KLocalizedString()},
        {"gate", "range", ki18nc("@label:slider noise gate", "Turn down by"), KLocalizedString(), true},
        {"gate", "attack", ki18nc("@label:slider noise gate", "Open over"), KLocalizedString(), true},
        {"gate", "hold", ki18nc("@label:slider noise gate", "Stay open for"), KLocalizedString(), true},
        {"gate", "release", ki18nc("@label:slider noise gate", "Close over"), KLocalizedString(), true},
        {"eq", "low_gain", ki18nc("@label:slider tone", "Low"), ki18n("Warmth and boom.")},
        {"eq", "low_frequency", ki18nc("@label:slider tone", "Low frequency"), KLocalizedString(), true},
        {"eq", "mud_gain", ki18nc("@label:slider tone", "Mud"), ki18n("Turn down for a boxy or muffled sound.")},
        {"eq", "mud_frequency", ki18nc("@label:slider tone", "Mud frequency"), KLocalizedString(), true},
        {"eq", "presence_gain", ki18nc("@label:slider tone", "Presence"), ki18n("Clarity of speech.")},
        {"eq", "presence_frequency", ki18nc("@label:slider tone", "Presence frequency"), KLocalizedString(), true},
        {"eq", "air_gain", ki18nc("@label:slider tone", "Air"), ki18n("Brightness and breath.")},
        {"eq", "air_frequency", ki18nc("@label:slider tone", "Air frequency"), KLocalizedString(), true},
        {"compressor", "threshold", ki18nc("@label:slider compressor", "Starts at"),
         ki18n("Speech louder than this is turned down.")},
        {"compressor", "makeup", ki18nc("@label:slider compressor", "Makeup gain"),
         ki18n("Raises the evened-out voice again.")},
        {"compressor", "ratio", ki18nc("@label:slider compressor", "Ratio"), KLocalizedString(), true},
        {"compressor", "knee", ki18nc("@label:slider compressor", "Knee"), KLocalizedString(), true},
        {"compressor", "attack", ki18nc("@label:slider compressor", "Attack"), KLocalizedString(), true},
        {"compressor", "release", ki18nc("@label:slider compressor", "Release"), KLocalizedString(), true},
        {"limiter", "ceiling", ki18nc("@label:slider limiter", "Ceiling"), KLocalizedString()},
        {"limiter", "release", ki18nc("@label:slider limiter", "Release"), KLocalizedString(), true},
    };
    return texts;
}

const ControlText *controlText(const QString &module, const QString &key)
{
    for (const auto &t : controlTexts()) {
        if (module == QLatin1String(t.module) && key == QLatin1String(t.key)) {
            return &t;
        }
    }
    return nullptr;
}

QString text(const KLocalizedString &s)
{
    return s.isEmpty() ? QString() : s.toString();
}

QString choiceName(micfx::AppChoice c)
{
    switch (c) {
    case micfx::AppChoice::Filtered: return QStringLiteral("filtered");
    case micfx::AppChoice::Raw: return QStringLiteral("raw");
    case micfx::AppChoice::Default: break;
    }
    return QStringLiteral("default");
}

} // namespace

MicFilters *MicFilters::s_instance = nullptr;

MicFilters::MicFilters(AppController *app, QObject *parent)
    : QObject(parent)
    , m_app(app)
{
    Q_ASSERT(!s_instance);
    s_instance = this;
    auto *engine = app->engine();
    connect(engine, &engine::Engine::micFiltersStateChanged, this, &MicFilters::stateChanged);
    connect(app, &AppController::statusChanged, this, &MicFilters::stateChanged);
    connect(app, &AppController::settingsChanged, this, [this] {
        ++m_revision;
        Q_EMIT settingsChanged();
        rebuildApps();
    });
    connect(engine, &engine::Engine::appsChanged, this, &MicFilters::rebuildApps);
    const auto checkGain = [this] {
        if (const bool w = gainWarning(); w != m_gainWarning) {
            m_gainWarning = w;
            Q_EMIT gainWarningChanged();
        }
    };
    connect(app, &AppController::levelsChanged, this, checkGain);
    connect(this, &MicFilters::settingsChanged, this, checkGain);
    m_gainWarning = gainWarning();
    rebuildApps();
}

MicFilters::~MicFilters()
{
    s_instance = nullptr;
}

MicFilters *MicFilters::create(QQmlEngine *, QJSEngine *)
{
    QJSEngine::setObjectOwnership(s_instance, QJSEngine::CppOwnership);
    return s_instance;
}

const micfx::Settings &MicFilters::settings() const
{
    return m_app->settings().micFilters;
}

void MicFilters::update(const std::function<void(micfx::Settings &)> &change)
{
    micfx::Settings s = settings();
    change(s);
    s = micfx::sanitize(s);
    if (s == settings()) {
        return;
    }
    m_app->settings().micFilters = s;
    m_app->saveSettingsSoon();
    m_app->engine()->setMicFilters(s);
    Q_EMIT m_app->settingsChanged();
}

bool MicFilters::available() const
{
    return unavailableReason().isEmpty();
}

QString MicFilters::unavailableReason() const
{
    return m_app->engine()->micFiltersUnavailable();
}

bool MicFilters::hasDenoise() const
{
    return m_app->engine()->micFiltersHaveDenoise();
}

QString MicFilters::state() const
{
    using S = engine::Engine::MicFxState;
    switch (m_app->engine()->micFiltersState()) {
    case S::Starting: return QStringLiteral("starting");
    case S::Active: return QStringLiteral("active");
    case S::Failed: return QStringLiteral("failed");
    case S::Off: break;
    }
    return QStringLiteral("off");
}

QString MicFilters::error() const
{
    return m_app->engine()->micFilterError();
}

bool MicFilters::enabled() const
{
    return settings().enabled;
}

void MicFilters::setEnabled(bool on)
{
    update([on](micfx::Settings &s) { s.enabled = on; });
}

QString MicFilters::scope() const
{
    return micfx::scopeName(settings().scope);
}

void MicFilters::setScope(const QString &scope)
{
    if (const auto sc = micfx::scopeFromString(scope)) {
        update([sc](micfx::Settings &s) { s.scope = *sc; });
    }
}

QString MicFilters::preset() const
{
    return micfx::matchingPreset(settings());
}

QVariantList MicFilters::presets() const
{
    static const QHash<QString, KLocalizedString> labels = {
        {QStringLiteral("light"), ki18nc("@item mic filter preset", "Light")},
        {QStringLiteral("streaming"), ki18nc("@item mic filter preset", "Streaming")},
        {QStringLiteral("noisy"), ki18nc("@item mic filter preset", "Noisy room")},
        {QStringLiteral("broadcast"), ki18nc("@item mic filter preset", "Broadcast")},
    };
    static const QHash<QString, KLocalizedString> descriptions = {
        {QStringLiteral("light"), ki18n("Removes noise and rumble, and catches peaks. Your voice stays as it is.")},
        {QStringLiteral("streaming"), ki18n("Clean, clear and even. A good start for most mics.")},
        {QStringLiteral("noisy"), ki18n("Stronger cleanup for fans, keyboards and busy rooms.")},
        {QStringLiteral("broadcast"), ki18n("Close, dense radio sound with heavier compression.")},
    };
    QVariantList rows;
    for (const QString &id : micfx::presetIds()) {
        rows << QVariantMap{{QStringLiteral("id"), id},
                            {QStringLiteral("label"), labels.value(id).toString()},
                            {QStringLiteral("description"), descriptions.value(id).toString()}};
    }
    return rows;
}

QVariantList MicFilters::modules() const
{
    QVariantList rows;
    for (const auto &m : moduleTexts()) {
        const QString id = micfx::moduleName(m.module);
        QVariantList switches;
        for (const auto &s : micfx::switches()) {
            if (s.module != m.module || qstrcmp(s.key, "enabled") == 0) {
                continue;
            }
            const ControlText *t = controlText(id, QLatin1String(s.key));
            switches << QVariantMap{{QStringLiteral("key"), QLatin1String(s.key)},
                                    {QStringLiteral("label"), t ? text(t->label) : QLatin1String(s.key)},
                                    {QStringLiteral("description"), t ? text(t->description) : QString()}};
        }
        QVariantList params;
        // Main controls first, in the order of the texts above.
        for (const bool advanced : {false, true}) {
            for (const auto &t : controlTexts()) {
                const micfx::Param *p = micfx::findParam(id, QLatin1String(t.key));
                if (!p || p->module != m.module || t.advanced != advanced) {
                    continue;
                }
                params << QVariantMap{{QStringLiteral("key"), QLatin1String(t.key)},
                                      {QStringLiteral("label"), text(t.label)},
                                      {QStringLiteral("description"), text(t.description)},
                                      {QStringLiteral("min"), p->min},
                                      {QStringLiteral("max"), p->max},
                                      {QStringLiteral("step"), p->step},
                                      {QStringLiteral("advanced"), t.advanced}};
            }
        }
        rows << QVariantMap{{QStringLiteral("id"), id},
                            {QStringLiteral("label"), m.label.toString()},
                            {QStringLiteral("description"), m.description.toString()},
                            {QStringLiteral("needsDenoise"), m.module == Module::Denoise},
                            {QStringLiteral("switches"), switches},
                            {QStringLiteral("params"), params}};
    }
    return rows;
}

bool MicFilters::gainWarning() const
{
    const auto &s = settings();
    return s.enabled && s.limiter && m_app->micGain() > 1.0 + 1e-6;
}

double MicFilters::value(const QString &module, const QString &key) const
{
    const micfx::Param *p = micfx::findParam(module, key);
    return p ? settings().*(p->field) : 0.0;
}

void MicFilters::setValue(const QString &module, const QString &key, double value)
{
    const micfx::Param *p = micfx::findParam(module, key);
    if (!p || !std::isfinite(value)) {
        return;
    }
    update([p, value](micfx::Settings &s) { s.*(p->field) = value; });
}

bool MicFilters::isOn(const QString &module, const QString &key) const
{
    const micfx::Switch *s = micfx::findSwitch(module, key);
    return s && settings().*(s->field);
}

void MicFilters::setOn(const QString &module, bool on, const QString &key)
{
    const micfx::Switch *sw = micfx::findSwitch(module, key);
    if (!sw) {
        return;
    }
    update([sw, on](micfx::Settings &s) { s.*(sw->field) = on; });
}

QString MicFilters::format(const QString &module, const QString &key, double value) const
{
    const micfx::Param *p = micfx::findParam(module, key);
    const int decimals = p && p->step < 1.0 ? 1 : 0;
    const QLocale locale;
    const auto num = [&](double v, int d) { return locale.toString(v, 'f', d); };
    if (key.endsWith(QLatin1String("frequency"))) {
        return value >= 1000 ? i18nc("@item value in kilohertz", "%1 kHz", num(value / 1000.0, 1))
                             : i18nc("@item value in hertz", "%1 Hz", num(value, 0));
    }
    if (key == QLatin1String("strength")) {
        return i18nc("@item percent", "%1 %", num(value, 0));
    }
    if (key == QLatin1String("voice_threshold")) {
        return value <= 0 ? i18nc("@item voice threshold", "Off") : i18nc("@item percent", "%1 %", num(value, 0));
    }
    if (key == QLatin1String("ratio")) {
        return i18nc("@item compressor ratio", "%1:1", num(value, 1));
    }
    if (key == QLatin1String("attack") || key == QLatin1String("hold") || key == QLatin1String("release")) {
        return i18nc("@item value in milliseconds", "%1 ms", num(value, decimals));
    }
    // Gains show their sign, so a cut and a boost read apart at a glance.
    const QString db = num(value, decimals);
    if (key.endsWith(QLatin1String("gain")) && value > 0) {
        return i18nc("@item positive value in decibels", "+%1 dB", db);
    }
    return i18nc("@item value in decibels", "%1 dB", db);
}

void MicFilters::applyPreset(const QString &id)
{
    if (!micfx::presetIds().contains(id)) {
        return;
    }
    update([&id](micfx::Settings &s) { s = micfx::applyPreset(s, id); });
}

void MicFilters::resetModule(const QString &module)
{
    const micfx::Settings defaults = micfx::applyPreset(micfx::Settings{}, QStringLiteral("streaming"));
    update([&](micfx::Settings &s) {
        for (const auto &p : micfx::params()) {
            if (micfx::moduleName(p.module) == module) {
                s.*(p.field) = defaults.*(p.field);
            }
        }
        for (const auto &sw : micfx::switches()) {
            if (micfx::moduleName(sw.module) == module && qstrcmp(sw.key, "enabled") != 0) {
                s.*(sw.field) = defaults.*(sw.field);
            }
        }
    });
}

void MicFilters::setAppFiltered(const QString &appKey, bool filtered)
{
    bool excluded = false;
    for (const auto &row : std::as_const(m_apps)) {
        const QVariantMap m = row.toMap();
        if (m.value(QStringLiteral("key")).toString() == appKey) {
            excluded = m.value(QStringLiteral("excludedByDefault")).toBool();
            break;
        }
    }
    const bool byDefault = settings().scope == micfx::Scope::AllApps && !excluded;
    const micfx::AppChoice choice = filtered == byDefault ? micfx::AppChoice::Default
                                    : filtered            ? micfx::AppChoice::Filtered
                                                          : micfx::AppChoice::Raw;
    update([&](micfx::Settings &s) { s = micfx::setAppChoice(s, appKey, choice); });
}

void MicFilters::forgetApp(const QString &appKey)
{
    update([&](micfx::Settings &s) { s = micfx::setAppChoice(s, appKey, micfx::AppChoice::Default); });
}

void MicFilters::rebuildApps()
{
    const micfx::Settings &s = settings();
    micfx::Settings on = s;
    on.enabled = true; // the switches show the choice even while filters are off
    QVariantList rows;
    QSet<QString> seen;
    for (const auto &a : m_app->engine()->micApps()) {
        const QString key = a.identity.key.toString();
        seen.insert(key.toLower());
        rows << QVariantMap{{QStringLiteral("key"), key},
                            {QStringLiteral("name"), a.identity.displayName},
                            {QStringLiteral("icon"), Apps::iconFor(a.iconNames)},
                            {QStringLiteral("running"), true},
                            {QStringLiteral("filtered"), a.filtered},
                            {QStringLiteral("choice"), choiceName(a.choice)},
                            {QStringLiteral("excludedByDefault"), a.excludedByDefault},
                            {QStringLiteral("wantsFiltered"), micfx::useFiltered(on, a.choice, a.excludedByDefault)}};
    }
    QList<QVariantMap> saved;
    for (const QString &key : s.filteredApps + s.rawApps) {
        if (seen.contains(key.toLower())) {
            continue;
        }
        seen.insert(key.toLower());
        const AppKey k = AppKey::fromString(key);
        StreamProps props;
        (k.key == MatchKey::Binary ? props.binary : props.appName) = k.match;
        AppRule rule;
        rule.key = k.key;
        rule.match = k.match;
        const auto choice = micfx::appChoice(s, key);
        saved << QVariantMap{{QStringLiteral("key"), key},
                             {QStringLiteral("name"), identify(props).displayName},
                             {QStringLiteral("icon"), Apps::iconFor(m_app->engine()->ruleIconCandidates(rule))},
                             {QStringLiteral("running"), false},
                             {QStringLiteral("filtered"), false},
                             {QStringLiteral("choice"), choiceName(choice)},
                             {QStringLiteral("excludedByDefault"), false},
                             {QStringLiteral("wantsFiltered"), choice == micfx::AppChoice::Filtered}};
    }
    std::sort(saved.begin(), saved.end(), [](const QVariantMap &a, const QVariantMap &b) {
        return a.value(QStringLiteral("name")).toString().compare(b.value(QStringLiteral("name")).toString(),
                                                                   Qt::CaseInsensitive) < 0;
    });
    for (const auto &m : std::as_const(saved)) {
        rows << m;
    }
    if (rows != m_apps) {
        m_apps = rows;
        Q_EMIT appsChanged();
    }
}

} // namespace rostrum::app
