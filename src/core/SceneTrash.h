#pragma once

#include "core/Model.h"

#include <QDateTime>
#include <QList>
#include <QString>

#include <optional>

namespace rostrum {

// Deleted scenes, kept as scene files named <UTC time>-<slug>.toml so a delete can be undone.
// The time in the name, not the file's mtime, decides when an entry is purged.
class SceneTrash
{
public:
    static constexpr int kKeepDays = 30;

    struct Entry
    {
        QString file;
        QString name;
        QDateTime deleted;
    };

    explicit SceneTrash(QString dir);

    QString dir() const { return m_dir; }
    // Returns the trash file, or an empty string with *error set.
    QString put(const Scene &scene, const QDateTime &now, QString *error = nullptr) const;
    QList<Entry> entries() const; // newest first; unreadable files are left alone and skipped
    // Reads the scene back and deletes its trash file.
    std::optional<Scene> take(const QString &file, QString *error = nullptr) const;
    int purge(const QDateTime &now, int keepDays = kKeepDays) const;

private:
    QString m_dir;
};

} // namespace rostrum
