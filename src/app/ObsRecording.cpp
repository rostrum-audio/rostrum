#include "app/ObsRecording.h"

#include "app/AppController.h"
#include "app/Obs.h"
#include "core/Paths.h"

#include <KLocalizedString>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QPointer>
#include <QSaveFile>
#include <QSettings>

namespace rostrum::app {
namespace {
// UI-only choice; kept tracks are omitted from the persisted assignment map.
const QString kKeepObs = QStringLiteral(":keep-obs:");
constexpr int kMaxInFlightSnapshots = 2;
using obs::recordingAssignments;
using obs::recordingScopeKey;
using obs::savedRecordingAssignments;

QString problemText(const QString &problem)
{
    if (problem == QLatin1String("activeOutput"))
        return i18nc("@info", "Stop streaming and recording before changing recording tracks.");
    if (problem == QLatin1String("advancedOutput"))
        return i18nc("@info",
                     "In OBS Settings → Output, select Advanced mode to use separate recording tracks.");
    if (problem == QLatin1String("standardRecording"))
        return i18nc("@info",
                     "In OBS Settings → Output → Recording, select Standard recording to use this setup.");
    if (problem == QLatin1String("scopeChanged"))
        return i18nc("@info",
                     "The OBS profile or scene collection changed. Review the recording tracks again.");
    if (problem == QLatin1String("duplicateBus"))
        return i18nc("@info", "Choose a different bus for each recording track.");
    if (problem.startsWith(QLatin1String("unknownBus:")))
        return i18nc("@info", "Missing bus: %1. Choose another bus or leave the track unused.",
                     problem.mid(11));
    if (problem.startsWith(QLatin1String("unknownInput:")))
        return i18nc("@info", "OBS audio assignments could not be read for %1.", problem.mid(13));
    if (problem == QLatin1String("cyclicScenes"))
        return i18nc("@info",
                     "OBS scene nesting contains a loop. Recording setup cannot safely place captures.");
    if (problem == QLatin1String("journalFailed"))
        return i18nc("@info", "The recording Undo record could not be saved.");
    return i18nc("@info", "Recording configuration could not be verified: %1", problem);
}

QString trackNames(quint32 mask)
{
    QStringList names;
    for (int n = 1; n <= 6; ++n)
        if (mask & (1u << (n - 1)))
            names << QString::number(n);
    return names.isEmpty() ? i18nc("@info audio track assignment", "none") : names.join(QStringLiteral(", "));
}
} // namespace

ObsRecording::ObsRecording(AppController *app, Obs *obs, obs::Client *client,
                           SnapshotProvider snapshotProvider)
    : QObject(obs)
    , m_app(app)
    , m_obs(obs)
    , m_client(client)
    , m_snapshotProvider(snapshotProvider ? std::move(snapshotProvider) : obs::fetchRecordingSnapshot)
{
    connect(obs, &Obs::planChanged, this, &ObsRecording::refresh);
    connect(client, &obs::Client::statusChanged, this, &ObsRecording::refresh);
    connect(obs, &Obs::changed, this, [this] {
        if (m_obs->busy())
            cancelFetch();
        Q_EMIT changed();
    });
    connect(obs, &Obs::liveChanged, this, [this] {
        if (m_obs->streaming() || m_obs->recording()) {
            m_havePreview = false;
            m_preview = {};
            m_previewFingerprint.clear();
        }
        Q_EMIT changed();
    });
    // Profile track names have no dedicated WebSocket change event. Keep the
    // read-only snapshot fresh while this page is open, including its write guards.
    m_snapshotRefresh.setInterval(5000);
    connect(&m_snapshotRefresh, &QTimer::timeout, this, &ObsRecording::refresh);
    connect(obs, &Obs::activeChanged, this, [this] {
        if (m_obs->pageActive()) {
            m_snapshotRefresh.start();
            refresh();
        } else {
            m_snapshotRefresh.stop();
        }
    });
    m_refetch.setSingleShot(true);
    m_refetch.setInterval(250);
    connect(&m_refetch, &QTimer::timeout, this, &ObsRecording::refresh);
    connect(client, &obs::Client::event, this, [this](const QString &type, const QJsonObject &) {
        const QStringList relevant{QStringLiteral("InputCreated"),
                                   QStringLiteral("InputRemoved"),
                                   QStringLiteral("InputNameChanged"),
                                   QStringLiteral("InputMuteStateChanged"),
                                   QStringLiteral("InputAudioTracksChanged"),
                                   QStringLiteral("InputSettingsChanged"),
                                   QStringLiteral("CurrentSceneCollectionChanged"),
                                   QStringLiteral("CurrentProfileChanged")};
        if (relevant.contains(type) || type.startsWith(QLatin1String("Scene"))) {
            const bool wasFetching = m_fetching || m_deferredFetch != DeferredFetch::None;
            if (!m_obs->busy())
                cancelFetch();
            m_snapshot.known = false;
            m_havePreview = false;
            m_preview = {};
            m_previewFingerprint.clear();
            Q_EMIT changed();
            if ((m_obs->pageActive() || wasFetching) && !m_obs->busy())
                m_refetch.start();
        }
    });
    connect(app->engine(), &engine::Engine::micFiltersChanged, this, [this] {
        if (m_previewing || m_deferredFetch == DeferredFetch::Preview)
            cancelFetch();
        m_havePreview = false;
        m_preview = {};
        m_previewFingerprint.clear();
        Q_EMIT changed(); // read-only: the changed mic path needs a new preview
    });
    connect(app->engine(), &engine::Engine::sceneChanged, this, [this] {
        // Bus ids can disappear when Rostrum changes scene. Never rewrite OBS here.
        Q_EMIT changed();
    });
}

void ObsRecording::setSnapshotProvider(SnapshotProvider provider, const QString &configDir)
{
    m_customSnapshotProvider = bool(provider);
    m_snapshotProvider = provider ? std::move(provider) : obs::fetchRecordingSnapshot;
    if (!configDir.isEmpty())
        m_obs->m_install = obs::Install{configDir, QStringLiteral("test")};
}

QVariantList ObsRecording::choices() const
{
    QVariantList out{QVariantMap{{QStringLiteral("id"), kKeepObs},
                                 {QStringLiteral("label"), i18nc("@item:inlistbox", "Keep OBS assignments")}},
                     QVariantMap{{QStringLiteral("id"), QString()},
                                 {QStringLiteral("label"), i18nc("@item:inlistbox", "Unused")}}};
    QSet<QString> ids;
    for (const auto &bus : m_app->engine()->scene().buses) {
        ids.insert(bus.id);
        out << QVariantMap{{QStringLiteral("id"), bus.id}, {QStringLiteral("label"), bus.name}};
    }
    for (const auto &id : m_draft)
        if (!id.isEmpty() && !ids.contains(id)) {
            ids.insert(id);
            out << QVariantMap{{QStringLiteral("id"), id},
                               {QStringLiteral("label"), i18nc("@item:inlistbox", "Missing (%1)", id)}};
        }
    return out;
}

QVariantList ObsRecording::rows() const
{
    QVariantList out;
    for (int n = 3; n <= 6; ++n) {
        QStringList sources;
        for (const auto &input : m_snapshot.state.inputs)
            if (input.tracksKnown && (input.tracks & (1u << (n - 1)))) {
                const auto device = input.settings.value(QLatin1String("device_id")).toString();
                sources << (device.isEmpty() ? input.name
                                             : i18nc("@info OBS capture and device", "%1 (%2)", input.name, device));
            }
        out << QVariantMap{{QStringLiteral("track"), n},
                           {QStringLiteral("busId"), m_draft.contains(n) ? m_draft.value(n) : kKeepObs},
                           {QStringLiteral("trackName"), m_snapshot.trackNames.value(n)},
                           {QStringLiteral("current"),
                            !m_snapshot.known ? i18nc("@info", "Assignments unavailable")
                            : sources.isEmpty()
                                ? i18nc("@info", "None")
                                : sources.join(QStringLiteral(", "))}};
    }
    return out;
}

QString ObsRecording::status() const
{
    if (m_client->status() != obs::Client::Status::Connected)
        return i18nc("@info", "Start OBS and connect its WebSocket server to set up recording tracks.");
    if (!m_error.isEmpty())
        return problemText(m_error);
    if (!m_snapshot.known || !m_age.isValid() || m_age.elapsed() >= 10000)
        return i18nc("@info", "Reading the current OBS audio assignments…");
    if (m_snapshot.streaming || m_snapshot.recording || m_obs->streaming() || m_obs->recording())
        return problemText(QStringLiteral("activeOutput"));
    const auto plan = obs::recordingPlan(m_snapshot, m_app->engine()->scene(), m_draft,
                                         m_app->engine()->micFilters().enabled);
    if (!plan.problems.isEmpty())
        return problemText(plan.problems.first());
    if (m_draft.isEmpty())
        return i18nc("@info", "Existing OBS assignments will be kept. Choose a bus to replace a track.");
    if (plan.changes.isEmpty())
        return i18nc("@info", "Recording tracks match. Recording contents and sound have not been tested.");
    return i18nc("@info",
                 "Review the changes before applying. Tracks 1 and 2 keep the stream and VOD mixes.");
}

bool ObsRecording::canPreview() const
{
    return m_snapshot.known && m_age.isValid() && m_age.elapsed() < 10000 && !m_obs->busy() &&
           m_client->status() == obs::Client::Status::Connected && !m_snapshot.streaming &&
           !m_snapshot.recording && !m_obs->streaming() && !m_obs->recording();
}

bool ObsRecording::canApply() const
{
    return canPreview() && m_havePreview && m_preview.problems.isEmpty() &&
           obs::recordingChangesJson(obs::recordingPlan(m_snapshot, m_app->engine()->scene(), m_draft,
                                                        m_app->engine()->micFilters().enabled)
                                         .changes) == obs::recordingChangesJson(m_preview.changes);
}

bool ObsRecording::canUndo() const
{
    const auto record = readUndo();
    return canPreview() && record.value(QLatin1String("profile")).toString() == m_snapshot.profile &&
           !record.value(QLatin1String("undo")).toArray().isEmpty();
}

QSet<QString> ObsRecording::intendedDevices() const
{
    if (!m_obs->m_install)
        return {};
    QString collection = m_snapshot.collection;
    if (m_client->status() != obs::Client::Status::Connected || collection.isEmpty()) {
        for (const char *file : {"user.ini", "global.ini"}) {
            QSettings ini(m_obs->m_install->configDir + QLatin1Char('/') + QLatin1String(file),
                          QSettings::IniFormat);
            collection = ini.value(QStringLiteral("Basic/SceneCollection")).toString();
            if (!collection.isEmpty())
                break;
        }
    }
    const auto saved =
        m_app->settings().obsRecordingTracks.value(recordingScopeKey(m_obs->m_install->configDir, collection));
    return obs::recordingDevices(m_app->engine()->scene(), recordingAssignments(saved),
                                 m_app->engine()->micFilters().enabled);
}

quint32 ObsRecording::assignedMicTracks() const
{
    if (!m_snapshot.known || m_client->status() != obs::Client::Status::Connected)
        return 0;
    quint32 mask = 0;
    const auto saved = recordingAssignments(m_app->settings().obsRecordingTracks.value(m_scope));
    for (auto it = saved.cbegin(); it != saved.cend(); ++it) {
        const auto *bus = m_app->engine()->scene().bus(it.value());
        if (it.key() >= 3 && it.key() <= 6 && bus && bus->isInput())
            mask |= 1u << (it.key() - 1);
    }
    return mask;
}

QVariantMap ObsRecording::readiness() const
{
    if (!m_app->settings().obsRecordingTracks.contains(m_scope) ||
        m_app->settings().obsRecordingTracks.value(m_scope).isEmpty())
        return {};
    auto snapshot = m_snapshot;
    snapshot.streaming = snapshot.recording = false; // activity blocks writes, not read-only checks
    const auto plan = obs::recordingPlan(
        snapshot, m_app->engine()->scene(),
        recordingAssignments(m_app->settings().obsRecordingTracks.value(m_scope)),
        m_app->engine()->micFilters().enabled);
    const bool known = snapshot.known && m_age.isValid() && m_age.elapsed() < 10000 &&
                       m_client->status() == obs::Client::Status::Connected && m_error.isEmpty();
    const bool matches = known && plan.problems.isEmpty() && plan.changes.isEmpty();
    return {{QStringLiteral("id"), QStringLiteral("obs-recording-tracks")},
            {QStringLiteral("title"), i18nc("@title", "Recording tracks")},
            {QStringLiteral("status"), !known    ? QStringLiteral("unknown")
                                       : matches ? QStringLiteral("verified")
                                                 : QStringLiteral("attention")},
            {QStringLiteral("label"), !known    ? i18nc("@info", "Not verified")
                                      : matches ? i18nc("@info", "Configuration matches")
                                                : i18nc("@info", "Needs attention")},
            {QStringLiteral("detail"),
             !known    ? i18nc("@info", "A fresh OBS recording configuration is unavailable.")
             : matches ? i18nc("@info", "Recording track assignments and enabled output tracks match. "
                                        "Recording contents and sound have not been tested.")
                       : i18nc("@info", "OBS differs from the saved recording assignments. Review Recording "
                                        "tracks to see the changes.")}};
}

void ObsRecording::acceptSnapshot(const obs::RecordingSnapshot &snapshot)
{
    m_snapshot = snapshot;
    m_age.start();
    if (!m_obs->m_install)
        return;
    const auto scope = recordingScopeKey(m_obs->m_install->configDir, snapshot.collection);
    if (m_scope != scope) {
        m_scope = scope;
        m_draftEdited = false;
        m_havePreview = false;
    }
    if (!m_draftEdited) {
        m_draft = obs::recordingDraft(m_app->settings().obsRecordingTracks, m_obs->m_install->configDir,
                                      m_app->engine()->scene(), snapshot);
    }
    if (m_havePreview && m_previewFingerprint != obs::recordingFingerprint(snapshot))
        m_havePreview = false;
}

void ObsRecording::cancelFetch()
{
    ++m_generation;
    m_fetching = false;
    m_previewing = false;
    m_deferredFetch = DeferredFetch::None;
    m_havePreview = false;
    m_preview = {};
    m_previewFingerprint.clear();
}

void ObsRecording::startSnapshotFetch(int generation, bool forPreview)
{
    ++m_inFlight;
    QPointer<ObsRecording> self = this;
    m_snapshotProvider(
        m_client,
        [self, generation, forPreview, called = false](const auto &snapshot,
                                                       const QString &error) mutable {
            if (!self || std::exchange(called, true))
                return;
            --self->m_inFlight;
        const auto resumeDeferred = [self] {
            if (self->m_inFlight >= kMaxInFlightSnapshots || self->m_deferredFetch == DeferredFetch::None)
                return;
            const auto next = std::exchange(self->m_deferredFetch, DeferredFetch::None);
            if (self->m_client->status() != obs::Client::Status::Connected || self->m_obs->busy()) {
                self->cancelFetch();
                return;
            }
            self->startSnapshotFetch(self->m_generation, next == DeferredFetch::Preview);
        };
        if (generation != self->m_generation ||
            self->m_client->status() != obs::Client::Status::Connected || self->m_obs->busy()) {
            resumeDeferred();
            return;
        }
        self->m_fetching = false;
        self->m_previewing = false;
        self->m_error = error;
        if (error.isEmpty()) {
            self->acceptSnapshot(snapshot);
            if (forPreview) {
                self->m_preview = obs::recordingPlan(snapshot, self->m_app->engine()->scene(), self->m_draft,
                                                     self->m_app->engine()->micFilters().enabled);
                if (self->m_obs->streaming() || self->m_obs->recording())
                    self->m_preview.problems.prepend(QStringLiteral("activeOutput"));
                self->m_havePreview = self->m_preview.problems.isEmpty();
                self->m_previewFingerprint = obs::recordingFingerprint(snapshot);
                if (!self->m_preview.problems.isEmpty()) {
                    self->m_preview.changes.clear();
                    self->m_error = self->m_preview.problems.first();
                }
            }
        } else {
            self->m_snapshot.known = false;
            self->m_havePreview = false;
            self->m_preview = {};
            self->m_previewFingerprint.clear();
        }
        Q_EMIT self->changed();
        self->m_obs->rebuildReadiness();
        // Refresh conflict classification only; reconnect never applies a recording plan.
        self->m_fetching = true;
        if (self->m_obs->m_haveState)
            self->m_obs->setPlan(self->m_obs->m_state, self->m_obs->m_plan.mode);
        self->m_fetching = false;
        if (forPreview && self->m_havePreview)
            Q_EMIT self->previewReady();
    });
}

void ObsRecording::settingsRestored()
{
    ++m_settingsGeneration;
    cancelFetch();
    m_draftEdited = false;
    m_error.clear();
    if (m_obs->m_install && !m_snapshot.collection.isEmpty()) {
        const auto scope = recordingScopeKey(m_obs->m_install->configDir, m_snapshot.collection);
        if (m_snapshot.known || m_app->settings().obsRecordingTracks.contains(scope)) {
            m_scope = scope;
            m_draft = obs::recordingDraft(m_app->settings().obsRecordingTracks, m_obs->m_install->configDir,
                                          m_app->engine()->scene(), m_snapshot);
        } else {
            m_scope.clear();
            m_draft.clear();
        }
    } else {
        m_scope.clear();
        m_draft.clear();
    }
    Q_EMIT changed();
    m_obs->rebuildReadiness();
    m_fetching = true;
    if (m_obs->m_haveState)
        m_obs->setPlan(m_obs->m_state, m_obs->m_plan.mode);
    else
        m_obs->rebuildRecordings();
    m_fetching = false;
    refresh();
}

void ObsRecording::refresh()
{
    if (m_client->status() != obs::Client::Status::Connected) {
        cancelFetch();
        m_snapshot.known = false;
        m_error.clear();
        Q_EMIT changed();
        m_obs->rebuildReadiness();
        return;
    }
    Q_EMIT changed();
    if (m_fetching || m_obs->busy() || (m_obs->m_offscreen && !m_customSnapshotProvider) || !m_obs->m_install)
        return;
    m_fetching = true;
    m_previewing = false;
    const int generation = ++m_generation;
    if (m_inFlight >= kMaxInFlightSnapshots) {
        if (m_deferredFetch == DeferredFetch::None)
            m_deferredFetch = DeferredFetch::Refresh;
        return;
    }
    startSnapshotFetch(generation, false);
}

void ObsRecording::choose(int track, const QString &busId)
{
    if (track < 3 || track > 6 || m_obs->busy())
        return;
    if (m_previewing || m_deferredFetch == DeferredFetch::Preview)
        cancelFetch();
    if (busId == kKeepObs)
        m_draft.remove(track);
    else
        m_draft[track] = busId;
    m_draftEdited = true;
    m_error.clear();
    m_havePreview = false;
    m_preview = {};
    m_previewFingerprint.clear();
    Q_EMIT changed();
}

void ObsRecording::preview()
{
    if (!canPreview() || (m_fetching && m_previewing))
        return;
    m_fetching = true;
    m_previewing = true;
    m_havePreview = false;
    m_preview = {};
    m_previewFingerprint.clear();
    m_error.clear();
    const int generation = ++m_generation;
    if (m_inFlight >= kMaxInFlightSnapshots) {
        m_deferredFetch = DeferredFetch::Preview;
        return;
    }
    m_deferredFetch = DeferredFetch::None;
    startSnapshotFetch(generation, true);
}

QStringList ObsRecording::previewItems() const
{
    QStringList out;
    for (int track = 3; track <= 6; ++track) {
        const auto label = m_snapshot.trackNames.value(track);
        const auto title = label.isEmpty() ? i18nc("@label", "Track %1", track)
                                           : i18nc("@label", "Track %1 (%2)", track, label);
        if (!m_draft.contains(track)) {
            out << i18nc("@info", "%1: keep OBS assignments and recording output selection.", title);
            continue;
        }
        QStringList removed;
        for (const auto &input : m_snapshot.state.inputs)
            if (input.tracksKnown && (input.tracks & (1u << (track - 1))) &&
                !(m_preview.inputTracks.value(input.name, input.tracks) & (1u << (track - 1))))
                removed << input.name;
        const auto id = m_draft.value(track);
        if (id.isEmpty())
            out << i18nc("@info", "%1: clear source assignments and disable this recording track.", title);
        else if (const auto *bus = m_app->engine()->scene().bus(id)) {
            out << i18nc("@info", "%1: record the %2 bus exclusively.", title, bus->name);
            if (bus->isInput())
                out << i18nc("@info", "Mic recording source: %1.",
                             QString::fromLatin1(m_app->engine()->micFilters().enabled
                                                     ? obs::kFilteredMicDevice : obs::kMicDevice));
        }
        if (!removed.isEmpty())
            out << i18nc("@info", "Remove from %1: %2.", title, removed.join(QStringLiteral(", ")));
    }
    for (const auto &c : m_preview.changes) {
        const auto name = c.data.value(QLatin1String("inputName")).toString();
        if (c.method == QLatin1String("CreateInput"))
            out << i18nc("@info", "Add %1 capturing %2.", name,
                         c.data.value(QLatin1String("inputSettings"))
                             .toObject()
                             .value(QLatin1String("device_id"))
                             .toString());
        else if (c.method == QLatin1String("CreateSceneItem"))
            out << i18nc("@info", "Add %1 to scene %2.", c.data.value(QLatin1String("sourceName")).toString(),
                         c.data.value(QLatin1String("sceneName")).toString());
        else if (c.method == QLatin1String("SetInputAudioTracks")) {
            const quint32 next = m_preview.inputTracks.value(name);
            const auto *old = m_snapshot.state.input(name);
            out << i18nc("@info", "%1: tracks %2 → %3.", name, trackNames(old ? old->tracks : 0),
                         trackNames(next));
        } else if (c.method == QLatin1String("SetInputMute"))
            out << i18nc("@info", "Unmute %1 in OBS. Its Rostrum bus mute still applies.", name);
        else if (c.method == QLatin1String("SetProfileParameter"))
            out << i18nc("@info", "Recording output: tracks %1 → %2.", trackNames(m_snapshot.recordingTracks),
                         trackNames(m_preview.recordingTracks));
    }
    if (out.isEmpty())
        out << i18nc("@info", "No OBS changes are needed. Save these assignments for this scene collection.");
    return out;
}

void ObsRecording::setBusy(bool busy)
{
    m_obs->m_busy = busy;
    if (busy)
        cancelFetch();
    Q_EMIT m_obs->changed();
    Q_EMIT changed();
}

void ObsRecording::saveAssignments(const obs::RecordingAssignments &a, bool present)
{
    m_draftEdited = false;
    if (present && m_app->settings().obsRecordingTracks.contains(m_scope) &&
        m_app->settings().obsRecordingTracks.value(m_scope) == savedRecordingAssignments(a))
        return;
    if (present)
        m_app->settings().obsRecordingTracks[m_scope] = savedRecordingAssignments(a);
    else
        m_app->settings().obsRecordingTracks.remove(m_scope);
    m_app->saveSettingsNow();
}

QString ObsRecording::undoPath() const
{
    return paths::stateDir() + QLatin1String("/obs-recording/") + m_scope + QLatin1String(".json");
}

QJsonObject ObsRecording::readUndo() const
{
    QFile file(undoPath());
    if (!file.open(QIODevice::ReadOnly))
        return {};
    const auto record = QJsonDocument::fromJson(file.readAll()).object();
    if (!m_obs->m_install ||
        record.value(QLatin1String("configDir")).toString() != m_obs->m_install->configDir ||
        record.value(QLatin1String("collection")).toString() != m_snapshot.collection)
        return {};
    return record;
}

bool ObsRecording::writeUndo(const QJsonObject &record) const
{
    if (!QDir().mkpath(paths::stateDir() + QLatin1String("/obs-recording")))
        return false;
    QSaveFile file(undoPath());
    if (!file.open(QIODevice::WriteOnly))
        return false;
    file.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner);
    const auto data = QJsonDocument(record).toJson();
    return file.write(data) == data.size() && file.commit();
}

void ObsRecording::notify(const QString &text)
{
    Q_EMIT m_app->toast(text);
}

void ObsRecording::apply()
{
    if (!canApply())
        return;
    const auto expected = m_previewFingerprint;
    const auto draft = m_draft;
    const auto expectedScope = m_scope;
    const auto expectedChanges = obs::recordingChangesJson(m_preview.changes);
    const int settingsGen = m_settingsGeneration;
    setBusy(true);
    QPointer<ObsRecording> self = this;
    m_snapshotProvider(m_client, [self, settingsGen, expected, draft, expectedScope, expectedChanges,
                                  called = false](const auto &fresh, const QString &error) mutable {
        if (!self || std::exchange(called, true))
            return;
        if (!error.isEmpty() || obs::recordingFingerprint(fresh) != expected ||
            expectedScope != self->m_scope) {
            self->setBusy(false);
            self->m_havePreview = false;
            self->notify(
                i18nc("@info",
                      "OBS changed since the preview. Nothing was changed; review recording tracks again."));
            self->refresh();
            return;
        }
        auto plan = obs::recordingPlan(fresh, self->m_app->engine()->scene(), draft,
                                       self->m_app->engine()->micFilters().enabled);
        if (self->m_obs->streaming() || self->m_obs->recording())
            plan.problems.prepend(QStringLiteral("activeOutput"));
        if (!plan.problems.isEmpty()) {
            self->setBusy(false);
            self->m_havePreview = false;
            self->notify(problemText(plan.problems.first()));
            self->refresh();
            return;
        }
        if (settingsGen != self->m_settingsGeneration || self->m_draft != draft ||
            obs::recordingChangesJson(plan.changes) != expectedChanges) {
            self->setBusy(false);
            self->m_havePreview = false;
            self->notify(i18nc(
                "@info",
                "Rostrum changed since the preview. Nothing was changed; review recording tracks again."));
            self->refresh();
            return;
        }
        if (plan.changes.isEmpty()) {
            self->saveAssignments(draft);
            self->setBusy(false);
            self->m_havePreview = false;
            self->notify(i18nc("@info", "Recording tracks already match. No OBS changes were made."));
            self->refresh();
            return;
        }
        const auto old = self->m_app->settings().obsRecordingTracks.value(self->m_scope);
        QJsonObject previous;
        for (auto it = old.cbegin(); it != old.cend(); ++it)
            previous.insert(it.key(), it.value());
        auto record = std::make_shared<QJsonObject>(QJsonObject{
            {QStringLiteral("configDir"), self->m_obs->m_install->configDir},
            {QStringLiteral("collection"), fresh.collection},
            {QStringLiteral("profile"), fresh.profile},
            {QStringLiteral("previousAssignments"), previous},
            {QStringLiteral("previousPresent"),
             self->m_app->settings().obsRecordingTracks.contains(self->m_scope)},
            {QStringLiteral("planned"), obs::recordingChangesJson(plan.changes)},
            {QStringLiteral("undo"), QJsonArray()},
            {QStringLiteral("createdAt"), QDateTime::currentDateTimeUtc().toString(Qt::ISODate)}});
        if (!self->writeUndo(*record)) {
            self->setBusy(false);
            self->notify(i18nc("@info", "The recording backup could not be saved. Nothing was changed."));
            return;
        }
        obs::applyRecordingChanges(
            self->m_client, fresh, plan.changes,
            [self, record](const auto &applied) {
                if (!self)
                    return false;
                obs::RecordingPlan completed;
                completed.changes = applied;
                record->insert(QStringLiteral("undo"),
                               obs::recordingChangesJson(obs::recordingUndo(completed).changes));
                return self->writeUndo(*record);
            },
            [self, settingsGen, record, draft, fresh](const auto &applied, const QString &error) {
                if (!self)
                    return;
                obs::RecordingPlan completed;
                completed.changes = applied;
                record->insert(QStringLiteral("undo"),
                               obs::recordingChangesJson(obs::recordingUndo(completed).changes));
                self->writeUndo(*record);
                self->m_havePreview = false;
                if (!error.isEmpty()) {
                    self->runUndo(*record, fresh, true, settingsGen);
                    return;
                }
                if (settingsGen == self->m_settingsGeneration)
                    self->saveAssignments(draft);
                self->setBusy(false);
                self->notify(
                    i18nc("@info",
                          "Recording tracks configured. Recording contents and sound have not been tested."));
                self->m_obs->fetchLive();
                self->refresh();
            });
    });
}

void ObsRecording::runUndo(QJsonObject record, const obs::RecordingSnapshot &scope, bool rollback,
                           int settingsGen)
{
    const auto changes = obs::recordingChangesFromJson(record.value(QLatin1String("undo")).toArray());
    auto remaining = std::make_shared<QJsonObject>(record);
    QPointer<ObsRecording> self = this;
    obs::applyRecordingChanges(
        m_client, scope, changes,
        [self, remaining, changes](const auto &applied) {
            if (!self)
                return false;
            remaining->insert(QStringLiteral("undo"), obs::recordingChangesJson(changes.mid(applied.size())));
            return self->writeUndo(*remaining);
        },
        [self, settingsGen, remaining, rollback](const auto &, const QString &error) {
            if (!self)
                return;
            if (error.isEmpty()) {
                QMap<QString, QString> saved;
                const auto previous = remaining->value(QLatin1String("previousAssignments")).toObject();
                for (auto it = previous.begin(); it != previous.end(); ++it)
                    saved.insert(it.key(), it.value().toString());
                if (settingsGen == self->m_settingsGeneration) {
                    self->saveAssignments(recordingAssignments(saved),
                                          remaining->value(QLatin1String("previousPresent")).toBool());
                    self->m_draftEdited = false;
                    if (remaining->value(QLatin1String("previousPresent")).toBool()) {
                        self->m_draft = recordingAssignments(saved);
                    } else {
                        // Rebuild unsaved suggestions from the restored live snapshot,
                        // rather than the stale pre-Undo assignments.
                        self->m_scope.clear();
                        self->m_draft.clear();
                    }
                }
                QFile::remove(self->undoPath());
            }
            self->setBusy(false);
            self->m_havePreview = false;
            self->notify(
                error.isEmpty()
                    ? rollback ? i18nc("@info",
                                       "Recording setup failed. The previous OBS assignments were restored.")
                               : i18nc("@info", "The previous recording assignments were restored.")
                    : i18nc("@info",
                            "Some recording changes remain. Stop outputs and use Undo Recording Changes. %1",
                            problemText(error)));
            self->m_obs->fetchLive();
            self->refresh();
        });
}

void ObsRecording::undo()
{
    if (!canUndo())
        return;
    const auto record = readUndo();
    const int settingsGen = m_settingsGeneration;
    setBusy(true);
    QPointer<ObsRecording> self = this;
    m_snapshotProvider(m_client, [self, settingsGen, record,
                                  called = false](const auto &fresh, const QString &error) mutable {
        if (!self || std::exchange(called, true))
            return;
        if (!error.isEmpty() ||
            fresh.collection != record.value(QLatin1String("collection")).toString() ||
            fresh.profile != record.value(QLatin1String("profile")).toString() || fresh.streaming ||
            fresh.recording || self->m_obs->streaming() || self->m_obs->recording()) {
            self->setBusy(false);
            self->notify(i18nc("@info", "Recording Undo requires the original OBS profile and scene collection "
                                        "with outputs stopped."));
            self->refresh();
            return;
        }
        self->runUndo(record, fresh, false, settingsGen);
    });
}
} // namespace rostrum::app
