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
    void destroyObject(uint32_t id);
    void createLink(uint32_t outPort, uint32_t inPort);
    void setNodeVolume(uint32_t nodeId, float linear, bool mute);
    void setNodeVolume(uint32_t nodeId, float linear); // leaves mute alone
    void setMetadata(uint32_t subject, const QString &key, const QString &type, const QString &value);
    void clearMetadata(uint32_t subject, const QString &key);

    pw_thread_loop *threadLoop() const;
    pw_core *core() const;

Q_SIGNALS:
    void stateChanged();
    void graphChanged(); // coalesced, at most once per event-loop turn
    void nodeAdded(uint32_t id);
    void nodeRemoved(uint32_t id, const QString &name);
    void metadataChanged(uint32_t subject, const QString &key);
    void defaultsChanged();
    void createFailed(const QString &message);

public:
    struct Impl;
    void scheduleChanged();

private:
    void sendProps(uint32_t nodeId, float linear, int mute); // mute: -1 untouched, 0/1
    std::unique_ptr<Impl> d;
};

} // namespace rostrum::pw
