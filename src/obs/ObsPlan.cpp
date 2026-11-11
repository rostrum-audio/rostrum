#include "obs/ObsPlan.h"

#include "obs/RecordingTracks.h"

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

// The value a profile key had, for undo: what OBS reported, else OBS's default.
QString profileValueBefore(const VodTrackSettings &before, const ProfileParameter &p)
{
    const QString key = p.key();
    QString value;
    if (key == QLatin1String("AdvOut/VodTrackEnabled")) {
        value = before.advEnabled;
    } else if (key == QLatin1String("AdvOut/VodTrackIndex")) {
        value = before.advTrack;
    } else if (key == QLatin1String("SimpleOutput/VodTrackEnabled")) {
        value = before.simpleEnabled;
    }
    if (value.isEmpty()) {
        value = p.name == QLatin1String("VodTrackIndex") ? QStringLiteral("2") : QStringLiteral("false");
    }
    return value;
}

// Undo only puts back the keys Set Up OBS writes, whatever an edited journal says.
bool restorableProfileKey(const QString &key)
{
    return key == QLatin1String("AdvOut/VodTrackEnabled") || key == QLatin1String("AdvOut/VodTrackIndex") ||
           key == QLatin1String("SimpleOutput/VodTrackEnabled");
}

} // namespace

Plan makePlan(const State &state, const Facts &facts, Mode mode, const QSet<QString> &recordingDevices)
{
    Plan plan;
    plan.mode = mode;

    // An unread device reads as "default", so it would look like the hardware mic or output and
    // be retargeted, duplicated or muted; an unread mute reads as unmuted, so a Mute's undo would
    // unmute it. Plan nothing until everything was read.
    for (const auto &i : state.inputs) {
        if (isAudioCaptureKind(i.kind) && !(i.settingsKnown && i.muteKnown && i.tracksKnown)) {
            plan.unreadInputs << i.name;
        }
    }
    const bool twitch = state.streamService.compare(QLatin1String("Twitch"), Qt::CaseInsensitive) == 0;
    plan.streamSettingsUnread =
        mode == Mode::Live && (!state.streamServiceKnown || (twitch && !state.vodKnown));
    if (plan.incomplete()) {
        return plan;
    }

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
        auto recordingOnlyMic = [&](const Input &input) {
            return t.want == Capture::RostrumMic && intendedRecording(input, recordingDevices);
        };
        // Already recording the Rostrum device.
        for (const auto &i : state.inputs) {
            if (caps.value(i.name) == t.want && !i.muted && !recordingOnlyMic(i)) {
                used.insert(i.name);
                return;
            }
        }
        for (const auto &i : state.inputs) {
            if (caps.value(i.name) == t.want && !recordingOnlyMic(i)) {
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
                    i.channel.isEmpty() == global || used.contains(i.name)) {
                    continue;
                }
                if (global && (!t.family || !i.channel.startsWith(QLatin1String(t.family)))) {
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
            set.tracks = tracks;
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
        if (mode == Mode::Offline && t.family) {
            const int count = QString::fromLatin1(t.family) == QLatin1String("desktop") ? 2 : 4;
            QString ch;
            for (int n = 1; n <= count; ++n) {
                const QString candidate = QString::fromLatin1(t.family) + QString::number(n);
                if (state.channel(candidate)) {
                    continue;
                }
                bool takenInPlan = false;
                for (const auto &act : plan.actions) {
                    if (act.type == Action::Type::CreateGlobal && act.channel == candidate) {
                        takenInPlan = true;
                        break;
                    }
                }
                if (!takenInPlan) {
                    ch = candidate;
                    break;
                }
            }
            create.channel = ch;
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
           (1u << 0) | (1u << 1));
    ensure({Capture::RostrumStream, Action::Role::Stream, kStreamDevice, kPulseOutput, "desktop", "Desktop Audio",
            kStreamInputName, Capture::Output, false},
           1u << 0);
    ensure({Capture::RostrumVod, Action::Role::Vod, kVodDevice, kPulseOutput, nullptr, nullptr,
            kVodInputName, Capture::None, false},
           1u << 1);

    for (const auto &i : state.inputs) {
        const Capture c = caps.value(i.name);
        if (i.muted || used.contains(i.name) || c == Capture::None ||
            intendedRecording(i, recordingDevices)) {
            continue;
        }
        Action mute{Action::Type::Mute, Action::Role::Conflict, c, i.name};
        plan.actions << mute;
    }

    // Only what OBS reads for the current output mode. Streaming track 2, or simple output
    // without custom encoder settings, is left to the user: readiness says what to change.
    const QList<ProfileParameter> vodWrites =
        mode == Mode::Live && twitch ? twitchVodWrites(state.vod) : QList<ProfileParameter>{};
    if (!vodWrites.isEmpty()) {
        Action setTwitchVod;
        setTwitchVod.type = Action::Type::SetTwitchVodTrack;
        setTwitchVod.role = Action::Role::Vod;
        setTwitchVod.input = QStringLiteral("Twitch VOD Track");
        setTwitchVod.profile = vodWrites;
        plan.actions << setTwitchVod;
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
        op.tracks = i ? i->tracks : 0;
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
    case Action::Type::SetTwitchVodTrack:
        op.type = UndoOp::Type::RestoreProfile;
        for (const ProfileParameter &p : action.profile) {
            op.profile << ProfileParameter{p.category, p.name, profileValueBefore(before.vod, p)};
        }
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
            if (op.tracks) {
                o.insert(QStringLiteral("tracks"), int(op.tracks));
            }
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
        case UndoOp::Type::RestoreProfile: {
            o.insert(QStringLiteral("type"), QStringLiteral("profile"));
            QJsonArray values;
            for (const ProfileParameter &p : op.profile) {
                values.append(QJsonObject{{QStringLiteral("category"), p.category},
                                          {QStringLiteral("name"), p.name},
                                          {QStringLiteral("value"), p.value}});
            }
            o.insert(QStringLiteral("parameters"), values);
            break;
        }
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
            op.tracks = quint32(o.value(QLatin1String("tracks")).toInt(0));
        } else if (type == QLatin1String("mute")) {
            op.type = UndoOp::Type::RestoreMute;
            op.muted = o.value(QLatin1String("muted")).toBool();
        } else if (type == QLatin1String("remove")) {
            op.type = UndoOp::Type::RemoveInput;
        } else if (type == QLatin1String("removeGlobal")) {
            op.type = UndoOp::Type::RemoveGlobal;
            op.channel = o.value(QLatin1String("channel")).toString();
        } else if (type == QLatin1String("profile")) {
            op.type = UndoOp::Type::RestoreProfile;
            for (const auto &pv : o.value(QLatin1String("parameters")).toArray()) {
                const QJsonObject p = pv.toObject();
                const ProfileParameter param{p.value(QLatin1String("category")).toString(),
                                             p.value(QLatin1String("name")).toString(),
                                             p.value(QLatin1String("value")).toString()};
                if (restorableProfileKey(param.key()) && p.value(QLatin1String("value")).isString()) {
                    op.profile << param;
                }
            }
            if (op.profile.isEmpty()) {
                continue;
            }
        } else if (type == QLatin1String("twitchVodTrack")) {
            // Rostrum 0.2.0 set AdvOut/VodTrackIndex to 2 (and keys OBS does not read). Put back
            // the index it read; it read an unset one as 0, and then OBS was already on 2.
            const int track = o.value(QLatin1String("track")).toInt(0);
            if (track <= 0) {
                continue;
            }
            op.type = UndoOp::Type::RestoreProfile;
            op.profile << ProfileParameter{QStringLiteral("AdvOut"), QStringLiteral("VodTrackIndex"),
                                           QString::number(track)};
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
