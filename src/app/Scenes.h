#pragma once

#include <QObject>
#include <QUrl>
#include <QVariantList>
#include <QtQml/qqmlregistration.h>

class QQmlEngine;
class QJSEngine;

namespace rostrum {
struct Scene;
}

namespace rostrum::app {

class AppController;

// Scene management for the Scenes page. Browsing never loads a scene; load() does.
class Scenes : public QObject
{
    Q_OBJECT
    QML_ELEMENT
    QML_SINGLETON

    // Rows: {name, summary, isDefault, isCurrent, slot, shortcut}. slot: the "Load scene N" hotkey
    // this scene answers to (its place in the order), 0 if none; shortcut: that key, or empty.
    Q_PROPERTY(QVariantList rows READ rows NOTIFY changed)
    // Rows: {id, name, description, icon}
    Q_PROPERTY(QVariantList presets READ presets CONSTANT)

public:
    Scenes(AppController *app, QObject *parent);
    ~Scenes() override;

    static Scenes *create(QQmlEngine *, QJSEngine *);

    QVariantList rows() const { return m_rows; }
    QVariantList presets() const;

    // Each returns the resulting scene name, or empty on failure (with a toast).
    Q_INVOKABLE QString createScene(const QString &name);
    // The live scene's buses and app rules with the preset's levels.
    Q_INVOKABLE QString createFromPreset(const QString &presetId, const QString &name);
    Q_INVOKABLE QString duplicate(const QString &name);
    Q_INVOKABLE QString rename(const QString &oldName, const QString &newName);
    Q_INVOKABLE QString saveAs(const QString &name);
    Q_INVOKABLE bool remove(const QString &name);
    Q_INVOKABLE void setDefault(const QString &name);
    // Reorders; next/previous and the scene hotkeys follow the order.
    Q_INVOKABLE bool move(const QString &name, int toIndex);
    Q_INVOKABLE bool moveBy(const QString &name, int delta);
    Q_INVOKABLE bool exportTo(const QUrl &file);
    Q_INVOKABLE int importFrom(const QUrl &file);
    Q_INVOKABLE QString uniqueName(const QString &base) const;
    // Empty when the name is usable, otherwise why not.
    Q_INVOKABLE QString nameProblem(const QString &name, const QString &except = QString()) const;

    static QString summary(const Scene &scene);

Q_SIGNALS:
    void changed();

private:
    void rebuild();

    static Scenes *s_instance;
    AppController *m_app = nullptr;
    QVariantList m_rows;
};

} // namespace rostrum::app
