#pragma once

#include <QObject>
#include <QTimer>

namespace rostrum::pw {

class PwContext;

namespace miccheck {
inline constexpr int kRate = 48000;
inline constexpr int kSeconds = 5;
inline constexpr int kFrames = kRate * kSeconds;

// How the recording's loudest moment sits, in dBFS. Speech for a stream should peak well above
// the noise floor and stay clear of 0 dBFS. Loud starts above the mic limiter's default -1 dB
// ceiling, so a limiter doing its job is not reported as clipping.
enum class Verdict { Silent, Quiet, Good, Loud };
inline constexpr double kSilentBelowDb = -60.0;
inline constexpr double kQuietBelowDb = -24.0;
inline constexpr double kLoudFromDb = -0.5;
Verdict verdict(double peakDb);
double toDb(float peak); // -inf for silence
} // namespace miccheck

// Records a few seconds of a source, then plays them back once on a sink: "how do I sound?".
// Both streams are internal, so the router and other programs never move them. One check at a
// time; starting again replaces the running one.
class MicCheck : public QObject
{
    Q_OBJECT
public:
    enum class Phase { Idle, Recording, Playing };
    Q_ENUM(Phase)

    explicit MicCheck(PwContext *pw, QObject *parent = nullptr);
    ~MicCheck() override;

    // node.name of each. The sink is required: a check must never play into the default sink,
    // which may be one of Rostrum's buses and so reach the stream.
    bool start(const QString &source, const QString &sink);
    void stop();
    Phase phase() const { return m_phase; }
    double progress() const; // 0..1 through the current phase
    float peak() const;      // loudest sample recorded, 0..1+

    struct State;

Q_SIGNALS:
    void phaseChanged();
    void progressChanged();
    // The recording ended and playback starts; peak() is final.
    void recorded();

private:
    void poll();
    bool startPlayback();
    void destroyStream();
    void setPhase(Phase phase);

    PwContext *m_pw = nullptr;
    State *m_state = nullptr;
    Phase m_phase = Phase::Idle;
    QString m_sink;
    QTimer m_poll;
};

} // namespace rostrum::pw
