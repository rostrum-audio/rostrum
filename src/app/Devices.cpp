#include "app/Devices.h"

#include "app/AppController.h"
#include "pw/BluetoothProfiles.h"

#include <KLocalizedString>

#include <QEventLoop>
#include <QJSEngine>
#include <QProcess>
#include <QTimer>

#include <algorithm>
#include <optional>

namespace rostrum::app {

namespace {

bool isBluetooth(const pw::Node &n)
{
    return n.prop("device.api") == QLatin1String("bluez5") || n.name.startsWith(QLatin1String("bluez_"));
}

bool isHeadsetProfile(const pw::Node &n, QString *labelOut = nullptr)
{
    const QString p = n.prop("api.bluez5.profile").toLower();
    const bool headset = p.contains(QLatin1String("head")) || p.contains(QLatin1String("hfp")) ||
                         p.contains(QLatin1String("hsp"));
    if (!labelOut) {
        return headset;
    }
    if (headset) {
        *labelOut = i18nc("bluetooth profile", "Headset (HSP/HFP)");
    } else if (p.contains(QLatin1String("a2dp"))) {
        *labelOut = i18nc("bluetooth profile", "A2DP");
    } else if (p.contains(QLatin1String("bap"))) {
        *labelOut = i18nc("bluetooth profile", "LE Audio");
    } else {
        *labelOut = p;
    }
    return headset;
}

bool listed(const pw::Node &n)
{
    return !n.isRostrum() && n.prop("rostrum.internal") != QLatin1String("true") &&
           (n.isSink() || (n.isSource() && !n.mediaClass.contains(QLatin1String("Monitor"))));
}

const pw::Device *deviceForHeadphones(const pw::Graph &g, const QString &nodeName, const QString &btAddress)
{
    if (const pw::Node *n = g.nodeByName(nodeName)) {
        const uint32_t deviceId = n->prop("device.id").toUInt();
        if (deviceId != 0) {
            if (const pw::Device *d = g.device(deviceId)) {
                return d;
            }
        }
    }
    if (btAddress.isEmpty()) {
        return nullptr;
    }
    for (const auto &n : g.nodes) {
        if (!n.isSink() || n.prop("api.bluez5.address") != btAddress) {
            continue;
        }
        const uint32_t deviceId = n.prop("device.id").toUInt();
        if (deviceId != 0) {
            if (const pw::Device *d = g.device(deviceId)) {
                return d;
            }
        }
    }
    for (const auto &d : g.devices) {
        if (d.bluezAddress() == btAddress) {
            return &d;
        }
    }
    return nullptr;
}

bool headphonesStillBad(const pw::Graph &g, const QString &inUse, const QString &btAddress)
{
    if (const pw::Node *n = g.nodeByName(inUse)) {
        if (n->error) {
            return true;
        }
        if (isBluetooth(*n) && isHeadsetProfile(*n)) {
            return true;
        }
        return false;
    }
    if (btAddress.isEmpty()) {
        return true;
    }
    for (const auto &n : g.nodes) {
        if (n.prop("api.bluez5.address") != btAddress || !n.isSink()) {
            continue;
        }
        if (n.error || isHeadsetProfile(n)) {
            return true;
        }
        return false;
    }
    return true;
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
    connect(app->engine(), &engine::Engine::devicesChanged, this, [this] {
        rebuild();
        Q_EMIT choiceChanged();
    });
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

    bool headphonesHeadset = false;
    bool headphonesBt = false;
    const QString inUse = m_app->engine()->resolvedSinkName();
    const QString btAddress = m_app->engine()->headphonesBtAddress();
    if (const pw::Node *n = g.nodeByName(inUse); n && isBluetooth(*n)) {
        headphonesBt = true;
        headphonesHeadset = isHeadsetProfile(*n);
    } else if (!btAddress.isEmpty()) {
        for (const auto *n : sinks) {
            if (n->prop("api.bluez5.address") == btAddress && isHeadsetProfile(*n)) {
                headphonesHeadset = true;
                headphonesBt = true;
                break;
            }
        }
    }
    const bool unhealthy = m_app->engine()->headphonesTransportUnhealthy();

    auto rows = [&](const QList<const pw::Node *> &nodes, const QString &def) {
        QVariantList out;
        for (const auto *n : nodes) {
            bool headset = false;
            QString subtitle;
            if (isBluetooth(*n)) {
                QString profile;
                headset = isHeadsetProfile(*n, &profile);
                if (!profile.isEmpty()) {
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
    const bool canHq = canPreferHighQualityHeadphones();
    if (outs != m_outputs || ins != m_inputs || virt != m_virtual || headphonesHeadset != m_headphonesHeadset ||
        canHq != m_canPreferHighQuality || headphonesBt != m_headphonesAreBluetooth ||
        unhealthy != m_headphonesTransportUnhealthy) {
        m_outputs = outs;
        m_inputs = ins;
        m_virtual = virt;
        m_headphonesHeadset = headphonesHeadset;
        m_canPreferHighQuality = canHq;
        m_headphonesAreBluetooth = headphonesBt;
        m_headphonesTransportUnhealthy = unhealthy;
        Q_EMIT listsChanged();
    }
}

bool Devices::headphonesTransportUnhealthy() const
{
    return m_app->engine()->headphonesTransportUnhealthy();
}

QString Devices::headphones() const { return m_app->engine()->headphoneDevice(); }

void Devices::setHeadphones(const QString &nodeName)
{
    if (nodeName == headphones()) {
        // Still refresh the Bluetooth address if the graph now knows it.
    } else {
        m_app->engine()->setHeadphoneDevice(nodeName);
    }
    QString address;
    if (const pw::Node *n = m_app->pw()->graph().nodeByName(nodeName)) {
        address = n->prop("api.bluez5.address");
    }
    m_app->engine()->setHeadphonesBtAddress(address);
    m_app->saveSettingsSoon();
    Q_EMIT choiceChanged();
    rebuild();
}

QString Devices::mic() const { return m_app->engine()->micDevice(); }

void Devices::setMic(const QString &nodeName)
{
    if (nodeName != mic()) {
        m_app->engine()->setMicDevice(nodeName);
    }
    QString address;
    if (!nodeName.isEmpty()) {
        if (const pw::Node *n = m_app->pw()->graph().nodeByName(nodeName)) {
            address = n->prop("api.bluez5.address");
        }
    }
    m_app->engine()->setMicBtAddress(address);
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

bool Devices::canPreferHighQualityHeadphones() const
{
    const pw::Graph &g = m_app->pw()->graph();
    const pw::Device *device =
        deviceForHeadphones(g, m_app->engine()->headphoneDevice(), m_app->engine()->headphonesBtAddress());
    return device && !pw::rankedHighQualityProfiles(*device).isEmpty();
}

bool Devices::preferHighQualityHeadphones()
{
    pw::PwContext *pw = m_app->pw();
    const pw::Device *device = deviceForHeadphones(pw->graph(), m_app->engine()->headphoneDevice(),
                                                   m_app->engine()->headphonesBtAddress());
    if (!device) {
        Q_EMIT m_app->toast(i18n("Could not find that Bluetooth device. Open Sound Settings to switch it."));
        return false;
    }
    const uint32_t deviceId = device->id;
    const QList<uint32_t> ranked = pw::rankedHighQualityProfiles(*device);
    if (ranked.isEmpty()) {
        Q_EMIT m_app->toast(i18n("High quality mode is not available. Open Sound Settings to switch it."));
        return false;
    }

    const QString btAddress = m_app->engine()->headphonesBtAddress();
    for (uint32_t index : ranked) {
        if (!pw->setDeviceProfile(deviceId, index, true)) {
            continue;
        }
        // Wait briefly for PipeWire / WirePlumber to rebuild sinks after the profile change.
        QEventLoop loop;
        QTimer::singleShot(600, &loop, &QEventLoop::quit);
        const auto conn = connect(pw, &pw::PwContext::graphChanged, &loop, &QEventLoop::quit);
        loop.exec();
        disconnect(conn);
        pw->roundtrip(300);

        const QString inUse = m_app->engine()->resolvedSinkName();
        if (!headphonesStillBad(pw->graph(), inUse, btAddress)) {
            Q_EMIT m_app->toast(i18n("Requested high quality Bluetooth audio."));
            rebuild();
            return true;
        }
    }
    Q_EMIT m_app->toast(i18n("Could not switch to high quality mode. Open Sound Settings to try again."));
    rebuild();
    return false;
}

} // namespace rostrum::app
