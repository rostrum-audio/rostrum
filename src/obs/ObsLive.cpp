#include "obs/ObsLive.h"

#include <QJsonArray>
#include <QPointer>
#include <algorithm>
#include <iterator>
#include <memory>
#include <optional>
#include <utility>

namespace rostrum::obs {

namespace {

QJsonObject tracksJson(quint32 tracks)
{
    QJsonObject o;
    for (int n = 0; n < 6; ++n) {
        o.insert(QString::number(n + 1), bool(tracks & (1u << n)));
    }
    return o;
}

quint32 tracksFrom(const QJsonObject &o)
{
    quint32 tracks = 0;
    for (int n = 0; n < 6; ++n) {
        if (o.value(QString::number(n + 1)).toBool()) {
            tracks |= 1u << n;
        }
    }
    return tracks;
}

struct Step
{
    QString type;
    QJsonObject data;
    std::optional<UndoOp> undo;
    bool tolerant = false; // a refusal is counted, not fatal
    qsizetype op = -1;     // undo: the op this step puts back
};

struct Run
{
    QPointer<Client> client;
    QList<Step> steps;
    qsizetype next = 0;
    QList<UndoOp> applied;
    QList<qsizetype> settled; // undo: ops whose every step OBS answered
    int failed = 0;
    QString lastError;
    std::function<void(const std::shared_ptr<Run> &, const QString &fatal)> done;
};

bool connected(const QPointer<Client> &client)
{
    return client && client->status() == Client::Status::Connected;
}

void step(const std::shared_ptr<Run> &run)
{
    if (run->next >= run->steps.size() || !connected(run->client)) {
        run->done(run, connected(run->client) ? QString() : QStringLiteral("Disconnected from OBS."));
        return;
    }
    const qsizetype index = run->next++;
    const Step s = run->steps.at(index);
    run->client->request(s.type, s.data, [run, s, index](bool ok, const QJsonObject &, const QString &error) {
        // OBS going away is not a refusal: what is left was not tried.
        if (!ok && !connected(run->client)) {
            run->done(run, QStringLiteral("Disconnected from OBS."));
            return;
        }
        if (!ok) {
            run->failed++;
            run->lastError = error;
            if (!s.tolerant) {
                run->done(run, error.isEmpty() ? s.type : error);
                return;
            }
        } else if (s.undo) {
            run->applied << *s.undo;
        }
        if (s.op >= 0 && (index + 1 >= run->steps.size() || run->steps.at(index + 1).op != s.op)) {
            run->settled << s.op;
        }
        step(run);
    });
}

// Reports an empty error once OBS is still on this scene collection and (if given) profile.
void checkScope(const QPointer<Client> &client, const QString &collection, const QString &profile,
                std::function<void(const QString &error)> done)
{
    const auto failed = [client](const QString &error) {
        return connected(client) ? (error.isEmpty() ? QStringLiteral("OBS did not answer.") : error)
                                 : QStringLiteral("Disconnected from OBS.");
    };
    if (!connected(client)) {
        done(QStringLiteral("Disconnected from OBS."));
        return;
    }
    const QString switched = QStringLiteral("OBS switched to another scene collection or profile.");
    client->request(QStringLiteral("GetSceneCollectionList"), {},
                    [=](bool ok, const QJsonObject &d, const QString &error) {
                        if (!ok || !connected(client)) {
                            done(failed(error));
                            return;
                        }
                        if (collection.isEmpty() ||
                            d.value(QLatin1String("currentSceneCollectionName")).toString() != collection) {
                            done(switched);
                            return;
                        }
                        if (profile.isEmpty()) {
                            done({});
                            return;
                        }
                        client->request(QStringLiteral("GetProfileList"), {},
                                        [=](bool ok, const QJsonObject &d, const QString &error) {
                                            if (!ok || !connected(client)) {
                                                done(failed(error));
                                                return;
                                            }
                                            done(d.value(QLatin1String("currentProfileName")).toString() ==
                                                         profile
                                                     ? QString()
                                                     : switched);
                                        });
                    });
}

} // namespace

void fetchState(Client *client, std::function<void(const State &, const QString &)> done,
                bool includeUnsupported)
{
    struct Fetch
    {
        State state;
        QHash<QString, QString> channelOf; // input name -> channel
        int pending = 0;
        bool finished = false;
        std::function<void(const State &, const QString &)> done;

        // Reports once. A failure always carries a message, so callers never mistake it for
        // a complete read.
        void finish(const State &result, const QString &error)
        {
            if (!std::exchange(finished, true)) {
                done(result, error);
            }
        }
        void fail(const QString &error, const QString &fallback)
        {
            finish({}, error.isEmpty() ? fallback : error);
        }
    };
    auto f = std::make_shared<Fetch>();
    f->done = std::move(done);
    QPointer<Client> c(client);

    // True when the fetch is over: already reported, or OBS went away. A reply that failed
    // while OBS is still connected (refused or timed out) leaves its field unknown instead.
    auto stopped = [f, c](const QString &err) {
        if (f->finished) {
            return true;
        }
        if (!c || c->status() != Client::Status::Connected) {
            f->fail(err, QStringLiteral("Disconnected from OBS."));
            return true;
        }
        return false;
    };
    auto finishOne = [f] {
        if (--f->pending == 0) {
            f->finish(f->state, QString());
        }
    };

    auto readAll = [=] {
        c->request(
            QStringLiteral("GetSpecialInputs"), {},
            [=](bool ok, const QJsonObject &special, const QString &err) {
                if (stopped(err)) {
                    return;
                }
                if (!ok) {
                    f->fail(err, QStringLiteral("OBS did not list its global audio devices."));
                    return;
                }
                if (includeUnsupported) {
                    for (const char *key : {"desktop1", "desktop2", "mic1", "mic2", "mic3", "mic4"}) {
                        const auto v = special.value(QLatin1String(key));
                        if (!v.isString() && !v.isNull()) {
                            f->finish({}, QStringLiteral("Global audio channels not verified."));
                            return;
                        }
                    }
                }
                for (auto it = special.begin(); it != special.end(); ++it) {
                    if (it.value().isString()) {
                        f->channelOf.insert(it.value().toString(), it.key());
                    }
                }
                c->request(
                    QStringLiteral("GetSceneList"), {},
                    [=](bool ok, const QJsonObject &scenes, const QString &err) {
                        if (stopped(err)) {
                            return;
                        }
                        if (!ok) {
                            f->fail(err, QStringLiteral("OBS did not list its scenes."));
                            return;
                        }
                        if (!scenes.value(QLatin1String("scenes")).isArray()) {
                            f->finish({}, QStringLiteral("Scene list not verified."));
                            return;
                        }
                        const QJsonArray list = scenes.value(QLatin1String("scenes")).toArray();
                        // obs-websocket lists scenes bottom first.
                        for (qsizetype n = list.size() - 1; n >= 0; --n) {
                            f->state.scenes
                                << list.at(n).toObject().value(QLatin1String("sceneName")).toString();
                        }
                        c->request(
                            QStringLiteral("GetInputList"), {},
                            [=](bool ok, const QJsonObject &inputs, const QString &err) {
                                if (stopped(err)) {
                                    return;
                                }
                                if (!ok) {
                                    f->fail(err, QStringLiteral("OBS did not list its inputs."));
                                    return;
                                }
                                if (!inputs.value(QLatin1String("inputs")).isArray()) {
                                    f->finish({}, QStringLiteral("Input list not verified."));
                                    return;
                                }
                                for (const auto &v : inputs.value(QLatin1String("inputs")).toArray()) {
                                    const QJsonObject o = v.toObject();
                                    Input in;
                                    in.name = o.value(QLatin1String("inputName")).toString();
                                    in.kind = o.value(QLatin1String("unversionedInputKind")).toString();
                                    if (in.kind.isEmpty()) {
                                        in.kind = o.value(QLatin1String("inputKind")).toString();
                                    }
                                    if (includeUnsupported && (in.name.isEmpty() || in.kind.isEmpty())) {
                                        f->finish({}, QStringLiteral("An OBS input identity is incomplete."));
                                        return;
                                    }
                                    if (includeUnsupported || isAudioCaptureKind(in.kind)) {
                                        in.channel = f->channelOf.value(in.name);
                                        f->state.inputs << in;
                                    }
                                }
                                struct VodKey
                                {
                                    const char *category, *name;
                                    QString VodTrackSettings::*value;
                                };
                                static const VodKey vodKeys[] = {
                                    {"Output", "Mode", &VodTrackSettings::outputMode},
                                    {"AdvOut", "VodTrackEnabled", &VodTrackSettings::advEnabled},
                                    {"AdvOut", "VodTrackIndex", &VodTrackSettings::advTrack},
                                    {"AdvOut", "TrackIndex", &VodTrackSettings::advStreamTrack},
                                    {"SimpleOutput", "VodTrackEnabled", &VodTrackSettings::simpleEnabled},
                                    {"SimpleOutput", "UseAdvanced", &VodTrackSettings::simpleAdvanced},
                                };
                                const int vodCount = int(std::size(vodKeys));
                                f->pending = int(f->state.inputs.size()) * 3 + 1 + vodCount + 2;
                                // Read again last: the state belongs to them only if OBS did not switch
                                // meanwhile.
                                for (const bool collection : {true, false}) {
                                    c->request(
                                        collection ? QStringLiteral("GetSceneCollectionList")
                                                   : QStringLiteral("GetProfileList"),
                                        {}, [=](bool ok, const QJsonObject &d, const QString &err) {
                                            if (stopped(err)) {
                                                return;
                                            }
                                            const QString name =
                                                d.value(collection
                                                            ? QLatin1String("currentSceneCollectionName")
                                                            : QLatin1String("currentProfileName"))
                                                    .toString();
                                            const bool same =
                                                ok && !name.isEmpty() &&
                                                name == (collection ? f->state.collection : f->state.profile);
                                            (collection ? f->state.collectionKnown : f->state.profileKnown) =
                                                same;
                                            finishOne();
                                        });
                                }
                                c->request(
                                    QStringLiteral("GetStreamServiceSettings"), {},
                                    [=](bool ok, const QJsonObject &d, const QString &err) {
                                        if (stopped(err)) {
                                            return;
                                        }
                                        if (ok &&
                                            d.value(QLatin1String("streamServiceSettings")).isObject()) {
                                            const QJsonObject settings =
                                                d.value(QLatin1String("streamServiceSettings")).toObject();
                                            const QJsonValue service =
                                                settings.value(QLatin1String("service"));
                                            // Custom RTMP servers have no service name.
                                            f->state.streamServiceKnown =
                                                service.isString() || service.isUndefined();
                                            f->state.streamService = service.toString();
                                        }
                                        finishOne();
                                    });
                                // The settings OBS reads for a Twitch VOD track. Known only once every one
                                // is.
                                auto vodRead = std::make_shared<int>(0);
                                for (const VodKey &k : vodKeys) {
                                    c->request(
                                        QStringLiteral("GetProfileParameter"),
                                        {{QStringLiteral("parameterCategory"), QLatin1String(k.category)},
                                         {QStringLiteral("parameterName"), QLatin1String(k.name)}},
                                        [=](bool ok, const QJsonObject &d, const QString &err) {
                                            if (stopped(err)) {
                                                return;
                                            }
                                            // obs-websocket answers OBS's default for a parameter the profile
                                            // never set, and null when there is none either.
                                            QJsonValue value = d.value(QLatin1String("parameterValue"));
                                            if (value.isNull() &&
                                                d.value(QLatin1String("defaultParameterValue")).isString()) {
                                                value = d.value(QLatin1String("defaultParameterValue"));
                                            }
                                            if (ok && (value.isString() || value.isNull())) {
                                                f->state.vod.*(k.value) = value.toString();
                                                f->state.vodKnown = ++*vodRead == vodCount;
                                            }
                                            finishOne();
                                        });
                                }
                                for (qsizetype n = 0; n < f->state.inputs.size(); ++n) {
                                    const QJsonObject who{
                                        {QStringLiteral("inputName"), f->state.inputs.at(n).name}};
                                    c->request(QStringLiteral("GetInputSettings"), who,
                                               [=](bool ok, const QJsonObject &d, const QString &err) {
                                                   if (stopped(err)) {
                                                       return;
                                                   }
                                                   if (ok) {
                                                       f->state.inputs[n].settingsKnown =
                                                           d.value(QLatin1String("inputSettings")).isObject();
                                                       f->state.inputs[n].settings =
                                                           d.value(QLatin1String("inputSettings")).toObject();
                                                   }
                                                   finishOne();
                                               });
                                    c->request(QStringLiteral("GetInputMute"), who,
                                               [=](bool ok, const QJsonObject &d, const QString &err) {
                                                   if (stopped(err)) {
                                                       return;
                                                   }
                                                   if (ok) {
                                                       f->state.inputs[n].muteKnown =
                                                           d.value(QLatin1String("inputMuted")).isBool();
                                                       f->state.inputs[n].muted =
                                                           d.value(QLatin1String("inputMuted")).toBool();
                                                   }
                                                   finishOne();
                                               });
                                    c->request(
                                        QStringLiteral("GetInputAudioTracks"), who,
                                        [=](bool ok, const QJsonObject &d, const QString &err) {
                                            if (stopped(err)) {
                                                return;
                                            }
                                            if (ok) {
                                                const auto tracks =
                                                    d.value(QLatin1String("inputAudioTracks")).toObject();
                                                bool complete = true;
                                                for (int track = 1; track <= 6; ++track)
                                                    complete = complete &&
                                                               tracks.value(QString::number(track)).isBool();
                                                f->state.inputs[n].tracksKnown = complete;
                                                f->state.inputs[n].tracks = tracksFrom(
                                                    d.value(QLatin1String("inputAudioTracks")).toObject());
                                            }
                                            finishOne();
                                        });
                                }
                            });
                    });
            });
    };
    // The scene collection and profile come first, so a switch while reading shows.
    client->request(
        QStringLiteral("GetSceneCollectionList"), {}, [=](bool ok, const QJsonObject &d, const QString &err) {
            if (stopped(err)) {
                return;
            }
            if (ok) {
                f->state.collection = d.value(QLatin1String("currentSceneCollectionName")).toString();
            }
            c->request(QStringLiteral("GetProfileList"), {},
                       [=](bool ok, const QJsonObject &d, const QString &err) {
                           if (stopped(err)) {
                               return;
                           }
                           if (ok) {
                               f->state.profile = d.value(QLatin1String("currentProfileName")).toString();
                           }
                           readAll();
                       });
        });
}

void fetchReadinessState(Client *client, std::function<void(const State &, const QString &)> done)
{
    QPointer<Client> c(client);
    fetchState(
        client,
        [c, done](const State &state, const QString &error) {
            if (!c || !error.isEmpty()) {
                done({}, error.isEmpty() ? QStringLiteral("OBS disconnected.") : error);
                return;
            }
            auto result = std::make_shared<State>(state);
            c->request(
                QStringLiteral("GetCurrentProgramScene"), {},
                [c, result, done](bool ok, const QJsonObject &d, const QString &error) {
                    result->programScene = d.value(QLatin1String("currentProgramSceneName")).toString();
                    if (result->programScene.isEmpty())
                        result->programScene = d.value(QLatin1String("sceneName")).toString();
                    if (!c || !ok || result->programScene.isEmpty()) {
                        done(*result,
                             error.isEmpty() ? QStringLiteral("Program scene not verified.") : error);
                        return;
                    }
                    c->request(
                        QStringLiteral("GetSceneItemList"),
                        {{QStringLiteral("sceneName"), result->programScene}},
                        [c, result, done](bool ok, const QJsonObject &d, const QString &error) {
                            if (!c || !ok || !d.value(QLatin1String("sceneItems")).isArray()) {
                                done(*result,
                                     error.isEmpty() ? QStringLiteral("Scene items not verified.") : error);
                                return;
                            }
                            result->scopeKnown = true;
                            for (const auto &v : d.value(QLatin1String("sceneItems")).toArray()) {
                                const auto item = v.toObject();
                                if (item.value(QLatin1String("isGroup")).toBool() ||
                                    item.value(QLatin1String("sourceType")).toString() ==
                                        QLatin1String("OBS_SOURCE_TYPE_SCENE") ||
                                    !item.value(QLatin1String("sceneItemEnabled")).isBool() ||
                                    !item.value(QLatin1String("sourceName")).isString())
                                    result->scopeKnown = false;
                                if (item.value(QLatin1String("sceneItemEnabled")).toBool())
                                    result->programInputs.insert(
                                        item.value(QLatin1String("sourceName")).toString());
                            }
                            if (result->inputs.isEmpty()) {
                                done(*result, {});
                                return;
                            }
                            auto pending = std::make_shared<int>(int(result->inputs.size()));
                            for (qsizetype n = 0; n < result->inputs.size(); ++n) {
                                c->request(QStringLiteral("GetInputVolume"),
                                           {{QStringLiteral("inputName"), result->inputs[n].name}},
                                           [result, pending, done, n](bool ok, const QJsonObject &d,
                                                                      const QString &) {
                                               result->inputs[n].gainKnown =
                                                   ok && d.value(QLatin1String("inputVolumeMul")).isDouble();
                                               result->inputs[n].gain =
                                                   d.value(QLatin1String("inputVolumeMul")).toDouble();
                                               if (--*pending == 0)
                                                   done(*result, {});
                                           });
                            }
                        });
                });
        },
        true);
}

void applyPlan(Client *client, const QList<Action> &actions, const State &before,
               std::function<void(const QList<UndoOp> &, const QString &)> done)
{
    auto run = std::make_shared<Run>();
    run->client = client;
    for (const auto &a : actions) {
        const QJsonObject who{{QStringLiteral("inputName"), a.input}};
        switch (a.type) {
        case Action::Type::SetDevice: {
            QJsonObject d = who;
            d.insert(QStringLiteral("inputSettings"), QJsonObject{{QStringLiteral("device_id"), a.device}});
            d.insert(QStringLiteral("overlay"), true);
            run->steps << Step{QStringLiteral("SetInputSettings"), d, undoFor(a, before)};
            if (a.tracks) {
                QJsonObject td = who;
                td.insert(QStringLiteral("inputAudioTracks"), tracksJson(a.tracks));
                run->steps << Step{QStringLiteral("SetInputAudioTracks"), td, std::nullopt, true};
            }
            break;
        }
        case Action::Type::Unmute:
        case Action::Type::Mute: {
            QJsonObject d = who;
            d.insert(QStringLiteral("inputMuted"), a.type == Action::Type::Mute);
            run->steps << Step{QStringLiteral("SetInputMute"), d, undoFor(a, before)};
            break;
        }
        case Action::Type::CreateInput: {
            if (a.scenes.isEmpty()) {
                break;
            }
            run->steps << Step{QStringLiteral("CreateInput"),
                               {{QStringLiteral("sceneName"), a.scenes.first()},
                                {QStringLiteral("inputName"), a.input},
                                {QStringLiteral("inputKind"), a.kind},
                                {QStringLiteral("inputSettings"), QJsonObject{{QStringLiteral("device_id"), a.device}}},
                                {QStringLiteral("sceneItemEnabled"), true}},
                               undoFor(a, before)};
            for (qsizetype n = 1; n < a.scenes.size(); ++n) {
                run->steps << Step{QStringLiteral("CreateSceneItem"),
                                   {{QStringLiteral("sceneName"), a.scenes.at(n)},
                                    {QStringLiteral("sourceName"), a.input},
                                    {QStringLiteral("sceneItemEnabled"), true}},
                                   std::nullopt,
                                   true};
            }
            if (a.tracks) {
                QJsonObject d = who;
                d.insert(QStringLiteral("inputAudioTracks"), tracksJson(a.tracks));
                run->steps << Step{QStringLiteral("SetInputAudioTracks"), d, std::nullopt, true};
            }
            break;
        }
        case Action::Type::CreateGlobal:
            break;
        case Action::Type::SetTwitchVodTrack: {
            // One step per value, each undoing only itself, so a refusal part way still undoes
            // what was written.
            const UndoOp all = undoFor(a, before);
            for (qsizetype n = 0; n < a.profile.size(); ++n) {
                const ProfileParameter &p = a.profile.at(n);
                UndoOp one = all;
                one.profile = {all.profile.at(n)};
                run->steps << Step{QStringLiteral("SetProfileParameter"),
                                   {{QStringLiteral("parameterCategory"), p.category},
                                    {QStringLiteral("parameterName"), p.name},
                                    {QStringLiteral("parameterValue"), p.value}},
                                   one};
            }
            break;
        }
        }
    }
    run->done = [done = std::move(done)](const std::shared_ptr<Run> &r, const QString &fatal) { done(r->applied, fatal); };
    // The plan was made from what OBS showed for one collection and profile.
    checkScope(client, before.collection, before.profile, [run](const QString &error) {
        if (!error.isEmpty()) {
            run->done(run, error);
            return;
        }
        step(run);
    });
}

void applyUndo(Client *client, const Undo &undo, const UndoScope &scope,
               std::function<void(const UndoResult &)> done)
{
    auto run = std::make_shared<Run>();
    run->client = client;
    for (auto it = undo.ops.crbegin(); it != undo.ops.crend(); ++it) {
        const qsizetype first = run->steps.size();
        QJsonObject d{{QStringLiteral("inputName"), it->input}};
        switch (it->type) {
        case UndoOp::Type::RestoreDevice:
            d.insert(QStringLiteral("inputSettings"), QJsonObject{{QStringLiteral("device_id"), it->device}});
            d.insert(QStringLiteral("overlay"), true);
            run->steps << Step{QStringLiteral("SetInputSettings"), d, std::nullopt, true};
            if (it->tracks) {
                QJsonObject td{{QStringLiteral("inputName"), it->input}};
                td.insert(QStringLiteral("inputAudioTracks"), tracksJson(it->tracks));
                run->steps << Step{QStringLiteral("SetInputAudioTracks"), td, std::nullopt, true};
            }
            break;
        case UndoOp::Type::RestoreMute:
            d.insert(QStringLiteral("inputMuted"), it->muted);
            run->steps << Step{QStringLiteral("SetInputMute"), d, std::nullopt, true};
            break;
        case UndoOp::Type::RemoveInput:
        case UndoOp::Type::RemoveGlobal:
            run->steps << Step{QStringLiteral("RemoveInput"), d, std::nullopt, true};
            break;
        case UndoOp::Type::RestoreProfile:
            for (const ProfileParameter &p : it->profile) {
                run->steps << Step{QStringLiteral("SetProfileParameter"),
                                   {{QStringLiteral("parameterCategory"), p.category},
                                    {QStringLiteral("parameterName"), p.name},
                                    {QStringLiteral("parameterValue"), p.value}},
                                   std::nullopt,
                                   true};
            }
            break;
        }
        const qsizetype op = undo.ops.size() - 1 - (it - undo.ops.crbegin());
        for (qsizetype n = first; n < run->steps.size(); ++n) {
            run->steps[n].op = op;
        }
        if (first == run->steps.size()) {
            run->settled << op; // nothing to send
        }
    }
    run->done = [done = std::move(done)](const std::shared_ptr<Run> &r, const QString &stopped) {
        UndoResult result;
        result.settled = r->settled;
        std::sort(result.settled.begin(), result.settled.end());
        result.failed = r->failed;
        result.lastError = r->lastError;
        result.stopped = stopped;
        done(result);
    };
    checkScope(client, scope.collection, scope.profile, [run](const QString &error) {
        if (!error.isEmpty()) {
            run->done(run, error);
            return;
        }
        step(run);
    });
}

} // namespace rostrum::obs
