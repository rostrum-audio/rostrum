#pragma once

#include <QList>
#include <QPair>
#include <QString>
#include <QStringList>

namespace rostrum::engine {

class Engine;
class SceneManager;

// Runs the actions in core/Settings.h (hotkeys, tray, D-Bus, command line) on the live mix.
// Scene actions only pick the scene; the caller decides whether to ask before switching.
class Controls
{
public:
    enum class Result
    {
        Done,
        SwitchScene,
        UnknownAction,
        NoSuchScene,
        NoSuchBus
    };
    struct Outcome
    {
        Result result = Result::Done;
        QString scene; // SwitchScene: where to go
    };

    // One stream volume step, in fader travel.
    static constexpr double kVolumeStep = 0.05;

    Controls(Engine *engine, SceneManager *scenes);

    // A key went down, or a one-shot trigger. Hold actions stay on until release().
    Outcome press(const QString &id);
    void release(const QString &id);
    bool isHeld(const QString &id) const;

    // {id, name} of every playback bus in the live scene, then those only in saved scenes.
    QList<QPair<QString, QString>> buses() const;
    // actions::all() plus one mute action per bus above.
    QStringList actionIds() const;

private:
    Engine *m_engine = nullptr;
    SceneManager *m_scenes = nullptr;
};

} // namespace rostrum::engine
