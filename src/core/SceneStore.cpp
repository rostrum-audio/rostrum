#include "core/SceneStore.h"

#include "core/SceneToml.h"

#include <QDir>
#include <QFile>
#include <QSaveFile>

namespace rostrum {

SceneStore::SceneStore(QString dir)
    : m_dir(std::move(dir))
{
}

QString SceneStore::fileFor(const QString &name) const
{
    return m_dir + QLatin1Char('/') + makeSlug(name, {}, QStringLiteral("scene")) + QStringLiteral(".toml");
}

bool SceneStore::writeFile(const QString &path, const QString &text, QString *error)
{
    if (!QDir().mkpath(QFileInfo(path).absolutePath())) {
        if (error) {
            *error = QStringLiteral("Could not create the folder %1.").arg(QFileInfo(path).absolutePath());
        }
        return false;
    }
    QSaveFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Text)) {
        if (error) {
            *error = QStringLiteral("Could not write %1: %2").arg(path, f.errorString());
        }
        return false;
    }
    f.write(text.toUtf8());
    if (!f.commit()) {
        if (error) {
            *error = QStringLiteral("Could not write %1: %2").arg(path, f.errorString());
        }
        return false;
    }
    return true;
}

QString SceneStore::readFile(const QString &path, QString *error)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) {
        if (error) {
            *error = QStringLiteral("Could not read %1: %2").arg(path, f.errorString());
        }
        return {};
    }
    return QString::fromUtf8(f.readAll());
}

QList<Scene> SceneStore::loadAll(QStringList *errors) const
{
    QList<Scene> out;
    const QDir d(m_dir);
    const auto files = d.entryInfoList({QStringLiteral("*.toml")}, QDir::Files, QDir::Name);
    for (const auto &fi : files) {
        QString err;
        const QString text = readFile(fi.absoluteFilePath(), &err);
        auto scene = err.isEmpty() ? toml_io::parseScene(text, &err) : std::nullopt;
        if (!scene) {
            if (errors) {
                *errors << QStringLiteral("%1: %2").arg(fi.fileName(), err);
            }
            continue;
        }
        const bool dup = std::any_of(out.cbegin(), out.cend(), [&](const Scene &s) {
            return s.name.compare(scene->name, Qt::CaseInsensitive) == 0;
        });
        if (!dup) {
            out.append(*scene);
        }
    }
    return out;
}

bool SceneStore::save(const Scene &scene, QString *error) const
{
    return writeFile(fileFor(scene.name), toml_io::serializeScene(scene), error);
}

bool SceneStore::remove(const QString &name, QString *error) const
{
    QFile f(fileFor(name));
    if (f.exists() && !f.remove()) {
        if (error) {
            *error = QStringLiteral("Could not delete %1: %2").arg(f.fileName(), f.errorString());
        }
        return false;
    }
    return true;
}

} // namespace rostrum
