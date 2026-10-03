#include "core/SceneHistory.h"

namespace rostrum {

namespace {

Scene normalized(Scene s)
{
    s.name.clear();
    if (Bus *mic = s.micBus()) {
        mic->muted = false;
    }
    return s;
}

Scene withLive(Scene step, const Scene &live)
{
    step.name = live.name;
    if (Bus *mic = step.micBus()) {
        const Bus *liveMic = live.micBus();
        mic->muted = liveMic ? liveMic->muted : false;
    }
    return step;
}

QStringList busIds(const Scene &s)
{
    QStringList ids;
    for (const auto &b : s.buses) {
        ids << b.id;
    }
    return ids;
}

} // namespace

void SceneHistory::reset(const Scene &scene)
{
    m_steps = {normalized(scene)};
    m_pos = 0;
}

bool SceneHistory::record(const Scene &scene)
{
    if (m_pos < 0) {
        reset(scene);
        return false;
    }
    const Scene step = normalized(scene);
    if (m_steps.at(m_pos) == step) {
        return false;
    }
    m_steps.resize(m_pos + 1);
    m_steps.append(step);
    while (m_steps.size() > kLimit + 1) {
        m_steps.removeFirst();
    }
    m_pos = int(m_steps.size()) - 1;
    return true;
}

std::optional<Scene> SceneHistory::undo(const Scene &live)
{
    if (!canUndo()) {
        return std::nullopt;
    }
    return withLive(m_steps.at(--m_pos), live);
}

std::optional<Scene> SceneHistory::redo(const Scene &live)
{
    if (!canRedo()) {
        return std::nullopt;
    }
    return withLive(m_steps.at(++m_pos), live);
}

SceneHistory::Change SceneHistory::undoChange() const
{
    return canUndo() ? describe(m_steps.at(m_pos - 1), m_steps.at(m_pos)) : Change{};
}

SceneHistory::Change SceneHistory::redoChange() const
{
    return canRedo() ? describe(m_steps.at(m_pos), m_steps.at(m_pos + 1)) : Change{};
}

SceneHistory::Change SceneHistory::pendingChange(const Scene &live) const
{
    return m_pos >= 0 ? describe(m_steps.at(m_pos), normalized(live)) : Change{};
}

SceneHistory::Change SceneHistory::describe(const Scene &from, const Scene &to)
{
    QList<Change> changes;
    auto add = [&](Kind kind, const QString &bus = QString()) {
        if (!changes.contains(Change{kind, bus})) {
            changes.append({kind, bus});
        }
    };
    if (from.masterPhones != to.masterPhones || from.masterPhonesMuted != to.masterPhonesMuted) {
        add(Kind::PhonesMaster);
    }
    if (from.masterStream != to.masterStream || from.masterStreamMuted != to.masterStreamMuted) {
        add(Kind::StreamMaster);
    }
    if (from.sidetoneVolume != to.sidetoneVolume) {
        add(Kind::Sidetone);
    }
    const QStringList fromIds = busIds(from);
    const QStringList toIds = busIds(to);
    for (const auto &b : to.buses) {
        if (!fromIds.contains(b.id)) {
            add(Kind::AddBus, b.name);
        }
    }
    for (const auto &b : from.buses) {
        if (!toIds.contains(b.id)) {
            add(Kind::RemoveBus, b.name);
        }
    }
    QStringList common = fromIds;
    common.removeIf([&](const QString &id) { return !toIds.contains(id); });
    QStringList commonTo = toIds;
    commonTo.removeIf([&](const QString &id) { return !fromIds.contains(id); });
    if (common != commonTo) {
        add(Kind::BusOrder);
    }
    for (const auto &a : from.buses) {
        const Bus *b = to.bus(a.id);
        if (!b) {
            continue;
        }
        if (a.name != b->name) {
            add(Kind::RenameBus, b->name);
        }
        if (a.color != b->color) {
            add(Kind::BusColor, b->name);
        }
        if (a.volume != b->volume) {
            add(Kind::Volume, b->name);
        }
        if (a.muted != b->muted) {
            add(Kind::Mute, b->name);
        }
        if (a.destination != b->destination) {
            add(Kind::Destination, b->name);
        }
        if (a.autoCategory != b->autoCategory) {
            add(Kind::AutoCategory, b->name);
        }
    }
    if (from.rules != to.rules) {
        add(Kind::AppRules);
    }
    // Removing a bus also drops its rules; that is still one "remove bus" step.
    if (changes.size() == 2 && changes.first().kind == Kind::RemoveBus &&
        changes.last().kind == Kind::AppRules) {
        changes.removeLast();
    }
    if (changes.isEmpty()) {
        return {};
    }
    return changes.size() == 1 ? changes.first() : Change{Kind::Several, {}};
}

} // namespace rostrum
