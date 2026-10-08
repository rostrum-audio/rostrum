#include "obs/RecordingLive.h"

#include "obs/ObsLive.h"

#include <QPointer>
#include <memory>

namespace rostrum::obs {
namespace {
struct Query
{
    QString method;
    QJsonObject data;
    std::function<bool(const QJsonObject &)> accept;
};
struct Fetch
{
    QPointer<Client> client;
    RecordingSnapshot snapshot;
    QList<Query> queries;
    QSet<QString> queued;
    std::function<void(const RecordingSnapshot &, const QString &)> done;
};

void fetchNext(const std::shared_ptr<Fetch> &f);
void queueScene(const std::shared_ptr<Fetch> &f, const QString &name, bool group)
{
    if (f->queued.contains(name))
        return;
    f->queued.insert(name);
    // The callback holds a weak reference: the queue must not own its own Fetch.
    std::weak_ptr<Fetch> weak = f;
    f->queries << Query{group ? QStringLiteral("GetGroupSceneItemList") : QStringLiteral("GetSceneItemList"),
                        {{QStringLiteral("sceneName"), name}},
                        [weak, name](const QJsonObject &data) {
                            const auto f = weak.lock();
                            if (!f || !data.value(QLatin1String("sceneItems")).isArray())
                                return false;
                            const auto items = data.value(QLatin1String("sceneItems")).toArray();
                            for (const auto &v : items) {
                                const auto item = v.toObject();
                                if (!item.value(QLatin1String("sourceName")).isString() ||
                                    !item.value(QLatin1String("sceneItemEnabled")).isBool() ||
                                    !item.value(QLatin1String("sceneItemId")).isDouble())
                                    return false;
                                if (item.value(QLatin1String("isGroup")).toBool())
                                    queueScene(f, item.value(QLatin1String("sourceName")).toString(), true);
                            }
                            f->snapshot.sceneItems.insert(name, items);
                            return true;
                        }};
}

void fetchNext(const std::shared_ptr<Fetch> &f)
{
    if (!f->client) {
        f->done({}, QStringLiteral("disconnected"));
        return;
    }
    if (f->queries.isEmpty()) {
        f->snapshot.known = true;
        f->done(f->snapshot, {});
        return;
    }
    const auto query = f->queries.takeFirst();
    f->client->request(query.method, query.data,
                       [f, query](bool ok, const QJsonObject &data, const QString &error) {
                           if (!ok || !query.accept(data)) {
                               f->done({}, error.isEmpty() ? query.method : error);
                               return;
                           }
                           fetchNext(f);
                       });
}

struct Run
{
    QPointer<Client> client;
    RecordingSnapshot scope;
    QList<RecordingChange> pending, applied;
    QMap<QString, int> createdItems;
    std::function<bool(const QList<RecordingChange> &)> journal;
    std::function<void(const QList<RecordingChange> &, const QString &)> done;
};

void runNext(const std::shared_ptr<Run> &run);
void guard(const std::shared_ptr<Run> &run, int stage)
{
    if (!run->client || run->client->status() != Client::Status::Connected) {
        run->done(run->applied, QStringLiteral("disconnected"));
        return;
    }
    if (stage == 4) {
        auto c = run->pending.takeFirst();
        if (c.method == QLatin1String("SetSceneItemEnabled") &&
            c.data.value(QLatin1String("sceneItemId")).toInt() == -1) {
            const auto key = c.data.value(QLatin1String("sceneName")).toString() + QLatin1Char('\n') +
                             c.data.value(QLatin1String("sourceName")).toString();
            if (!run->createdItems.contains(key)) {
                run->done(run->applied, QStringLiteral("unknownSceneItem"));
                return;
            }
            c.data.insert(QStringLiteral("sceneItemId"), run->createdItems.value(key));
        }
        QJsonObject data = c.data;
        if (c.method == QLatin1String("SetSceneItemEnabled"))
            data.remove(QStringLiteral("sourceName"));
        run->client->request(c.method, data,
                             [run, c](bool ok, const QJsonObject &result, const QString &error) mutable {
                                 if (!ok) {
                                     run->done(run->applied, error.isEmpty() ? c.method : error);
                                     return;
                                 }
                                 if (c.method == QLatin1String("CreateInput") ||
                                     c.method == QLatin1String("CreateSceneItem")) {
                                     const auto name = c.method == QLatin1String("CreateInput")
                                                           ? c.data.value(QLatin1String("inputName"))
                                                           : c.data.value(QLatin1String("sourceName"));
                                     const auto key = c.data.value(QLatin1String("sceneName")).toString() +
                                                      QLatin1Char('\n') + name.toString();
                                     const auto id = result.value(QLatin1String("sceneItemId"));
                                     if (!id.isDouble()) {
                                         run->applied << c;
                                         run->journal(run->applied);
                                         run->done(run->applied, QStringLiteral("unknownSceneItem"));
                                         return;
                                     }
                                     run->createdItems.insert(key, id.toInt());
                                     if (c.undoMethod == QLatin1String("RemoveSceneItem"))
                                         c.undoData.insert(QStringLiteral("sceneItemId"), id);
                                 }
                                 run->applied << c;
                                 if (!run->journal(run->applied)) {
                                     run->done(run->applied, QStringLiteral("journalFailed"));
                                     return;
                                 }
                                 runNext(run);
                             });
        return;
    }
    const QStringList methods{QStringLiteral("GetSceneCollectionList"), QStringLiteral("GetProfileList"),
                              QStringLiteral("GetStreamStatus"), QStringLiteral("GetRecordStatus")};
    run->client->request(
        methods.at(stage), {}, [run, stage](bool ok, const QJsonObject &data, const QString &error) {
            const bool matches =
                stage == 0 ? data.value(QLatin1String("currentSceneCollectionName")).toString() ==
                                 run->scope.collection
                : stage == 1
                    ? data.value(QLatin1String("currentProfileName")).toString() == run->scope.profile
                    : data.value(QLatin1String("outputActive")).isBool() &&
                          !data.value(QLatin1String("outputActive")).toBool();
            if (!ok || !matches) {
                run->done(run->applied, !ok         ? error
                                        : stage < 2 ? QStringLiteral("scopeChanged")
                                                    : QStringLiteral("activeOutput"));
                return;
            }
            guard(run, stage + 1);
        });
}

void runNext(const std::shared_ptr<Run> &run)
{
    if (run->pending.isEmpty())
        run->done(run->applied, {});
    else
        guard(run, 0);
}
} // namespace

void fetchRecordingSnapshot(Client *client,
                            std::function<void(const RecordingSnapshot &, const QString &)> done)
{
    auto f = std::make_shared<Fetch>();
    f->client = client;
    f->done = std::move(done);
    fetchState(
        client,
        [f](const State &state, const QString &error) {
            if (!error.isEmpty() || !f->client) {
                f->done({}, error.isEmpty() ? QStringLiteral("disconnected") : error);
                return;
            }
            f->snapshot.state = state;
            for (const auto &input : state.inputs)
                if ((isAudioCaptureKind(input.kind) || input.kind == QLatin1String("browser_source") ||
                     input.kind == QLatin1String("ffmpeg_source") ||
                     input.kind == QLatin1String("vlc_source")) &&
                    (!input.tracksKnown || !input.settingsKnown || !input.muteKnown)) {
                    f->done({}, QStringLiteral("unknownInput:") + input.name);
                    return;
                }
            std::weak_ptr<Fetch> weak = f;
            auto query = [&](const QString &method, const QString &key, QString RecordingSnapshot::*field) {
                f->queries << Query{method, {}, [weak, key, field](const QJsonObject &data) {
                                        const auto f = weak.lock();
                                        if (!f || !data.value(key).isString() ||
                                            data.value(key).toString().isEmpty())
                                            return false;
                                        f->snapshot.*field = data.value(key).toString();
                                        return true;
                                    }};
            };
            query(QStringLiteral("GetSceneCollectionList"), QStringLiteral("currentSceneCollectionName"),
                  &RecordingSnapshot::collection);
            query(QStringLiteral("GetProfileList"), QStringLiteral("currentProfileName"),
                  &RecordingSnapshot::profile);
            for (const auto &[method, field] :
                 {std::pair{QStringLiteral("GetStreamStatus"), &RecordingSnapshot::streaming},
                  std::pair{QStringLiteral("GetRecordStatus"), &RecordingSnapshot::recording}})
                f->queries << Query{method, {}, [weak, field](const QJsonObject &data) {
                                        const auto f = weak.lock();
                                        if (!f || !data.value(QLatin1String("outputActive")).isBool())
                                            return false;
                                        f->snapshot.*field =
                                            data.value(QLatin1String("outputActive")).toBool();
                                        return true;
                                    }};
            f->queries << Query{QStringLiteral("GetProfileParameter"),
                                {{QStringLiteral("parameterCategory"), QStringLiteral("Output")},
                                 {QStringLiteral("parameterName"), QStringLiteral("Mode")}},
                                [weak](const QJsonObject &data) {
                                    const auto f = weak.lock();
                                    if (!f || !data.value(QLatin1String("parameterValue")).isString())
                                        return false;
                                    f->snapshot.outputMode =
                                        data.value(QLatin1String("parameterValue")).toString();
                                    return true;
                                }};
            f->queries << Query{QStringLiteral("GetProfileParameter"),
                                {{QStringLiteral("parameterCategory"), QStringLiteral("AdvOut")},
                                 {QStringLiteral("parameterName"), QStringLiteral("RecType")}},
                                [weak](const QJsonObject &data) {
                                    const auto f = weak.lock();
                                    if (!f || !data.value(QLatin1String("parameterValue")).isString())
                                        return false;
                                    f->snapshot.recordingType =
                                        data.value(QLatin1String("parameterValue")).toString();
                                    return true;
                                }};
            f->queries << Query{QStringLiteral("GetProfileParameter"),
                                {{QStringLiteral("parameterCategory"), QStringLiteral("AdvOut")},
                                 {QStringLiteral("parameterName"), QStringLiteral("RecTracks")}},
                                [weak](const QJsonObject &data) {
                                    const auto f = weak.lock();
                                    bool ok = false;
                                    const uint tracks =
                                        data.value(QLatin1String("parameterValue")).toString().toUInt(&ok);
                                    if (!f || !ok || tracks > kAllTracks)
                                        return false;
                                    f->snapshot.recordingTracks = tracks;
                                    return true;
                                }};
            for (int track = 1; track <= 6; ++track)
                f->queries << Query{QStringLiteral("GetProfileParameter"),
                                    {{QStringLiteral("parameterCategory"), QStringLiteral("AdvOut")},
                                     {QStringLiteral("parameterName"),
                                      QStringLiteral("Track%1Name").arg(track)}},
                                    [weak, track](const QJsonObject &data) {
                                        const auto f = weak.lock();
                                        const auto value = data.value(QLatin1String("parameterValue"));
                                        // OBS returns null when no custom name has been set.
                                        if (!f || (!value.isString() && !value.isNull()))
                                            return false;
                                        f->snapshot.trackNames.insert(track, value.toString());
                                        return true;
                                    }};
            for (const auto &scene : state.scenes)
                queueScene(f, scene, false);
            fetchNext(f);
        },
        true);
}

void applyRecordingChanges(Client *client, const RecordingSnapshot &scope,
                           const QList<RecordingChange> &changes,
                           std::function<bool(const QList<RecordingChange> &)> journal,
                           std::function<void(const QList<RecordingChange> &, const QString &)> done)
{
    auto run = std::make_shared<Run>();
    run->client = client;
    run->scope = scope;
    run->pending = changes;
    run->journal = std::move(journal);
    run->done = std::move(done);
    runNext(run);
}
} // namespace rostrum::obs
