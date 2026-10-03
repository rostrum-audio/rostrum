#pragma once

#include <QHash>
#include <QObject>
#include <QSet>
#include <QStringList>
#include <QTimer>

#include <memory>

namespace rostrum::pw {

class PwContext;

// Peak meters: one capture stream per target node. Sinks are read from their monitor, sources
// directly. Sink and stream meters are passive; source meters are not, so they wake the mic.
// Meter streams set node.dont-fallback and node.dont-reconnect so they never wander onto another
// device; that is allowed for meters only, never for app rules. A meter whose stream fails, or a
// source meter that stops receiving audio, is rebuilt.
class MeterBank : public QObject
{
    Q_OBJECT
public:
    explicit MeterBank(PwContext *pw, QObject *parent = nullptr);
    ~MeterBank() override;

    // Nodes to meter: a node.name, or "#<id>" for app streams, whose names are not unique.
    // Playback streams are read from their own output. Meter streams exist only while active
    // and the node is in the graph. `summed` targets report the peak of all channels added
    // together, the way rostrum.mic folds a hardware mic to mono.
    void setTargets(const QStringList &nodeNames, const QStringList &summed = {});
    void setActive(bool active);
    bool isActive() const { return m_active; }

    // Highest absolute sample since the previous call, linear 0..1+, then resets.
    float takePeak(const QString &nodeName);

    struct Meter;

private:
    void sync();
    void destroyAll();
    void destroyMeter(const QString &name);
    void checkHealth();

    PwContext *m_pw = nullptr;
    QStringList m_targets;
    QStringList m_summed;
    bool m_active = false;
    QHash<QString, Meter *> m_meters;
    QTimer m_watchdog;
    QSet<QString> m_reported;
};

} // namespace rostrum::pw
