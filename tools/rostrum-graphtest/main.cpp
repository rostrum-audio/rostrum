// rostrum-graphtest: headless driver for the manual graph tests in docs/manual-tests.md.
// It runs the same Engine as the app, without any UI.

#include "core/Paths.h"
#include "core/Settings.h"
#include "engine/Engine.h"
#include "engine/SceneManager.h"
#include "pw/PwContext.h"
#include "core/Volume.h"
#include "pw/MeterBank.h"
#include "pw/TestTone.h"

#include <QCommandLineParser>
#include <QCoreApplication>
#include <QTextStream>
#include <QTimer>

#include <csignal>

using namespace rostrum;

namespace {

void printState(const pw::PwContext &pw)
{
    QTextStream out(stdout);
    out << "Rostrum nodes:\n";
    for (const auto &n : pw.graph().nodes) {
        if (n.isRostrum() && (n.isSink() || n.isSource())) {
            out << "  " << n.id << "  " << n.name << "  \"" << n.description << "\"  " << n.mediaClass << "\n";
        }
    }
    out.flush();
}

} // namespace

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("rostrum-graphtest"));

    QCommandLineParser parser;
    parser.setApplicationDescription(QStringLiteral("Drive the Rostrum audio graph without the UI."));
    parser.addHelpOption();
    QCommandLineOption seconds(QStringLiteral("seconds"), QStringLiteral("Exit after N seconds (0 = run until Ctrl-C)."),
                               QStringLiteral("N"), QStringLiteral("0"));
    QCommandLineOption teardown(QStringLiteral("teardown"), QStringLiteral("Remove every Rostrum node and exit."));
    QCommandLineOption rule(QStringLiteral("rule"), QStringLiteral("Add an app rule, e.g. name:pw-play=game."),
                            QStringLiteral("key=bus"));
    QCommandLineOption session(QStringLiteral("session"),
                               QStringLiteral("Assign an app for this launch only, e.g. binary:pw-cat=voice."),
                               QStringLiteral("key=bus"));
    QCommandLineOption unassignAfter(QStringLiteral("unassign-after"),
                                     QStringLiteral("Unassign every app after N seconds."), QStringLiteral("N"));
    QCommandLineOption listApps(QStringLiteral("list-apps"), QStringLiteral("Print app streams once ready."));
    QCommandLineOption dest(QStringLiteral("dest"), QStringLiteral("Set a bus destination, e.g. game=phones."),
                            QStringLiteral("bus=phones|stream|both"));
    QCommandLineOption headphones(QStringLiteral("headphones"), QStringLiteral("Headphone sink node.name."),
                                  QStringLiteral("node"));
    QCommandLineOption mic(QStringLiteral("mic"), QStringLiteral("Mic source node.name."), QStringLiteral("node"));
    QCommandLineOption micMuted(QStringLiteral("mic-muted"), QStringLiteral("Start with the mic muted."));
    QCommandLineOption micFallback(QStringLiteral("mic-fallback"),
                                   QStringLiteral("Use another mic while the chosen one is missing."));
    QCommandLineOption sidetone(QStringLiteral("sidetone"), QStringLiteral("Sidetone volume 0..1 (turns it on)."),
                                QStringLiteral("v"));
    QCommandLineOption solo(QStringLiteral("solo"), QStringLiteral("Solo a bus (session only)."),
                            QStringLiteral("bus"));
    QCommandLineOption config(QStringLiteral("config"),
                              QStringLiteral("Apply the saved default scene and devices from $XDG_CONFIG_HOME/rostrum."));
    QCommandLineOption tone(QStringLiteral("tone"),
                            QStringLiteral("Play the test chime on a sink (node.name) and exit."),
                            QStringLiteral("sink"));
    QCommandLineOption meter(QStringLiteral("meter"),
                             QStringLiteral("Print the peak level of a node (node.name) four times a second."),
                             QStringLiteral("node"));
    parser.addOptions({seconds, teardown, rule, session, unassignAfter, listApps, dest, headphones, mic, micMuted,
                       micFallback, sidetone, solo, config, tone, meter});
    parser.process(app);

    pw::PwContext pw;
    if (!pw.start()) {
        QTextStream(stderr) << pw.errorString() << "\n";
        return 2;
    }
    engine::Engine engine(&pw);
    engine::SceneManager scenes(&engine, paths::scenesDir());
    if (parser.isSet(config)) {
        const Settings settings = loadSettings(paths::settingsFile());
        scenes.load(settings.defaultScene);
        QTextStream(stdout) << "Scene: " << scenes.currentName() << "\n";
        if (!parser.isSet(headphones)) {
            engine.setHeadphoneDevice(settings.headphones);
        }
        if (!parser.isSet(mic)) {
            engine.setMicDevice(settings.mic);
        }
        engine.setMicFallback(settings.micFallback);
        engine.setMonoHeadphones(settings.monoHeadphones);
    }
    if (parser.isSet(micFallback)) {
        engine.setMicFallback(true);
    }

    std::signal(SIGINT, [](int) { QCoreApplication::quit(); });
    std::signal(SIGTERM, [](int) { QCoreApplication::quit(); });

    bool started = false;
    QObject::connect(&pw, &pw::PwContext::stateChanged, &app, [&] {
        if (pw.state() == pw::PwContext::State::Failed) {
            QTextStream(stderr) << pw.errorString() << "\n";
            QCoreApplication::exit(2);
            return;
        }
        if (pw.state() != pw::PwContext::State::Ready || started) {
            return;
        }
        started = true;
        if (parser.isSet(tone)) {
            auto *player = new pw::TestTone(&pw, &app);
            if (!player->play(parser.value(tone))) {
                QTextStream(stderr) << "No sink called " << parser.value(tone) << "\n";
                QCoreApplication::exit(1);
                return;
            }
            QObject::connect(player, &pw::TestTone::playingChanged, &app, [player] {
                if (!player->isPlaying()) {
                    QCoreApplication::quit();
                }
            });
            return;
        }
        if (parser.isSet(meter)) {
            auto *bank = new pw::MeterBank(&pw, &app);
            const QString target = parser.value(meter);
            bank->setTargets({target});
            bank->setActive(true);
            auto *poll = new QTimer(&app);
            QObject::connect(poll, &QTimer::timeout, &app, [bank, target] {
                const float peak = bank->takePeak(target);
                const double db = volume::linearToDb(peak);
                QTextStream(stdout) << QStringLiteral("%1 dB %2\n")
                                           .arg(db < -99.0 ? QStringLiteral("  -inf") : QString::number(db, 'f', 1).rightJustified(6))
                                           .arg(QString(int(volume::meterFraction(peak) * 40), QLatin1Char('#')));
            });
            poll->start(250);
            if (const int s = parser.value(seconds).toInt(); s > 0) {
                QTimer::singleShot(s * 1000, &app, &QCoreApplication::quit);
            }
            return;
        }
        if (parser.isSet(teardown)) {
            engine.destroyMix();
            QTimer::singleShot(500, &app, [&] {
                printState(pw);
                QCoreApplication::quit();
            });
            return;
        }
        if (parser.isSet(headphones) || !parser.isSet(config)) {
            engine.setHeadphoneDevice(parser.value(headphones));
        }
        if (parser.isSet(mic) || !parser.isSet(config)) {
            engine.setMicDevice(parser.value(mic));
        }
        for (const auto &spec : parser.values(dest)) {
            const auto parts = spec.split(QLatin1Char('='));
            if (const auto d = destinationFromString(parts.value(1))) {
                engine.setBusDestination(parts.value(0), *d);
            }
        }
        if (parser.isSet(micMuted)) {
            engine.setMicMuted(true);
        }
        if (parser.isSet(sidetone)) {
            engine.setSidetoneEnabled(true);
            engine.setSidetoneVolume(parser.value(sidetone).toDouble());
        }
        for (const auto &bus : parser.values(solo)) {
            engine.setSolo(bus, true);
        }
        engine.createMix();
        auto apply = [&](const QStringList &specs, bool always) {
            for (const auto &spec : specs) {
                const auto eq = spec.lastIndexOf(QLatin1Char('='));
                engine.assignApp(AppKey::fromString(spec.left(eq)), spec.mid(eq + 1), always);
            }
        };
        apply(parser.values(rule), true);
        apply(parser.values(session), false);
        if (parser.isSet(unassignAfter)) {
            QTimer::singleShot(parser.value(unassignAfter).toInt() * 1000, &app, [&] {
                QTextStream(stdout) << "Unassigning every app.\n";
                for (const auto &s : engine.appStreams()) {
                    engine.unassignStream(s.nodeId);
                }
            });
        }
    });

    if (parser.isSet(listApps)) {
        QObject::connect(&engine, &engine::Engine::appsChanged, &app, [&] {
            static QString last;
            QString text;
            QTextStream out(&text);
            for (const auto &s : engine.appStreams()) {
                out << "  app " << s.nodeId << " \"" << s.identity.displayName << "\" key=" << s.identity.key.toString()
                    << " bus=" << (s.busId.isEmpty() ? QStringLiteral("-") : s.busId)
                    << " target.object=" << pw.metadataValue(s.nodeId, QStringLiteral("target.object")) << "\n";
            }
            if (text != last) {
                last = text;
                QTextStream(stdout) << "Apps:\n" << text;
            }
        });
    }

    QObject::connect(&engine, &engine::Engine::mixStateChanged, &app, [&] {
        if (engine.mixReady()) {
            QTextStream(stdout) << "Mix ready.\n";
            printState(pw);
        } else if (!engine.mixError().isEmpty()) {
            QTextStream(stderr) << "Mix error: " << engine.mixError() << "\n";
        }
    });

    QObject::connect(&engine, &engine::Engine::headphonesLost, &app, [&](const QString &desc) {
        QTextStream(stdout) << "Headphones disconnected (" << desc << "), scene held. Falling back to "
                            << engine.resolvedSinkName() << "\n";
    });
    QObject::connect(&engine, &engine::Engine::headphonesRestored, &app, [&] {
        QTextStream(stdout) << "Headphones back: " << engine.resolvedSinkName() << "\n";
    });
    QObject::connect(&engine, &engine::Engine::micLost, &app, [&](const QString &desc) {
        if (engine.micSilenced()) {
            QTextStream(stdout) << "Mic disconnected (" << desc << "), stream mic silent.\n";
        } else {
            QTextStream(stdout) << "Mic disconnected (" << desc << "). Falling back to "
                                << engine.resolvedSourceName() << "\n";
        }
    });
    QObject::connect(&engine, &engine::Engine::micRestored, &app, [&] {
        QTextStream(stdout) << "Mic back: " << engine.resolvedSourceName() << "\n";
    });

    if (const int s = parser.value(seconds).toInt(); s > 0) {
        QTimer::singleShot(s * 1000, &app, &QCoreApplication::quit);
    }
    return app.exec();
}
