#pragma once

#include <QHash>
#include <QObject>
#include <QStringList>

#include <memory>

namespace rostrum::pw {

class PwContext;

// Peak meters: one capture stream per target node. Sinks are read from their monitor, sources
// directly. Sink and stream meters are passive; source meters are not, so they wake the mic. Meter streams set node.dont-fallback and node.dont-reconnect so they never
// wander onto another device; that is allowed for meters only, never for app rules.
class MeterBank : public QObject
{
    Q_OBJECT
public:
    explicit MeterBank(PwContext *pw, QObject *parent = nullptr);
    ~MeterBank() override;

    // Nodes to meter: a node.name, or "#<id>" for app streams, whose names are not unique.
    // Playback streams are read from their own output. Meter streams exist only while active
    // and the node is in the graph.
    void setTargets(const QStringList &nodeNames);
    void setActive(bool active);
    bool isActive() const { return m_active; }

    // Highest absolute sample since the previous call, linear 0..1+, then resets.
    float takePeak(const QString &nodeName);

    struct Meter;

private:
    void sync();
    void destroyAll();
    void destroyMeter(const QString &name);

    PwContext *m_pw = nullptr;
    QStringList m_targets;
    bool m_active = false;
    QHash<QString, Meter *> m_meters;
};

} // namespace rostrum::pw
