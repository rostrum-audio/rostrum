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
#include <QSaveFile>
#include <QSettings>

namespace rostrum::app {
namespace {
// UI-only choice; kept tracks are omitted from the persisted assignment map.
const QString kKeepObs = QStringLiteral(":keep-obs:");
QString scopeKey(const QString &configDir, const QString &collection)
{
    return QString::fromLatin1(
        QCryptographicHash::hash((configDir + QChar(0) + collection).toUtf8(), QCryptographicHash::Sha256)
            .toHex());
}

obs::RecordingAssignments assignments(const QMap<QString, QString> &saved)
{
    obs::RecordingAssignments out;
    for (auto it = saved.cbegin(); it != saved.cend(); ++it)
        out.insert(it.key().toInt(), it.value());
    return out;
}

QMap<QString, QString> savedAssignments(const obs::RecordingAssignments &draft)
{
    QMap<QString, QString> out;
    for (int track = 3; track <= 6; ++track)
        if (draft.contains(track))
            out.insert(QString::number(track), draft.value(track));
    return out;
}

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

ObsRecording::ObsRecording(AppController *app, Obs *obs, obs::Client *client)
    : QObject(obs), m_app(app), m_obs(obs), m_client(client)
{
    connect(obs, &Obs::planChanged, this, &ObsRecording::refresh);
    connect(client, &obs::Client::statusChanged, this, &ObsRecording::refresh);
    connect(obs, &Obs::liveChanged, this, &ObsRecording::changed);
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
            m_snapshot.known = false;
            m_havePreview = false;
            Q_EMIT changed();
            if (m_obs->pageActive() && !m_obs->busy())
                m_refetch.start();
        }
    });
    connect(app->engine(), &engine::Engine::micFiltersChanged, this, [this] {
        m_havePreview = false;
        Q_EMIT changed(); // read-only: the changed mic path needs a new preview
    });
    connect(app->engine(), &engine::Engine::sceneChanged, this, [this] {
        // Bus ids can disappear when Rostrum changes scene. Never rewrite OBS here.
        Q_EMIT changed();
    });
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
        m_app->settings().obsRecordingTracks.value(scopeKey(m_obs->m_install->configDir, collection));
    return obs::recordingDevices(m_app->engine()->scene(), assignments(saved),
                                 m_app->engine()->micFilters().enabled);
}

quint32 ObsRecording::assignedMicTracks() const
{
    if (!m_snapshot.known || m_client->status() != obs::Client::Status::Connected)
        return 0;
    quint32 mask = 0;
    const auto saved = assignments(m_app->settings().obsRecordingTracks.value(m_scope));
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
    const auto plan = obs::recordingPlan(snapshot, m_app->engine()->scene(),
                                         assignments(m_app->settings().obsRecordingTracks.value(m_scope)),
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
    const auto scope = scopeKey(m_obs->m_install->configDir, snapshot.collection);
    if (m_scope != scope) {
        m_scope = scope;
        const auto &saved = m_app->settings().obsRecordingTracks;
        m_draft = saved.contains(scope) ? assignments(saved.value(scope))
                                        : obs::recordingSuggestion(m_app->engine()->scene(), snapshot);
        m_draftEdited = false;
        m_havePreview = false;
    } else if (!m_draftEdited && !m_app->settings().obsRecordingTracks.contains(scope)) {
        m_draft = obs::recordingSuggestion(m_app->engine()->scene(), snapshot);
    }
    if (m_havePreview && m_previewFingerprint != obs::recordingFingerprint(snapshot))
        m_havePreview = false;
}

void ObsRecording::refresh()
{
    Q_EMIT changed();
    if (m_client->status() != obs::Client::Status::Connected) {
        ++m_generation;
        m_fetching = false;
        m_snapshot.known = false;
        m_havePreview = false;
        return;
    }
    if (m_fetching || m_obs->busy() || m_obs->m_offscreen || !m_obs->m_install)
        return;
    m_fetching = true;
    const int generation = ++m_generation;
    obs::fetchRecordingSnapshot(m_client, [this, generation](const auto &snapshot, const QString &error) {
        if (generation != m_generation)
            return;
        m_fetching = false;
        m_error = error;
        if (error.isEmpty())
            acceptSnapshot(snapshot);
        else
            m_snapshot.known = false;
        Q_EMIT changed();
        m_obs->rebuildReadiness();
        // Refresh conflict classification only; reconnect never applies a recording plan.
        m_fetching = true;
        if (m_obs->m_haveState)
            m_obs->setPlan(m_obs->m_state, m_obs->m_plan.mode);
        m_fetching = false;
    });
}

void ObsRecording::choose(int track, const QString &busId)
{
    if (track < 3 || track > 6 || m_obs->busy())
        return;
    if (busId == kKeepObs)
        m_draft.remove(track);
    else
        m_draft[track] = busId;
    m_draftEdited = true;
    m_error.clear();
    m_havePreview = false;
    Q_EMIT changed();
}

void ObsRecording::preview()
{
    if (!canPreview() || m_fetching)
        return;
    m_fetching = true;
    obs::fetchRecordingSnapshot(m_client, [this](const auto &snapshot, const QString &error) {
        m_fetching = false;
        m_error = error;
        if (error.isEmpty()) {
            acceptSnapshot(snapshot);
            m_preview = obs::recordingPlan(snapshot, m_app->engine()->scene(), m_draft,
                                           m_app->engine()->micFilters().enabled);
            m_havePreview = m_preview.problems.isEmpty();
            m_previewFingerprint = obs::recordingFingerprint(snapshot);
            if (!m_preview.problems.isEmpty())
                m_error = m_preview.problems.first();
        }
        Q_EMIT changed();
        if (m_havePreview)
            Q_EMIT previewReady();
    });
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
    if (busy) {
        ++m_generation;
        m_fetching = false;
    }
    Q_EMIT m_obs->changed();
    Q_EMIT changed();
}

void ObsRecording::saveAssignments(const obs::RecordingAssignments &a, bool present)
{
    if (present && m_app->settings().obsRecordingTracks.contains(m_scope) &&
        m_app->settings().obsRecordingTracks.value(m_scope) == savedAssignments(a))
        return;
    if (present)
        m_app->settings().obsRecordingTracks[m_scope] = savedAssignments(a);
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
    setBusy(true);
    obs::fetchRecordingSnapshot(m_client, [this, expected, draft, expectedScope,
                                           expectedChanges](const auto &fresh, const QString &error) {
        if (!error.isEmpty() || obs::recordingFingerprint(fresh) != expected || expectedScope != m_scope) {
            setBusy(false);
            m_havePreview = false;
            notify(
                i18nc("@info",
                      "OBS changed since the preview. Nothing was changed; review recording tracks again."));
            refresh();
            return;
        }
        const auto plan =
            obs::recordingPlan(fresh, m_app->engine()->scene(), draft, m_app->engine()->micFilters().enabled);
        if (obs::recordingChangesJson(plan.changes) != expectedChanges) {
            setBusy(false);
            m_havePreview = false;
            notify(i18nc(
                "@info",
                "Rostrum changed since the preview. Nothing was changed; review recording tracks again."));
            refresh();
            return;
        }
        if (!plan.problems.isEmpty()) {
            setBusy(false);
            notify(problemText(plan.problems.first()));
            refresh();
            return;
        }
        if (plan.changes.isEmpty()) {
            saveAssignments(draft);
            setBusy(false);
            m_havePreview = false;
            notify(i18nc("@info", "Recording tracks already match. No OBS changes were made."));
            refresh();
            return;
        }
        const auto old = m_app->settings().obsRecordingTracks.value(m_scope);
        QJsonObject previous;
        for (auto it = old.cbegin(); it != old.cend(); ++it)
            previous.insert(it.key(), it.value());
        auto record = std::make_shared<QJsonObject>(QJsonObject{
            {QStringLiteral("configDir"), m_obs->m_install->configDir},
            {QStringLiteral("collection"), fresh.collection},
            {QStringLiteral("profile"), fresh.profile},
            {QStringLiteral("previousAssignments"), previous},
            {QStringLiteral("previousPresent"), m_app->settings().obsRecordingTracks.contains(m_scope)},
            {QStringLiteral("planned"), obs::recordingChangesJson(plan.changes)},
            {QStringLiteral("undo"), QJsonArray()},
            {QStringLiteral("createdAt"), QDateTime::currentDateTimeUtc().toString(Qt::ISODate)}});
        if (!writeUndo(*record)) {
            setBusy(false);
            notify(i18nc("@info", "The recording backup could not be saved. Nothing was changed."));
            return;
        }
        obs::applyRecordingChanges(
            m_client, fresh, plan.changes,
            [this, record](const auto &applied) {
                obs::RecordingPlan completed;
                completed.changes = applied;
                record->insert(QStringLiteral("undo"),
                               obs::recordingChangesJson(obs::recordingUndo(completed).changes));
                return writeUndo(*record);
            },
            [this, record, draft, fresh](const auto &applied, const QString &error) {
                obs::RecordingPlan completed;
                completed.changes = applied;
                record->insert(QStringLiteral("undo"),
                               obs::recordingChangesJson(obs::recordingUndo(completed).changes));
                writeUndo(*record);
                m_havePreview = false;
                if (!error.isEmpty()) {
                    runUndo(*record, fresh, true);
                    return;
                }
                saveAssignments(draft);
                setBusy(false);
                notify(
                    i18nc("@info",
                          "Recording tracks configured. Recording contents and sound have not been tested."));
                m_obs->fetchLive();
                refresh();
            });
    });
}

void ObsRecording::runUndo(QJsonObject record, const obs::RecordingSnapshot &scope, bool rollback)
{
    const auto changes = obs::recordingChangesFromJson(record.value(QLatin1String("undo")).toArray());
    auto remaining = std::make_shared<QJsonObject>(record);
    obs::applyRecordingChanges(
        m_client, scope, changes,
        [this, remaining, changes](const auto &applied) {
            remaining->insert(QStringLiteral("undo"), obs::recordingChangesJson(changes.mid(applied.size())));
            return writeUndo(*remaining);
        },
        [this, remaining, rollback](const auto &, const QString &error) {
            if (error.isEmpty()) {
                QMap<QString, QString> saved;
                const auto previous = remaining->value(QLatin1String("previousAssignments")).toObject();
                for (auto it = previous.begin(); it != previous.end(); ++it)
                    saved.insert(it.key(), it.value().toString());
                saveAssignments(assignments(saved),
                                remaining->value(QLatin1String("previousPresent")).toBool());
                QFile::remove(undoPath());
                m_draftEdited = false;
                if (remaining->value(QLatin1String("previousPresent")).toBool()) {
                    m_draft = assignments(saved);
                } else {
                    // Rebuild unsaved suggestions from the restored live snapshot,
                    // rather than the stale pre-Undo assignments.
                    m_scope.clear();
                    m_draft.clear();
                }
            }
            setBusy(false);
            m_havePreview = false;
            notify(
                error.isEmpty()
                    ? rollback ? i18nc("@info",
                                       "Recording setup failed. The previous OBS assignments were restored.")
                               : i18nc("@info", "The previous recording assignments were restored.")
                    : i18nc("@info",
                            "Some recording changes remain. Stop outputs and use Undo Recording Changes. %1",
                            problemText(error)));
            m_obs->fetchLive();
            refresh();
        });
}

void ObsRecording::undo()
{
    if (!canUndo())
        return;
    const auto record = readUndo();
    setBusy(true);
    obs::fetchRecordingSnapshot(m_client, [this, record](const auto &fresh, const QString &error) {
        if (!error.isEmpty() || fresh.collection != record.value(QLatin1String("collection")).toString() ||
            fresh.profile != record.value(QLatin1String("profile")).toString() || fresh.streaming ||
            fresh.recording) {
            setBusy(false);
            notify(i18nc("@info", "Recording Undo requires the original OBS profile and scene collection "
                                  "with outputs stopped."));
            refresh();
            return;
        }
        runUndo(record, fresh, false);
    });
}
} // namespace rostrum::app
