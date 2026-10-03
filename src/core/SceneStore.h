#pragma once

#include "core/Model.h"

#include <QList>
#include <QString>

namespace rostrum {

// Scene files under $XDG_CONFIG_HOME/rostrum/scenes/<slug>.toml. The name inside the file wins.
class SceneStore
{
public:
    explicit SceneStore(QString dir);

    QString dir() const { return m_dir; }
    QList<Scene> loadAll(QStringList *errors = nullptr) const;
    bool save(const Scene &scene, QString *error = nullptr) const;
    bool remove(const QString &name, QString *error = nullptr) const;
    QString fileFor(const QString &name) const;

    static bool writeFile(const QString &path, const QString &text, QString *error);
    static QString readFile(const QString &path, QString *error);

private:
    QString m_dir;
};

} // namespace rostrum
