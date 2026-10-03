#include "core/CrashReport.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QRegularExpression>

#include <algorithm>

namespace rostrum::crash {

namespace {

bool matches(const QString &s, const char *pattern)
{
    return QRegularExpression(QString::fromLatin1(pattern)).match(s).hasMatch();
}

bool isAddress(const QString &s) { return matches(s, "^0x[0-9a-fA-F]{1,16}$"); }
quint64 address(const QString &s) { return isAddress(s) ? s.mid(2).toULongLong(nullptr, 16) : 0; }
bool isUuid(const QString &s) { return matches(s, "^[0-9a-f]{8}-?[0-9a-f]{4}-?[0-9a-f]{4}-?[0-9a-f]{4}-?[0-9a-f]{12}$"); }
bool isHex(const QString &s) { return matches(s, "^[0-9a-f]{1,128}$"); }
bool isSymbol(const QString &s) { return matches(s, "^[A-Za-z0-9_.$@]{1,512}$"); }
bool isSignal(const QString &s) { return matches(s, "^[A-Z0-9_]{1,32}$"); }
// Short words only: no slashes, so no paths.
bool isPlainText(const QString &s) { return matches(s, "^[A-Za-z0-9 ._:()-]{1,64}$"); }
bool isRelease(const QString &s) { return matches(s, "^[A-Za-z0-9._+@-]{1,64}$"); }

void copyIf(QJsonObject &to, const QJsonObject &from, const char *key, bool (*ok)(const QString &))
{
    const QString k = QString::fromLatin1(key);
    const QString v = from.value(k).toString();
    if (!v.isEmpty() && ok(v)) {
        to.insert(k, v);
    }
}

void copySanitized(QJsonObject &to, const QJsonObject &from, const char *key)
{
    const QString k = QString::fromLatin1(key);
    if (const QString v = sanitizeValue(from.value(k).toString()); !v.isEmpty()) {
        to.insert(k, v);
    }
}

QJsonObject scrubFrame(const QJsonObject &in)
{
    QJsonObject out;
    copyIf(out, in, "instruction_addr", isAddress);
    copyIf(out, in, "image_addr", isAddress);
    copyIf(out, in, "symbol_addr", isAddress);
    copyIf(out, in, "function", isSymbol);
    if (const QString package = in.value(QStringLiteral("package")).toString(); !package.isEmpty()) {
        out.insert(QStringLiteral("package"), fileName(package));
    }
    return out;
}

QJsonObject scrubException(const QJsonObject &in)
{
    QJsonObject out;
    copyIf(out, in, "type", isSignal);
    copyIf(out, in, "value", isPlainText);

    const QJsonObject mechIn = in.value(QStringLiteral("mechanism")).toObject();
    QJsonObject mech;
    copyIf(mech, mechIn, "type", isPlainText);
    for (const char *flag : {"handled", "synthetic"}) {
        if (const QJsonValue v = mechIn.value(QLatin1String(flag)); v.isBool()) {
            mech.insert(QLatin1String(flag), v);
        }
    }
    const QJsonObject sigIn = mechIn.value(QStringLiteral("meta")).toObject().value(QStringLiteral("signal")).toObject();
    QJsonObject sig;
    copyIf(sig, sigIn, "name", isSignal);
    if (const QJsonValue n = sigIn.value(QStringLiteral("number")); n.isDouble()) {
        sig.insert(QStringLiteral("number"), n.toInt());
    }
    if (!sig.isEmpty()) {
        mech.insert(QStringLiteral("meta"), QJsonObject{{QStringLiteral("signal"), sig}});
    }
    if (!mech.isEmpty()) {
        out.insert(QStringLiteral("mechanism"), mech);
    }

    // Sentry orders frames caller first; the crash end is the part worth keeping.
    const QJsonArray framesIn = in.value(QStringLiteral("stacktrace")).toObject().value(QStringLiteral("frames")).toArray();
    QJsonArray frames;
    for (qsizetype i = std::max<qsizetype>(0, framesIn.size() - kMaxFrames); i < framesIn.size(); ++i) {
        const QJsonObject frame = scrubFrame(framesIn.at(i).toObject());
        if (frame.contains(QStringLiteral("instruction_addr"))) {
            frames.append(frame);
        }
    }
    if (!frames.isEmpty()) {
        out.insert(QStringLiteral("stacktrace"), QJsonObject{{QStringLiteral("frames"), frames}});
    }
    return out;
}

QJsonObject scrubImage(const QJsonObject &in)
{
    QJsonObject out;
    if (in.value(QStringLiteral("type")).toString() == QLatin1String("elf")) {
        out.insert(QStringLiteral("type"), QStringLiteral("elf"));
    }
    if (const QString file = in.value(QStringLiteral("code_file")).toString(); !file.isEmpty()) {
        out.insert(QStringLiteral("code_file"), fileName(file));
    }
    copyIf(out, in, "image_addr", isAddress);
    if (const QJsonValue size = in.value(QStringLiteral("image_size")); size.isDouble()) {
        out.insert(QStringLiteral("image_size"), size.toInteger());
    }
    copyIf(out, in, "code_id", isHex);
    copyIf(out, in, "debug_id", isUuid);
    return out;
}

} // namespace

Dsn parseDsn(const QString &dsn)
{
    const QUrl url(dsn, QUrl::StrictMode);
    if (!url.isValid() || url.userName().isEmpty() || url.host().isEmpty()) {
        return {};
    }
    QString path = url.path();
    const qsizetype slash = path.lastIndexOf(QLatin1Char('/'));
    const QString project = path.mid(slash + 1);
    if (slash < 0 || !matches(project, "^[0-9]{1,20}$")) {
        return {};
    }
    QUrl envelope;
    envelope.setScheme(url.scheme());
    envelope.setHost(url.host());
    envelope.setPort(url.port());
    envelope.setPath(path.left(slash) + QStringLiteral("/api/") + project + QStringLiteral("/envelope/"));
    return {envelope, url.userName()};
}

QJsonObject eventFromEnvelope(const QByteArray &envelope)
{
    qsizetype pos = envelope.indexOf('\n');
    if (pos < 0) {
        return {};
    }
    ++pos;
    while (pos < envelope.size()) {
        const qsizetype headerEnd = envelope.indexOf('\n', pos);
        if (headerEnd < 0) {
            return {};
        }
        const QJsonObject header = QJsonDocument::fromJson(envelope.mid(pos, headerEnd - pos)).object();
        if (header.isEmpty()) {
            return {};
        }
        pos = headerEnd + 1;
        qsizetype length = -1;
        if (const QJsonValue l = header.value(QStringLiteral("length")); l.isDouble()) {
            length = qsizetype(l.toInteger());
        } else {
            const qsizetype end = envelope.indexOf('\n', pos);
            length = (end < 0 ? envelope.size() : end) - pos;
        }
        if (length < 0 || pos + length > envelope.size()) {
            return {};
        }
        if (header.value(QStringLiteral("type")).toString() == QLatin1String("event")) {
            return QJsonDocument::fromJson(envelope.mid(pos, length)).object();
        }
        pos += length + 1;
    }
    return {};
}

QJsonObject scrubEvent(const QJsonObject &event)
{
    QJsonObject out;
    copyIf(out, event, "event_id", isUuid);
    out.insert(QStringLiteral("platform"), QStringLiteral("native"));
    const QString level = event.value(QStringLiteral("level")).toString();
    out.insert(QStringLiteral("level"), level == QLatin1String("error") ? level : QStringLiteral("fatal"));
    copyIf(out, event, "release", isRelease);
    copyIf(out, event, "environment", isRelease);

    const QJsonObject sdkIn = event.value(QStringLiteral("sdk")).toObject();
    QJsonObject sdk;
    copySanitized(sdk, sdkIn, "name");
    copySanitized(sdk, sdkIn, "version");
    sdk.insert(QStringLiteral("settings"), QJsonObject{{QStringLiteral("infer_ip"), QStringLiteral("never")}});
    out.insert(QStringLiteral("sdk"), sdk);
    // Sentry looks up a location from the upload's IP address unless the event already has one,
    // even with IP storage turned off. An empty one stops the lookup.
    out.insert(QStringLiteral("user"), QJsonObject{{QStringLiteral("geo"), QJsonObject{}}});

    QJsonArray exceptions;
    for (const QJsonValue &v : event.value(QStringLiteral("exception")).toObject().value(QStringLiteral("values")).toArray()) {
        if (exceptions.size() < 4) {
            exceptions.append(scrubException(v.toObject()));
        }
    }
    if (!exceptions.isEmpty()) {
        out.insert(QStringLiteral("exception"), QJsonObject{{QStringLiteral("values"), exceptions}});
    }

    const QJsonObject contextsIn = event.value(QStringLiteral("contexts")).toObject();
    QJsonObject contexts;
    const QJsonObject osIn = contextsIn.value(QStringLiteral("os")).toObject();
    QJsonObject os;
    for (const char *key : {"name", "version", "build", "kernel_version", "distribution_name", "distribution_version"}) {
        copySanitized(os, osIn, key);
    }
    if (!os.isEmpty()) {
        contexts.insert(QStringLiteral("os"), os);
    }
    const QJsonObject ownIn = contextsIn.value(QStringLiteral("rostrum")).toObject();
    QJsonObject own;
    for (const QString &key : contextFields()) {
        copySanitized(own, ownIn, key.toLatin1().constData());
    }
    if (!own.isEmpty()) {
        contexts.insert(QStringLiteral("rostrum"), own);
    }
    if (!contexts.isEmpty()) {
        out.insert(QStringLiteral("contexts"), contexts);
    }

    // Only the libraries the crash ran through: the full list of loaded libraries would say what
    // else is installed (plugins, overlays), and Sentry needs just these to symbolicate.
    QList<quint64> addresses;
    for (const QJsonValue &e : std::as_const(exceptions)) {
        for (const QJsonValue &f : e.toObject().value(QStringLiteral("stacktrace")).toObject().value(QStringLiteral("frames")).toArray()) {
            addresses << address(f.toObject().value(QStringLiteral("instruction_addr")).toString());
        }
    }
    QJsonArray images;
    for (const QJsonValue &v : event.value(QStringLiteral("debug_meta")).toObject().value(QStringLiteral("images")).toArray()) {
        if (images.size() >= kMaxImages) {
            break;
        }
        const QJsonObject image = scrubImage(v.toObject());
        const quint64 start = address(image.value(QStringLiteral("image_addr")).toString());
        const quint64 size = quint64(image.value(QStringLiteral("image_size")).toInteger());
        const bool used = start != 0 && std::any_of(addresses.cbegin(), addresses.cend(), [&](quint64 ip) {
            return ip >= start && (size == 0 ? ip == start : ip - start < size);
        });
        if (used) {
            images.append(image);
        }
    }
    if (!images.isEmpty()) {
        out.insert(QStringLiteral("debug_meta"), QJsonObject{{QStringLiteral("images"), images}});
    }
    return out;
}

QByteArray toEnvelope(const QJsonObject &event)
{
    const QByteArray payload = QJsonDocument(event).toJson(QJsonDocument::Compact);
    QJsonObject header;
    if (event.contains(QStringLiteral("event_id"))) {
        header.insert(QStringLiteral("event_id"), event.value(QStringLiteral("event_id")));
    }
    const QJsonObject item{{QStringLiteral("type"), QStringLiteral("event")}, {QStringLiteral("length"), payload.size()}};
    return QJsonDocument(header).toJson(QJsonDocument::Compact) + '\n' + QJsonDocument(item).toJson(QJsonDocument::Compact) +
           '\n' + payload + '\n';
}

QDateTime eventTime(const QJsonObject &event)
{
    return QDateTime::fromString(event.value(QStringLiteral("timestamp")).toString(), Qt::ISODateWithMs);
}

QStringList contextFields()
{
    return {QStringLiteral("install"), QStringLiteral("arch"),     QStringLiteral("desktop"),
            QStringLiteral("session"), QStringLiteral("qt"),       QStringLiteral("kf"),
            QStringLiteral("pipewire"), QStringLiteral("wireplumber")};
}

QString sanitizeValue(const QString &value)
{
    static const QRegularExpression outside(QStringLiteral(R"([^A-Za-z0-9 ._:+()/-])"));
    QString v = value;
    v.remove(outside);
    return v.trimmed().left(64);
}

QString fileName(const QString &path)
{
    const QString name = path.mid(path.lastIndexOf(QLatin1Char('/')) + 1);
    return matches(name, "^[A-Za-z0-9._+-]{1,64}$") ? name : QStringLiteral("?");
}

} // namespace rostrum::crash
