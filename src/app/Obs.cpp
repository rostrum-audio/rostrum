#include "app/Obs.h"

#include "app/AppController.h"
#include "core/Paths.h"
#include "obs/ObsCaptures.h"
#include "obs/ObsLive.h"
#include "obs/SceneCollection.h"

#include <KLocalizedString>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
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

    m_poll.setInterval(kPollMs);
    connect(&m_poll, &QTimer::timeout, this, &Obs::refresh);
    m_refetch.setSingleShot(true);
    m_refetch.setInterval(300);
    connect(&m_refetch, &QTimer::timeout, this, &Obs::fetchLive);
    m_graphDebounce.setSingleShot(true);
    m_graphDebounce.setInterval(250);
    connect(&m_graphDebounce, &QTimer::timeout, this, &Obs::rebuildRecordings);

    connect(app->pw(), &pw::PwContext::graphChanged, this, [this] {
        if (m_active) {
            m_graphDebounce.start();
        }
    });
    connect(&m_client, &obs::Client::statusChanged, this, [this] {
        switch (m_client.status()) {
        case obs::Client::Status::Connected:
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
        if (!m_busy && type != QLatin1String("InputVolumeChanged")) {
            m_refetch.start();
        }
    });
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
    if (active == m_active) {
        return;
    }
    m_active = active;
    if (active) {
        m_poll.start();
        refresh();
        rebuildRecordings();
    } else {
        m_poll.stop();
        if (!m_busy) {
            m_client.close();
            m_stateName.clear();
        }
    }
    Q_EMIT activeChanged();
}

void Obs::refresh()
{
    if (m_busy) {
        return;
    }
    const auto installs = obs::findInstalls();
    if (installs.isEmpty()) {
        m_install.reset();
        m_client.close();
        clearPlan();
        setStateName(obs::obsBinaryInstalled() ? QStringLiteral("neverRun") : QStringLiteral("notInstalled"));
        Q_EMIT changed();
        return;
    }
    m_install = installs.first();
    m_running = obs::isObsRunning();
    m_ws = obs::readWebSocketConfig(m_install->configDir);

    if (!m_running) {
        m_client.close();
        setStateName(QStringLiteral("closed"));
        loadOffline();
    } else if (!m_ws.enabled) {
        m_client.close();
        setStateName(QStringLiteral("noWebSocket"));
        clearPlan();
    } else {
        // A refused connection (OBS still starting) is retried quietly on every poll; a refused
        // password only once OBS's settings hold a different one.
        const auto status = m_client.status();
        if (status == obs::Client::Status::Disconnected ||
            (status == obs::Client::Status::AuthFailed && m_ws.password != m_rejectedPassword)) {
            setStateName(QStringLiteral("connecting"));
            m_client.open(m_ws.port, m_ws.password, obs::Client::kEventScenes | obs::Client::kEventInputs);
        } else if (status == obs::Client::Status::Failed) {
            m_client.open(m_ws.port, m_ws.password, obs::Client::kEventScenes | obs::Client::kEventInputs);
        }
    }
    Q_EMIT changed();
}

void Obs::fetchLive()
{
    if (m_client.status() != obs::Client::Status::Connected) {
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
    m_plan = obs::makePlan(state, obs::factsFrom(*m_app->pw()), mode);
    for (auto &a : m_plan.actions) {
        if (a.role == obs::Action::Role::Conflict && unticked.contains(a.input)) {
            a.enabled = false;
        }
    }
    m_havePlan = true;
    rebuildRecordings();
    Q_EMIT planChanged();
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
        QString title;
        QString detail;
        switch (a.type) {
        case T::SetDevice:
            title = mic ? i18n("Switch %1 to Rostrum Mic", quoted(a.input))
                        : i18n("Switch %1 to Rostrum Stream Mix", quoted(a.input));
            detail = i18n("The source keeps its filters, tracks and scenes.");
            break;
        case T::Unmute:
            title = i18n("Unmute %1", quoted(a.input));
            detail = mic ? i18n("It already records Rostrum Mic.") : i18n("It already records Rostrum Stream Mix.");
            break;
        case T::CreateInput:
            title = i18np("Add %2 to your scene", "Add %2 to all %1 scenes", a.scenes.size(), quoted(a.input));
            detail = mic ? i18n("It records Rostrum Mic: your voice after Rostrum's gain and mute.")
                         : i18n("It records Rostrum Stream Mix: every bus sent to Stream or Both. OBS mixes it once, however many scenes it's in.");
            break;
        case T::CreateGlobal:
            title = mic ? i18n("Set OBS's Mic/Aux device to Rostrum Mic")
                        : i18n("Set OBS's Desktop Audio device to Rostrum Stream Mix");
            detail = i18n("Global audio devices play in every scene.");
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
                detail = i18n("Another source already records the same Rostrum device, so this one would double it.");
                break;
            case obs::Capture::None:
                break;
            }
            break;
        }
        if ((a.type == T::CreateInput || a.type == T::CreateGlobal) && a.tracks) {
            QStringList tracks;
            for (int t = 0; t < 6; ++t) {
                if (a.tracks & (1u << t)) {
                    tracks << QString::number(t + 1);
                }
            }
            detail += QLatin1Char(' ') + i18np("Track %2, like the source it replaces.", "Tracks %2, like the source it replaces.",
                                               tracks.size(), tracks.join(QStringLiteral(", ")));
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
                text = i18n("%1, a single bus that Rostrum Stream Mix already includes: viewers hear it twice", r.what);
                break;
            case obs::Capture::Mic:
                if (in && obs::classify(*in, obs::factsFrom(*m_app->pw())) == obs::Capture::RostrumMic) {
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

} // namespace rostrum::app
