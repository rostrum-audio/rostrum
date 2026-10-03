#pragma once

#include "core/SceneHistory.h"

#include <QObject>
#include <QTimer>
#include <QtQml/qqmlregistration.h>

class QQmlEngine;
class QJSEngine;

namespace rostrum::app {

class AppController;

// Undo and redo of level and structure edits to the live scene, for this session only. A burst
// of changes (a fader drag) becomes one step once nothing has changed for a moment. Loading
// another scene starts a fresh history.
class History : public QObject
{
    Q_OBJECT
    QML_ELEMENT
    QML_SINGLETON

    Q_PROPERTY(bool canUndo READ canUndo NOTIFY changed)
    Q_PROPERTY(bool canRedo READ canRedo NOTIFY changed)
    // "Undo Game Volume", or plain "Undo" when there is nothing to undo.
    Q_PROPERTY(QString undoText READ undoText NOTIFY changed)
    Q_PROPERTY(QString redoText READ redoText NOTIFY changed)

public:
    static constexpr int kIdleMs = 500;

    History(AppController *app, QObject *parent);
    ~History() override;

    static History *create(QQmlEngine *, QJSEngine *);

    bool canUndo() const;
    bool canRedo() const;
    QString undoText() const;
    QString redoText() const;

    Q_INVOKABLE void undo();
    Q_INVOKABLE void redo();

    static QString describe(const SceneHistory::Change &change);

Q_SIGNALS:
    void changed();

private:
    void commit();
    void onCurrentChanged();
    void apply(const std::optional<Scene> &step);

    static History *s_instance;

    AppController *m_app;
    SceneHistory m_history;
    QTimer m_idle;
    QString m_current;
    bool m_applying = false;
};

} // namespace rostrum::app
