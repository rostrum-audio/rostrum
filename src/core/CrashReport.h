#pragma once

#include <QDateTime>
#include <QJsonObject>
#include <QString>
#include <QStringList>
#include <QUrl>

// Crash reports go to Sentry. sentry-native captures the crash and hands Rostrum an envelope;
// only scrubEvent() output ever leaves the computer. Kept free of sentry and the network so the
// scrubbing is unit tested.
namespace rostrum::crash {

constexpr int kMaxFrames = 128;
constexpr int kMaxImages = 256;

struct Dsn
{
    QUrl envelopeUrl;  // https://<host>/api/<project>/envelope/
    QString publicKey; // the DSN's user part; public by design
    bool valid() const { return envelopeUrl.isValid() && !publicKey.isEmpty(); }
};

Dsn parseDsn(const QString &dsn);

// The "event" item of a Sentry envelope, or an empty object.
QJsonObject eventFromEnvelope(const QByteArray &envelope);

// A copy holding only whitelisted fields: no user or installation ID, no registers, no paths
// (library and image paths become file names), no timestamps, tags, breadcrumbs or extras, and
// only the libraries the stack trace runs through.
QJsonObject scrubEvent(const QJsonObject &event);

// A one-event envelope for the Sentry envelope endpoint.
QByteArray toEnvelope(const QJsonObject &event);

// When the crash happened, from the unscrubbed event.
QDateTime eventTime(const QJsonObject &event);

// Keys allowed in the "rostrum" context Rostrum sets for each crash.
QStringList contextFields();

// Letters, digits, space and . _ : + ( ) / - only, at most 64 characters.
QString sanitizeValue(const QString &value);

// The last path component if it looks like a library or program file name, else "?".
QString fileName(const QString &path);

} // namespace rostrum::crash
