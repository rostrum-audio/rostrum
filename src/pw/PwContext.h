#pragma once

#include "pw/Graph.h"

#include <QMap>
#include <QObject>

#include <memory>

struct pw_thread_loop;
struct pw_core;

namespace rostrum::pw {

// Owns the PipeWire connection on a pw_thread_loop and mirrors the registry on the Qt thread.
// All public methods are called from the Qt thread; commands take the loop lock internally.
class PwContext : public QObject
{
    Q_OBJECT
public:
    enum class State { Idle, Connecting, Ready, Failed };
    Q_ENUM(State)

    explicit PwContext(QObject *parent = nullptr);
    ~PwContext() override;

    // Connects and enumerates. Returns false (and sets errorString) if PipeWire is unreachable.
    bool start();
    void stop();

    State state() const;
    QString errorString() const;
    int errorCode() const; // errno from the last failed connect, 0 otherwise
    QString serverVersion() const;
    QString libraryVersion() const;
    QString wireplumberVersion() const;
    bool hasWirePlumber() const;

    const Graph &graph() const;
    QString defaultSinkName() const;
    QString defaultSourceName() const;
    QString metadataValue(uint32_t subject, const QString &key) const;

    // Creates a support.null-audio-sink adapter node in the daemon with the given properties.
    void createNullNode(const QMap<QString, QString> &props);
    // Creates a bare SPA node (props must carry factory.name) through spa-node-factory. It has
    // no ports until setPortConfig.
    void createSpaNode(const QMap<QString, QString> &props);
    // Puts a node's input or output side in DSP mode with this many channels (1 = MONO, 2 = FL FR).
    void setPortConfig(uint32_t nodeId, bool output, int channels);
    // Sets entries of a node's Props "params" struct. Values may be bool, numbers or strings.
    void setNodeParams(uint32_t nodeId, const QList<QPair<QString, QVariant>> &params);
    void destroyObject(uint32_t id);
    void createLink(uint32_t outPort, uint32_t inPort);
    void setNodeVolume(uint32_t nodeId, float linear, bool mute);
    void setNodeVolumes(uint32_t nodeId, const QList<float> &perChannel, bool mute); // the last repeats
    void setNodeVolume(uint32_t nodeId, float linear); // leaves mute alone
    void setNodeMute(uint32_t nodeId, bool mute);      // leaves volume alone
    // Blocks until the daemon has handled every request sent so far, or the timeout passes.
    bool roundtrip(int timeoutMs);
    void setMetadata(uint32_t subject, const QString &key, const QString &type, const QString &value);
    void clearMetadata(uint32_t subject, const QString &key);

    pw_thread_loop *threadLoop() const;
    pw_core *core() const;

Q_SIGNALS:
    void stateChanged();
    void aboutToStop(); // the loop and core are still valid; drop streams created on them
    void graphChanged(); // coalesced, at most once per event-loop turn
    void nodeAdded(uint32_t id);
    void nodeRemoved(uint32_t id, const QString &name);
    void metadataChanged(uint32_t subject, const QString &key);
    void defaultsChanged();
    // nodeName: the node.name of a node that could not be created, empty for links.
    void createFailed(const QString &message, const QString &nodeName);

public:
    struct Impl;
    void scheduleChanged();

private:
    // volumes: one per channel, a single value for all, or empty to leave them; mute: -1 untouched, 0/1
    void sendProps(uint32_t nodeId, const QList<float> &volumes, int mute);
    void createObject(const char *factory, const char *type, uint32_t version, const QMap<QString, QString> &props);
    std::unique_ptr<Impl> d;
};

} // namespace rostrum::pw
