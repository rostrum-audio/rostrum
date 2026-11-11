#pragma once

#include "core/Model.h"

#include <QHash>
#include <QList>
#include <QString>

#include <optional>

namespace rostrum {

// Scene files under $XDG_CONFIG_HOME/rostrum/scenes/. The name inside the file wins: a scene lives
// in whichever file holds it, found by reading the folder. A new scene gets <slug>.toml, or
// <slug>-2.toml and so on when that file exists, so names that slug alike ("Live" and "Live!", or
// any two names without Latin letters) never share, overwrite or delete each other's file.
class SceneStore
{
public:
    explicit SceneStore(QString dir);

    QString dir() const { return m_dir; }
    QList<Scene> loadAll(QStringList *errors = nullptr) const;
    bool save(const Scene &scene, QString *error = nullptr) const;
    // Deletes only the file that holds this scene; true when no file holds it.
    bool remove(const QString &name, QString *error = nullptr) const;
    // The file that holds this scene (names compare case-insensitively), or the one a new scene of
    // this name would be saved to.
    QString fileFor(const QString &name) const;

    static bool writeFile(const QString &path, const QString &text, QString *error);
    static QString readFile(const QString &path, QString *error);

    // Scenes named in `order` first, in that order (names compare case-insensitively); the rest
    // keep their current relative order after them.
    static QList<Scene> applyOrder(QList<Scene> scenes, const QStringList &order);

private:
    using Index = QHash<QString, QString>; // lower-case scene name -> file path
    Index &index() const;
    void scan(QList<Scene> *scenes, QStringList *errors) const;
    QString newFileFor(const QString &name) const;

    QString m_dir;
    mutable std::optional<Index> m_index;
};

} // namespace rostrum
