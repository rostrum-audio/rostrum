#pragma once

#include <QObject>
#include <QTimer>

namespace rostrum::pw {

class PwContext;

// A one-second 440 Hz tone at −14 dBFS on a chosen sink, for "is this my headset?". One tone at a
// time; starting another replaces it. The stream is internal, so the router never moves it.
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
