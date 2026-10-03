#pragma once

#include "core/Model.h"
#include "core/SceneStore.h"
#include "core/SceneTrash.h"

#include <QObject>
#include <QTimer>

namespace rostrum::engine {

class Engine;

// Saved scenes, the current scene name and the dirty flag. Structure (bus names, colors, bus
// list, app rules) is written to disk right away. Levels (faders, mutes, destinations, masters)
// stay unsaved until Save, or, with auto-save on, are saved shortly after they stop changing.
class SceneManager : public QObject
{
    Q_OBJECT
public:
    SceneManager(Engine *engine, const QString &dir, QObject *parent = nullptr);

    // Loads every scene file. Creates and saves the default "Live" scene if there are none.
    void load(const QString &defaultName);

    // The user's order, which next/previous and the scene hotkey slots follow. Names not in the
    // list keep file-name order after the listed ones. New scenes go to the end.
    void setOrder(const QStringList &names);
    bool move(const QString &name, int toIndex);

    const QList<Scene> &scenes() const { return m_saved; }
    QStringList names() const;
    const Scene *saved(const QString &name) const;
    QString currentName() const { return m_current; }
    QString defaultName() const { return m_default; }
    bool dirty() const { return m_dirty; }
    QString lastError() const { return m_error; }

    bool switchTo(const QString &name);
    bool switchToIndex(int index);
    bool next();
    bool previous();

    // Off by default. On: level changes are saved after a short pause, and before a switch.
    void setAutoSave(bool on);
    bool autoSave() const { return m_autoSave; }
    // With auto-save on, writes pending level changes now (before quitting).
    void flush();

    bool save();
    bool saveAs(const QString &name);
    bool create(const QString &name); // a fresh scene with the default buses
    bool create(const Scene &scene);  // under a unique version of scene.name
    bool duplicate(const QString &name, const QString &newName = QString());
    bool rename(const QString &oldName, const QString &newName);
    bool remove(const QString &name);
    void setDefault(const QString &name);

    // With a trash folder, remove() moves the scene there instead of deleting it, and entries
    // older than SceneTrash::kKeepDays are purged. Off until called; tests and tools leave it off.
    void setTrashDir(const QString &dir);
    QList<SceneTrash::Entry> trash() const;
    QString lastTrashed() const { return m_lastTrashed.file; }
    // Brings a trashed scene back under a unique name: the last deleted one at its old place (and
    // as default again if it was), others at the end. Returns the name, empty on failure.
    QString restore(const QString &trashFile);

    bool exportTo(const QString &path);
    int importFrom(const QString &path); // returns scenes imported, -1 on error
    // From a settings backup: replaces scenes of the same name (the old version goes to the trash,
    // if there is one) and adds the rest. The live scene reloads if it was replaced. Returns the
    // number written.
    int restoreScenes(const QList<Scene> &scenes);

    QString uniqueName(const QString &base) const;

    // Keeps the PipeWire rule fragments in step with the default scene's app rules.
    // Off until called; tests and tools leave it off.
    void enableRuleExport(const QString &pulseFragment, const QString &clientFragment);
    void exportRules();

Q_SIGNALS:
    void scenesChanged();
    void currentChanged();
    void renamed(const QString &from, const QString &to); // before currentChanged, if it was current
    void dirtyChanged();
    void defaultChanged();
    void trashChanged();
    void errorOccurred(const QString &message);

private:
    void onEngineSceneChanged();
    void onStructureChanged();
    bool write(const Scene &scene);
    int indexOf(const QString &name) const;
    void fail(const QString &message);

    Engine *m_engine = nullptr;
    SceneStore m_store;
    QList<Scene> m_saved;
    QString m_current;
    QString m_default;
    bool m_dirty = false;
    bool m_autoSave = false;
    QTimer m_autoSaveTimer;
    QString m_error;
    QString m_pulseFragment;
    QString m_clientFragment;
    QStringList m_order;
    std::optional<SceneTrash> m_trash;
    struct Trashed
    {
        QString file;
        int index = -1;
        bool wasDefault = false;
    } m_lastTrashed;
};

} // namespace rostrum::engine
