#include "core/DspPlugin.h"

#include "core/Paths.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>

namespace rostrum::dsp {

namespace {

constexpr const char *kFileName = "librostrum-dsp.so";

bool usable(const QString &path)
{
    const QFileInfo info(path);
    return !path.isEmpty() && info.isAbsolute() && info.isFile() && info.isReadable();
}

bool inside(const QString &path, const QString &dir)
{
    if (dir.isEmpty()) {
        return false;
    }
    const QString base = QDir::cleanPath(dir) + QLatin1Char('/');
    return QDir::cleanPath(path).startsWith(base);
}

QString located()
{
    const QString override = qEnvironmentVariable("ROSTRUM_DSP_PLUGIN");
    if (!override.isEmpty()) {
        return usable(override) ? QDir::cleanPath(override) : QString();
    }
#ifdef ROSTRUM_DSP_BUILD_PATH
    const QString appDir = QCoreApplication::applicationDirPath();
    if (inside(appDir, QStringLiteral(ROSTRUM_BINARY_DIR)) && usable(QStringLiteral(ROSTRUM_DSP_BUILD_PATH))) {
        return QStringLiteral(ROSTRUM_DSP_BUILD_PATH);
    }
    const QString relative = appDir + QStringLiteral("/../" ROSTRUM_INSTALL_LIBDIR "/rostrum/") +
                             QLatin1String(kFileName);
    if (usable(relative)) {
        return QDir::cleanPath(relative);
    }
    if (usable(QStringLiteral(ROSTRUM_DSP_INSTALL_PATH))) {
        return QStringLiteral(ROSTRUM_DSP_INSTALL_PATH);
    }
#endif
    return {};
}

} // namespace

bool built()
{
#ifdef ROSTRUM_DSP_BUILD_PATH
    return true;
#else
    return false;
#endif
}

bool hasDenoise()
{
#ifdef ROSTRUM_DSP_DENOISE
    return true;
#else
    return false;
#endif
}

QString pluginPath(QString *error)
{
    const QString path = located();
    if (path.isEmpty()) {
        if (error) {
            *error = built() ? QStringLiteral("The mic filter plugin (%1) is not installed.").arg(QLatin1String(kFileName))
                             : QStringLiteral("This build of Rostrum has no mic filters.");
        }
        return {};
    }
    const QString appDir = qEnvironmentVariable("APPDIR");
    if (!qEnvironmentVariable("APPIMAGE").isEmpty() && inside(path, appDir)) {
        return stableCopy(path, paths::dspDir(), error);
    }
    return path;
}

QString stableCopy(const QString &source, const QString &dir, QString *error)
{
    auto fail = [error](const QString &message) {
        if (error) {
            *error = message;
        }
        return QString();
    };
    QFile in(source);
    if (!in.open(QIODevice::ReadOnly)) {
        return fail(QStringLiteral("Cannot read %1: %2").arg(source, in.errorString()));
    }
    const QByteArray data = in.readAll();
    const QString hash = QString::fromLatin1(QCryptographicHash::hash(data, QCryptographicHash::Sha256).toHex().left(16));
    const QString name = QFileInfo(source).fileName();
    const QString target = dir + QLatin1Char('/') + hash + QLatin1Char('/') + name;

    QFile existing(target);
    if (existing.open(QIODevice::ReadOnly) && existing.readAll() == data) {
        return target;
    }
    existing.close();

    if (!QDir().mkpath(QFileInfo(target).absolutePath())) {
        return fail(QStringLiteral("Cannot create %1").arg(QFileInfo(target).absolutePath()));
    }
    QSaveFile out(target);
    if (!out.open(QIODevice::WriteOnly) || out.write(data) != data.size()) {
        return fail(QStringLiteral("Cannot write %1: %2").arg(target, out.errorString()));
    }
    out.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner);
    if (!out.commit()) {
        return fail(QStringLiteral("Cannot write %1: %2").arg(target, out.errorString()));
    }

    // Unlinking a copy PipeWire still has loaded is safe; it keeps its mapping.
    const QDir root(dir);
    for (const QString &other : root.entryList(QDir::Dirs | QDir::NoDotAndDotDot)) {
        if (other != hash && other.size() == 16) {
            QDir(root.filePath(other)).removeRecursively();
        }
    }
    return target;
}

} // namespace rostrum::dsp
