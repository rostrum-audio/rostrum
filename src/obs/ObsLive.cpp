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

void fetchState(Client *client, std::function<void(const State &, const QString &)> done)
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
                for (const auto &v : inputs.value(QLatin1String("inputs")).toArray()) {
                    const QJsonObject o = v.toObject();
                    Input in;
                    in.name = o.value(QLatin1String("inputName")).toString();
                    in.kind = o.value(QLatin1String("unversionedInputKind")).toString();
                    if (in.kind.isEmpty()) {
                        in.kind = o.value(QLatin1String("inputKind")).toString();
                    }
                    if (isAudioCaptureKind(in.kind)) {
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
                            f->state.inputs[n].settings = d.value(QLatin1String("inputSettings")).toObject();
                        }
                        finishOne();
                    });
                    c->request(QStringLiteral("GetInputMute"), who, [=](bool ok, const QJsonObject &d, const QString &) {
                        if (ok) {
                            f->state.inputs[n].muted = d.value(QLatin1String("inputMuted")).toBool();
                        }
                        finishOne();
                    });
                    c->request(QStringLiteral("GetInputAudioTracks"), who, [=](bool ok, const QJsonObject &d, const QString &) {
                        if (ok) {
                            f->state.inputs[n].tracks = tracksFrom(d.value(QLatin1String("inputAudioTracks")).toObject());
                        }
                        finishOne();
                    });
                }
            });
        });
    });
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
