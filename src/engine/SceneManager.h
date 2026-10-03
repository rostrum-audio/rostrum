#pragma once

#include "core/Model.h"
#include "core/SceneStore.h"

#include <QObject>

namespace rostrum::engine {

class Engine;

// Saved scenes, the current scene name and the dirty flag. Fader moves stay unsaved until
// Save; structure (bus names, colors, bus list, app rules) is written to disk right away.
class SceneManager : public QObject
{
    Q_OBJECT
public:
    SceneManager(Engine *engine, const QString &dir, QObject *parent = nullptr);

    // Loads every scene file. Creates and saves the default "Live" scene if there are none.
    void load(const QString &defaultName);

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

    bool save();
    bool saveAs(const QString &name);
    bool create(const QString &name); // a fresh scene with the default buses
    bool duplicate(const QString &name, const QString &newName = QString());
    bool rename(const QString &oldName, const QString &newName);
    bool remove(const QString &name);
    void setDefault(const QString &name);

    bool exportTo(const QString &path);
    int importFrom(const QString &path); // returns scenes imported, -1 on error

    QString uniqueName(const QString &base) const;

    // Keeps the PipeWire rule fragments in step with the default scene's app rules.
    // Off until called; tests and tools leave it off.
    void enableRuleExport(const QString &pulseFragment, const QString &clientFragment);
    void exportRules();

Q_SIGNALS:
    void scenesChanged();
    void currentChanged();
    void dirtyChanged();
    void defaultChanged();
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
    QString m_error;
    QString m_pulseFragment;
    QString m_clientFragment;
};

} // namespace rostrum::engine
