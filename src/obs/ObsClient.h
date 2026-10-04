#pragma once

#include <QHash>
#include <QJsonObject>
#include <QObject>
#include <QString>
#include <QWebSocket>

#include <functional>

namespace rostrum::obs {

// A minimal obs-websocket 5 client on localhost: hello, identify (with the SHA-256 challenge
// when OBS asks for a password), requests and events.
class Client : public QObject
{
    Q_OBJECT
public:
    enum class Status { Disconnected, Connecting, Connected, AuthFailed, Failed };
    Q_ENUM(Status)

    // ok, responseData, error text
    using Callback = std::function<void(bool, const QJsonObject &, const QString &)>;

    explicit Client(QObject *parent = nullptr);
    ~Client() override;

    // `events`: obs-websocket EventSubscription bits, 0 for none.
    void open(quint16 port, const QString &password, int events = 0);
    void close();

    Status status() const { return m_status; }
    QString errorString() const { return m_error; }
    QString obsVersion() const { return m_obsVersion; }

    void request(const QString &type, const QJsonObject &data, Callback done);

    static QString authResponse(const QString &password, const QString &salt, const QString &challenge);

    static constexpr int kEventScenes = 1 << 2;
    static constexpr int kEventInputs = 1 << 3;
    static constexpr int kEventSceneItems = 1 << 7;
    static constexpr int kEventOutputs = 1 << 6;

Q_SIGNALS:
    void statusChanged();
    void event(const QString &type, const QJsonObject &data);

private:
    void onMessage(const QString &text);
    void setStatus(Status s, const QString &error = QString());
    void failPending(const QString &error);

    QWebSocket m_socket;
    Status m_status = Status::Disconnected;
    QString m_error;
    QString m_password;
    QString m_obsVersion;
    int m_events = 0;
    quint64 m_nextId = 1;
    QHash<QString, Callback> m_pending;
};

} // namespace rostrum::obs
