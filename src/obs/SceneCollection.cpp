#include "obs/SceneCollection.h"

#include <QJsonArray>
#include <QUuid>

#include <functional>

namespace rostrum::obs {

namespace {

const QStringList kChannels{QStringLiteral("desktop1"), QStringLiteral("desktop2"), QStringLiteral("mic1"),
                            QStringLiteral("mic2"),     QStringLiteral("mic3"),     QStringLiteral("mic4")};

Input inputFrom(const QJsonObject &o, const QString &channel)
{
    Input in;
    in.name = o.value(QLatin1String("name")).toString();
    in.kind = o.value(QLatin1String("id")).toString();
    in.settings = o.value(QLatin1String("settings")).toObject();
    in.muted = o.value(QLatin1String("muted")).toBool();
    in.tracks = quint32(o.value(QLatin1String("mixers")).toInt(0));
    in.channel = channel;
    return in;
}

// Runs `edit` on the global device or source called `name`. Returns false if there is none.
bool editSource(QJsonObject &doc, const QString &name, const std::function<void(QJsonObject &)> &edit)
{
    for (const QString &ch : kChannels) {
        const QString key = globalKey(ch);
        QJsonObject o = doc.value(key).toObject();
        if (!o.isEmpty() && o.value(QLatin1String("name")).toString() == name) {
            edit(o);
            doc.insert(key, o);
            return true;
        }
    }
    QJsonArray sources = doc.value(QLatin1String("sources")).toArray();
    for (qsizetype n = 0; n < sources.size(); ++n) {
        QJsonObject o = sources.at(n).toObject();
        if (o.value(QLatin1String("name")).toString() == name) {
            edit(o);
            sources.replace(n, o);
            doc.insert(QStringLiteral("sources"), sources);
            return true;
        }
    }
    return false;
}

void setDevice(QJsonObject &doc, const QString &name, const QString &device, quint32 tracks = 0)
{
    editSource(doc, name, [&](QJsonObject &o) {
        QJsonObject settings = o.value(QLatin1String("settings")).toObject();
        settings.insert(QStringLiteral("device_id"), device);
        o.insert(QStringLiteral("settings"), settings);
        if (tracks > 0) {
            o.insert(QStringLiteral("mixers"), int(tracks));
        }
    });
}

void setMuted(QJsonObject &doc, const QString &name, bool muted)
{
    editSource(doc, name, [&](QJsonObject &o) { o.insert(QStringLiteral("muted"), muted); });
}

} // namespace

QString globalKey(const QString &channel)
{
    if (channel.startsWith(QLatin1String("desktop"))) {
        return QLatin1String("DesktopAudioDevice") + channel.mid(7);
    }
    if (channel.startsWith(QLatin1String("mic"))) {
        return QLatin1String("AuxAudioDevice") + channel.mid(3);
    }
    return {};
}

State stateFromCollection(const QJsonObject &doc)
{
    State state;
    for (const QString &ch : kChannels) {
        const QJsonObject o = doc.value(globalKey(ch)).toObject();
        if (!o.isEmpty()) {
            state.inputs << inputFrom(o, ch);
        }
    }
    QStringList scenes;
    for (const auto &v : doc.value(QLatin1String("sources")).toArray()) {
        const QJsonObject o = v.toObject();
        const QString id = o.value(QLatin1String("id")).toString();
        if (id == QLatin1String("scene")) {
            scenes << o.value(QLatin1String("name")).toString();
        } else if (isAudioCaptureKind(id)) {
            state.inputs << inputFrom(o, QString());
        }
    }
    for (const auto &v : doc.value(QLatin1String("scene_order")).toArray()) {
        const QString name = v.toObject().value(QLatin1String("name")).toString();
        if (scenes.contains(name)) {
            state.scenes << name;
        }
    }
    for (const QString &name : std::as_const(scenes)) {
        if (!state.scenes.contains(name)) {
            state.scenes << name;
        }
    }
    return state;
}

QJsonObject applyToCollection(QJsonObject doc, const QList<Action> &actions)
{
    for (const auto &a : actions) {
        switch (a.type) {
        case Action::Type::SetDevice:
            setDevice(doc, a.input, a.device, a.tracks);
            break;
        case Action::Type::Unmute:
            setMuted(doc, a.input, false);
            break;
        case Action::Type::Mute:
            setMuted(doc, a.input, true);
            break;
        case Action::Type::CreateGlobal: {
            const QString key = globalKey(a.channel);
            if (key.isEmpty() || doc.contains(key)) {
                break;
            }
            doc.insert(key, QJsonObject{
                                {QStringLiteral("id"), a.kind},
                                {QStringLiteral("versioned_id"), a.kind},
                                {QStringLiteral("name"), a.input},
                                {QStringLiteral("uuid"), QUuid::createUuid().toString(QUuid::WithoutBraces)},
                                {QStringLiteral("settings"), QJsonObject{{QStringLiteral("device_id"), a.device}}},
                                {QStringLiteral("mixers"), int(a.tracks ? a.tracks : kAllTracks)},
                                {QStringLiteral("muted"), false},
                                {QStringLiteral("enabled"), true},
                                {QStringLiteral("volume"), 1.0},
                                {QStringLiteral("balance"), 0.5},
                            });
            break;
        }
        case Action::Type::CreateInput: {
            QJsonArray sources = doc.value(QLatin1String("sources")).toArray();
            sources.append(QJsonObject{
                {QStringLiteral("id"), a.kind},
                {QStringLiteral("versioned_id"), a.kind},
                {QStringLiteral("name"), a.input},
                {QStringLiteral("uuid"), QUuid::createUuid().toString(QUuid::WithoutBraces)},
                {QStringLiteral("settings"), QJsonObject{{QStringLiteral("device_id"), a.device}}},
                {QStringLiteral("mixers"), int(a.tracks ? a.tracks : kAllTracks)},
                {QStringLiteral("muted"), false},
                {QStringLiteral("enabled"), true},
                {QStringLiteral("volume"), 1.0},
                {QStringLiteral("balance"), 0.5},
            });
            for (qsizetype n = 0; n < sources.size(); ++n) {
                QJsonObject o = sources.at(n).toObject();
                if (o.value(QLatin1String("id")).toString() == QLatin1String("scene") &&
                    a.scenes.contains(o.value(QLatin1String("name")).toString())) {
                    QJsonObject settings = o.value(QLatin1String("settings")).toObject();
                    QJsonArray items = settings.value(QLatin1String("items")).toArray();
                    items.append(QJsonObject{{QStringLiteral("name"), a.input}});
                    settings.insert(QStringLiteral("items"), items);
                    o.insert(QStringLiteral("settings"), settings);
                    sources.replace(n, o);
                }
            }
            doc.insert(QStringLiteral("sources"), sources);
            break;
        }
        case Action::Type::SetTwitchVodTrack:
            break;
        }
    }
    return doc;
}

QJsonObject undoInCollection(QJsonObject doc, const Undo &undo)
{
    for (auto it = undo.ops.crbegin(); it != undo.ops.crend(); ++it) {
        const UndoOp &op = *it;
        switch (op.type) {
        case UndoOp::Type::RestoreDevice:
            setDevice(doc, op.input, op.device, op.tracks);
            break;
        case UndoOp::Type::RestoreMute:
            setMuted(doc, op.input, op.muted);
            break;
        case UndoOp::Type::RemoveGlobal: {
            const QString key = globalKey(op.channel);
            if (doc.value(key).toObject().value(QLatin1String("name")).toString() == op.input) {
                doc.remove(key);
            }
            break;
        }
        case UndoOp::Type::RemoveInput: {
            QJsonArray sources = doc.value(QLatin1String("sources")).toArray();
            for (qsizetype n = sources.size() - 1; n >= 0; --n) {
                QJsonObject o = sources.at(n).toObject();
                if (o.value(QLatin1String("name")).toString() == op.input) {
                    sources.removeAt(n);
                    continue;
                }
                if (o.value(QLatin1String("id")).toString() != QLatin1String("scene")) {
                    continue;
                }
                QJsonObject settings = o.value(QLatin1String("settings")).toObject();
                QJsonArray items = settings.value(QLatin1String("items")).toArray();
                for (qsizetype k = items.size() - 1; k >= 0; --k) {
                    if (items.at(k).toObject().value(QLatin1String("name")).toString() == op.input) {
                        items.removeAt(k);
                    }
                }
                settings.insert(QStringLiteral("items"), items);
                o.insert(QStringLiteral("settings"), settings);
                sources.replace(n, o);
            }
            doc.insert(QStringLiteral("sources"), sources);
            break;
        }
        }
    }
    return doc;
}

} // namespace rostrum::obs
