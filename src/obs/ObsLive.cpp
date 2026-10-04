#include "obs/ObsLive.h"

#include <QJsonArray>
#include <QPointer>

#include <memory>
#include <optional>

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
};

struct Run
{
    QPointer<Client> client;
    QList<Step> steps;
    qsizetype next = 0;
    QList<UndoOp> applied;
    int failed = 0;
    QString lastError;
    std::function<void(const std::shared_ptr<Run> &, const QString &fatal)> done;
};

void step(const std::shared_ptr<Run> &run)
{
    if (run->next >= run->steps.size() || !run->client) {
        run->done(run, run->client ? QString() : QStringLiteral("Disconnected from OBS."));
        return;
    }
    const Step s = run->steps.at(run->next++);
    run->client->request(s.type, s.data, [run, s](bool ok, const QJsonObject &, const QString &error) {
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
        step(run);
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
        std::function<void(const State &, const QString &)> done;
    };
    auto f = std::make_shared<Fetch>();
    f->done = std::move(done);
    QPointer<Client> c(client);

    auto finishOne = [f] {
        if (--f->pending == 0) {
            f->done(f->state, QString());
        }
    };

    client->request(QStringLiteral("GetSpecialInputs"), {}, [=](bool ok, const QJsonObject &special, const QString &err) {
        if (!ok || !c) {
            f->done({}, err);
            return;
        }
        if (includeUnsupported) {
            for (const char *key : {"desktop1", "desktop2", "mic1", "mic2", "mic3", "mic4"}) {
                const auto v = special.value(QLatin1String(key));
                if (!v.isString() && !v.isNull()) {
                    f->done({}, QStringLiteral("Global audio channels not verified."));
                    return;
                }
            }
        }
        for (auto it = special.begin(); it != special.end(); ++it) {
            if (it.value().isString()) {
                f->channelOf.insert(it.value().toString(), it.key());
            }
        }
        c->request(QStringLiteral("GetSceneList"), {}, [=](bool ok, const QJsonObject &scenes, const QString &err) {
            if (!ok || !c) {
                f->done({}, err);
                return;
            }
            if (!scenes.value(QLatin1String("scenes")).isArray()) {
                f->done({}, QStringLiteral("Scene list not verified."));
                return;
            }
            const QJsonArray list = scenes.value(QLatin1String("scenes")).toArray();
            // obs-websocket lists scenes bottom first.
            for (qsizetype n = list.size() - 1; n >= 0; --n) {
                f->state.scenes << list.at(n).toObject().value(QLatin1String("sceneName")).toString();
            }
            c->request(QStringLiteral("GetInputList"), {}, [=](bool ok, const QJsonObject &inputs, const QString &err) {
                if (!ok || !c) {
                    f->done({}, err);
                    return;
                }
                if (!inputs.value(QLatin1String("inputs")).isArray()) {
                    f->done({}, QStringLiteral("Input list not verified."));
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
                        f->done({}, QStringLiteral("An OBS input identity is incomplete."));
                        return;
                    }
                    if (includeUnsupported || isAudioCaptureKind(in.kind)) {
                        in.channel = f->channelOf.value(in.name);
                        f->state.inputs << in;
                    }
                }
                if (f->state.inputs.isEmpty()) {
                    f->done(f->state, QString());
                    return;
                }
                f->pending = int(f->state.inputs.size()) * 3;
                for (qsizetype n = 0; n < f->state.inputs.size(); ++n) {
                    const QJsonObject who{{QStringLiteral("inputName"), f->state.inputs.at(n).name}};
                    c->request(QStringLiteral("GetInputSettings"), who, [=](bool ok, const QJsonObject &d, const QString &) {
                        if (ok) {
                            f->state.inputs[n].settingsKnown =
                                d.value(QLatin1String("inputSettings")).isObject();
                            f->state.inputs[n].settings = d.value(QLatin1String("inputSettings")).toObject();
                        }
                        finishOne();
                    });
                    c->request(QStringLiteral("GetInputMute"), who, [=](bool ok, const QJsonObject &d, const QString &) {
                        if (ok) {
                            f->state.inputs[n].muteKnown = d.value(QLatin1String("inputMuted")).isBool();
                            f->state.inputs[n].muted = d.value(QLatin1String("inputMuted")).toBool();
                        }
                        finishOne();
                    });
                    c->request(QStringLiteral("GetInputAudioTracks"), who, [=](bool ok, const QJsonObject &d, const QString &) {
                        if (ok) {
                            const auto tracks = d.value(QLatin1String("inputAudioTracks")).toObject();
                            bool complete = true;
                            for (int track = 1; track <= 6; ++track)
                                complete = complete && tracks.value(QString::number(track)).isBool();
                            f->state.inputs[n].tracksKnown = complete;
                            f->state.inputs[n].tracks = tracksFrom(d.value(QLatin1String("inputAudioTracks")).toObject());
                        }
                        finishOne();
                    });
                }
            });
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
        }
    }
    run->done = [done = std::move(done)](const std::shared_ptr<Run> &r, const QString &fatal) { done(r->applied, fatal); };
    step(run);
}

void applyUndo(Client *client, const Undo &undo, std::function<void(int, const QString &)> done)
{
    auto run = std::make_shared<Run>();
    run->client = client;
    for (auto it = undo.ops.crbegin(); it != undo.ops.crend(); ++it) {
        QJsonObject d{{QStringLiteral("inputName"), it->input}};
        switch (it->type) {
        case UndoOp::Type::RestoreDevice:
            d.insert(QStringLiteral("inputSettings"), QJsonObject{{QStringLiteral("device_id"), it->device}});
            d.insert(QStringLiteral("overlay"), true);
            run->steps << Step{QStringLiteral("SetInputSettings"), d, std::nullopt, true};
            break;
        case UndoOp::Type::RestoreMute:
            d.insert(QStringLiteral("inputMuted"), it->muted);
            run->steps << Step{QStringLiteral("SetInputMute"), d, std::nullopt, true};
            break;
        case UndoOp::Type::RemoveInput:
        case UndoOp::Type::RemoveGlobal:
            run->steps << Step{QStringLiteral("RemoveInput"), d, std::nullopt, true};
            break;
        }
    }
    run->done = [done = std::move(done)](const std::shared_ptr<Run> &r, const QString &) { done(r->failed, r->lastError); };
    step(run);
}

} // namespace rostrum::obs
