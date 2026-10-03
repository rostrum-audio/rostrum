#include "app/Tray.h"

#include "app/AppController.h"
#include "app/Obs.h"

#include <KLocalizedString>
#include <KStatusNotifierItem>
#include <QActionGroup>
#include <QDateTime>
#include <QIcon>
#include <QLocale>
#include <QMenu>
#include <QWindow>

namespace rostrum::app {

namespace {
QIcon trayIcon(bool muted)
{
    return QIcon(muted ? QStringLiteral(":/icons/" ROSTRUM_APP_ID "-tray-muted.svg")
                       : QStringLiteral(":/icons/" ROSTRUM_APP_ID "-tray.svg"));
}
} // namespace

Tray::Tray(AppController *app, QObject *parent) : QObject(parent), m_app(app)
{
    m_item = new KStatusNotifierItem(QStringLiteral(ROSTRUM_APP_ID), this);
    m_item->setCategory(KStatusNotifierItem::ApplicationStatus);
    m_item->setStatus(KStatusNotifierItem::Active);
    m_item->setTitle(i18n("Rostrum"));
    m_item->setToolTipTitle(i18n("Rostrum"));
    // Our own Quit saves settings first; the built-in one would just exit.
    m_item->setStandardActionsEnabled(false);

    m_menu = new QMenu();
    m_show = m_menu->addAction(QIcon::fromTheme(QStringLiteral("window")), QString());
    connect(m_show, &QAction::triggered, this, [this] {
        if (m_window && m_window->isVisible()) {
            m_window->hide();
        } else {
            Q_EMIT m_app->raiseRequested();
        }
    });
    m_menu->addSeparator();
    m_mute = m_menu->addAction(QString());
    connect(m_mute, &QAction::triggered, m_app, &AppController::toggleMicMute);
    m_next =
        m_menu->addAction(QIcon::fromTheme(QStringLiteral("go-next")), i18nc("@action:inmenu", "Next Scene"));
    connect(m_next, &QAction::triggered, this,
            [this] { m_app->triggerAction(QString::fromLatin1(actions::kNextScene)); });
    m_scenesMenu = m_menu->addMenu(QIcon::fromTheme(QStringLiteral("view-media-playlist")),
                                   i18nc("@title:menu", "Scenes"));
    m_menu->addSeparator();
    QAction *quit = m_menu->addAction(QIcon::fromTheme(QStringLiteral("application-exit")),
                                      i18nc("@action:inmenu", "Quit"));
    connect(quit, &QAction::triggered, m_app, &AppController::quit);
    m_item->setContextMenu(m_menu);

    connect(m_app, &AppController::levelsChanged, this, &Tray::updateState);
    connect(m_app, &AppController::devicesChanged, this, &Tray::updateState);
    connect(m_app, &AppController::statusChanged, this, &Tray::updateState);
    connect(m_app, &AppController::scenesChanged, this, &Tray::updateState);
    connect(m_app, &AppController::scenesChanged, this, &Tray::rebuildScenes);
    connect(m_menu, &QMenu::aboutToShow, this, &Tray::updateState);
    if (Obs::instance()) {
        connect(Obs::instance(), &Obs::liveChanged, this, &Tray::updateState);
    }
    updateState();
    rebuildScenes();
}

Tray::~Tray()
{
    delete m_menu;
}

void Tray::setWindow(QWindow *window)
{
    m_window = window;
    m_item->setAssociatedWindow(window);
    connect(window, &QWindow::visibleChanged, this, &Tray::updateState);
    updateState();
}

void Tray::updateState()
{
    const bool muted = m_app->micMuted();
    if (!m_iconSet || muted != m_lastMuted) {
        m_item->setIconByPixmap(trayIcon(muted));
        m_item->setToolTipIconByPixmap(trayIcon(muted));
        m_lastMuted = muted;
        m_iconSet = true;
    }
    const QString mic = !m_app->connected() ? i18nc("@info:tooltip", "PipeWire missing")
                        : !m_app->hasMic() ? i18nc("@info:tooltip", "No mic")
                        : muted          ? i18nc("@info:tooltip", "Mic muted")
                                         : i18nc("@info:tooltip", "Mic live");
    // Levels change many times a second while a fader moves; only talk to the tray host on news.
    QString tip = i18nc("@info:tooltip mic state, scene name", "%1 · Scene: %2", mic, m_app->currentScene());
    // A start time, not a running clock: the tray host is only told about changes.
    if (const Obs *obs = Obs::instance()) {
        const auto since = [](double ms) {
            return QLocale().toString(QDateTime::fromMSecsSinceEpoch(qint64(ms)).time(),
                                      QLocale::ShortFormat);
        };
        if (obs->recording()) {
            tip = obs->recordPaused() ? i18nc("@info:tooltip recording paused, rest", "REC paused · %1", tip)
                                      : i18nc("@info:tooltip recording since time, rest", "REC since %1 · %2",
                                              since(obs->recordStartMs()), tip);
        }
        if (obs->streaming()) {
            tip = i18nc("@info:tooltip streaming since time, rest", "LIVE since %1 · %2",
                        since(obs->streamStartMs()), tip);
        }
    }
    if (tip != m_item->toolTipSubTitle()) {
        m_item->setToolTipSubTitle(tip);
    }

    m_show->setText(m_window && m_window->isVisible() ? i18nc("@action:inmenu", "Hide Rostrum")
                                                      : i18nc("@action:inmenu", "Show Rostrum"));
    m_mute->setText(muted ? i18nc("@action:inmenu", "Unmute Mic") : i18nc("@action:inmenu", "Mute Mic"));
    m_mute->setIcon(QIcon::fromTheme(muted ? QStringLiteral("microphone-sensitivity-high")
                                           : QStringLiteral("microphone-sensitivity-muted")));
    m_mute->setEnabled(m_app->hasMic());
    const bool scenes = m_app->sceneNames().size() > 1;
    m_next->setEnabled(scenes && m_app->connected());
    m_scenesMenu->setEnabled(m_app->connected());
}

void Tray::rebuildScenes()
{
    m_scenesMenu->clear();
    qDeleteAll(m_scenesMenu->findChildren<QActionGroup *>(Qt::FindDirectChildrenOnly));
    auto *group = new QActionGroup(m_scenesMenu);
    for (const QString &name : m_app->sceneNames()) {
        QAction *a = m_scenesMenu->addAction(name);
        a->setCheckable(true);
        a->setChecked(name == m_app->currentScene());
        group->addAction(a);
        connect(a, &QAction::triggered, this, [this, name] { m_app->requestSceneSwitch(name); });
    }
}

} // namespace rostrum::app
