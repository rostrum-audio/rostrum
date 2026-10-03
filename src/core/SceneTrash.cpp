#include "core/SceneTrash.h"

#include "core/SceneStore.h"
#include "core/SceneToml.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>

namespace rostrum {

namespace {

const QString kStampFormat = QStringLiteral("yyyyMMdd'T'HHmmsszzz'Z'");

QDateTime stampOf(const QString &fileName)
{
    static const QRegularExpression re(QStringLiteral("^(\\d{8}T\\d{9}Z)-"));
    const auto m = re.match(fileName);
    if (!m.hasMatch()) {
        return {};
    }
    QDateTime t = QDateTime::fromString(m.captured(1), kStampFormat);
    t.setTimeZone(QTimeZone::UTC);
    return t;
}

} // namespace

SceneTrash::SceneTrash(QString dir) : m_dir(std::move(dir)) {}

QString SceneTrash::put(const Scene &scene, const QDateTime &now, QString *error) const
{
    const QString stem = now.toUTC().toString(kStampFormat) + QLatin1Char('-') +
                         makeSlug(scene.name, {}, QStringLiteral("scene"));
    QString path = m_dir + QLatin1Char('/') + stem + QStringLiteral(".toml");
    for (int n = 2; QFileInfo::exists(path); ++n) {
        path = m_dir + QLatin1Char('/') + stem + QStringLiteral("-%1.toml").arg(n);
    }
    if (!SceneStore::writeFile(path, toml_io::serializeScene(scene), error)) {
        return {};
    }
    return path;
}

QList<SceneTrash::Entry> SceneTrash::entries() const
{
    QList<Entry> out;
    const QDir d(m_dir);
    // Names start with the UTC time, so reverse name order is newest first.
    const auto files = d.entryInfoList({QStringLiteral("*.toml")}, QDir::Files, QDir::Name | QDir::Reversed);
    for (const auto &fi : files) {
        const QDateTime deleted = stampOf(fi.fileName());
        if (!deleted.isValid()) {
            continue;
        }
        QString err;
        const QString text = SceneStore::readFile(fi.absoluteFilePath(), &err);
        const auto scene = err.isEmpty() ? toml_io::parseScene(text, &err) : std::nullopt;
        if (scene) {
            out.append({fi.absoluteFilePath(), scene->name, deleted});
        }
    }
    return out;
}

std::optional<Scene> SceneTrash::take(const QString &file, QString *error) const
{
    const QFileInfo fi(file);
    if (fi.absolutePath() != QFileInfo(m_dir).absoluteFilePath()) {
        if (error) {
            *error = QStringLiteral("%1 is not in the trash.").arg(file);
        }
        return std::nullopt;
    }
    QString err;
    const QString text = SceneStore::readFile(file, &err);
    auto scene = err.isEmpty() ? toml_io::parseScene(text, &err) : std::nullopt;
    if (!scene) {
        if (error) {
            *error = err;
        }
        return std::nullopt;
    }
    QFile::remove(file);
    return scene;
}

int SceneTrash::purge(const QDateTime &now, int keepDays) const
{
    int removed = 0;
    const QDateTime cutoff = now.toUTC().addDays(-keepDays);
    const QDir d(m_dir);
    for (const auto &fi : d.entryInfoList({QStringLiteral("*.toml")}, QDir::Files)) {
        const QDateTime deleted = stampOf(fi.fileName());
        if (deleted.isValid() && deleted < cutoff && QFile::remove(fi.absoluteFilePath())) {
            ++removed;
        }
    }
    return removed;
}

} // namespace rostrum
