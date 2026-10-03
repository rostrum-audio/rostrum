#include "app/History.h"

#include "app/AppController.h"

#include <KLocalizedString>

#include <QJSEngine>

namespace rostrum::app {

History *History::s_instance = nullptr;

History::History(AppController *app, QObject *parent) : QObject(parent), m_app(app)
{
    Q_ASSERT(!s_instance);
    s_instance = this;
    m_idle.setSingleShot(true);
    m_idle.setInterval(kIdleMs);
    connect(&m_idle, &QTimer::timeout, this, &History::commit);
    connect(app->engine(), &engine::Engine::sceneChanged, this, [this] {
        if (!m_applying) {
            const bool pending = m_idle.isActive();
            m_idle.start();
            if (!pending) {
                Q_EMIT changed();
            }
        }
    });
    connect(app->scenes(), &engine::SceneManager::currentChanged, this, &History::onCurrentChanged);
    // Renaming the live scene keeps its mix, so it keeps its history too.
    connect(app->scenes(), &engine::SceneManager::renamed, this,
            [this](const QString &from, const QString &to) {
                if (from == m_current) {
                    m_current = to;
                }
            });
    m_current = app->scenes()->currentName();
    m_history.reset(app->engine()->scene());
}

History::~History()
{
    s_instance = nullptr;
}

History *History::create(QQmlEngine *, QJSEngine *)
{
    QJSEngine::setObjectOwnership(s_instance, QJSEngine::CppOwnership);
    return s_instance;
}

void History::onCurrentChanged()
{
    if (m_app->scenes()->currentName() == m_current) {
        return;
    }
    m_current = m_app->scenes()->currentName();
    m_idle.stop();
    m_history.reset(m_app->engine()->scene());
    Q_EMIT changed();
}

void History::commit()
{
    m_idle.stop();
    m_history.record(m_app->engine()->scene());
    Q_EMIT changed();
}

bool History::canUndo() const
{
    return m_history.canUndo() || m_idle.isActive();
}

bool History::canRedo() const
{
    return m_history.canRedo() && !m_idle.isActive();
}

QString History::undoText() const
{
    if (m_idle.isActive()) {
        const auto change = m_history.pendingChange(m_app->engine()->scene());
        if (change.kind != SceneHistory::Kind::None) {
            return i18nc("@action:inmenu %1 is what gets undone", "Undo %1", describe(change));
        }
    }
    return m_history.canUndo()
               ? i18nc("@action:inmenu %1 is what gets undone", "Undo %1", describe(m_history.undoChange()))
               : i18nc("@action:inmenu", "Undo");
}

QString History::redoText() const
{
    return canRedo()
               ? i18nc("@action:inmenu %1 is what gets redone", "Redo %1", describe(m_history.redoChange()))
               : i18nc("@action:inmenu", "Redo");
}

void History::undo()
{
    if (m_idle.isActive()) {
        commit();
    }
    apply(m_history.undo(m_app->engine()->scene()));
}

void History::redo()
{
    if (m_idle.isActive()) {
        commit();
    }
    apply(m_history.redo(m_app->engine()->scene()));
}

void History::apply(const std::optional<Scene> &step)
{
    if (!step) {
        return;
    }
    m_applying = true;
    m_app->engine()->restoreScene(*step);
    m_applying = false;
    Q_EMIT changed();
}

QString History::describe(const SceneHistory::Change &change)
{
    using Kind = SceneHistory::Kind;
    switch (change.kind) {
    case Kind::Volume:
        return i18nc("@item undo step, %1 is a bus", "%1 Volume", change.bus);
    case Kind::Mute:
        return i18nc("@item undo step, %1 is a bus", "%1 Mute", change.bus);
    case Kind::Destination:
        return i18nc("@item undo step, %1 is a bus", "%1 Destination", change.bus);
    case Kind::PhonesMaster:
        return i18nc("@item undo step", "Headphones Master");
    case Kind::StreamMaster:
        return i18nc("@item undo step", "Stream Master");
    case Kind::Sidetone:
        return i18nc("@item undo step", "Sidetone");
    case Kind::AddBus:
        return i18nc("@item undo step, %1 is a bus", "Add Bus “%1”", change.bus);
    case Kind::RemoveBus:
        return i18nc("@item undo step, %1 is a bus", "Remove Bus “%1”", change.bus);
    case Kind::RenameBus:
        return i18nc("@item undo step, %1 is the new name", "Rename Bus to “%1”", change.bus);
    case Kind::BusColor:
        return i18nc("@item undo step, %1 is a bus", "%1 Color", change.bus);
    case Kind::BusOrder:
        return i18nc("@item undo step", "Bus Order");
    case Kind::AutoCategory:
        return i18nc("@item undo step, %1 is a bus", "%1 Automatic Apps", change.bus);
    case Kind::AppRules:
        return i18nc("@item undo step", "App Assignment");
    case Kind::Several:
    case Kind::None:
        break;
    }
    return i18nc("@item undo step", "Changes");
}

} // namespace rostrum::app
