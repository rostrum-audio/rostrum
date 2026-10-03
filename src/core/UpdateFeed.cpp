#include "core/UpdateFeed.h"

#include <QDateTime>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QVersionNumber>

namespace rostrum::updates {

namespace {

// A plain "1.2.3" (an optional leading "v" is dropped); empty for anything with a suffix.
QString cleanVersion(QString v)
{
    v = v.trimmed();
    if (v.startsWith(QLatin1Char('v')) || v.startsWith(QLatin1Char('V'))) {
        v.remove(0, 1);
    }
    static const QRegularExpression plain(QStringLiteral(R"(^\d+(\.\d+){0,3}$)"));
    return plain.match(v).hasMatch() ? v : QString();
}

QByteArray cleanSha(QString hex)
{
    hex = hex.trimmed().toLower();
    if (hex.startsWith(QLatin1String("sha256:"))) {
        hex.remove(0, 7);
    }
    static const QRegularExpression re(QStringLiteral("^[0-9a-f]{64}$"));
    return re.match(hex).hasMatch() ? hex.toLatin1() : QByteArray();
}

Release fromRostrumFeed(const QJsonObject &o, const QString &arch)
{
    Release r;
    r.version = cleanVersion(o.value(QStringLiteral("version")).toString());
    r.notes = o.value(QStringLiteral("notes")).toString();
    r.page = QUrl(o.value(QStringLiteral("url")).toString());
    r.date = QDate::fromString(o.value(QStringLiteral("date")).toString(), Qt::ISODate);
    const QJsonObject image = o.value(QStringLiteral("appimage")).toObject().value(arch).toObject();
    r.appImage.url = QUrl(image.value(QStringLiteral("url")).toString());
    r.appImage.sha256 = cleanSha(image.value(QStringLiteral("sha256")).toString());
    r.appImage.size = image.value(QStringLiteral("size")).toInteger();
    return r;
}

Release fromGitHub(const QJsonObject &o, const QString &arch)
{
    Release r;
    if (o.value(QStringLiteral("draft")).toBool() || o.value(QStringLiteral("prerelease")).toBool()) {
        return r;
    }
    r.version = cleanVersion(o.value(QStringLiteral("tag_name")).toString());
    r.notes = o.value(QStringLiteral("body")).toString();
    r.page = QUrl(o.value(QStringLiteral("html_url")).toString());
    r.date = QDateTime::fromString(o.value(QStringLiteral("published_at")).toString(), Qt::ISODate).date();
    const QString suffix = QLatin1Char('-') + arch + QStringLiteral(".appimage");
    for (const auto &a : o.value(QStringLiteral("assets")).toArray()) {
        const QJsonObject asset = a.toObject();
        if (asset.value(QStringLiteral("name")).toString().toLower().endsWith(suffix)) {
            r.appImage.url = QUrl(asset.value(QStringLiteral("browser_download_url")).toString());
            r.appImage.sha256 = cleanSha(asset.value(QStringLiteral("digest")).toString());
            r.appImage.size = asset.value(QStringLiteral("size")).toInteger();
            break;
        }
    }
    return r;
}

} // namespace

QString kindName(InstallKind kind)
{
    switch (kind) {
    case InstallKind::Source:
        return QStringLiteral("source");
    case InstallKind::Package:
        return QStringLiteral("package");
    case InstallKind::Flatpak:
        return QStringLiteral("flatpak");
    case InstallKind::AppImage:
        return QStringLiteral("appimage");
    }
    return QStringLiteral("source");
}

InstallKind detectInstallKind(const QString &executable, const QString &appImage, bool flatpak, bool snap)
{
    if (flatpak) {
        return InstallKind::Flatpak;
    }
    if (!appImage.isEmpty()) {
        return InstallKind::AppImage;
    }
    if (snap || (executable.startsWith(QLatin1String("/usr/")) && !executable.startsWith(QLatin1String("/usr/local/")))) {
        return InstallKind::Package;
    }
    return InstallKind::Source;
}

Release parseFeed(const QByteArray &json, const QString &arch, QString *error)
{
    QJsonParseError parseError;
    const QJsonDocument doc = QJsonDocument::fromJson(json, &parseError);
    if (!doc.isObject()) {
        if (error) {
            *error = parseError.error != QJsonParseError::NoError ? parseError.errorString()
                                                                   : QStringLiteral("not a JSON object");
        }
        return {};
    }
    const QJsonObject o = doc.object();
    Release r = o.contains(QStringLiteral("tag_name")) ? fromGitHub(o, arch) : fromRostrumFeed(o, arch);
    if (!r.valid() && error) {
        *error = QStringLiteral("no usable version in the feed");
    }
    if (!trustedUrl(r.appImage.url)) {
        r.appImage = {};
    }
    if (!trustedUrl(r.page)) {
        r.page = QUrl();
    }
    return r;
}

bool isNewer(const QString &candidate, const QString &current)
{
    const QString c = cleanVersion(candidate);
    const QString cur = cleanVersion(current);
    if (c.isEmpty() || cur.isEmpty()) {
        return false;
    }
    return QVersionNumber::compare(QVersionNumber::fromString(c), QVersionNumber::fromString(cur)) > 0;
}

QString feedArch(const QString &qtCpuArchitecture)
{
    if (qtCpuArchitecture == QLatin1String("arm64")) {
        return QStringLiteral("aarch64");
    }
    return qtCpuArchitecture;
}

bool trustedUrl(const QUrl &url)
{
    if (!url.isValid() || url.host().isEmpty()) {
        return false;
    }
    if (url.scheme() == QLatin1String("https")) {
        return true;
    }
    const QString host = url.host();
    return url.scheme() == QLatin1String("http") &&
           (host == QLatin1String("localhost") || host == QLatin1String("::1") || host.startsWith(QLatin1String("127.")));
}

} // namespace rostrum::updates
