#include "obs/ObsClient.h"

#include <QCryptographicHash>
#include <QJsonDocument>
#include <QUrl>
#include <QWebSocketHandshakeOptions>

#include <utility>

namespace rostrum::obs {

namespace {
enum Op { Hello = 0, Identify = 1, Identified = 2, Event = 5, Request = 6, RequestResponse = 7 };
constexpr int kCloseAuthFailed = 4009;
} // namespace

Client::Client(QObject *parent)
    : QObject(parent)
{
    connect(&m_socket, &QWebSocket::textMessageReceived, this, &Client::onMessage);
    connect(&m_socket, &QWebSocket::disconnected, this, [this] {
        if (m_socket.closeCode() == kCloseAuthFailed) {
            setStatus(Status::AuthFailed, tr("OBS rejected the WebSocket password."));
        } else if (m_status == Status::Connecting || m_status == Status::Connected) {
            setStatus(Status::Failed, m_socket.closeReason().isEmpty() ? tr("OBS closed the connection.")
                                                                       : m_socket.closeReason());
        }
        failPending(m_error.isEmpty() ? tr("Disconnected from OBS.") : m_error);
    });
    connect(&m_socket, &QWebSocket::errorOccurred, this, [this](QAbstractSocket::SocketError) {
        if (m_status == Status::Connecting) {
            setStatus(Status::Failed, m_socket.errorString());
        }
    });
}

Client::~Client()
{
    m_socket.disconnect(this);
    m_socket.abort();
}

void Client::open(quint16 port, const QString &password, int events)
{
    close();
    m_password = password;
    m_events = events;
    setStatus(Status::Connecting);
    QWebSocketHandshakeOptions options;
    options.setSubprotocols({QStringLiteral("obswebsocket.json")});
    m_socket.open(QUrl(QStringLiteral("ws://127.0.0.1:%1").arg(port)), options);
}

void Client::close()
{
    setStatus(Status::Disconnected);
    if (m_socket.state() != QAbstractSocket::UnconnectedState) {
        m_socket.abort();
    }
    failPending(tr("Disconnected from OBS."));
}

void Client::request(const QString &type, const QJsonObject &data, Callback done)
{
    if (m_status != Status::Connected) {
        if (done) {
            done(false, {}, tr("Not connected to OBS."));
        }
        return;
    }
    const QString id = QString::number(m_nextId++);
    m_pending.insert(id, std::move(done));
    const QJsonObject msg{{QStringLiteral("op"), Request},
                          {QStringLiteral("d"), QJsonObject{{QStringLiteral("requestType"), type},
                                                            {QStringLiteral("requestId"), id},
                                                            {QStringLiteral("requestData"), data}}}};
    m_socket.sendTextMessage(QString::fromUtf8(QJsonDocument(msg).toJson(QJsonDocument::Compact)));
}

QString Client::authResponse(const QString &password, const QString &salt, const QString &challenge)
{
    const QByteArray secret =
        QCryptographicHash::hash((password + salt).toUtf8(), QCryptographicHash::Sha256).toBase64();
    return QString::fromLatin1(
        QCryptographicHash::hash(secret + challenge.toUtf8(), QCryptographicHash::Sha256).toBase64());
}

void Client::onMessage(const QString &text)
{
    const QJsonObject msg = QJsonDocument::fromJson(text.toUtf8()).object();
    const int op = msg.value(QLatin1String("op")).toInt(-1);
    const QJsonObject d = msg.value(QLatin1String("d")).toObject();
    switch (op) {
    case Hello: {
        m_obsVersion.clear();
        QJsonObject identify{{QStringLiteral("rpcVersion"), 1}, {QStringLiteral("eventSubscriptions"), m_events}};
        const QJsonObject auth = d.value(QLatin1String("authentication")).toObject();
        if (!auth.isEmpty()) {
            if (m_password.isEmpty()) {
                setStatus(Status::AuthFailed, tr("OBS asks for a WebSocket password, and none was found."));
                m_socket.abort();
                return;
            }
            identify.insert(QStringLiteral("authentication"),
                            authResponse(m_password, auth.value(QLatin1String("salt")).toString(),
                                         auth.value(QLatin1String("challenge")).toString()));
        }
        const QJsonObject reply{{QStringLiteral("op"), Identify}, {QStringLiteral("d"), identify}};
        m_socket.sendTextMessage(QString::fromUtf8(QJsonDocument(reply).toJson(QJsonDocument::Compact)));
        break;
    }
    case Identified:
        setStatus(Status::Connected);
        request(QStringLiteral("GetVersion"), {}, [this](bool ok, const QJsonObject &data, const QString &) {
            if (ok) {
                m_obsVersion = data.value(QLatin1String("obsVersion")).toString();
                Q_EMIT statusChanged();
            }
        });
        break;
    case Event:
        Q_EMIT event(d.value(QLatin1String("eventType")).toString(), d.value(QLatin1String("eventData")).toObject());
        break;
    case RequestResponse: {
        Callback done = m_pending.take(d.value(QLatin1String("requestId")).toString());
        if (!done) {
            return;
        }
        const QJsonObject status = d.value(QLatin1String("requestStatus")).toObject();
        const bool ok = status.value(QLatin1String("result")).toBool();
        done(ok, d.value(QLatin1String("responseData")).toObject(),
             ok ? QString() : status.value(QLatin1String("comment")).toString());
        break;
    }
    default:
        break;
    }
}

void Client::setStatus(Status s, const QString &error)
{
    if (s == m_status && error == m_error) {
        return;
    }
    m_status = s;
    m_error = error;
    Q_EMIT statusChanged();
}

void Client::failPending(const QString &error)
{
    const auto pending = std::exchange(m_pending, {});
    for (const auto &done : pending) {
        if (done) {
            done(false, {}, error);
        }
    }
}

} // namespace rostrum::obs
