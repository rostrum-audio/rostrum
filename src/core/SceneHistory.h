#pragma once

#include "core/Model.h"

#include <QList>
#include <QString>

#include <optional>

namespace rostrum {

// Undo and redo for the live scene within one session. Each step is a whole Scene, so restoring
// one is exact. Solo is not part of Scene and is never undone. Scene names are ignored: renaming
// or saving a scene is not a step. The mic mute is not a step either and undo/redo keep the live
// one, so undoing can never open a mic the user muted.
class SceneHistory
{
public:
    static constexpr int kLimit = 50; // steps kept back from the present

    enum class Kind
    {
        None,
        Volume,
        Mute,
        Balance,
        Destination,
        PhonesMaster,
        StreamMaster,
        Sidetone,
        AddBus,
        RemoveBus,
        RenameBus,
        BusColor,
        BusOrder,
        AutoCategory,
        AppRules,
        AppMute,
        Several,
    };
    struct Change
    {
        Kind kind = Kind::None;
        QString bus; // the bus's name, for kinds about one bus
        bool operator==(const Change &) const = default;
    };

    // Forgets every step; `scene` becomes the present.
    void reset(const Scene &scene);
    // Adds `scene` as the new present if it differs from it, and drops any redo steps.
    bool record(const Scene &scene);
    // The step to apply, with `live`'s name and mic mute.
    std::optional<Scene> undo(const Scene &live);
    std::optional<Scene> redo(const Scene &live);

    bool canUndo() const { return m_pos > 0; }
    bool canRedo() const { return m_pos >= 0 && m_pos < m_steps.size() - 1; }
    Change undoChange() const;
    Change redoChange() const;
    Change pendingChange(const Scene &live) const; // from the present to a not yet recorded state
    int size() const { return int(m_steps.size()); }

    static Change describe(const Scene &from, const Scene &to);

private:
    QList<Scene> m_steps;
    int m_pos = -1;
};

} // namespace rostrum
