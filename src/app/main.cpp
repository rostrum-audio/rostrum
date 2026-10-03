#include "app/AppController.h"
#include "app/Apps.h"
#include "app/Cli.h"
#include "app/CrashReports.h"
#include "app/DBusControl.h"
#include "app/Desktop.h"
#include "app/Devices.h"
#include "app/History.h"
#include "app/Logging.h"
#include "app/MicCheck.h"
#include "app/MicFilters.h"
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
#include <QCommandLineParser>
#include <QIcon>
#include <QProcess>
#include <QQmlApplicationEngine>
#include <QQuickStyle>
#include <QQuickWindow>
#include <QTimer>
#include <memory>

int main(int argc, char *argv[])
{
    // Help, version and the lists print and exit, so they don't need a display.
    std::unique_ptr<QCoreApplication> app = rostrum::app::cli::needsGui(argc, argv)
                                                ? std::make_unique<QApplication>(argc, argv)
                                                : std::make_unique<QCoreApplication>(argc, argv);

    KLocalizedString::setApplicationDomain("rostrum");
    KAboutData about(QStringLiteral("Rostrum"), i18n("Rostrum"), QStringLiteral(ROSTRUM_VERSION),
                     i18n("A stream mix console for Linux"), KAboutLicense::Apache_V2);
    about.setOrganizationDomain("getrostrum.dev");
    about.setDesktopFileName(QStringLiteral(ROSTRUM_APP_ID));
    about.setBugAddress("https://github.com/rostrum-audio/rostrum/issues");
    about.setHomepage(QStringLiteral("https://getrostrum.dev"));
    KAboutData::setApplicationData(about);

    QCommandLineParser parser;
    about.setupCommandLine(&parser);
    rostrum::app::cli::addOptions(parser);
    parser.process(*app);
    about.processCommandLine(&parser);
    const auto request = rostrum::app::cli::parse(parser);
    for (const QString &e : request.errors) {
        std::fprintf(stderr, "rostrum: %s\n", qPrintable(e));
    }
    if (!request.errors.isEmpty()) {
        return rostrum::app::cli::kExitRefused;
    }
    // A restart: the old copy still holds the single-instance name and its PipeWire links until it
    // has exited.
    if (const QString pid = parser.value(QString::fromLatin1(rostrum::app::cli::kRestartAfter)); !pid.isEmpty()) {
        if (!rostrum::app::cli::waitForExit(pid.toLongLong(), 10000)) {
            qWarning("Rostrum %s is still running", qPrintable(pid));
        }
    }
    if (request.hasControls() || request.hasQueries()) {
        if (rostrum::app::cli::instanceRunning()) {
            return rostrum::app::cli::forward(request);
        }
        if (!request.hasControls()) {
            return rostrum::app::cli::answerOffline(request);
        }
    }

    rostrum::logging::install(rostrum::paths::logFile());
    QApplication::setWindowIcon(QIcon::fromTheme(QStringLiteral(ROSTRUM_APP_ID),
                                                 QIcon(QStringLiteral(":/icons/" ROSTRUM_APP_ID ".svg"))));
    // Closing the window hides to the tray when there is one; Main.qml decides.
    QApplication::setQuitOnLastWindowClosed(false);

    // One instance only: a second plain launch raises the running window and exits. Command line
    // options normally reach it over D-Bus above; if two launches race, they arrive here instead.
    KDBusService service(KDBusService::Unique);
    if (service.serviceName() != QLatin1String(ROSTRUM_APP_ID)) {
        qWarning("Registered as %s, but the command line looks for %s", qPrintable(service.serviceName()),
                 ROSTRUM_APP_ID);
    }

    // Follow the Plasma style and icons; elsewhere fall back to Breeze rather than Basic.
    if (qEnvironmentVariableIsEmpty("QT_QUICK_CONTROLS_STYLE")) {
        QQuickStyle::setStyle(QStringLiteral("org.kde.desktop"));
    }
    QIcon::setFallbackThemeName(QStringLiteral("breeze"));

    rostrum::app::AppController controller(nullptr);
    rostrum::app::DBusControl control(&controller, nullptr);
    QObject::connect(&service, &KDBusService::activateRequested, &controller,
                     [&controller, &control](const QStringList &arguments, const QString &) {
                         QCommandLineParser forwarded;
                         rostrum::app::cli::addOptions(forwarded);
                         forwarded.parse(arguments);
                         const auto r = rostrum::app::cli::parse(forwarded);
                         if (r.hasControls() && r.errors.isEmpty()) {
                             rostrum::app::cli::apply(r, control);
                         } else {
                             Q_EMIT controller.raiseRequested();
                         }
                     });
    QObject::connect(app.get(), &QCoreApplication::aboutToQuit, &controller, [&controller] {
        controller.engine()->releaseHolds();
        controller.scenes()->flush();
        controller.saveSettingsNow();
        const bool levels = controller.engine()->releaseTransientLevels();
        if (controller.engine()->releaseAppMutes() || levels) {
            controller.pw()->roundtrip(500);
        }
    });
    controller.start();
    control.registerOn(QDBusConnection::sessionBus());
    rostrum::app::cli::apply(request, control);
    rostrum::app::Mixer mixer(&controller, nullptr);
    QObject::connect(&controller, &rostrum::app::AppController::settingsChanged, &mixer,
                     &rostrum::app::Mixer::notifySettingsChanged);
    rostrum::app::Apps apps(&controller, nullptr);
    rostrum::app::Scenes scenes(&controller, nullptr);
    rostrum::app::History history(&controller, nullptr);
    rostrum::app::Devices devices(&controller, nullptr);
    rostrum::app::MicFilters micFilters(&controller, nullptr);
    rostrum::app::MicCheck micCheck(&controller, nullptr);
    // Before Desktop: the tray shows OBS's live state.
    rostrum::app::Obs obs(&controller, nullptr);
    rostrum::app::Desktop desktop(&controller, nullptr);
    rostrum::app::Preferences preferences(&controller, nullptr);
    rostrum::app::CrashReports crashReports(&controller, nullptr);
    crashReports.start();
    rostrum::app::Updater updater(&controller, nullptr);
    updater.start();
    // A restart quits normally and starts the new copy at the very end; the new copy also waits for
    // this process to exit, so the two never hold the name or the graph at once.
    QString restartProgram;
    QStringList restartArguments;
    const auto restartLater = [&](const QString &program, bool hidden) {
        controller.saveSettingsNow();
        restartProgram = program;
        restartArguments = rostrum::app::cli::restartArguments(hidden);
        QCoreApplication::quit();
    };
    QObject::connect(&updater, &rostrum::app::Updater::restartRequested, app.get(),
                     [&](const QString &program) { restartLater(program, false); });
    QObject::connect(&controller, &rostrum::app::AppController::restartRequested, app.get(),
                     [&](bool hidden) { restartLater(rostrum::app::cli::restartProgram(), hidden); });

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
        QTimer::singleShot(3000, app.get(), [window, shot] {
            if (window && !window->grabWindow().save(shot)) {
                qWarning("Could not write %s", qPrintable(shot));
            }
            QCoreApplication::quit();
        });
    }
    const int status = app->exec();
    if (!restartProgram.isEmpty()) {
        service.unregister();
        if (!QProcess::startDetached(restartProgram, restartArguments)) {
            qWarning("Could not start %s", qPrintable(restartProgram));
        }
    }
    return status;
}
