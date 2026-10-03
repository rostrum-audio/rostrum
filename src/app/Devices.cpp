#include "app/Devices.h"

#include "app/AppController.h"

#include <KLocalizedString>

#include <QJSEngine>
#include <QProcess>

namespace rostrum::app {

namespace {

bool isBluetooth(const pw::Node &n)
{
    return n.prop("device.api") == QLatin1String("bluez5") || n.name.startsWith(QLatin1String("bluez_"));
}

bool isAlsa(const pw::Node &n)
{
    return n.prop("device.api") == QLatin1String("alsa") || n.name.startsWith(QLatin1String("alsa_"));
}

// "a2dp-sink", "headset-head-unit", "bap-sink", ... as the user knows them.
QString profileLabel(const pw::Node &n, bool *headset)
{
    const QString p = n.prop("api.bluez5.profile").toLower();
    *headset = p.contains(QLatin1String("head")) || p.contains(QLatin1String("hfp")) ||
               p.contains(QLatin1String("hsp"));
    if (*headset) {
        return i18nc("bluetooth profile", "Headset (HSP/HFP)");
    }
    if (p.contains(QLatin1String("a2dp"))) {
        return i18nc("bluetooth profile", "A2DP");
    }
    if (p.contains(QLatin1String("bap"))) {
        return i18nc("bluetooth profile", "LE Audio");
    }
    return p;
}

bool listed(const pw::Node &n)
{
    return !n.isRostrum() && n.prop("rostrum.internal") != QLatin1String("true") &&
           (n.isSink() || (n.isSource() && !n.mediaClass.contains(QLatin1String("Monitor"))));
}

} // namespace

Devices *Devices::s_instance = nullptr;

Devices::Devices(AppController *app, QObject *parent)
    : QObject(parent)
    , m_app(app)
    , m_meters(app->pw())
    , m_tone(app->pw())
{
    Q_ASSERT(!s_instance);
    s_instance = this;
    m_clock.start();
    connect(app->pw(), &pw::PwContext::graphChanged, this, &Devices::rebuild);
    connect(app->pw(), &pw::PwContext::defaultsChanged, this, &Devices::rebuild);
    connect(app->pw(), &pw::PwContext::stateChanged, this, &Devices::rebuild);
    connect(app->engine(), &engine::Engine::devicesChanged, this, &Devices::choiceChanged);
    connect(app, &AppController::devicesChanged, this, &Devices::choiceChanged);
    connect(&m_tone, &pw::TestTone::playingChanged, this, &Devices::toneChanged);
    connect(&m_timer, &QTimer::timeout, this, &Devices::tick);
    rebuild();
}

Devices::~Devices()
{
    s_instance = nullptr;
}

Devices *Devices::create(QQmlEngine *, QJSEngine *)
{
    QJSEngine::setObjectOwnership(s_instance, QJSEngine::CppOwnership);
    return s_instance;
}

void Devices::rebuild()
{
    const pw::PwContext *pw = m_app->pw();
    const pw::Graph &g = pw->graph();
    const QString defSink = pw->defaultSinkName();
    const QString defSource = pw->defaultSourceName();

    QList<const pw::Node *> sinks;
    QList<const pw::Node *> sources;
    QVariantList virt;
    for (const auto &n : g.nodes) {
        if (n.isRostrum() && (n.isSink() || n.isSource())) {
            virt << QVariantMap{{QStringLiteral("name"), n.name},
                                {QStringLiteral("description"), n.description},
                                {QStringLiteral("id"), n.id}};
        } else if (listed(n)) {
            (n.isSink() ? sinks : sources) << &n;
        }
    }

    bool anyHeadset = false;
    auto rows = [&](const QList<const pw::Node *> &nodes, const QString &def) {
        // The Bluetooth profile matters only when the user also has a wired device to compare.
        bool bt = false;
        bool wired = false;
        for (const auto *n : nodes) {
            bt = bt || isBluetooth(*n);
            wired = wired || isAlsa(*n);
        }
        QVariantList out;
        for (const auto *n : nodes) {
            bool headset = false;
            QString subtitle;
            if (isBluetooth(*n)) {
                const QString profile = profileLabel(*n, &headset);
                anyHeadset = anyHeadset || headset;
                if (bt && wired && !profile.isEmpty()) {
                    subtitle = i18nc("device subtitle: Bluetooth, profile", "Bluetooth, %1", profile);
                }
            }
            out << QVariantMap{{QStringLiteral("name"), n->name},
                               {QStringLiteral("description"), n->label()},
                               {QStringLiteral("subtitle"), subtitle},
                               {QStringLiteral("isDefault"), n->name == def},
                               {QStringLiteral("bluetooth"), isBluetooth(*n)},
                               {QStringLiteral("headsetProfile"), headset},
                               {QStringLiteral("id"), n->id}};
        }
        std::sort(out.begin(), out.end(), [](const QVariant &a, const QVariant &b) {
            return a.toMap().value(QStringLiteral("description")).toString().localeAwareCompare(
                       b.toMap().value(QStringLiteral("description")).toString()) < 0;
        });
        return out;
    };
    const QVariantList outs = rows(sinks, defSink);
    const QVariantList ins = rows(sources, defSource);

    QStringList names;
    for (const auto *n : sources) {
        names << n->name;
    }
    if (names != m_inputNames) {
        m_inputNames = names;
        m_meters.setTargets(names);
    }
    if (outs != m_outputs || ins != m_inputs || virt != m_virtual || anyHeadset != m_anyHeadset) {
        m_outputs = outs;
        m_inputs = ins;
        m_virtual = virt;
        m_anyHeadset = anyHeadset;
        Q_EMIT listsChanged();
    }
}

QString Devices::headphones() const { return m_app->engine()->headphoneDevice(); }

void Devices::setHeadphones(const QString &nodeName)
{
    if (nodeName == headphones()) {
        return;
    }
    m_app->engine()->setHeadphoneDevice(nodeName);
    m_app->saveSettingsSoon();
    Q_EMIT choiceChanged();
}

QString Devices::mic() const { return m_app->engine()->micDevice(); }

void Devices::setMic(const QString &nodeName)
{
    if (nodeName == mic()) {
        return;
    }
    m_app->engine()->setMicDevice(nodeName);
    m_app->saveSettingsSoon();
    Q_EMIT choiceChanged();
}

QString Devices::headphonesInUse() const { return m_app->engine()->resolvedSinkName(); }
QString Devices::micInUse() const { return m_app->engine()->resolvedSourceName(); }
bool Devices::micFallback() const { return m_app->settings().micFallback; }

void Devices::setMicFallback(bool on)
{
    if (on == micFallback()) {
        return;
    }
    m_app->settings().micFallback = on;
    m_app->engine()->setMicFallback(on);
    m_app->saveSettingsSoon();
    Q_EMIT choiceChanged();
}

bool Devices::monoHeadphones() const { return m_app->settings().monoHeadphones; }

void Devices::setMonoHeadphones(bool on)
{
    if (on == monoHeadphones()) {
        return;
    }
    m_app->settings().monoHeadphones = on;
    m_app->engine()->setMonoHeadphones(on);
    m_app->saveSettingsSoon();
    Q_EMIT choiceChanged();
}

void Devices::setMetersActive(bool active)
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

void Devices::tick()
{
    const qint64 now = m_clock.elapsed();
    const double dt = std::max<qint64>(1, now - m_lastTick) / 1000.0;
    m_lastTick = now;
    QVariantMap levels;
    for (const QString &name : std::as_const(m_inputNames)) {
        meters::State &st = m_state[name];
        meters::advance(st, m_meters.takePeak(name), false, dt, now);
        levels.insert(name, st.fraction);
    }
    if (levels != m_levels) {
        m_levels = levels;
        Q_EMIT levelsChanged();
    }
}

void Devices::refresh()
{
    rebuild();
    Q_EMIT listsChanged();
    Q_EMIT choiceChanged();
}

QString Devices::toneTarget() const
{
    return m_tone.isPlaying() ? m_tone.target() : QString();
}

bool Devices::playTone(const QString &nodeName)
{
    if (!m_tone.play(nodeName)) {
        Q_EMIT m_app->toast(i18n("Could not play a test tone on that device."));
        return false;
    }
    return true;
}

void Devices::openSoundSettings()
{
    QProcess::startDetached(QStringLiteral("systemsettings"), {QStringLiteral("kcm_pulseaudio")});
}

} // namespace rostrum::app
