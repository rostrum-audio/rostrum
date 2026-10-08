#pragma once
#include "obs/RecordingTracks.h"

#include <algorithm>

// In-memory scene collection used to inspect the planner without recording audio.
namespace rostrum::obs {
inline RecordingSnapshot recordingResult(RecordingSnapshot before, const RecordingPlan &plan)
{
    for (const auto &c : plan.changes) {
        const auto name = c.data.value(QLatin1String("inputName")).toString();
        auto it = std::find_if(before.state.inputs.begin(), before.state.inputs.end(),
                               [&](const Input &i) { return i.name == name; });
        if (c.method == QLatin1String("CreateInput")) {
            Input input;
            input.name = name;
            input.kind = c.data.value(QLatin1String("inputKind")).toString();
            input.settings = c.data.value(QLatin1String("inputSettings")).toObject();
            input.tracks = kAllTracks;
            input.settingsKnown = input.tracksKnown = input.muteKnown = true;
            before.state.inputs << input;
        } else if (c.method == QLatin1String("SetInputAudioTracks") && it != before.state.inputs.end()) {
            it->tracks = 0;
            const auto mask = c.data.value(QLatin1String("inputAudioTracks")).toObject();
            for (int track = 1; track <= 6; ++track)
                if (mask.value(QString::number(track)).toBool())
                    it->tracks |= 1u << (track - 1);
        } else if (c.method == QLatin1String("SetInputMute") && it != before.state.inputs.end()) {
            it->muted = c.data.value(QLatin1String("inputMuted")).toBool();
        } else if (c.method == QLatin1String("RemoveInput")) {
            if (it != before.state.inputs.end())
                before.state.inputs.erase(it);
            for (auto &items : before.sceneItems)
                for (qsizetype n = items.size() - 1; n >= 0; --n)
                    if (items.at(n).toObject().value(QLatin1String("sourceName")).toString() == name)
                        items.removeAt(n);
        } else if (c.method == QLatin1String("SetProfileParameter")) {
            before.recordingTracks = c.data.value(QLatin1String("parameterValue")).toString().toUInt();
        }
        if (c.method == QLatin1String("CreateInput") || c.method == QLatin1String("CreateSceneItem"))
            before.sceneItems[c.data.value(QLatin1String("sceneName")).toString()].append(QJsonObject{
                {QStringLiteral("sourceName"),
                 name.isEmpty() ? c.data.value(QLatin1String("sourceName")).toString() : name},
                {QStringLiteral("sceneItemEnabled"), c.data.value(QLatin1String("sceneItemEnabled"))}});
        else if (c.method == QLatin1String("SetSceneItemEnabled")) {
            auto &items = before.sceneItems[c.data.value(QLatin1String("sceneName")).toString()];
            for (qsizetype n = 0; n < items.size(); ++n) {
                auto item = items.at(n).toObject();
                if (item.value(QLatin1String("sourceName")) == c.data.value(QLatin1String("sourceName"))) {
                    item.insert(QStringLiteral("sceneItemEnabled"),
                                c.data.value(QLatin1String("sceneItemEnabled")));
                    items.replace(n, item);
                }
            }
        } else if (c.method == QLatin1String("RemoveSceneItem")) {
            auto &items = before.sceneItems[c.data.value(QLatin1String("sceneName")).toString()];
            const int id = c.data.value(QLatin1String("sceneItemId")).toInt(-1);
            for (qsizetype n = items.size() - 1; n >= 0; --n)
                if (items.at(n).toObject().value(QLatin1String("sceneItemId")).toInt(-2) == id)
                    items.removeAt(n);
        }
    }
    return before;
}

} // namespace rostrum::obs
