#pragma once

#include <QByteArray>
#include <QDate>
#include <QString>
#include <QUrl>

namespace rostrum::updates {

// How this copy was installed decides who updates it.
enum class InstallKind {
    Source,   // built from source (~/.local, /usr/local, a build folder): Rostrum tells the user
    Package,  // a distro or Snap package: the package manager updates it
    Flatpak,  // Flathub and the software center update it; Rostrum does not check
    AppImage, // Rostrum downloads, verifies and swaps the file itself
};

QString kindName(InstallKind kind);
InstallKind detectInstallKind(const QString &executable, const QString &appImage, bool flatpak, bool snap);

struct Download
{
    QUrl url;
    QByteArray sha256; // lower-case hex
    qint64 size = 0;
    bool valid() const { return url.isValid() && sha256.size() == 64; }
};

struct Release
{
    QString version;
    QString notes;
    QUrl page;
    QDate date;
    Download appImage; // for the running CPU architecture
    bool valid() const { return !version.isEmpty(); }
};

// Reads Rostrum's own feed (getrostrum.dev/releases/latest.json) or a GitHub "latest release"
// response. Pre-releases and versions with a suffix ("0.3.0-rc1") are ignored.
Release parseFeed(const QByteArray &json, const QString &arch, QString *error = nullptr);

bool isNewer(const QString &candidate, const QString &current);

// "x86_64" or "aarch64", the names release files use.
QString feedArch(const QString &qtCpuArchitecture);

// Downloads only over HTTPS. Plain HTTP is allowed to loopback, for testing a feed locally.
bool trustedUrl(const QUrl &url);

} // namespace rostrum::updates
