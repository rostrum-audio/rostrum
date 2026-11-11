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

void SceneStore::scan(QList<Scene> *scenes, QStringList *errors) const
{
    Index found;
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
        const QString key = scene->name.toLower();
        if (found.contains(key)) {
            continue; // the first file with a name wins
        }
        found.insert(key, fi.absoluteFilePath());
        if (scenes) {
            scenes->append(*scene);
        }
    }
    m_index = found;
}

SceneStore::Index &SceneStore::index() const
{
    if (!m_index) {
        scan(nullptr, nullptr);
    }
    return *m_index;
}

QString SceneStore::newFileFor(const QString &name) const
{
    // Every file in the folder is taken, including ones Rostrum cannot read: they are never
    // overwritten.
    QStringList taken;
    const QDir d(m_dir);
    for (const auto &fi : d.entryInfoList({QStringLiteral("*.toml")}, QDir::Files)) {
        taken << fi.completeBaseName();
    }
    for (const QString &path : std::as_const(index())) {
        taken << QFileInfo(path).completeBaseName();
    }
    return QDir(m_dir).absoluteFilePath(makeSlug(name, taken, QStringLiteral("scene")) + QStringLiteral(".toml"));
}

QString SceneStore::fileFor(const QString &name) const
{
    const QString path = index().value(name.toLower());
    return path.isEmpty() ? newFileFor(name) : path;
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
    scan(&out, errors);
    return out;
}

QList<Scene> SceneStore::applyOrder(QList<Scene> scenes, const QStringList &order)
{
    QList<Scene> out;
    for (const QString &name : order) {
        const auto it = std::find_if(scenes.begin(), scenes.end(), [&](const Scene &s) {
            return s.name.compare(name, Qt::CaseInsensitive) == 0;
        });
        if (it != scenes.end()) {
            out.append(*it);
            scenes.erase(it);
        }
    }
    out.append(scenes);
    return out;
}

bool SceneStore::save(const Scene &scene, QString *error) const
{
    const QString path = fileFor(scene.name);
    if (!writeFile(path, toml_io::serializeScene(scene), error)) {
        return false;
    }
    index().insert(scene.name.toLower(), path);
    return true;
}

bool SceneStore::remove(const QString &name, QString *error) const
{
    const QString key = name.toLower();
    const QString path = index().value(key);
    if (path.isEmpty()) {
        return true;
    }
    // The file may have been edited since it was read: never delete another scene.
    const auto onDisk = toml_io::parseScene(readFile(path, nullptr));
    if (onDisk && onDisk->name.compare(name, Qt::CaseInsensitive) != 0) {
        index().remove(key);
        return true;
    }
    QFile f(path);
    if (f.exists() && !f.remove()) {
        if (error) {
            *error = QStringLiteral("Could not delete %1: %2").arg(f.fileName(), f.errorString());
        }
        return false;
    }
    index().remove(key);
    return true;
}

} // namespace rostrum
