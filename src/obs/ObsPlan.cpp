#include "obs/ObsPlan.h"

#include <QJsonArray>
#include <QSet>

namespace rostrum::obs {

QList<Action> Plan::enabledActions() const
{
    QList<Action> out;
    for (const auto &a : actions) {
        if (a.enabled) {
            out << a;
        }
    }
    return out;
}

namespace {

QString uniqueName(const State &state, const QString &base)
{
    QString name = base;
    for (int n = 2; state.input(name); ++n) {
        name = base + QLatin1Char(' ') + QString::number(n);
    }
    return name;
}

struct Target
{
    Capture want;
    Action::Role role;
    const char *device;
    const char *kind;
    const char *family;     // global channel family
    const char *globalName; // OBS's own name for that global device
    const char *createName;
    Capture replaces;       // what the input it takes over records today
    bool retargetLocal;     // whether a non-global input may be taken over
};

} // namespace

Plan makePlan(const State &state, const Facts &facts, Mode mode)
{
    Plan plan;
    plan.mode = mode;

    QHash<QString, Capture> caps;
    for (const auto &i : state.inputs) {
        caps.insert(i.name, classify(i, facts));
    }
    QSet<QString> used;

    // The tracks of the input a new one replaces, in order of preference.
    auto tracksOf = [&](std::initializer_list<Capture> kinds) -> quint32 {
        for (const Capture kind : kinds) {
            for (const auto &i : state.inputs) {
                if (!i.muted && (i.tracks & kAllTracks) && caps.value(i.name) == kind) {
                    return i.tracks & kAllTracks;
                }
            }
        }
        return 0;
    };

    auto ensure = [&](const Target &t, quint32 tracks) {
        // Already recording the Rostrum device.
        for (const auto &i : state.inputs) {
            if (caps.value(i.name) == t.want && !i.muted) {
                used.insert(i.name);
                return;
            }
        }
        for (const auto &i : state.inputs) {
            if (caps.value(i.name) == t.want) {
                plan.actions << Action{Action::Type::Unmute, t.role, t.want, i.name};
                used.insert(i.name);
                return;
            }
        }

        // Take over an input that records the hardware today: it keeps its filters, tracks and
        // scene placement. Global devices first, unmuted before muted.
        const Input *pick = nullptr;
        auto consider = [&](bool global, bool muted) {
            for (const auto &i : state.inputs) {
                if (pick || i.kind != QLatin1String(t.kind) || caps.value(i.name) != t.replaces || i.muted != muted ||
                    i.channel.isEmpty() == global) {
                    continue;
                }
                if (global && !i.channel.startsWith(QLatin1String(t.family))) {
                    continue;
                }
                pick = &i;
            }
        };
        consider(true, false);
        if (t.retargetLocal) {
            consider(false, false);
        }
        consider(true, true);
        if (t.retargetLocal) {
            consider(false, true);
        }
        if (pick) {
            Action set{Action::Type::SetDevice, t.role, t.want, pick->name};
            set.device = QString::fromLatin1(t.device);
            plan.actions << set;
            if (pick->muted) {
                plan.actions << Action{Action::Type::Unmute, t.role, t.want, pick->name};
            }
            used.insert(pick->name);
            return;
        }

        Action create;
        create.role = t.role;
        create.capture = t.want;
        create.kind = QString::fromLatin1(t.kind);
        create.device = QString::fromLatin1(t.device);
        create.tracks = tracks;
        if (mode == Mode::Offline) {
            create.channel = state.freeChannel(QString::fromLatin1(t.family));
            if (create.channel.isEmpty()) {
                return;
            }
            create.type = Action::Type::CreateGlobal;
            create.input = state.input(QString::fromLatin1(t.globalName)) ? uniqueName(state, QString::fromLatin1(t.createName))
                                                                          : QString::fromLatin1(t.globalName);
        } else {
            if (state.scenes.isEmpty()) {
                return;
            }
            create.type = Action::Type::CreateInput;
            create.input = uniqueName(state, QString::fromLatin1(t.createName));
            create.scenes = state.scenes;
        }
        plan.actions << create;
    };

    ensure({Capture::RostrumMic, Action::Role::Mic, kMicDevice, kPulseInput, "mic", "Mic/Aux", kMicInputName,
            Capture::Mic, true},
           tracksOf({Capture::Mic}));
    ensure({Capture::RostrumStream, Action::Role::Stream, kStreamDevice, kPulseOutput, "desktop", "Desktop Audio",
            kStreamInputName, Capture::Output, false},
           tracksOf({Capture::Output, Capture::App, Capture::RostrumBus}));

    for (const auto &i : state.inputs) {
        const Capture c = caps.value(i.name);
        if (i.muted || used.contains(i.name) || c == Capture::None) {
            continue;
        }
        Action mute{Action::Type::Mute, Action::Role::Conflict, c, i.name};
        plan.actions << mute;
    }
    return plan;
}

UndoOp undoFor(const Action &action, const State &before)
{
    UndoOp op;
    op.input = action.input;
    switch (action.type) {
    case Action::Type::SetDevice: {
        op.type = UndoOp::Type::RestoreDevice;
        const Input *i = before.input(action.input);
        op.device = i ? i->settings.value(QLatin1String("device_id")).toString(QStringLiteral("default"))
                      : QStringLiteral("default");
        break;
    }
    case Action::Type::Unmute:
        op.type = UndoOp::Type::RestoreMute;
        op.muted = true;
        break;
    case Action::Type::Mute:
        op.type = UndoOp::Type::RestoreMute;
        op.muted = false;
        break;
    case Action::Type::CreateInput:
        op.type = UndoOp::Type::RemoveInput;
        break;
    case Action::Type::CreateGlobal:
        op.type = UndoOp::Type::RemoveGlobal;
        op.channel = action.channel;
        break;
    }
    return op;
}

QJsonObject Undo::toJson() const
{
    QJsonArray list;
    for (const auto &op : ops) {
        QJsonObject o{{QStringLiteral("input"), op.input}};
        switch (op.type) {
        case UndoOp::Type::RestoreDevice:
            o.insert(QStringLiteral("type"), QStringLiteral("device"));
            o.insert(QStringLiteral("device"), op.device);
            break;
        case UndoOp::Type::RestoreMute:
            o.insert(QStringLiteral("type"), QStringLiteral("mute"));
            o.insert(QStringLiteral("muted"), op.muted);
            break;
        case UndoOp::Type::RemoveInput:
            o.insert(QStringLiteral("type"), QStringLiteral("remove"));
            break;
        case UndoOp::Type::RemoveGlobal:
            o.insert(QStringLiteral("type"), QStringLiteral("removeGlobal"));
            o.insert(QStringLiteral("channel"), op.channel);
            break;
        }
        list.append(o);
    }
    return {{QStringLiteral("format"), 1}, {QStringLiteral("configDir"), configDir}, {QStringLiteral("ops"), list}};
}

Undo Undo::fromJson(const QJsonObject &json)
{
    Undo undo;
    undo.configDir = json.value(QLatin1String("configDir")).toString();
    for (const auto &v : json.value(QLatin1String("ops")).toArray()) {
        const QJsonObject o = v.toObject();
        UndoOp op;
        op.input = o.value(QLatin1String("input")).toString();
        const QString type = o.value(QLatin1String("type")).toString();
        if (type == QLatin1String("device")) {
            op.type = UndoOp::Type::RestoreDevice;
            op.device = o.value(QLatin1String("device")).toString(QStringLiteral("default"));
        } else if (type == QLatin1String("mute")) {
            op.type = UndoOp::Type::RestoreMute;
            op.muted = o.value(QLatin1String("muted")).toBool();
        } else if (type == QLatin1String("remove")) {
            op.type = UndoOp::Type::RemoveInput;
        } else if (type == QLatin1String("removeGlobal")) {
            op.type = UndoOp::Type::RemoveGlobal;
            op.channel = o.value(QLatin1String("channel")).toString();
        } else {
            continue;
        }
        if (!op.input.isEmpty()) {
            undo.ops << op;
        }
    }
    return undo;
}

} // namespace rostrum::obs
