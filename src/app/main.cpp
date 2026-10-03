#include "app/AppController.h"
#include "app/Logging.h"
#include "core/Paths.h"

#include <KAboutData>
#include <KDBusService>
#include <KLocalizedQmlContext>
#include <KLocalizedString>

#include <QApplication>
#include <QIcon>
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
    about.setOrganizationDomain("rostrum_audio.github.io");
    about.setDesktopFileName(QStringLiteral(ROSTRUM_APP_ID));
    about.setBugAddress("https://github.com/rostrum-audio/rostrum/issues");
    KAboutData::setApplicationData(about);
    QApplication::setWindowIcon(QIcon::fromTheme(QStringLiteral(ROSTRUM_APP_ID),
                                                 QIcon::fromTheme(QStringLiteral("audio-card"))));

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
    QObject::connect(&app, &QCoreApplication::aboutToQuit, &controller,
                     &rostrum::app::AppController::saveSettingsNow);
    controller.start();

    QQmlApplicationEngine engine;
    KLocalization::setupLocalizedContext(&engine);
    engine.loadFromModule("Rostrum", "Main");
    if (engine.rootObjects().isEmpty()) {
        return 1;
    }

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
