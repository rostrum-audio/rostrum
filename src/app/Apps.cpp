#include "app/Apps.h"

#include "app/AppController.h"

#include <KFormat>
#include <KLocalizedString>

#include <QJSEngine>

namespace rostrum::app {

namespace {
constexpr int kRelativeTimeRefreshMs = 60 * 1000;
}

Apps *Apps::s_instance = nullptr;

Apps::Apps(AppController *app, QObject *parent)
    : QObject(parent)
    , m_app(app)
    , m_meters(app->pw())
{
    Q_ASSERT(!s_instance);
    s_instance = this;
    m_clock.start();
    engine::Engine *e = app->engine();
    for (auto sig : {&engine::Engine::appsChanged, &engine::Engine::sceneChanged, &engine::Engine::structureChanged}) {
        connect(e, sig, this, &Apps::rebuild);
    }
    connect(app->pw(), &pw::PwContext::graphChanged, this, &Apps::rebuild);
    connect(&m_timer, &QTimer::timeout, this, &Apps::tick);
    // "Last seen 5 minutes ago" ages while the page is open.
    m_relativeTimeTimer.setInterval(kRelativeTimeRefreshMs);
    connect(&m_relativeTimeTimer, &QTimer::timeout, this, &Apps::rebuild);
    m_relativeTimeTimer.start();
    rebuild();
}

Apps::~Apps()
{
    s_instance = nullptr;
}

Apps *Apps::create(QQmlEngine *, QJSEngine *)
{
    QJSEngine::setObjectOwnership(s_instance, QJSEngine::CppOwnership);
    return s_instance;
}

namespace {

QString kindOf(AppCategory c)
{
    switch (c) {
    case AppCategory::Game:
        return i18nc("@info kind of app", "a game");
    case AppCategory::Voice:
        return i18nc("@info kind of app", "a voice chat app");
    case AppCategory::Music:
        return i18nc("@info kind of app", "a music player");
    case AppCategory::Alerts:
        return i18nc("@info kind of app", "a stream alerts tool");
    case AppCategory::Desktop:
        return i18nc("@info kind of app", "a desktop app");
    case AppCategory::None:
        break;
    }
    return {};
}

QString kindPlural(AppCategory c)
{
    switch (c) {
    case AppCategory::Game:
        return i18nc("@info kind of app, plural", "games");
    case AppCategory::Voice:
        return i18nc("@info kind of app, plural", "voice chat");
    case AppCategory::Music:
        return i18nc("@info kind of app, plural", "music players");
    case AppCategory::Alerts:
        return i18nc("@info kind of app, plural", "stream alerts");
    case AppCategory::Desktop:
        return i18nc("@info kind of app, plural", "desktop apps");
    case AppCategory::None:
        break;
    }
    return {};
}

} // namespace

QString Apps::reason(const engine::AppStream &a)
{
    const Classification &c = a.detected;
    if (c.excluded) {
        return c.evidence == Evidence::OwnOutput
                   ? i18nc("@info", "Chose its own output in its settings, so Rostrum leaves it there")
                   : i18nc("@info", "Audio tools, OBS and screen readers are never assigned automatically");
    }
    switch (c.evidence) {
    case Evidence::Catalog:
        return i18nc("@info %1 is a kind of app", "Recognised as %1", kindOf(c.category));
    case Evidence::Steam:
        return i18nc("@info", "Steam game");
    case Evidence::Wine:
        return i18nc("@info", "Windows game under Wine or Proton");
    case Evidence::MediaRole:
        return i18nc("@info %1 is a kind of app", "The app reports itself as %1", kindOf(c.category));
    case Evidence::DesktopEntry:
        return i18nc("@info %1 is a kind of app", "Its app menu entry lists it as %1", kindOf(c.category));
    case Evidence::GameEngine:
        return i18nc("@info", "Plays through a game audio engine");
    case Evidence::OwnOutput:
    case Evidence::None:
        break;
    }
    return {};
}

void Apps::rebuild()
{
    const engine::Engine *e = m_app->engine();
    const Scene &scene = e->scene();

    QVariantList buses;
    for (const auto &b : scene.buses) {
        if (!b.isInput()) {
            buses << QVariantMap{{QStringLiteral("id"), b.id},
                                 {QStringLiteral("name"), b.name},
                                 {QStringLiteral("color"), b.color}};
        }
    }

    // One row per app. Streams of one app share the key Rostrum acts on: the rule that placed
    // them, or the key a new rule would use.
    QVariantList running;
    QHash<QString, int> rowOf;
    QHash<QString, QStringList> targets;
    QSet<QString> runningRuleKeys;
    QVariantMap unnamed;
    for (const auto &a : e->appStreams()) {
        const AppKey key = a.ruleKey.isValid() ? a.ruleKey : a.identity.key;
        const QString keyString = key.toString();
        targets[keyString] << QStringLiteral("#%1").arg(a.nodeId);
        if (a.ruleKey.isValid()) {
            runningRuleKeys.insert(a.ruleKey.toString().toLower());
        }
        if (a.identity.unnamed && unnamed.isEmpty()) {
            unnamed = {{QStringLiteral("key"), keyString}, {QStringLiteral("binary"), a.identity.binary}};
        }
        if (auto it = rowOf.find(keyString); it != rowOf.end()) {
            QVariantMap row = running.at(*it).toMap();
            QVariantList ids = row.value(QStringLiteral("nodeIds")).toList();
            ids << a.nodeId;
            row.insert(QStringLiteral("nodeIds"), ids);
            running[*it] = row;
            continue;
        }
        const Bus *bus = scene.bus(a.busId);
        const QString why = reason(a);
        QString detail;
        if (a.automatic || a.detected.excluded) {
            detail = why;
        } else if (!bus && !why.isEmpty()) {
            if (!e->autoAssign()) {
                detail = i18nc("@info %1 is why Rostrum recognised the app", "%1. Automatic assignment is off.", why);
            } else if (a.skipped) {
                detail = i18nc("@info %1 is why Rostrum recognised the app", "%1. You took it off its bus.", why);
            } else if (!scene.busFor(a.detected.category)) {
                detail = i18nc("@info %1 is why Rostrum recognised the app, %2 a kind of app",
                               "%1. No bus receives %2 yet.", why, kindPlural(a.detected.category));
            }
        }
        rowOf.insert(keyString, int(running.size()));
        running << QVariantMap{
            {QStringLiteral("key"), keyString},
            {QStringLiteral("name"), a.identity.displayName},
            {QStringLiteral("binary"), a.identity.binary},
            {QStringLiteral("matchKey"), matchKeyName(key.key)},
            {QStringLiteral("busId"), bus ? bus->id : QString()},
            {QStringLiteral("busName"), bus ? bus->name : QString()},
            {QStringLiteral("busColor"), bus ? bus->color : QString()},
            {QStringLiteral("volume"), a.volume},
            {QStringLiteral("muted"), a.muted},
            {QStringLiteral("always"), a.ruleKey.isValid() && !a.sessionOnly},
            {QStringLiteral("unnamed"), a.identity.unnamed},
            {QStringLiteral("nodeIds"), QVariantList{a.nodeId}},
            {QStringLiteral("automatic"), a.automatic},
            {QStringLiteral("detail"), detail},
        };
    }

    QVariantList rules;
    KFormat format;
    for (const auto &r : scene.rules) {
        const Bus *bus = scene.bus(r.busId);
        const QString keyString = AppKey{r.key, r.match}.toString();
        const bool isRunning = runningRuleKeys.contains(keyString.toLower());
        QString seen;
        if (isRunning) {
            seen = i18nc("@info rule last seen", "Playing now");
        } else if (r.lastSeen.isValid()) {
            seen = i18nc("@info rule last seen, %1 is a relative time", "Last seen %1",
                         format.formatRelativeDateTime(r.lastSeen.toLocalTime(), QLocale::ShortFormat));
        } else {
            seen = i18nc("@info rule last seen", "Not seen yet");
        }
        rules << QVariantMap{
            {QStringLiteral("key"), keyString},
            {QStringLiteral("match"), r.match},
            {QStringLiteral("matchKey"), matchKeyName(r.key)},
            {QStringLiteral("label"), r.label},
            {QStringLiteral("busId"), r.busId},
            {QStringLiteral("busName"), bus ? bus->name : r.busId},
            {QStringLiteral("busColor"), bus ? bus->color : QString()},
            {QStringLiteral("lastSeen"), seen},
            {QStringLiteral("running"), isRunning},
        };
    }

    if (targets != m_meterTargets) {
        m_meterTargets = targets;
        QStringList all;
        for (const auto &list : std::as_const(targets)) {
            all << list;
        }
        m_meters.setTargets(all);
    }
    if (running != m_running || rules != m_rules || buses != m_buses || unnamed != m_unnamed) {
        m_running = running;
        m_rules = rules;
        m_buses = buses;
        m_unnamed = unnamed;
        m_runningModel.setRows(running);
        m_rulesModel.setRows(rules);
        Q_EMIT changed();
    }
}

void Apps::setMetersActive(bool active)
{
    if (active == m_metersActive) {
        return;
    }
    m_metersActive = active;
    m_meters.setActive(active);
    if (active) {
        m_lastTick = m_clock.elapsed();
        m_timer.start(m_app->settings().meterSpeed == QLatin1String("low") ? meters::kLowIntervalMs
                                                                           : meters::kNormalIntervalMs);
    } else {
        m_timer.stop();
        m_state.clear();
        m_levels.clear();
        Q_EMIT levelsChanged();
    }
    Q_EMIT metersActiveChanged();
}

void Apps::tick()
{
    const qint64 now = m_clock.elapsed();
    const double dt = std::max<qint64>(1, now - m_lastTick) / 1000.0;
    m_lastTick = now;
    QVariantMap levels;
    for (auto it = m_meterTargets.cbegin(); it != m_meterTargets.cend(); ++it) {
        float peak = 0.0f;
        for (const QString &t : it.value()) {
            peak = std::max(peak, m_meters.takePeak(t));
        }
        meters::State &st = m_state[it.key()];
        meters::advance(st, peak, false, dt, now);
        levels.insert(it.key(), st.fraction);
    }
    if (levels != m_levels) {
        m_levels = levels;
        Q_EMIT levelsChanged();
    }
}

void Apps::assign(const QString &key, const QString &busId)
{
    m_app->engine()->assignApp(AppKey::fromString(key), busId, true);
}

void Apps::setAlways(const QString &key, bool always)
{
    engine::Engine *e = m_app->engine();
    const AppKey k = AppKey::fromString(key);
    QString busId;
    for (const auto &row : std::as_const(m_running)) {
        const QVariantMap m = row.toMap();
        if (m.value(QStringLiteral("key")).toString() == key) {
            busId = m.value(QStringLiteral("busId")).toString();
            break;
        }
    }
    if (busId.isEmpty()) {
        return;
    }
    if (always) {
        e->assignApp(k, busId, true);
        return;
    }
    const double volume = e->appVolume(k);
    const bool muted = e->appMuted(k);
    e->unassignApp(k);
    e->assignApp(k, busId, false);
    e->setAppVolume(k, volume);
    e->setAppMuted(k, muted);
}

void Apps::unassign(const QString &key) { m_app->engine()->unassignApp(AppKey::fromString(key)); }
void Apps::setVolume(const QString &key, double volume) { m_app->engine()->setAppVolume(AppKey::fromString(key), volume); }
void Apps::setMuted(const QString &key, bool muted)
{
    m_app->engine()->setAppMuted(AppKey::fromString(key), muted);
}
void Apps::removeRule(const QString &key) { m_app->engine()->removeRule(AppKey::fromString(key)); }

bool Apps::editMatch(const QString &key, const QString &match, const QString &matchKey)
{
    const AppKey newKey{matchKeyFromString(matchKey).value_or(MatchKey::Name), match.trimmed()};
    const AppKey oldKey = AppKey::fromString(key);
    if (!newKey.isValid()) {
        return false;
    }
    if (!(newKey == oldKey) && m_app->engine()->scene().rule(newKey.key, newKey.match)) {
        Q_EMIT m_app->toast(i18n("A rule for “%1” already exists.", newKey.match));
        return false;
    }
    m_app->engine()->editRule(oldKey, newKey);
    return true;
}

void Apps::nameApp(const QString &key, const QString &label, const QString &busId)
{
    const AppKey k = AppKey::fromString(key);
    m_app->engine()->assignApp(k, busId, true);
    m_app->engine()->setRuleLabel(k, label);
}

} // namespace rostrum::app
