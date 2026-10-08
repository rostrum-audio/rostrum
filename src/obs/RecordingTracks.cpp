#include "obs/RecordingTracks.h"

#include <QCryptographicHash>
#include <QJsonDocument>
#include <algorithm>

namespace rostrum::obs {
namespace {
constexpr quint32 kIsolatedTracks = 0x3c;

QString deviceFor(const Bus &bus)
{
    return bus.isInput() ? QString::fromLatin1(kMicDevice) : bus.nodeName() + QLatin1String(".monitor");
}

bool containsInput(const RecordingSnapshot &s, const QString &scene, const QString &input,
                   QSet<QString> visiting = {})
{
    if (visiting.contains(scene))
        return false;
    visiting.insert(scene);
    for (const auto &value : s.sceneItems.value(scene)) {
        const auto item = value.toObject();
        if (!item.value(QLatin1String("sceneItemEnabled")).toBool())
            continue;
        const auto name = item.value(QLatin1String("sourceName")).toString();
        if (name == input || (s.sceneItems.contains(name) && containsInput(s, name, input, visiting)))
            return true;
    }
    return false;
}

RecordingChange tracksChange(const QString &input, quint32 next, quint32 old)
{
    return {QStringLiteral("SetInputAudioTracks"),
            {{QStringLiteral("inputName"), input},
             {QStringLiteral("inputAudioTracks"), recordingTracksJson(next)}},
            QStringLiteral("SetInputAudioTracks"),
            {{QStringLiteral("inputName"), input},
             {QStringLiteral("inputAudioTracks"), recordingTracksJson(old)}}};
}

} // namespace

QJsonObject recordingTracksJson(quint32 mask)
{
    QJsonObject out;
    for (int n = 1; n <= 6; ++n)
        out.insert(QString::number(n), bool(mask & (1u << (n - 1))));
    return out;
}

RecordingAssignments recordingSuggestion(const Scene &scene, const RecordingSnapshot &before)
{
    if (!before.known)
        return {};
    RecordingAssignments out;
    if (scene.micBus())
        out.insert(3, scene.micBus()->id);
    for (const auto &[track, category] :
         {std::pair{4, AppCategory::Game}, {5, AppCategory::Voice}, {6, AppCategory::Music}})
        if (const auto *bus = scene.busFor(category))
            out.insert(track, bus->id);
    // Even muted or disabled sources are existing user assignments. Names are
    // labels, so never infer a Rostrum bus from an OBS track name.
    for (const auto &input : before.state.inputs)
        if (input.tracksKnown)
            for (int track = 3; track <= 6; ++track)
                if (input.tracks & (1u << (track - 1)))
                    out.remove(track);
    return out;
}

QSet<QString> recordingDevices(const Scene &scene, const RecordingAssignments &assignments)
{
    QSet<QString> out;
    for (const auto &id : assignments)
        if (!id.isEmpty()) {
            const auto *bus = scene.bus(id);
            // Missing buses retain their declared intent across scene changes.
            out.insert(bus ? deviceFor(*bus)
                       : id == QLatin1String(kMicBusId)
                           ? QString::fromLatin1(kMicDevice)
                           : QStringLiteral("rostrum.") + id + QLatin1String(".monitor"));
        }
    return out;
}

bool intendedRecording(const Input &input, const QSet<QString> &devices)
{
    const auto device = input.settings.value(QLatin1String("device_id")).toString();
    return devices.contains(device) && (input.kind == QLatin1String(kPulseOutput) ||
                                        (input.kind == QLatin1String(kPulseInput) &&
                                         device == QLatin1String(kMicDevice) && !(input.tracks & 3u)));
}

RecordingPlan recordingPlan(const RecordingSnapshot &before, const Scene &scene,
                            const RecordingAssignments &assignments)
{
    RecordingPlan p;
    if (!before.known || before.collection.isEmpty() || before.profile.isEmpty() ||
        before.state.scenes.isEmpty())
        p.problems << QStringLiteral("unknownSnapshot");
    if (before.streaming || before.recording)
        p.problems << QStringLiteral("activeOutput");
    if (before.outputMode != QLatin1String("Advanced"))
        p.problems << QStringLiteral("advancedOutput");
    if (before.recordingType != QLatin1String("Standard"))
        p.problems << QStringLiteral("standardRecording");
    QSet<QString> assigned;
    quint32 managedTracks = 0;
    for (auto it = assignments.cbegin(); it != assignments.cend(); ++it) {
        if (it.key() < 3 || it.key() > 6)
            p.problems << QStringLiteral("reservedTrack");
        else
            managedTracks |= 1u << (it.key() - 1);
        if (it.value().isEmpty())
            continue;
        if (!scene.bus(it.value()))
            p.problems << QStringLiteral("unknownBus:") + it.value();
        if (assigned.contains(it.value()))
            p.problems << QStringLiteral("duplicateBus");
        assigned.insert(it.value());
    }
    if (!p.problems.isEmpty())
        return p;

    // Only explicitly chosen slots are exclusive. Omitted slots keep OBS input masks
    // and recording output selection, including assignments on tracks 1 and 2.
    for (const auto &input : before.state.inputs) {
        if (!input.tracksKnown)
            continue; // non-audio inputs
        if (!input.settingsKnown || !input.muteKnown) {
            p.problems << QStringLiteral("unknownInput:") + input.name;
            continue;
        }
        p.inputTracks.insert(input.name, input.tracks & ~managedTracks);
    }
    RecordingSnapshot placement = before;
    QList<RecordingChange> enablePlacements;
    QList<RecordingChange> unmuteInputs;
    p.recordingTracks = before.recordingTracks & ~managedTracks;
    QSet<QString> names;
    for (const auto &input : before.state.inputs)
        names.insert(input.name);
    for (auto it = assignments.cbegin(); it != assignments.cend(); ++it) {
        if (it.value().isEmpty())
            continue;
        const Bus &bus = *scene.bus(it.value());
        const quint32 track = 1u << (it.key() - 1);
        p.recordingTracks |= track;
        const auto device = deviceFor(bus);
        const auto kind = QString::fromLatin1(bus.isInput() ? kPulseInput : kPulseOutput);
        const Input *pick = nullptr;
        for (const auto &input : before.state.inputs)
            if (input.kind == kind && input.settings.value(QLatin1String("device_id")).toString() == device) {
                const quint32 kept = input.tracks & ~managedTracks;
                // Sharing a capture must not unmute kept tracks or add their audio to
                // new scenes. An unmuted global mic already covers every scene, and
                // adding an isolated slot leaves its existing track 1/2 use intact.
                const bool sharedGlobalMic = bus.isInput() && !input.channel.isEmpty() &&
                                             !input.muted && !(kept & kIsolatedTracks);
                if (kept && !sharedGlobalMic)
                    continue;
                if (!pick || (pick->muted && !input.muted))
                    pick = &input;
            }
        QString name;
        if (pick) {
            name = pick->name;
            if (!pick->tracksKnown || !pick->muteKnown || !pick->settingsKnown)
                p.problems << QStringLiteral("unknownInput:") + name;
            p.inputTracks[name] = bus.isInput() ? ((pick->tracks & ~managedTracks) | track) : track;
            if (pick->muted)
                unmuteInputs << RecordingChange{
                    QStringLiteral("SetInputMute"),
                    {{QStringLiteral("inputName"), name}, {QStringLiteral("inputMuted"), false}},
                    QStringLiteral("SetInputMute"),
                    {{QStringLiteral("inputName"), name}, {QStringLiteral("inputMuted"), true}}};
        } else {
            const QString base = QStringLiteral("Rostrum %1 (Recording)").arg(bus.name);
            name = base;
            for (int n = 2; names.contains(name); ++n)
                name = base + QLatin1Char(' ') + QString::number(n);
            names.insert(name);
            p.inputTracks[name] = track;
        }
        if (pick && !pick->channel.isEmpty())
            continue; // global mic already covers every scene

        // Children first: reuse a capture through enabled scene/group nesting rather than
        // creating a second placement on its parent. Cyclic collections are refused.
        QSet<QString> done;
        QStringList pending = before.sceneItems.keys();
        while (!pending.isEmpty()) {
            bool progress = false;
            for (const QString &parent : QStringList(pending)) {
                bool waiting = false;
                for (const auto &v : before.sceneItems.value(parent)) {
                    const auto item = v.toObject();
                    const auto child = item.value(QLatin1String("sourceName")).toString();
                    waiting |= item.value(QLatin1String("sceneItemEnabled")).toBool() &&
                               before.sceneItems.contains(child) && !done.contains(child);
                }
                if (waiting)
                    continue;
                // Groups are inspected but only top-level OBS scenes get new placements.
                if (before.state.scenes.contains(parent) && !containsInput(placement, parent, name)) {
                    int disabledIndex = -1;
                    for (qsizetype n = 0; n < placement.sceneItems.value(parent).size(); ++n) {
                        const auto item = placement.sceneItems.value(parent).at(n).toObject();
                        if (item.value(QLatin1String("sourceName")).toString() == name &&
                            !item.value(QLatin1String("sceneItemEnabled")).toBool() &&
                            item.value(QLatin1String("sceneItemId")).isDouble()) {
                            disabledIndex = int(n);
                            break;
                        }
                    }
                    if (disabledIndex >= 0) {
                        auto item = placement.sceneItems[parent].at(disabledIndex).toObject();
                        QJsonObject next{
                            {QStringLiteral("sceneName"), parent},
                            {QStringLiteral("sourceName"), name},
                            {QStringLiteral("sceneItemId"), item.value(QLatin1String("sceneItemId"))},
                            {QStringLiteral("sceneItemEnabled"), true}};
                        auto old = next;
                        old[QStringLiteral("sceneItemEnabled")] = false;
                        enablePlacements << RecordingChange{QStringLiteral("SetSceneItemEnabled"), next,
                                                            QStringLiteral("SetSceneItemEnabled"), old};
                        item[QStringLiteral("sceneItemEnabled")] = true;
                        placement.sceneItems[parent][disabledIndex] = item;
                    } else if (!pick && !placement.state.input(name)) {
                        p.changes << RecordingChange{QStringLiteral("CreateInput"),
                                                     {{QStringLiteral("sceneName"), parent},
                                                      {QStringLiteral("inputName"), name},
                                                      {QStringLiteral("inputKind"), kind},
                                                      {QStringLiteral("inputSettings"),
                                                       QJsonObject{{QStringLiteral("device_id"), device}}},
                                                      {QStringLiteral("sceneItemEnabled"), false}},
                                                     QStringLiteral("RemoveInput"),
                                                     {{QStringLiteral("inputName"), name}}};
                        Input created;
                        created.name = name;
                        placement.state.inputs << created;
                    } else {
                        p.changes << RecordingChange{QStringLiteral("CreateSceneItem"),
                                                     {{QStringLiteral("sceneName"), parent},
                                                      {QStringLiteral("sourceName"), name},
                                                      {QStringLiteral("sceneItemEnabled"), false}},
                                                     QStringLiteral("RemoveSceneItem"),
                                                     {{QStringLiteral("sceneName"), parent}}};
                    }
                    if (disabledIndex < 0) {
                        enablePlacements << RecordingChange{QStringLiteral("SetSceneItemEnabled"),
                                                            {{QStringLiteral("sceneName"), parent},
                                                             {QStringLiteral("sourceName"), name},
                                                             {QStringLiteral("sceneItemId"), -1},
                                                             {QStringLiteral("sceneItemEnabled"), true}},
                                                            {},
                                                            {}};
                        placement.sceneItems[parent].append(
                            QJsonObject{{QStringLiteral("sourceName"), name},
                                        {QStringLiteral("sceneItemEnabled"), true}});
                    }
                }
                done.insert(parent);
                pending.removeAll(parent);
                progress = true;
            }
            if (!progress) {
                p.problems << QStringLiteral("cyclicScenes");
                break;
            }
        }
    }
    for (auto it = p.inputTracks.cbegin(); it != p.inputTracks.cend(); ++it) {
        const auto *input = before.state.input(it.key());
        if (!input || input->tracks != it.value())
            p.changes << tracksChange(it.key(), it.value(), input ? input->tracks : kAllTracks);
    }
    if (p.recordingTracks != before.recordingTracks) {
        const QJsonObject param{{QStringLiteral("parameterCategory"), QStringLiteral("AdvOut")},
                                {QStringLiteral("parameterName"), QStringLiteral("RecTracks")}};
        auto next = param, old = param;
        next.insert(QStringLiteral("parameterValue"), QString::number(p.recordingTracks));
        old.insert(QStringLiteral("parameterValue"), QString::number(before.recordingTracks));
        p.changes << RecordingChange{QStringLiteral("SetProfileParameter"), next,
                                     QStringLiteral("SetProfileParameter"), old};
    }
    p.changes += unmuteInputs;
    p.changes += enablePlacements;
    if (!p.problems.isEmpty())
        p.changes.clear();
    return p;
}

RecordingPlan recordingUndo(const RecordingPlan &plan)
{
    RecordingPlan out;
    for (auto it = plan.changes.crbegin(); it != plan.changes.crend(); ++it)
        if (!it->undoMethod.isEmpty())
            out.changes << RecordingChange{it->undoMethod, it->undoData, it->method, it->data};
    return out;
}

QJsonArray recordingChangesJson(const QList<RecordingChange> &changes)
{
    QJsonArray out;
    for (const auto &c : changes)
        out.append(QJsonObject{{QStringLiteral("method"), c.method},
                               {QStringLiteral("data"), c.data},
                               {QStringLiteral("undoMethod"), c.undoMethod},
                               {QStringLiteral("undoData"), c.undoData}});
    return out;
}

QList<RecordingChange> recordingChangesFromJson(const QJsonArray &json)
{
    QList<RecordingChange> out;
    for (const auto &v : json) {
        const auto o = v.toObject();
        out << RecordingChange{
            o.value(QLatin1String("method")).toString(), o.value(QLatin1String("data")).toObject(),
            o.value(QLatin1String("undoMethod")).toString(), o.value(QLatin1String("undoData")).toObject()};
    }
    return out;
}

QByteArray recordingFingerprint(const RecordingSnapshot &s)
{
    QJsonObject inputs, items, names;
    for (const auto &i : s.state.inputs)
        if (i.tracksKnown)
            inputs.insert(
                i.name, QJsonObject{{QStringLiteral("kind"), i.kind},
                                    {QStringLiteral("device"), i.settings.value(QLatin1String("device_id"))},
                                    {QStringLiteral("tracks"), int(i.tracks)},
                                    {QStringLiteral("muted"), i.muted}});
    for (auto it = s.sceneItems.cbegin(); it != s.sceneItems.cend(); ++it)
        items.insert(it.key(), it.value());
    for (auto it = s.trackNames.cbegin(); it != s.trackNames.cend(); ++it)
        names.insert(QString::number(it.key()), it.value());
    const QJsonObject o{{QStringLiteral("trackNames"), names},
                        {QStringLiteral("inputs"), inputs},
                        {QStringLiteral("items"), items},
                        {QStringLiteral("collection"), s.collection},
                        {QStringLiteral("profile"), s.profile},
                        {QStringLiteral("mode"), s.outputMode},
                        {QStringLiteral("recordingType"), s.recordingType},
                        {QStringLiteral("recordingTracks"), int(s.recordingTracks)}};
    return QCryptographicHash::hash(QJsonDocument(o).toJson(QJsonDocument::Compact),
                                    QCryptographicHash::Sha256);
}
} // namespace rostrum::obs
