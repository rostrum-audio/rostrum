#include "engine/Controls.h"

#include "core/Settings.h"
#include "engine/Engine.h"
#include "engine/SceneManager.h"

#include <algorithm>

namespace rostrum::engine {

Controls::Controls(Engine *engine, SceneManager *scenes) : m_engine(engine), m_scenes(scenes) {}

Controls::Outcome Controls::press(const QString &id)
{
    using namespace actions;
    Engine &e = *m_engine;
    const Scene &s = e.scene();
    if (id == QLatin1String(kMuteMic)) {
        e.setMicMuted(!e.effectiveMicMuted());
    } else if (id == QLatin1String(kPushToTalk)) {
        e.setPushToTalk(true);
    } else if (id == QLatin1String(kPushToMute)) {
        e.setPushToMute(true);
    } else if (id == QLatin1String(kPanicMute)) {
        e.setPanic(!e.panic());
    } else if (id == QLatin1String(kToggleSidetone)) {
        e.setSidetoneEnabled(!e.sidetoneEnabled());
    } else if (id == QLatin1String(kToggleMicFilters)) {
        micfx::Settings fx = e.micFilters();
        fx.enabled = !fx.enabled;
        e.setMicFilters(fx);
    } else if (id == QLatin1String(kMuteStream)) {
        e.setMasterStreamMuted(!e.effectiveStreamMuted());
    } else if (id == QLatin1String(kMuteHeadphones)) {
        e.setMasterPhonesMuted(!s.masterPhonesMuted);
    } else if (id == QLatin1String(kStreamVolumeUp)) {
        e.setMasterStream(s.masterStream + kVolumeStep);
    } else if (id == QLatin1String(kStreamVolumeDown)) {
        e.setMasterStream(s.masterStream - kVolumeStep);
    } else if (const QString busId = busOfAction(id); !busId.isEmpty()) {
        const Bus *b = s.bus(busId);
        if (!b || b->isInput()) {
            return {Result::NoSuchBus, {}};
        }
        e.setBusMuted(busId, !b->muted);
    } else if (id == QLatin1String(kNextScene) || id == QLatin1String(kPrevScene) || sceneSlot(id) > 0) {
        const QStringList names = m_scenes->names();
        const int current = std::max(0, int(names.indexOf(m_scenes->currentName())));
        const int slot = sceneSlot(id);
        if (names.isEmpty() || slot > names.size()) {
            return {Result::NoSuchScene, {}};
        }
        const int n = int(names.size());
        const int target = slot > 0                          ? slot - 1
                           : id == QLatin1String(kNextScene) ? (current + 1) % n
                                                             : (current - 1 + n) % n;
        return {Result::SwitchScene, names.at(target)};
    } else {
        return {Result::UnknownAction, {}};
    }
    return {};
}

void Controls::release(const QString &id)
{
    if (id == QLatin1String(actions::kPushToTalk)) {
        m_engine->setPushToTalk(false);
    } else if (id == QLatin1String(actions::kPushToMute)) {
        m_engine->setPushToMute(false);
    }
}

bool Controls::isHeld(const QString &id) const
{
    return (id == QLatin1String(actions::kPushToTalk) && m_engine->pushToTalk()) ||
           (id == QLatin1String(actions::kPushToMute) && m_engine->pushToMute());
}

QList<QPair<QString, QString>> Controls::buses() const
{
    QList<QPair<QString, QString>> out;
    QStringList seen;
    auto add = [&](const Scene &scene) {
        for (const auto &b : scene.buses) {
            if (!b.isInput() && !seen.contains(b.id)) {
                seen << b.id;
                out.append({b.id, b.name});
            }
        }
    };
    add(m_engine->scene());
    for (const auto &scene : m_scenes->scenes()) {
        add(scene);
    }
    return out;
}

QStringList Controls::actionIds() const
{
    QStringList ids = actions::all();
    for (const auto &[id, name] : buses()) {
        ids << actions::muteBusAction(id);
    }
    return ids;
}

} // namespace rostrum::engine
