#include "app/AppController.h"

#include "core/Paths.h"
#include "core/Requirements.h"

#include <KLocalizedString>

#include <QClipboard>
#include <QGuiApplication>
#include <QJSEngine>
#include <QLoggingCategory>

#include <cstring>

Q_LOGGING_CATEGORY(lcApp, "rostrum.app")

namespace rostrum::app {

namespace {
constexpr int kSaveDelayMs = 500;
constexpr int kRetryMs = 5000;
} // namespace

AppController *AppController::s_instance = nullptr;

AppController::AppController(QObject *parent)
    : QObject(parent)
    , m_engine(&m_pw)
    , m_scenes(&m_engine, paths::scenesDir())
{
    Q_ASSERT(!s_instance);
    s_instance = this;

    m_saveTimer.setSingleShot(true);
    m_saveTimer.setInterval(kSaveDelayMs);
    connect(&m_saveTimer, &QTimer::timeout, this, &AppController::saveSettingsNow);

    // While PipeWire is gone, try again quietly; the Retry button does the same thing now.
    m_retryTimer.setInterval(kRetryMs);
    connect(&m_retryTimer, &QTimer::timeout, this, &AppController::retry);

    connect(&m_pw, &pw::PwContext::stateChanged, this, &AppController::onConnectionChanged);
    connect(&m_pw, &pw::PwContext::graphChanged, this, &AppController::updateStatus);
    connect(&m_pw, &pw::PwContext::graphChanged, this, &AppController::devicesChanged);
    connect(&m_pw, &pw::PwContext::defaultsChanged, this, &AppController::devicesChanged);

    connect(&m_engine, &engine::Engine::mixStateChanged, this, [this] {
        updateStatus();
        Q_EMIT mixChanged();
    });
    connect(&m_engine, &engine::Engine::devicesChanged, this, [this] {
        updateStatus();
        Q_EMIT devicesChanged();
    });
    connect(&m_engine, &engine::Engine::levelsChanged, this, &AppController::levelsChanged);
    connect(&m_engine, &engine::Engine::sceneChanged, this, &AppController::levelsChanged);
    connect(&m_engine, &engine::Engine::headphonesLost, this, &AppController::headphonesLost);

    for (auto sig : {&engine::SceneManager::scenesChanged, &engine::SceneManager::currentChanged,
                     &engine::SceneManager::dirtyChanged}) {
        connect(&m_scenes, sig, this, &AppController::scenesChanged);
    }
    connect(&m_scenes, &engine::SceneManager::defaultChanged, this, [this] {
        m_settings.defaultScene = m_scenes.defaultName();
        saveSettingsSoon();
        Q_EMIT scenesChanged();
    });
    connect(&m_scenes, &engine::SceneManager::scenesChanged, this, [this] {
        if (const QStringList order = m_scenes.names(); order != m_settings.sceneOrder) {
            m_settings.sceneOrder = order;
            saveSettingsSoon();
        }
    });
    connect(&m_scenes, &engine::SceneManager::errorOccurred, this, &AppController::toast);
    connect(&m_engine, &engine::Engine::autoSkipChanged, this, [this] {
        m_settings.autoSkip = m_engine.autoSkip();
        saveSettingsSoon();
        Q_EMIT settingsChanged();
    });
}

AppController::~AppController()
{
    s_instance = nullptr;
}

AppController *AppController::create(QQmlEngine *, QJSEngine *)
{
    QJSEngine::setObjectOwnership(s_instance, QJSEngine::CppOwnership);
    return s_instance;
}

void AppController::start()
{
    QString err;
    m_settings = loadSettings(paths::settingsFile(), &err);
    if (!err.isEmpty()) {
        qCWarning(lcApp) << "Settings file could not be read, using defaults." << err;
    }
    m_engine.setHeadphoneDevice(m_settings.headphones);
    m_engine.setMicDevice(m_settings.mic);
    m_engine.setAutoAssign(m_settings.autoAssign);
    m_engine.setAutoSkip(m_settings.autoSkip);
    m_scenes.setAutoSave(m_settings.autoSaveScenes);
    m_scenes.setOrder(m_settings.sceneOrder);
    m_scenes.setTrashDir(paths::trashDir());
    m_scenes.load(m_settings.defaultScene);
    if (m_settings.wizardDone) {
        m_engine.createMix();
    }
    Q_EMIT settingsChanged();
    Q_EMIT windowStateChanged();
    qCInfo(lcApp) << "Starting with scene" << m_scenes.currentName();
    m_pw.start();
    onConnectionChanged();
}

void AppController::retry()
{
    if (m_pw.state() == pw::PwContext::State::Connecting) {
        return;
    }
    m_versionRefused = false;
    m_pw.start();
}

void AppController::onConnectionChanged()
{
    const auto state = m_pw.state();
    if (state == pw::PwContext::State::Failed) {
        m_retryTimer.start();
    } else {
        m_retryTimer.stop();
    }
    if (state == pw::PwContext::State::Ready) {
        if (!requirements::pipewireOk(m_pw.serverVersion())) {
            qCWarning(lcApp) << "PipeWire" << m_pw.serverVersion() << "is older than 1.0";
            m_versionRefused = true;
            m_pw.stop();
        }
    }
    updateStatus();
    Q_EMIT devicesChanged();
}

void AppController::updateStatus()
{
    QString status;
    QString detail;
    const auto state = m_pw.state();
    if (m_versionRefused) {
        status = QStringLiteral("missing");
        detail = i18n("PipeWire %1 is too old. Rostrum needs PipeWire 1.0 or newer.", m_pw.serverVersion());
    } else if (state == pw::PwContext::State::Failed || state == pw::PwContext::State::Idle) {
        status = QStringLiteral("missing");
        detail = connectFailureText();
    } else if (state == pw::PwContext::State::Connecting) {
        status = QStringLiteral("connecting");
    } else {
        QStringList problems;
        if (!m_pw.hasWirePlumber()) {
            problems << i18n("The session manager is not WirePlumber.");
        } else if (!requirements::wireplumberOk(m_pw.wireplumberVersion())) {
            problems << i18n("WirePlumber %1 is older than 0.5.", m_pw.wireplumberVersion());
        }
        if (!m_engine.mixError().isEmpty()) {
            problems << m_engine.mixError();
        }
        if (m_engine.headphonesMissing()) {
            problems << i18n("The saved headphones are not connected.");
        }
        if (m_engine.micMissing()) {
            problems << i18n("The saved mic is not connected.");
        }
        status = problems.isEmpty() ? QStringLiteral("ok") : QStringLiteral("degraded");
        detail = problems.join(QLatin1Char(' '));

        // Rule export only makes sense when WirePlumber does the fallback linking.
        if (!m_ruleExportOn && m_pw.hasWirePlumber()) {
            m_ruleExportOn = true;
            m_scenes.enableRuleExport(paths::pipewirePulseFragment(), paths::pipewireClientFragment());
        }
    }
    if (status != m_status || detail != m_detail) {
        m_status = status;
        m_detail = detail;
        Q_EMIT statusChanged();
    }
}

QString AppController::pipewireStatusText() const
{
    if (m_status == QLatin1String("ok")) {
        return i18n("PipeWire OK");
    }
    if (m_status == QLatin1String("degraded")) {
        return i18n("PipeWire degraded");
    }
    if (m_status == QLatin1String("connecting")) {
        return i18n("Connecting to PipeWire…");
    }
    return i18n("PipeWire missing");
}

QString AppController::connectFailureText() const
{
    if (m_pw.errorString().isEmpty()) {
        return i18n("PipeWire is not running.");
    }
    switch (requirements::connectProblem(m_pw.errorCode())) {
    case requirements::ConnectProblem::NotRunning:
        return i18n("PipeWire is not running in your session, so there is no audio server to connect to.");
    case requirements::ConnectProblem::NotAnswering:
        return i18n("PipeWire is not answering. The service may have stopped or crashed.");
    case requirements::ConnectProblem::NotAllowed:
        return i18n("Rostrum is not allowed to connect to PipeWire. Run it as your own user, not with sudo.");
    case requirements::ConnectProblem::Other:
        break;
    }
    return i18n("Could not connect to PipeWire: %1", QString::fromLocal8Bit(std::strerror(m_pw.errorCode())));
}

QString AppController::startCommand() const { return requirements::startCommand(); }

QVariantList AppController::installCommands() const
{
    QVariantList out;
    for (const auto &c : requirements::installCommands()) {
        out << QVariantMap{{QStringLiteral("distro"), c.distro}, {QStringLiteral("command"), c.command}};
    }
    return out;
}
bool AppController::connected() const { return m_pw.state() == pw::PwContext::State::Ready && !m_versionRefused; }
bool AppController::hasWirePlumber() const { return m_pw.hasWirePlumber(); }
QString AppController::pipewireVersion() const { return m_pw.serverVersion(); }
QString AppController::wireplumberVersion() const { return m_pw.wireplumberVersion(); }
QString AppController::version() const { return QStringLiteral(ROSTRUM_VERSION); }

QString AppController::describeNode(const QString &nodeName) const
{
    if (nodeName.isEmpty()) {
        return {};
    }
    const pw::Node *n = m_pw.graph().nodeByName(nodeName);
    if (!n) {
        return nodeName;
    }
    return n->description.isEmpty() ? n->name : n->description;
}

QString AppController::headphonesText() const
{
    if (!connected()) {
        return i18n("Unknown");
    }
    const QString d = describeNode(m_engine.resolvedSinkName());
    return d.isEmpty() ? i18n("None") : d;
}

QString AppController::micText() const
{
    if (!connected()) {
        return i18n("Unknown");
    }
    const QString d = describeNode(m_engine.resolvedSourceName());
    return d.isEmpty() ? i18n("None") : d;
}

bool AppController::headphonesMissing() const { return connected() && m_engine.headphonesMissing(); }
bool AppController::micMissing() const { return connected() && m_engine.micMissing(); }
bool AppController::hasMic() const { return connected() && m_engine.hasMic(); }

bool AppController::micMuted() const { return m_engine.micMuted(); }
void AppController::setMicMuted(bool muted) { m_engine.setMicMuted(muted); }
void AppController::toggleMicMute() { m_engine.setMicMuted(!m_engine.micMuted()); }

double AppController::micGain() const
{
    const Bus *mic = m_engine.scene().micBus();
    return mic ? mic->volume : 1.0;
}

void AppController::setMicGain(double position) { m_engine.setBusVolume(QString::fromLatin1(kMicBusId), position); }
bool AppController::sidetoneEnabled() const { return m_engine.sidetoneEnabled(); }
void AppController::setSidetoneEnabled(bool on) { m_engine.setSidetoneEnabled(on); }
double AppController::sidetoneVolume() const { return m_engine.scene().sidetoneVolume; }
void AppController::setSidetoneVolume(double position) { m_engine.setSidetoneVolume(position); }

QStringList AppController::sceneNames() const { return m_scenes.names(); }
QString AppController::currentScene() const { return m_scenes.currentName(); }
QString AppController::defaultScene() const { return m_scenes.defaultName(); }
bool AppController::sceneDirty() const { return m_scenes.dirty(); }

bool AppController::switchScene(const QString &name) { return m_scenes.switchTo(name); }

void AppController::requestSceneSwitch(const QString &name)
{
    if (name.isEmpty() || name == m_scenes.currentName()) {
        return;
    }
    if (m_settings.confirmSceneSwitch && !m_scenes.autoSave() && m_scenes.dirty()) {
        Q_EMIT raiseRequested();
        Q_EMIT sceneSwitchConfirmRequested(name);
        return;
    }
    m_scenes.switchTo(name);
}

void AppController::triggerAction(const QString &id)
{
    if (id == QLatin1String(actions::kMuteMic)) {
        toggleMicMute();
        return;
    }
    if (id == QLatin1String(actions::kMuteStream)) {
        m_engine.setMasterStreamMuted(!m_engine.scene().masterStreamMuted);
        return;
    }
    const QStringList names = m_scenes.names();
    if (names.isEmpty() || !connected()) {
        return;
    }
    const int current = names.indexOf(m_scenes.currentName());
    int target = -1;
    if (id == QLatin1String(actions::kNextScene)) {
        target = (current + 1) % names.size();
    } else if (id == QLatin1String(actions::kPrevScene)) {
        target = (current - 1 + names.size()) % names.size();
    } else if (const int slot = actions::sceneSlot(id); slot > 0) {
        // Slots index the user's scene order, so a moved scene takes its new number's hotkey.
        if (slot <= names.size()) {
            target = slot - 1;
        } else {
            Q_EMIT toast(i18n("There is no scene %1", slot));
        }
    }
    if (target >= 0) {
        requestSceneSwitch(names.at(target));
    }
}

bool AppController::saveScene()
{
    if (!m_scenes.save()) {
        return false;
    }
    Q_EMIT toast(i18n("Scene saved"));
    return true;
}

bool AppController::mixEnabled() const { return m_engine.mixEnabled(); }
bool AppController::mixReady() const { return m_engine.mixReady(); }
bool AppController::mixBusy() const { return m_engine.mixEnabled() && !m_engine.mixReady() && m_engine.mixError().isEmpty(); }
QString AppController::mixError() const { return m_engine.mixError(); }
void AppController::createMix() { m_engine.createMix(); }

void AppController::finishWizard()
{
    if (m_settings.wizardDone) {
        return;
    }
    m_settings.wizardDone = true;
    m_settings.setupVersion = kSetupVersion;
    m_settings.lastPage = QStringLiteral("mixer");
    if (m_scenes.dirty()) {
        m_scenes.save();
    }
    saveSettingsNow();
    Q_EMIT settingsChanged();
    Q_EMIT windowStateChanged();
}

void AppController::skipWizard()
{
    m_engine.createMix();
    finishWizard();
}

void AppController::finishSetupUpdate()
{
    if (!setupUpdateNeeded()) {
        return;
    }
    m_settings.setupVersion = kSetupVersion;
    saveSettingsNow();
    Q_EMIT settingsChanged();
}

void AppController::copyToClipboard(const QString &text, const QString &toastText)
{
    QGuiApplication::clipboard()->setText(text);
    Q_EMIT toast(toastText.isEmpty() ? i18n("Copied") : toastText);
}

void AppController::setLastPage(const QString &page)
{
    if (page == m_settings.lastPage) {
        return;
    }
    m_settings.lastPage = page;
    saveSettingsSoon();
    Q_EMIT windowStateChanged();
}

void AppController::setSidebarCollapsed(bool collapsed)
{
    if (collapsed == m_settings.sidebarCollapsed) {
        return;
    }
    m_settings.sidebarCollapsed = collapsed;
    saveSettingsSoon();
    Q_EMIT windowStateChanged();
}

void AppController::setWindowWidth(int w)
{
    if (w == m_settings.windowWidth) {
        return;
    }
    m_settings.windowWidth = w;
    saveSettingsSoon();
    Q_EMIT windowStateChanged();
}

void AppController::setWindowHeight(int h)
{
    if (h == m_settings.windowHeight) {
        return;
    }
    m_settings.windowHeight = h;
    saveSettingsSoon();
    Q_EMIT windowStateChanged();
}

void AppController::setCompactWindow(bool on)
{
    if (on == m_settings.compactWindow) {
        return;
    }
    m_settings.compactWindow = on;
    saveSettingsSoon();
    Q_EMIT windowStateChanged();
}

void AppController::setCompactWidth(int w)
{
    w = std::max(kCompactMinWidth, w);
    if (w == m_settings.compactWidth) {
        return;
    }
    m_settings.compactWidth = w;
    saveSettingsSoon();
    Q_EMIT windowStateChanged();
}

void AppController::setCompactHeight(int h)
{
    h = std::max(kCompactMinHeight, h);
    if (h == m_settings.compactHeight) {
        return;
    }
    m_settings.compactHeight = h;
    saveSettingsSoon();
    Q_EMIT windowStateChanged();
}

void AppController::setKeepOnTop(bool on)
{
    if (on == m_settings.keepOnTop) {
        return;
    }
    m_settings.keepOnTop = on;
    saveSettingsSoon();
    Q_EMIT windowStateChanged();
}

void AppController::saveSettingsSoon() { m_saveTimer.start(); }

void AppController::saveSettingsNow()
{
    m_saveTimer.stop();
    m_settings.headphones = m_engine.headphoneDevice();
    m_settings.mic = m_engine.micDevice();
    QString err;
    if (!saveSettings(paths::settingsFile(), m_settings, &err)) {
        qCWarning(lcApp) << "Could not save settings:" << err;
    }
}

void AppController::quit()
{
    m_scenes.flush();
    saveSettingsNow();
    QGuiApplication::quit();
}

} // namespace rostrum::app
