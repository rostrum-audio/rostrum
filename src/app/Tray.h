#pragma once

#include <QObject>
#include <QPointer>

class KStatusNotifierItem;
class QAction;
class QMenu;
class QWindow;

namespace rostrum::app {

class AppController;

// The StatusNotifierItem Plasma hosts: show/hide, mute mic and stream, previous and next scene, a
// scene submenu, Restart, Quit. Middle-click mutes the mic and the wheel moves the Stream master.
// The muted icon is a separate drawing with a badge, so the state does not rely on color.
class Tray : public QObject
{
    Q_OBJECT
public:
    Tray(AppController *app, QObject *parent = nullptr);
    ~Tray() override;

    void setWindow(QWindow *window);

private:
    void updateState();
    void rebuildScenes();

    AppController *m_app = nullptr;
    KStatusNotifierItem *m_item = nullptr;
    QMenu *m_menu = nullptr;
    QMenu *m_scenesMenu = nullptr;
    QAction *m_show = nullptr;
    QAction *m_mute = nullptr;
    QAction *m_muteStream = nullptr;
    QAction *m_previous = nullptr;
    QAction *m_next = nullptr;
    QPointer<QWindow> m_window;
    bool m_lastMuted = false;
    bool m_iconSet = false;
};

} // namespace rostrum::app
