#include "app/AppController.h"
#include "app/Apps.h"
#include "app/CrashReports.h"
#include "app/Desktop.h"
#include "app/Devices.h"
#include "app/Logging.h"
#include "app/Mixer.h"
#include "app/Obs.h"
#include "app/Preferences.h"
#include "app/Scenes.h"
#include "app/Updater.h"
#include "core/Paths.h"

#include <KAboutData>
#include <KDBusService>
#include <KLocalizedQmlContext>
#include <KLocalizedString>

#include <QApplication>
#include <QIcon>
#include <QProcess>
#include <QQmlApplicationEngine>
#include <QQuickStyle>
#include <QQuickWindow>
#include <QTimer>

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);
    rostrum::logging::install(rostrum::paths::logFile());

    KLocalizedString::setApplicationDomain("rostrum");
    KAboutData about(QStringLiteral("Rostrum"), i18n("Rostrum"), QStringLiteral(ROSTRUM_VERSION),
                     i18n("A stream mix console for Linux"), KAboutLicense::Apache_V2);
    about.setOrganizationDomain("getrostrum.dev");
    about.setDesktopFileName(QStringLiteral(ROSTRUM_APP_ID));
    about.setBugAddress("https://github.com/rostrum-audio/rostrum/issues");
    about.setHomepage(QStringLiteral("https://getrostrum.dev"));
    KAboutData::setApplicationData(about);
    QApplication::setWindowIcon(QIcon::fromTheme(QStringLiteral(ROSTRUM_APP_ID),
                                                 QIcon(QStringLiteral(":/icons/" ROSTRUM_APP_ID ".svg"))));
    // Closing the window hides to the tray when there is one; Main.qml decides.
    QApplication::setQuitOnLastWindowClosed(false);

    // One instance only: a second launch raises the running window and exits.
    KDBusService service(KDBusService::Unique);

    // Follow the Plasma style and icons; elsewhere fall back to Breeze rather than Basic.
    if (qEnvironmentVariableIsEmpty("QT_QUICK_CONTROLS_STYLE")) {
        QQuickStyle::setStyle(QStringLiteral("org.kde.desktop"));
    }
    QIcon::setFallbackThemeName(QStringLiteral("breeze"));

    rostrum::app::AppController controller(nullptr);
    QObject::connect(&service, &KDBusService::activateRequested, &controller,
                     &rostrum::app::AppController::raiseRequested);
    QObject::connect(&app, &QCoreApplication::aboutToQuit, &controller, [&controller] {
        controller.scenes()->flush();
        controller.saveSettingsNow();
    });
    controller.start();
    rostrum::app::Mixer mixer(&controller, nullptr);
    QObject::connect(&controller, &rostrum::app::AppController::settingsChanged, &mixer,
                     &rostrum::app::Mixer::notifySettingsChanged);
    rostrum::app::Apps apps(&controller, nullptr);
    rostrum::app::Scenes scenes(&controller, nullptr);
    rostrum::app::Devices devices(&controller, nullptr);
    // Before Desktop: the tray shows OBS's live state.
    rostrum::app::Obs obs(&controller, nullptr);
    rostrum::app::Desktop desktop(&controller, nullptr);
    rostrum::app::Preferences preferences(&controller, nullptr);
    rostrum::app::CrashReports crashReports(&controller, nullptr);
    crashReports.start();
    rostrum::app::Updater updater(&controller, nullptr);
    updater.start();
    // The new copy must not find this one still holding the single-instance name.
    QObject::connect(&updater, &rostrum::app::Updater::restartRequested, &app, [&](const QString &program) {
        controller.saveSettingsNow();
        service.unregister();
        if (!QProcess::startDetached(program, {})) {
            qWarning("Could not start %s", qPrintable(program));
        }
        QCoreApplication::quit();
    });

    QQmlApplicationEngine engine;
    KLocalization::setupLocalizedContext(&engine);
    engine.loadFromModule("Rostrum", "Main");
    if (engine.rootObjects().isEmpty()) {
        return 1;
    }
    desktop.setWindow(qobject_cast<QQuickWindow *>(engine.rootObjects().constFirst()));

    // Developer aid for docs: ROSTRUM_SCREENSHOT=out.png renders only Rostrum's own window
    // (works with QT_QPA_PLATFORM=offscreen) and quits.
    if (const QString shot = qEnvironmentVariable("ROSTRUM_SCREENSHOT"); !shot.isEmpty()) {
        auto *window = qobject_cast<QQuickWindow *>(engine.rootObjects().constFirst());
        QTimer::singleShot(3000, &app, [window, shot] {
            if (window && !window->grabWindow().save(shot)) {
                qWarning("Could not write %s", qPrintable(shot));
            }
            QCoreApplication::quit();
        });
    }
    return app.exec();
}
