#include "app/Obs.h"

#include "app/AppController.h"
#include "app/Desktop.h"
#include "core/Paths.h"
#include "obs/ObsCaptures.h"
#include "obs/ObsLive.h"
#include "obs/SceneCollection.h"

#include <KLocalizedString>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QJSEngine>
#include <QJsonDocument>
#include <QLoggingCategory>
#include <QSaveFile>
#include <QSet>

#include <algorithm>

Q_LOGGING_CATEGORY(lcObs, "rostrum.obs", QtInfoMsg)

namespace rostrum::app {

namespace {

constexpr int kPollMs = 3000;
// While only following OBS in the background: how often to look for a running OBS, and the
// longest wait between attempts while its WebSocket server refuses.
constexpr int kBackgroundPollMs = 5000;
constexpr qint64 kMaxRetryMs = 60000;
constexpr int kEvents = obs::Client::kEventScenes | obs::Client::kEventInputs | obs::Client::kEventOutputs |
                        obs::Client::kEventSceneItems;

QString undoFile()
{
    return paths::stateDir() + QLatin1String("/obs-undo.json");
}

QJsonObject readJson(const QString &path, QString *error)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        *error = f.errorString();
        return {};
    }
    QJsonParseError pe{};
    const QJsonDocument doc = QJsonDocument::fromJson(f.readAll(), &pe);
    if (pe.error != QJsonParseError::NoError || !doc.isObject()) {
        *error = pe.errorString();
        return {};
    }
    return doc.object();
}

bool writeJson(const QString &path, const QJsonObject &o, QString *error)
{
    QSaveFile f(path);
    if (!f.open(QIODevice::WriteOnly)) {
        *error = f.errorString();
        return false;
    }
    f.write(QJsonDocument(o).toJson(QJsonDocument::Indented));
    if (!f.commit()) {
        *error = f.errorString();
        return false;
    }
    return true;
}

QString quoted(const QString &name)
{
    return i18nc("@info a name in quotes", "“%1”", name);
}

} // namespace

Obs *Obs::s_instance = nullptr;

Obs::Obs(AppController *app, QObject *parent)
    : QObject(parent)
    , m_app(app)
{
    Q_ASSERT(!s_instance);
    s_instance = this;
    m_recordingTracks = new ObsRecording(app, this, &m_client);

    // Screenshot and test runs must never reach the user's own OBS on their own.
    m_offscreen = QGuiApplication::platformName() == QLatin1String("offscreen") ||
                  !qEnvironmentVariableIsEmpty("ROSTRUM_SCREENSHOT");

    m_poll.setInterval(kPollMs);
    connect(&m_poll, &QTimer::timeout, this, &Obs::poll);
    m_refetch.setSingleShot(true);
    m_refetch.setInterval(300);
    connect(&m_refetch, &QTimer::timeout, this, &Obs::fetchLive);
    m_graphDebounce.setSingleShot(true);
    m_graphDebounce.setInterval(250);
    connect(&m_graphDebounce, &QTimer::timeout, this, &Obs::rebuildRecordings);
    connect(&m_graphDebounce, &QTimer::timeout, this, &Obs::refreshWarnings);

    connect(app->pw(), &pw::PwContext::graphChanged, this, [this] {
        if (m_active || !m_problems.isEmpty()) {
            m_graphDebounce.start();
        }
    });
    connect(app, &AppController::levelsChanged, this, &Obs::refreshWarnings);
    connect(app->engine(), &engine::Engine::soloChanged, this, &Obs::refreshWarnings);
    connect(app, &AppController::scenesChanged, this, &Obs::sceneMapChanged);
    connect(&m_client, &obs::Client::statusChanged, this, [this] {
        switch (m_client.status()) {
        case obs::Client::Status::Connected:
            m_failures = 0;
            if (m_stateName != QLatin1String("connected")) {
                setStateName(QStringLiteral("connected"));
                fetchLive();
            }
            break;
        case obs::Client::Status::AuthFailed:
            m_rejectedPassword = m_ws.password;
            setStateName(QStringLiteral("authFailed"));
            clearPlan();
            break;
        case obs::Client::Status::Failed:
            // OBS still starting, or its server refusing: back off instead of knocking every poll.
            m_failures++;
            m_retryAt =
                QDateTime::currentMSecsSinceEpoch() +
                std::min<qint64>(kMaxRetryMs, qint64(kBackgroundPollMs) << std::min(m_failures - 1, 4));
            setStateName(QStringLiteral("failed"));
            clearPlan();
            break;
        case obs::Client::Status::Connecting:
        case obs::Client::Status::Disconnected:
            break;
        }
        Q_EMIT changed();
    });
    connect(&m_client, &obs::Client::event, this, [this](const QString &type, const QJsonObject &) {
        const bool affectsPlan =
            type.startsWith(QLatin1String("Input")) || type.startsWith(QLatin1String("Scene"));
        if (m_active && !m_busy && affectsPlan && type != QLatin1String("InputVolumeChanged")) {
            m_refetch.start();
        }
    });

    connect(&m_live, &obs::LiveStatus::changed, this, [this] {
        if (!m_live.streaming()) {
            refreshWarnings();
        }
        Q_EMIT liveChanged();
        Q_EMIT sceneMapChanged();
    });
    connect(&m_live, &obs::LiveStatus::streamStarted, this, &Obs::checkGoLive);
    connect(&m_live, &obs::LiveStatus::programSceneChanged, this, &Obs::followProgramScene);

    m_readinessRefresh.setInterval(5000);
    connect(&m_readinessRefresh, &QTimer::timeout, this, [this] {
        if (m_readinessRequested && m_pageActive && !m_readinessChecking)
            checkReadiness();
    });
    connect(app->pw(), &pw::PwContext::graphChanged, this, &Obs::rebuildReadiness);
    connect(app, &AppController::statusChanged, this, &Obs::rebuildReadiness);
    connect(app->engine(), &engine::Engine::levelsChanged, this, &Obs::rebuildReadiness);
    connect(app->engine(), &engine::Engine::levelsChanged, this, &Obs::refreshWarnings);
    connect(app->engine(), &engine::Engine::soloChanged, this, &Obs::rebuildReadiness);
    connect(app->engine(), &engine::Engine::appsChanged, this, &Obs::rebuildReadiness);
    connect(app->engine(), &engine::Engine::devicesChanged, this, &Obs::rebuildReadiness);
    connect(app->engine(), &engine::Engine::micFiltersStateChanged, this, &Obs::rebuildReadiness);
    connect(&m_client, &obs::Client::statusChanged, this, [this] {
        invalidateReadiness(i18n("OBS connection changed; a fresh capture snapshot is required."));
        if (m_readinessRequested && m_pageActive && m_client.status() == obs::Client::Status::Connected)
            checkReadiness();
    });
    connect(&m_client, &obs::Client::event, this, [this](const QString &type, const QJsonObject &) {
        if (type.startsWith(QLatin1String("Input")) || type.startsWith(QLatin1String("Scene")) ||
            type == QLatin1String("CurrentProgramSceneChanged")) {
            invalidateReadiness(i18n("OBS inputs or program scene changed; the previous snapshot is stale."));
            if (m_readinessRequested && m_pageActive)
                m_readinessRefresh.start(300);
        }
    });
    updatePolling();
}

Obs::~Obs()
{
    s_instance = nullptr;
}

Obs *Obs::create(QQmlEngine *, QJSEngine *)
{
    QJSEngine::setObjectOwnership(s_instance, QJSEngine::CppOwnership);
    return s_instance;
}

void Obs::setActive(bool active)
{
    if (active != m_pageActive) {
        m_pageActive = active;
        if (!active) {
            m_readinessRefresh.stop();
            invalidateReadiness(i18n("The OBS page was closed; capture observations are stale."));
        }
        updateActive();
        if (active && m_readinessRequested)
            checkReadiness();
        Q_EMIT activeChanged();
    }
}

void Obs::setWizardActive(bool active)
{
    if (active != m_wizardActive) {
        m_wizardActive = active;
        updateActive();
        Q_EMIT activeChanged();
    }
}

void Obs::updateActive()
{
    const bool active = m_pageActive || m_wizardActive;
    if (active == m_active) {
        return;
    }
    m_active = active;
    if (active) {
        refresh();
    } else if (!backgroundAllowed() && !m_busy) {
        m_client.close();
        m_stateName.clear();
    }
    rebuildRecordings();
    updatePolling();
}

bool Obs::backgroundAllowed() const
{
    return m_app->settings().obsBackground && !m_offscreen;
}

void Obs::updatePolling()
{
    m_poll.setInterval(m_active ? kPollMs : kBackgroundPollMs);
    if (m_active || backgroundAllowed()) {
        if (!m_poll.isActive()) {
            m_poll.start();
            poll();
        }
    } else {
        m_poll.stop();
    }
}

void Obs::refresh()
{
    m_retryAt = 0;
    poll();
    // Read OBS again even when nothing changed on its side: Rostrum's devices may have.
    if (m_active && m_install && m_stateName == QLatin1String("closed")) {
        loadOffline(true);
    }
    fetchLive();
}

void Obs::poll()
{
    if (m_busy) {
        return;
    }
    if (!m_active && !backgroundAllowed()) {
        m_client.close();
        return;
    }
    const auto installs = obs::findInstalls();
    if (installs.isEmpty()) {
        m_install.reset();
        m_running = false;
        m_client.close();
        clearPlan();
        setStateName(obs::obsBinaryInstalled() ? QStringLiteral("neverRun") : QStringLiteral("notInstalled"));
        Q_EMIT changed();
        return;
    }
    m_install = installs.first();
    const bool wasRunning = m_running;
    m_running = obs::isObsRunning();
    if (m_running && !wasRunning) {
        m_failures = 0;
        m_retryAt = 0;
    }
    m_ws = obs::readWebSocketConfig(m_install->configDir);

    if (!m_running) {
        m_client.close();
        setStateName(QStringLiteral("closed"));
        if (m_active) {
            loadOffline();
        }
    } else if (!m_ws.enabled) {
        m_client.close();
        setStateName(QStringLiteral("noWebSocket"));
        clearPlan();
    } else {
        openClient();
    }
    Q_EMIT changed();
}

void Obs::openClient()
{
    // A refused connection (OBS still starting) is retried quietly: on every poll while the page
    // is open, with a growing pause otherwise. A refused password only once OBS's settings hold
    // a different one.
    const auto status = m_client.status();
    if (status == obs::Client::Status::Connected || status == obs::Client::Status::Connecting) {
        return;
    }
    if (status == obs::Client::Status::AuthFailed && m_ws.password == m_rejectedPassword) {
        return;
    }
    if (status == obs::Client::Status::Failed && !m_active &&
        QDateTime::currentMSecsSinceEpoch() < m_retryAt) {
        return;
    }
    if (status != obs::Client::Status::Failed) {
        setStateName(QStringLiteral("connecting"));
    }
    m_client.open(m_ws.port, m_ws.password, kEvents);
}

void Obs::fetchLive()
{
    if (!m_active || m_client.status() != obs::Client::Status::Connected) {
        return;
    }
    obs::fetchState(&m_client, [this](const obs::State &state, const QString &error) {
        if (!error.isEmpty()) {
            qCWarning(lcObs) << "reading OBS failed:" << error;
            return;
        }
        setPlan(state, obs::Mode::Live);
    });
}

void Obs::loadOffline(bool force)
{
    const QString path = obs::sceneCollectionFile(m_install->configDir);
    const QString stamp = path + QLatin1Char('@') + QFileInfo(path).lastModified().toString(Qt::ISODateWithMs);
    if (!force && m_havePlan && m_plan.mode == obs::Mode::Offline && stamp == m_offlineStamp) {
        return;
    }
    m_offlineStamp = stamp;
    QString error;
    const QJsonObject doc = path.isEmpty() ? QJsonObject() : readJson(path, &error);
    if (doc.isEmpty()) {
        clearPlan();
        return;
    }
    setPlan(obs::stateFromCollection(doc), obs::Mode::Offline);
}

void Obs::setPlan(const obs::State &state, obs::Mode mode)
{
    // Keep the user's unticked conflicts across refreshes.
    QSet<QString> unticked;
    for (const auto &a : std::as_const(m_plan.actions)) {
        if (!a.enabled) {
            unticked.insert(a.input);
        }
    }
    m_state = state;
    m_haveState = true;
    m_plan = obs::makePlan(state, obs::factsFrom(*m_app->pw()), mode, m_recordingTracks->intendedDevices());
    for (auto &a : m_plan.actions) {
        if (a.role == obs::Action::Role::Conflict && unticked.contains(a.input)) {
            a.enabled = false;
        }
    }
    m_havePlan = true;
    rebuildRecordings();
    Q_EMIT planChanged();
    Q_EMIT sceneMapChanged();
    Q_EMIT changed();
}

void Obs::clearPlan()
{
    const bool had = m_havePlan;
    m_havePlan = false;
    m_haveState = false;
    m_plan = {};
    m_state = {};
    if (had) {
        Q_EMIT planChanged();
        Q_EMIT sceneMapChanged();
    }
    rebuildRecordings();
}

void Obs::setStateName(const QString &name)
{
    if (name != m_stateName) {
        m_stateName = name;
        Q_EMIT changed();
    }
}

QString Obs::summary() const
{
    const QString &s = m_stateName;
    if (s == QLatin1String("notInstalled")) {
        return i18n("OBS isn't installed.");
    }
    if (s == QLatin1String("neverRun")) {
        return i18n("Start OBS once, then come back here.");
    }
    if (s == QLatin1String("noWebSocket")) {
        return i18n("OBS is running, but its WebSocket server is off.");
    }
    if (s == QLatin1String("connecting")) {
        return i18n("Connecting to OBS…");
    }
    if (s == QLatin1String("authFailed")) {
        return i18n("OBS refused the WebSocket password saved in its settings.");
    }
    if (s == QLatin1String("failed")) {
        return i18n("Rostrum can't reach OBS.");
    }
    if (!m_havePlan) {
        return s == QLatin1String("closed") ? i18n("OBS is closed, and Rostrum can't read its scene collection.")
                                            : i18n("Reading OBS…");
    }
    if (m_plan.isEmpty()) {
        return i18n("OBS is set up: it records Rostrum Mic and Rostrum Stream Mix.");
    }
    if (onlyDoubling()) {
        return i18n("OBS records some audio twice.");
    }
    return i18n("OBS isn't recording through Rostrum yet.");
}

bool Obs::onlyDoubling() const
{
    if (!m_havePlan || m_plan.isEmpty()) {
        return false;
    }
    return std::all_of(m_plan.actions.cbegin(), m_plan.actions.cend(),
                       [](const obs::Action &a) { return a.role == obs::Action::Role::Conflict; });
}

QString Obs::detail() const
{
    const QString &s = m_stateName;
    if (s == QLatin1String("notInstalled")) {
        return i18n("Rostrum works without it. Any recorder that can capture an audio device can record Rostrum Mic and Rostrum Stream Mix.");
    }
    if (s == QLatin1String("noWebSocket")) {
        return i18n("In OBS, open Tools ▸ WebSocket Server Settings and tick “Enable WebSocket server”. Or close OBS, and Rostrum will set it up directly.");
    }
    if (s == QLatin1String("authFailed")) {
        return i18n("If you just changed the password in OBS, press Apply there. Or close OBS, and Rostrum will set it up directly.");
    }
    if (s == QLatin1String("failed")) {
        return m_client.errorString();
    }
    if (onlyDoubling()) {
        return i18n("Rostrum Mic and Rostrum Stream Mix are in place, but the sources marked below play on top of them. Fix OBS mutes them.");
    }
    if (s == QLatin1String("closed") && m_havePlan && !m_plan.isEmpty()) {
        return i18n("OBS is closed, so Rostrum will edit its scene collection. The changes show up the next time OBS starts.");
    }
    if (s == QLatin1String("connected") && !m_client.obsVersion().isEmpty()) {
        return i18n("Connected to OBS %1.", m_client.obsVersion());
    }
    return {};
}

bool Obs::canApply() const
{
    if (m_busy || !m_havePlan || m_plan.isEmpty()) {
        return false;
    }
    return m_stateName == QLatin1String("connected") || m_stateName == QLatin1String("closed");
}

bool Obs::canUndo() const
{
    if (m_busy || !m_install) {
        return false;
    }
    const obs::Undo u = loadUndo();
    return !u.isEmpty() && u.configDir == m_install->configDir;
}

QVariantList Obs::planItems() const
{
    using T = obs::Action::Type;
    using R = obs::Action::Role;
    QVariantList rows;
    for (qsizetype n = 0; n < m_plan.actions.size(); ++n) {
        const obs::Action &a = m_plan.actions.at(n);
        const bool mic = a.role == R::Mic;
        const bool vod = a.role == R::Vod;
        QString title;
        QString detail;
        switch (a.type) {
        case T::SetDevice:
            if (mic) {
                title = i18n("Switch %1 to Rostrum Mic", quoted(a.input));
                detail = i18n("The source keeps its filters, tracks and scenes.");
            } else if (vod) {
                title = i18n("Switch %1 to Rostrum VOD Mix", quoted(a.input));
                detail = i18n("It records your Twitch VOD mix on track 2 without music.");
            } else {
                title = i18n("Switch %1 to Rostrum Stream Mix", quoted(a.input));
                detail = i18n("The source keeps its filters, tracks and scenes.");
            }
            break;
        case T::Unmute:
            title = i18n("Unmute %1", quoted(a.input));
            if (mic) {
                detail = i18n("It already records Rostrum Mic.");
            } else if (vod) {
                detail = i18n("It already records Rostrum VOD Mix.");
            } else {
                detail = i18n("It already records Rostrum Stream Mix.");
            }
            break;
        case T::CreateInput:
            title = i18np("Add %2 to your scene", "Add %2 to all %1 scenes", a.scenes.size(), quoted(a.input));
            if (mic) {
                detail = i18n("It records Rostrum Mic: your voice after Rostrum's gain and mute.");
            } else if (vod) {
                detail = i18n("It records Rostrum VOD Mix: every bus sent to VOD on track 2 without music.");
            } else {
                detail = i18n("It records Rostrum Stream Mix: every bus sent to Stream or Both. OBS mixes it once, however many scenes it's in.");
            }
            break;
        case T::CreateGlobal:
            if (mic) {
                title = i18n("Set OBS's Mic/Aux device to Rostrum Mic");
            } else {
                title = i18n("Set OBS's Desktop Audio device to Rostrum Stream Mix");
            }
            detail = i18n("Global audio devices play in every scene.");
            break;
        case T::SetTwitchVodTrack:
            title = i18n("Set Output → Streaming → Twitch VOD Track to 2");
            detail = i18n("OBS sends track 2 to Twitch for your VOD.");
            break;
        case T::Mute:
            title = i18n("Mute %1", quoted(a.input));
            switch (a.capture) {
            case obs::Capture::Mic:
                detail = i18n("It records your mic directly: your voice would be doubled and skip Rostrum's mute.");
                break;
            case obs::Capture::Output:
                detail = i18n("It records everything you hear, including buses you keep off stream.");
                break;
            case obs::Capture::App:
                detail = i18n("It records one app directly: that app would be doubled and its bus ignored.");
                break;
            case obs::Capture::RostrumBus:
                detail = i18n("It records one Rostrum bus, which Rostrum Stream Mix already includes: viewers would hear it twice.");
                break;
            case obs::Capture::RostrumMic:
            case obs::Capture::RostrumStream:
            case obs::Capture::RostrumVod:
                detail = i18n("Another source already records the same Rostrum device, so this one would double it.");
                break;
            case obs::Capture::None:
                break;
            }
            break;
        }
        if ((a.type == T::CreateInput || a.type == T::CreateGlobal || a.type == T::SetDevice) && a.tracks) {
            QStringList tracks;
            for (int t = 0; t < 6; ++t) {
                if (a.tracks & (1u << t)) {
                    tracks << QString::number(t + 1);
                }
            }
            if (a.role == R::Stream) {
                detail += QLatin1Char(' ') + i18n("Track 1 for your live stream.");
            } else if (a.role == R::Vod) {
                detail += QLatin1Char(' ') + i18n("Track 2 for your Twitch VOD.");
            } else if (a.role == R::Mic) {
                detail += QLatin1Char(' ') + i18n("Tracks 1 and 2 for your stream and Twitch VOD.");
            } else {
                detail += QLatin1Char(' ') + i18np("Track %2, like the source it replaces.", "Tracks %2, like the source it replaces.",
                                                   tracks.size(), tracks.join(QStringLiteral(", ")));
            }
        }
        rows << QVariantMap{{QStringLiteral("index"), int(n)},
                            {QStringLiteral("title"), title},
                            {QStringLiteral("detail"), detail},
                            {QStringLiteral("optional"), a.role == R::Conflict},
                            {QStringLiteral("enabled"), a.enabled}};
    }
    return rows;
}

void Obs::setPlanItemEnabled(int index, bool enabled)
{
    if (index < 0 || index >= m_plan.actions.size() || m_plan.actions.at(index).role != obs::Action::Role::Conflict) {
        return;
    }
    m_plan.actions[index].enabled = enabled;
    Q_EMIT planChanged();
}

void Obs::rebuildRecordings()
{
    QVariantList rows;
    if (m_active && m_running) {
        for (const auto &r : obs::obsRecordings(m_app->pw()->graph())) {
            const obs::Input *in = m_haveState ? m_state.input(r.source) : nullptr;
            QString text;
            QString kind = QStringLiteral("warning");
            switch (r.capture) {
            case obs::Capture::RostrumMic:
            case obs::Capture::RostrumStream:
                text = r.what;
                kind = QStringLiteral("ok");
                break;
            case obs::Capture::RostrumBus:
                if (in && obs::intendedRecording(*in, m_recordingTracks->intendedDevices()) &&
                    !(in->tracks & 3u)) {
                    text = i18nc("@info", "%1, assigned to separate recording tracks", r.what);
                    kind = QStringLiteral("ok");
                } else {
                    text = i18n(
                        "%1, a single bus that Rostrum Stream Mix already includes: viewers hear it twice",
                        r.what);
                }
                break;
            case obs::Capture::Mic:
                if (in && obs::assignedRecordingMic(r, *in, m_recordingTracks->assignedMicTracks())) {
                    text = i18nc("@info", "%1, assigned to separate recording tracks", r.what);
                    kind = QStringLiteral("ok");
                } else if (in && obs::classify(*in, obs::factsFrom(*m_app->pw())) == obs::Capture::RostrumMic) {
                    text = i18n("%1. It's set to Rostrum Mic, but something moved it, often Easy Effects: add OBS to its excluded apps.", r.what);
                } else {
                    text = i18n("%1 directly: your voice skips Rostrum", r.what);
                }
                break;
            case obs::Capture::Output:
                text = i18n("%1: everything you hear, off-stream buses included", r.what);
                break;
            case obs::Capture::App:
                text = i18n("%1 directly, skipping its bus", r.what);
                break;
            case obs::Capture::None:
                continue;
            }
            if (in && in->muted) {
                kind = QStringLiteral("muted");
                text = i18n("%1 (muted in OBS)", text);
            }
            rows << QVariantMap{{QStringLiteral("source"), r.source}, {QStringLiteral("text"), text}, {QStringLiteral("kind"), kind}};
        }
    }
    if (rows != m_recordings) {
        m_recordings = rows;
        Q_EMIT recordingsChanged();
    }
}

void Obs::apply()
{
    if (!canApply()) {
        return;
    }
    const QList<obs::Action> actions = m_plan.enabledActions();
    const obs::State before = m_state;

    if (m_plan.mode == obs::Mode::Live) {
        m_busy = true;
        Q_EMIT changed();
        obs::applyPlan(&m_client, actions, before, [this](const QList<obs::UndoOp> &ops, const QString &error) {
            appendUndo(ops);
            m_busy = false;
            finish(error.isEmpty(), error.isEmpty() ? i18n("OBS is set up.") : i18n("OBS stopped the setup: %1", error));
            fetchLive();
        });
        return;
    }

    if (obs::isObsRunning()) {
        finish(false, i18n("OBS just started. Rostrum will use its WebSocket server instead."));
        refresh();
        return;
    }
    const QString path = obs::sceneCollectionFile(m_install->configDir);
    QString error;
    const QJsonObject doc = readJson(path, &error);
    if (doc.isEmpty()) {
        finish(false, i18n("Couldn't read OBS's scene collection: %1", error));
        return;
    }
    const QString backup = path + QLatin1String(".rostrum-") +
                           QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd-HHmmss")) + QLatin1String(".bak");
    if (!QFile::copy(path, backup)) {
        finish(false, i18n("Couldn't back up OBS's scene collection, so nothing was changed."));
        return;
    }
    if (!writeJson(path, obs::applyToCollection(doc, actions), &error)) {
        finish(false, i18n("Couldn't save OBS's scene collection: %1", error));
        return;
    }
    qCInfo(lcObs) << "edited" << path << "backup at" << backup;
    QList<obs::UndoOp> ops;
    for (const auto &a : actions) {
        ops << obs::undoFor(a, before);
    }
    appendUndo(ops);
    finish(true, i18n("OBS is set up. Start OBS to use it."));
    loadOffline(true);
}

void Obs::undo()
{
    if (m_busy || !canUndo()) {
        return;
    }
    const obs::Undo u = loadUndo();
    if (m_stateName == QLatin1String("connected")) {
        m_busy = true;
        Q_EMIT changed();
        obs::applyUndo(&m_client, u, [this](int failed, const QString &error) {
            QFile::remove(undoFile());
            m_busy = false;
            finish(failed == 0, failed == 0 ? i18n("OBS is back the way it was.")
                                            : i18np("OBS is mostly back. One change couldn't be undone: %2",
                                                    "OBS is mostly back. %1 changes couldn't be undone: %2", failed, error));
            fetchLive();
        });
        return;
    }
    if (m_stateName != QLatin1String("closed") || obs::isObsRunning()) {
        finish(false, i18n("To undo, turn on OBS's WebSocket server or close OBS."));
        return;
    }
    const QString path = obs::sceneCollectionFile(m_install->configDir);
    QString error;
    const QJsonObject doc = readJson(path, &error);
    if (doc.isEmpty() || !writeJson(path, obs::undoInCollection(doc, u), &error)) {
        finish(false, i18n("Couldn't change OBS's scene collection: %1", error));
        return;
    }
    QFile::remove(undoFile());
    finish(true, i18n("OBS is back the way it was."));
    loadOffline(true);
}

void Obs::finish(bool ok, const QString &message)
{
    if (!ok) {
        qCWarning(lcObs) << message;
    }
    Q_EMIT m_app->toast(message);
    Q_EMIT changed();
}

obs::Undo Obs::loadUndo() const
{
    QString error;
    return obs::Undo::fromJson(readJson(undoFile(), &error));
}

void Obs::saveUndo(const obs::Undo &undo) const
{
    QDir().mkpath(paths::stateDir());
    QString error;
    if (!writeJson(undoFile(), undo.toJson(), &error)) {
        qCWarning(lcObs) << "could not save the OBS undo record:" << error;
    }
}

void Obs::appendUndo(const QList<obs::UndoOp> &ops)
{
    if (ops.isEmpty() || !m_install) {
        return;
    }
    obs::Undo u = loadUndo();
    if (u.configDir != m_install->configDir) {
        u = {};
        u.configDir = m_install->configDir;
    }
    u.ops += ops;
    saveUndo(u);
}

bool Obs::background() const
{
    return m_app->settings().obsBackground;
}

void Obs::setBackground(bool on)
{
    if (m_app->settings().obsBackground == on) {
        return;
    }
    m_app->settings().obsBackground = on;
    m_app->saveSettingsSoon();
    applyBackground();
    Q_EMIT preferencesChanged();
}

void Obs::applyBackground()
{
    if (!m_app->settings().obsBackground && !m_active && !m_busy) {
        m_client.close();
        m_stateName.clear();
    }
    updatePolling();
}

void Obs::settingsRestored()
{
    applyBackground();
    refreshWarnings();
    Q_EMIT preferencesChanged();
    Q_EMIT sceneMapChanged();
}

bool Obs::goLiveWarnings() const
{
    return m_app->settings().obsGoLiveWarnings;
}

void Obs::setGoLiveWarnings(bool on)
{
    if (m_app->settings().obsGoLiveWarnings == on) {
        return;
    }
    m_app->settings().obsGoLiveWarnings = on;
    m_app->saveSettingsSoon();
    refreshWarnings();
    Q_EMIT preferencesChanged();
}

QVariantList Obs::sceneMap() const
{
    // While OBS is closed, the page's copy of the scene collection still knows its scenes.
    const bool knowScenes = m_live.known() || m_haveState;
    QStringList scenes = m_live.known() ? m_live.scenes() : (m_haveState ? m_state.scenes : QStringList());
    const QStringList present = scenes;
    const auto &map = m_app->settings().obsSceneMap;
    for (auto it = map.cbegin(); it != map.cend(); ++it) {
        if (!scenes.contains(it.key())) {
            scenes << it.key();
        }
    }
    const QStringList ours = m_app->sceneNames();
    QVariantList rows;
    for (const QString &name : std::as_const(scenes)) {
        const QString target = map.value(name);
        rows << QVariantMap{{QStringLiteral("obsScene"), name},
                            {QStringLiteral("rostrumScene"), ours.contains(target) ? target : QString()},
                            {QStringLiteral("present"), !knowScenes || present.contains(name)},
                            {QStringLiteral("onProgram"), m_live.known() && name == m_live.programScene()}};
    }
    return rows;
}

void Obs::setSceneMapping(const QString &obsScene, const QString &rostrumScene)
{
    auto &map = m_app->settings().obsSceneMap;
    if (obsScene.isEmpty() || map.value(obsScene) == rostrumScene) {
        return;
    }
    if (rostrumScene.isEmpty()) {
        map.remove(obsScene);
    } else {
        map.insert(obsScene, rostrumScene);
    }
    m_app->saveSettingsSoon();
    Q_EMIT sceneMapChanged();
}

void Obs::followProgramScene(const QString &obsScene)
{
    const QString target = obs::mappedScene(m_app->settings().obsSceneMap, obsScene, m_app->sceneNames());
    if (target.isEmpty() || target == m_app->currentScene() || !m_app->connected()) {
        return;
    }
    // A direct switch, like a hotkey with confirmation off: the live scene's pending levels are
    // saved first when auto-save is on. Asking would block the switch while the user is live.
    if (m_app->switchScene(target)) {
        qCInfo(lcObs) << "OBS switched scenes; loaded the mapped Rostrum scene";
        Q_EMIT m_app->toast(
            i18n("OBS switched to %1, so Rostrum loaded %2.", quoted(obsScene), quoted(target)));
    }
}

obs::ReadinessInput Obs::readinessInput() const
{
    obs::ReadinessInput input;
    const auto *engine = m_app->engine();
    input.scene = engine->scene();
    input.mutes = {engine->effectiveMicMuted(), engine->effectiveStreamMuted()};
    input.panic = engine->panic();
    input.pushToTalk = engine->pushToTalk();
    input.pushToMute = engine->pushToMute();
    for (const auto &bus : input.scene.buses)
        if (engine->isSoloed(bus.id))
            input.soloed.insert(bus.id);
    input.graph = m_app->connected() ? &m_app->pw()->graph() : nullptr;
    input.selectedMic = engine->micDevice();
    input.resolvedMic = engine->resolvedSourceName();
    input.selectedPhones = engine->headphoneDevice();
    input.resolvedPhones = engine->resolvedSinkName();
    input.micMissing = engine->micMissing();
    input.phonesMissing = engine->headphonesMissing();
    input.micFallback = engine->micFallback();
    input.monoPhones = engine->monoHeadphones();
    input.filtersWanted = engine->micFilters().enabled;
    input.filtersActive = engine->micFiltersState() == engine::Engine::MicFxState::Active;
    for (const auto &app : engine->appStreams())
        input.apps.append({app.nodeId, app.busId});
    input.facts = obs::factsFrom(*m_app->pw());
    input.recordingDevices = m_recordingTracks->intendedDevices();
    input.obsState = &m_readinessState;
    input.obsFresh = m_readinessFresh && m_readinessAge.isValid() && m_readinessAge.elapsed() < 10000 &&
                     m_client.status() == obs::Client::Status::Connected;
    input.obsError = input.obsFresh ? QString() : m_readinessError;
    return input;
}

QList<obs::GoLiveProblem> Obs::currentProblems() const
{
    // The same evaluator backs the explicit check and the existing start-only banner.
    auto input = readinessInput();
    // Preserve the start-only banner's observed-capture criteria. The explicit check
    // adds configuration, scope and freshness evidence; it never restores a banner.
    input.inspectRouting = false;
    input.obsFresh = false; // Only legacy observed captures may supply capture warnings here.
    const auto recordings = m_app->connected() && m_running
                                ? std::optional(obs::obsRecordings(m_app->pw()->graph()))
                                : std::nullopt;
    input.legacyRecordings = recordings ? &*recordings : nullptr;
    return obs::readinessWarnings(obs::evaluateReadiness(input));
}

void Obs::invalidateReadiness(const QString &reason)
{
    ++m_readinessGeneration;
    m_readinessFresh = false;
    m_readinessAge.invalidate();
    m_readinessChecking = false;
    m_readinessError = reason;
    rebuildReadiness();
}

void Obs::checkReadiness()
{
    m_readinessRequested = true;
    invalidateReadiness(i18n("A fresh live OBS capture snapshot has not been verified."));
    if (!m_pageActive || m_client.status() != obs::Client::Status::Connected || m_busy)
        return;
    m_readinessChecking = true;
    m_readinessAge.start();
    m_readinessRefresh.start(5000);
    Q_EMIT readinessChanged();
    const int generation = m_readinessGeneration;
    obs::fetchReadinessState(&m_client, [this, generation](const obs::State &state, const QString &error) {
        if (generation != m_readinessGeneration || !m_pageActive ||
            m_client.status() != obs::Client::Status::Connected)
            return;
        m_readinessChecking = false;
        m_readinessState = state;
        m_readinessFresh = error.isEmpty();
        m_readinessError = error;
        rebuildReadiness();
        refreshWarnings();
    });
}

void Obs::rebuildReadiness()
{
    if (!m_readinessRequested)
        return;
    QVariantList rows;
    for (const auto &r : obs::evaluateReadiness(readinessInput())) {
        QString status, label;
        switch (r.status) {
        case obs::ReadinessStatus::Verified:
            status = QStringLiteral("verified");
            label = i18n("Verified");
            break;
        case obs::ReadinessStatus::Attention:
            status = QStringLiteral("attention");
            label = i18n("Needs attention");
            break;
        case obs::ReadinessStatus::Excluded:
            status = QStringLiteral("excluded");
            label = i18n("Intentionally excluded/idle");
            break;
        case obs::ReadinessStatus::Unknown:
            status = QStringLiteral("unknown");
            label = i18n("Not verified");
            break;
        }
        rows << QVariantMap{{QStringLiteral("id"), r.id},
                            {QStringLiteral("title"), r.title},
                            {QStringLiteral("detail"), r.detail},
                            {QStringLiteral("status"), status},
                            {QStringLiteral("label"), label}};
    }
    const auto recording = m_recordingTracks->readiness();
    if (!recording.isEmpty())
        rows << recording;
    m_readiness = rows;
    Q_EMIT readinessChanged();
}

void Obs::checkGoLive()
{
    if (!goLiveWarnings()) {
        return;
    }
    m_problems = currentProblems();
    Q_EMIT warningsChanged();
    if (!m_problems.isEmpty()) {
        qCInfo(lcObs) << "stream started with" << m_problems.size() << "problem(s)";
        if (Desktop::instance()) {
            Desktop::instance()->notifyGoLive(warningText());
        }
    }
}

void Obs::refreshWarnings()
{
    if (m_problems.isEmpty()) {
        return;
    }
    if (!m_live.streaming() || !goLiveWarnings()) {
        m_problems.clear();
        Q_EMIT warningsChanged();
        return;
    }
    // Only fixed problems go away. Muting the mic later on purpose doesn't bring a banner back.
    const QList<obs::GoLiveProblem> now = currentProblems();
    const auto removed = m_problems.removeIf([&now](obs::GoLiveProblem p) { return !now.contains(p); });
    if (removed > 0) {
        Q_EMIT warningsChanged();
    }
}

void Obs::dismissWarnings()
{
    if (!m_problems.isEmpty()) {
        m_problems.clear();
        Q_EMIT warningsChanged();
    }
}

QStringList Obs::warnings() const
{
    QStringList out;
    for (const auto p : m_problems) {
        switch (p) {
        case obs::GoLiveProblem::MicMuted:
            out << QStringLiteral("micMuted");
            break;
        case obs::GoLiveProblem::StreamMixSilent:
            out << QStringLiteral("streamSilent");
            break;
        case obs::GoLiveProblem::NoStreamMixCapture:
            out << QStringLiteral("noStreamCapture");
            break;
        case obs::GoLiveProblem::NoMicCapture:
            out << QStringLiteral("noMicCapture");
            break;
        }
    }
    return out;
}

QString Obs::warningText() const
{
    QStringList lines;
    for (const auto p : m_problems) {
        switch (p) {
        case obs::GoLiveProblem::MicMuted:
            lines << i18n("Your mic is muted or kept off the stream.");
            break;
        case obs::GoLiveProblem::StreamMixSilent:
            lines << i18n(
                "No bus reaches the stream: Master Stream or every bus sent to the stream is muted.");
            break;
        case obs::GoLiveProblem::NoStreamMixCapture:
            lines << i18n("OBS isn't recording Rostrum Stream Mix, so viewers won't hear your buses.");
            break;
        case obs::GoLiveProblem::NoMicCapture:
            lines << i18n(
                "OBS isn't recording Rostrum Mic, so viewers won't hear your voice through Rostrum.");
            break;
        }
    }
    if (lines.isEmpty()) {
        return {};
    }
    return i18nc("@info %1 is one or more sentences", "You're live, but: %1", lines.join(QLatin1Char(' ')));
}

} // namespace rostrum::app
