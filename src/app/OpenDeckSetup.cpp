#include "app/OpenDeckSetup.h"

#include "app/AppController.h"

#include <KLocalizedString>
#include <QDesktopServices>
#include <QDir>
#include <QFileInfo>
#include <QJSEngine>
#include <QStandardPaths>
#include <QTimer>

namespace rostrum::app {
OpenDeckSetup *OpenDeckSetup::s_instance = nullptr;
OpenDeckSetup::OpenDeckSetup(AppController *app, QObject *parent) : QObject(parent), m_app(app)
{
    Q_ASSERT(!s_instance);
    s_instance = this;
    connect(app, &AppController::settingsChanged, this, &OpenDeckSetup::refresh);
    refresh();
}
OpenDeckSetup::~OpenDeckSetup()
{
    s_instance = nullptr;
}
OpenDeckSetup *OpenDeckSetup::create(QQmlEngine *, QJSEngine *)
{
    QJSEngine::setObjectOwnership(s_instance, QJSEngine::CppOwnership);
    return s_instance;
}
bool OpenDeckSetup::custom() const
{
    return !m_app->settings().openDeckPluginsFolder.isEmpty();
}
void OpenDeckSetup::refresh()
{
    m_targets = opendeck::findTargets({}, {}, m_app->settings().openDeckPluginsFolder);
    m_target = opendeck::selectTarget(m_targets, m_app->settings().openDeckInstallation);
    m_state = opendeck::inspect(m_target);
    m_python = !QStandardPaths::findExecutable(QStringLiteral("python3")).isEmpty();
    Q_EMIT changed();
}
QStringList OpenDeckSetup::installations() const
{
    QStringList result{i18nc("@item:inlistbox", "Choose an OpenDeck installation")};
    for (const auto &target : m_targets)
        result << (target.id == QLatin1String("native") ? i18nc("@item:inlistbox", "Native OpenDeck")
                   : target.id == QLatin1String("flatpak")
                       ? i18nc("@item:inlistbox", "Flatpak OpenDeck")
                       : i18nc("@item:inlistbox", "Custom plugins folder"));
    return result;
}
int OpenDeckSetup::selectedIndex() const
{
    for (qsizetype i = 0; i < m_targets.size(); ++i)
        if (m_targets[i].plugins == m_target.plugins)
            return int(i + 1);
    return 0;
}
QString OpenDeckSetup::status() const
{
    if (custom() && (!QDir::isAbsolutePath(m_target.plugins) || !QFileInfo(m_target.plugins).isDir()))
        return i18nc("@info",
                     "The saved plugins folder is missing: %1. Choose a folder or clear the custom location.",
                     m_target.plugins);
    if (needsChoice())
        return i18nc("@info",
                     "Choose the OpenDeck installation to use. Rostrum will install only its own plugin.");
    if (m_state == opendeck::State::NotFound)
        return i18nc("@info", "OpenDeck not found. Install and open OpenDeck, or choose its plugins folder.");
    if (!m_python)
        return i18nc("@info",
                     "Python 3 was not found. Install python3 before installing the Rostrum plugin.");
    switch (m_state) {
    case opendeck::State::Absent:
        return i18nc("@info", "Plugin absent. Install the bundled Rostrum plugin into %1.", m_target.plugins);
    case opendeck::State::Matches:
        return i18nc("@info",
                     "The plugin matches this release. Restart OpenDeck after installing or updating it, "
                     "then drag a Rostrum action onto a key. Plugins folder: %1",
                     m_target.plugins);
    case opendeck::State::Newer:
        return i18nc("@info", "The bundled copy is newer. Update the Rostrum plugin in %1.",
                     m_target.plugins);
    case opendeck::State::Incomplete:
        return i18nc(
            "@info",
            "Plugin files are incomplete or differ from this release. Repair the Rostrum plugin in %1.",
            m_target.plugins);
    case opendeck::State::NotFound:
        break;
    }
    return {};
}
QString OpenDeckSetup::actionText() const
{
    switch (m_state) {
    case opendeck::State::NotFound:
        return i18nc("@action:button", "Get OpenDeck");
    case opendeck::State::Absent:
        return i18nc("@action:button", "Install");
    case opendeck::State::Newer:
        return i18nc("@action:button", "Update");
    case opendeck::State::Incomplete:
        return i18nc("@action:button", "Repair");
    case opendeck::State::Matches:
        return i18nc("@action:button", "Installed");
    }
    return {};
}
bool OpenDeckSetup::actionEnabled() const
{
    if (m_busy || needsChoice())
        return false;
    if (m_state == opendeck::State::NotFound)
        return true;
    return m_state != opendeck::State::Matches && m_python &&
           (!custom() || (QDir::isAbsolutePath(m_target.plugins) && QFileInfo(m_target.plugins).isDir()));
}
void OpenDeckSetup::selectInstallation(int index)
{
    if (m_busy || index < 1 || index > m_targets.size())
        return;
    m_app->settings().openDeckInstallation = m_targets[index - 1].id;
    m_feedback.clear();
    m_app->saveSettingsSoon();
    Q_EMIT m_app->settingsChanged();
}
void OpenDeckSetup::chooseFolder(const QUrl &folder)
{
    if (m_busy || !folder.isLocalFile())
        return;
    m_app->settings().openDeckPluginsFolder = QDir::cleanPath(folder.toLocalFile());
    m_feedback.clear();
    m_app->saveSettingsSoon();
    Q_EMIT m_app->settingsChanged();
}
void OpenDeckSetup::clearFolder()
{
    if (m_busy)
        return;
    m_app->settings().openDeckPluginsFolder.clear();
    m_app->settings().openDeckInstallation.clear();
    m_feedback.clear();
    m_app->saveSettingsSoon();
    Q_EMIT m_app->settingsChanged();
}
void OpenDeckSetup::activate()
{
    if (!actionEnabled())
        return;
    if (m_state == opendeck::State::NotFound) {
        if (!QDesktopServices::openUrl(QUrl(QStringLiteral("https://opendeck.nekename.me/")))) {
            m_feedback = i18nc("@info", "Could not open the OpenDeck website.");
            Q_EMIT changed();
        }
        return;
    }
    m_busy = true;
    m_feedback.clear();
    Q_EMIT changed();
    // Let the button repaint before the small local bundle is staged and validated.
    QTimer::singleShot(0, this, [this, target = m_target] {
        const auto result =
            opendeck::install(target, !QStandardPaths::findExecutable(QStringLiteral("python3")).isEmpty());
        switch (result.error) {
        case opendeck::Error::None:
            m_feedback =
                result.changed
                    ? i18nc("@info",
                            "Plugin installed. Restart OpenDeck, then drag a Rostrum action onto a key.")
                    : i18nc("@info", "The plugin already matches this release. Nothing was changed.");
            break;
        case opendeck::Error::ChooseTarget:
            m_feedback = i18nc("@info", "Choose an OpenDeck installation or plugins folder first.");
            break;
        case opendeck::Error::MissingFolder:
            m_feedback =
                i18nc("@info", "The plugins folder is missing or could not be created: %1.", result.path);
            break;
        case opendeck::Error::NoPython:
            m_feedback = i18nc("@info", "Python 3 was not found. Install python3 and try again.");
            break;
        case opendeck::Error::InvalidBundle:
            m_feedback = i18nc(
                "@info", "The bundled plugin could not be validated. The installed plugin was not replaced.");
            break;
        case opendeck::Error::UnsafeTarget:
            m_feedback = i18nc(
                "@info", "The plugin destination is a link or is not a folder: %1. It was not replaced.",
                result.path);
            break;
        case opendeck::Error::StageFailed:
            m_feedback = i18nc("@info",
                               "Could not prepare the plugin at %1. Check folder permissions. The installed "
                               "plugin was not replaced.",
                               result.path);
            break;
        case opendeck::Error::ReplaceFailed:
            m_feedback = i18nc("@info", "Could not replace the plugin at %1. The previous copy was kept.",
                               result.path);
            break;
        case opendeck::Error::RestoreFailed:
            m_feedback = i18nc("@info",
                               "Could not restore the previous plugin to its original location. Its files "
                               "were preserved at %1.",
                               result.path);
            break;
        }
        m_busy = false;
        refresh();
    });
}
} // namespace rostrum::app
