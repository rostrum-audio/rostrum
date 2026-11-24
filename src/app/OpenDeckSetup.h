#pragma once
#include "core/OpenDeck.h"

#include <QObject>
#include <QUrl>
#include <QtQml/qqmlregistration.h>

class QQmlEngine;
class QJSEngine;
namespace rostrum::app {
class AppController;
class OpenDeckSetup : public QObject
{
    Q_OBJECT
    QML_ELEMENT
    QML_SINGLETON
    Q_PROPERTY(QString status READ status NOTIFY changed)
    Q_PROPERTY(QString actionText READ actionText NOTIFY changed)
    Q_PROPERTY(QString feedback READ feedback NOTIFY changed)
    Q_PROPERTY(QStringList installations READ installations NOTIFY changed)
    Q_PROPERTY(int selectedIndex READ selectedIndex NOTIFY changed)
    Q_PROPERTY(bool needsChoice READ needsChoice NOTIFY changed)
    Q_PROPERTY(bool custom READ custom NOTIFY changed)
    Q_PROPERTY(bool actionEnabled READ actionEnabled NOTIFY changed)
    Q_PROPERTY(bool busy READ busy NOTIFY changed)
public:
    OpenDeckSetup(AppController *app, QObject *parent);
    ~OpenDeckSetup() override;
    static OpenDeckSetup *create(QQmlEngine *, QJSEngine *);
    QString status() const;
    QString actionText() const;
    QString feedback() const { return m_feedback; }
    QStringList installations() const;
    int selectedIndex() const;
    bool needsChoice() const { return !m_targets.isEmpty() && m_target.plugins.isEmpty(); }
    bool custom() const;
    bool actionEnabled() const;
    bool busy() const { return m_busy; }
    Q_INVOKABLE void refresh();
    Q_INVOKABLE void selectInstallation(int index);
    Q_INVOKABLE void chooseFolder(const QUrl &folder);
    Q_INVOKABLE void clearFolder();
    Q_INVOKABLE void activate();
public Q_SLOTS:
    // Refreshes, then emits updateAvailable when an older plugin is installed and this bundle's
    // version has not been announced yet (opendeck::shouldNotify).
    void checkForUpdate();
Q_SIGNALS:
    void changed();
    void updateAvailable(const QString &bundled, const QString &installed);
    // Ends every activate(), once: succeeded when the installed plugin is now current, otherwise
    // feedback says why not. A press while an install runs is answered by that install's.
    void finished(bool succeeded, const QString &feedback);

private:
    static OpenDeckSetup *s_instance;
    AppController *m_app;
    QList<opendeck::Target> m_targets;
    opendeck::Target m_target;
    opendeck::State m_state = opendeck::State::NotFound;
    bool m_python = false;
    bool m_busy = false;
    QString m_feedback;
};
} // namespace rostrum::app
