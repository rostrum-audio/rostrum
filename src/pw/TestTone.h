#pragma once

#include <QObject>
#include <QTimer>

namespace rostrum::pw {

class PwContext;

// The test chime: four rising bell-like notes (C5 E5 G5 C6) in about 1.4 s, peaking near −10 dBFS.
// The first note leans left and the second right, so one press also checks both ear cups.
namespace chime {
int totalFrames();
int sampleRate();
// Realtime-safe: no allocation, no locks. Frames past the end are silent.
void sample(int frame, float &left, float &right);
} // namespace chime

// Plays the test chime on a chosen sink, for "is this my headset?". One chime at a time; starting
// another replaces it. The stream is internal, so the router never moves it.
class TestTone : public QObject
{
    Q_OBJECT
public:
    explicit TestTone(PwContext *pw, QObject *parent = nullptr);
    ~TestTone() override;

    // nodeName empty = the default sink. Returns false if PipeWire is not ready.
    bool play(const QString &nodeName);
    void stop();
    bool isPlaying() const { return m_state != nullptr; }
    QString target() const { return m_target; }

    struct State;

Q_SIGNALS:
    void playingChanged();

private:
    PwContext *m_pw = nullptr;
    State *m_state = nullptr;
    QString m_target;
    QTimer m_stopTimer;
};

} // namespace rostrum::pw
