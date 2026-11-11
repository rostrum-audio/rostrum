// rostrum-graphtest: headless driver for the manual graph tests in docs/manual-tests.md.
// It runs the same Engine as the app, without any UI.

#include "core/Paths.h"
#include "core/RuleExport.h"
#include "core/Settings.h"
#include "core/Volume.h"
#include "engine/Engine.h"
#include "engine/SceneManager.h"
#include "obs/CaptureWatch.h"
#include "obs/ObsCaptures.h"
#include "obs/ObsConfig.h"
#include "obs/Readiness.h"
#include "pw/MeterBank.h"
#include "pw/PwContext.h"
#include "pw/TestTone.h"

#include <QCommandLineParser>
#include <QCoreApplication>
#include <QDateTime>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
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
    QCommandLineOption exportRules(
        QStringLiteral("export-rules"),
        QStringLiteral("Write the PipeWire rule fragments for the --rule rules under $XDG_CONFIG_HOME/pipewire."));
    QCommandLineOption dest(QStringLiteral("dest"), QStringLiteral("Set a bus destination, e.g. game=phones."),
                            QStringLiteral("bus=phones|stream|both"));
    QCommandLineOption destinationFile(QStringLiteral("destination-file"),
                                       QStringLiteral("Poll a test command file: request-id bus=phones|stream|both."),
                                       QStringLiteral("path"));
    QCommandLineOption readinessFile(
        QStringLiteral("readiness-file"),
        QStringLiteral("Write read-only readiness snapshots for integration tests."), QStringLiteral("path"));
    QCommandLineOption headphones(QStringLiteral("headphones"), QStringLiteral("Headphone sink node.name."),
                                  QStringLiteral("node"));
    QCommandLineOption mic(QStringLiteral("mic"), QStringLiteral("Mic source node.name."), QStringLiteral("node"));
    QCommandLineOption micMuted(QStringLiteral("mic-muted"), QStringLiteral("Start with the mic muted."));
    QCommandLineOption micFilterPlugin(QStringLiteral("mic-filter-plugin"),
        QStringLiteral("Load mic filters from this DSP library, using saved filter settings."), QStringLiteral("path"));
    QCommandLineOption micFilterDenoise(QStringLiteral("mic-filter-denoise"),
        QStringLiteral("Include the RNNoise stage (the supplied plugin must support it)."));
    QCommandLineOption micFiltersUnavailable(
        QStringLiteral("mic-filters-unavailable"),
        QStringLiteral("Use saved filter settings with no DSP library, as when the plugin fails to load."));
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
    QCommandLineOption reconnectFile(
        QStringLiteral("reconnect-file"),
        QStringLiteral("After losing PipeWire, reconnect once this test command file holds a new request id, "
                       "as the app retries."),
        QStringLiteral("path"));
    QCommandLineOption micFilterFile(
        QStringLiteral("mic-filter-file"),
        QStringLiteral("Poll a test command file: request-id settings|unavailable|available. 'settings' reloads the "
                       "saved filter settings; the others make the filter library fail or load."),
        QStringLiteral("path"));
    QCommandLineOption captureLostFile(
        QStringLiteral("capture-lost-file"),
        QStringLiteral("Write capture-lost state for integration tests (same CaptureWatch logic as the app)."),
        QStringLiteral("path"));
    QCommandLineOption meter(QStringLiteral("meter"),
                             QStringLiteral("Print the peak level of a node (node.name) four times a second."),
                             QStringLiteral("node"));
    parser.addOptions({seconds,
                       teardown,
                       rule,
                       session,
                       unassignAfter,
                       listApps,
                       exportRules,
                       dest,
                       destinationFile,
                       readinessFile,
                       headphones,
                       mic,
                       micMuted,
                       micFallback,
                       sidetone,
                       solo,
                       config,
                       tone,
                       meter,
                       micFilterPlugin,
                       micFilterDenoise,
                       micFiltersUnavailable,
                       micFilterFile,
                       reconnectFile,
                       captureLostFile});
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
        engine.setSceneFadeMs(settings.sceneFadeMs);
        engine.setDucking(settings.ducking);
    }
    if (parser.isSet(micFallback)) {
        engine.setMicFallback(true);
    }
    if (parser.isSet(micFilterPlugin)) {
        engine.setMicFilterPlugin(parser.value(micFilterPlugin), parser.isSet(micFilterDenoise), {});
        engine.setMicFilters(loadSettings(paths::settingsFile()).micFilters);
    } else if (parser.isSet(micFiltersUnavailable)) {
        engine.setMicFilterPlugin({}, false, QStringLiteral("The mic filter library failed to load (test)."));
        engine.setMicFilters(loadSettings(paths::settingsFile()).micFilters);
    }

    std::signal(SIGINT, [](int) { QCoreApplication::quit(); });
    std::signal(SIGTERM, [](int) { QCoreApplication::quit(); });

    if (parser.isSet(readinessFile)) {
        auto publish = [&] {
            obs::ReadinessInput input;
            input.scene = engine.scene();
            input.graph = &pw.graph();
            input.mutes = {engine.effectiveMicMuted(), engine.effectiveStreamMuted()};
            input.selectedMic = engine.micDevice();
            input.resolvedMic = engine.resolvedSourceName();
            input.selectedPhones = engine.headphoneDevice();
            input.resolvedPhones = engine.resolvedSinkName();
            input.micMissing = engine.micMissing();
            input.phonesMissing = engine.headphonesMissing();
            input.micFallback = engine.micFallback();
            QJsonArray rows;
            for (const auto &r : obs::evaluateReadiness(input))
                rows.append(
                    QJsonObject{{QStringLiteral("id"), r.id}, {QStringLiteral("status"), int(r.status)}});
            QSaveFile file(parser.value(readinessFile));
            if (!file.open(QIODevice::WriteOnly) || file.write(QJsonDocument(rows).toJson()) < 0 ||
                !file.commit())
                QCoreApplication::exit(2);
        };
        QObject::connect(&pw, &pw::PwContext::graphChanged, &app, publish);
        QObject::connect(&engine, &engine::Engine::levelsChanged, &app, publish);
        QObject::connect(&engine, &engine::Engine::devicesChanged, &app, publish);
    }

    bool started = false;
    QTimer destinationPoll;
    QString lastDestinationRequest;
    if (parser.isSet(destinationFile)) {
        QObject::connect(&destinationPoll, &QTimer::timeout, &app, [&] {
            if (!started || !engine.mixReady()) {
                return;
            }
            QFile file(parser.value(destinationFile));
            if (!file.open(QIODevice::ReadOnly)) {
                return;
            }
            const QString request = QString::fromUtf8(file.readAll()).trimmed();
            if (request.isEmpty() || request == lastDestinationRequest) {
                return;
            }
            const auto words = request.split(QLatin1Char(' '));
            const auto spec = words.value(1).split(QLatin1Char('='));
            const auto destination = destinationFromString(spec.value(1));
            if (words.size() != 2 || spec.size() != 2 || !destination || !engine.scene().bus(spec.first())) {
                QTextStream(stderr) << "Invalid destination request: " << request << Qt::endl;
                QCoreApplication::exit(2);
                return;
            }
            engine.setBusDestination(spec.first(), *destination);
            lastDestinationRequest = request;
            QTextStream(stdout) << "Destination request " << words.first() << ": " << words.last() << Qt::endl;
        });
        destinationPoll.start(25); // Observe commands; tests wait for acknowledgement and the real graph.
    }
    // Live mic filter changes for tests: the engine is told exactly what the app's settings page would.
    QTimer micFilterPoll;
    QString lastMicFilterRequest;
    if (parser.isSet(micFilterFile)) {
        QObject::connect(&micFilterPoll, &QTimer::timeout, &app, [&] {
            QFile file(parser.value(micFilterFile));
            const QString request =
                file.open(QIODevice::ReadOnly) ? QString::fromUtf8(file.readAll()).trimmed() : QString();
            if (request.isEmpty() || request == lastMicFilterRequest) {
                return;
            }
            const auto words = request.split(QLatin1Char(' '));
            const QString what = words.value(1);
            if (words.size() != 2 || (what != QLatin1String("settings") && what != QLatin1String("unavailable") &&
                                      what != QLatin1String("available"))) {
                QTextStream(stderr) << "Invalid mic filter request: " << request << Qt::endl;
                QCoreApplication::exit(2);
                return;
            }
            if (what == QLatin1String("settings")) {
                engine.setMicFilters(loadSettings(paths::settingsFile()).micFilters);
            } else if (what == QLatin1String("unavailable")) {
                engine.setMicFilterPlugin({}, false, QStringLiteral("The mic filter library failed to load (test)."));
            } else {
                engine.setMicFilterPlugin(parser.value(micFilterPlugin), parser.isSet(micFilterDenoise), {});
            }
            lastMicFilterRequest = request;
            QTextStream(stdout) << "Mic filter request " << words.first() << ": " << what << Qt::endl;
        });
        micFilterPoll.start(25);
    }
    // The app retries on a timer; a test decides when, so it can restart PipeWire first.
    QTimer reconnectPoll;
    QString lastReconnectRequest;
    QObject::connect(&reconnectPoll, &QTimer::timeout, &app, [&] {
        QFile file(parser.value(reconnectFile));
        const QString request =
            file.open(QIODevice::ReadOnly) ? QString::fromUtf8(file.readAll()).trimmed() : QString();
        if (request.isEmpty() || request == lastReconnectRequest) {
            return;
        }
        lastReconnectRequest = request;
        reconnectPoll.stop();
        QTextStream(stdout) << "Reconnect request " << request << Qt::endl;
        pw.start();
    });
    // Exercise production CaptureWatch while the graph runs headlessly.
    obs::CaptureWatch captureWatch;
    bool pwReady = pw.state() == pw::PwContext::State::Ready;
    bool needsCaptureEpoch = false;
    const auto publishCaptureLost = [&] {
        if (!parser.isSet(captureLostFile)) {
            return;
        }
        QJsonObject row;
        row.insert(QStringLiteral("alert"), captureWatch.alertActive());
        QJsonArray missing;
        for (const QString &name : captureWatch.missingDevices()) {
            missing.append(name);
        }
        row.insert(QStringLiteral("missing"), missing);
        if (const auto obs = obs::runningObs()) {
            row.insert(QStringLiteral("obsPid"), obs->pid);
            row.insert(QStringLiteral("obsStart"), obs->startTime);
        }
        QSaveFile file(parser.value(captureLostFile));
        if (!file.open(QIODevice::WriteOnly) || file.write(QJsonDocument(row).toJson()) < 0 || !file.commit()) {
            QCoreApplication::exit(2);
        }
    };
    const auto updateCaptureWatch = [&] {
        if (!parser.isSet(captureLostFile)) {
            return;
        }
        const qint64 now = QDateTime::currentMSecsSinceEpoch();
        captureWatch.setNowMs(now);
        captureWatch.setMix(engine.mixEnabled(), engine.mixReady(), !engine.mixError().isEmpty());
        captureWatch.setObsLive(false);
        if (pw.state() == pw::PwContext::State::Ready) {
            captureWatch.observe(obs::obsRecordings(pw.graph()), obs::runningObs(), now);
        }
        captureWatch.tick(now);
        publishCaptureLost();
    };
    QTimer captureWatchTick;
    if (parser.isSet(captureLostFile)) {
        QObject::connect(&captureWatchTick, &QTimer::timeout, &app, updateCaptureWatch);
        captureWatchTick.start(250);
        QObject::connect(&pw, &pw::PwContext::graphChanged, &app, updateCaptureWatch);
        QObject::connect(&engine, &engine::Engine::mixStateChanged, &app, updateCaptureWatch);
    }

    QObject::connect(&pw, &pw::PwContext::stateChanged, &app, [&] {
        const bool ready = pw.state() == pw::PwContext::State::Ready;
        const qint64 now = QDateTime::currentMSecsSinceEpoch();
        if (parser.isSet(captureLostFile)) {
            if (pwReady && !ready) {
                captureWatch.onDisconnected(now);
                needsCaptureEpoch = true;
            } else if (!pwReady && ready && needsCaptureEpoch) {
                captureWatch.onReconnected(now);
                needsCaptureEpoch = false;
            }
            pwReady = ready;
            updateCaptureWatch();
        }
        if (pw.state() == pw::PwContext::State::Failed) {
            QTextStream(stderr) << pw.errorString() << "\n";
            if (parser.isSet(reconnectFile)) {
                reconnectPoll.start(25);
                return;
            }
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
        if (parser.isSet(exportRules)) {
            QString error;
            if (!rule_export::apply(engine.scene(), paths::pipewirePulseFragment(), paths::pipewireClientFragment(),
                                    &error)) {
                QTextStream(stderr) << "Rule export failed: " << error << "\n";
                QCoreApplication::exit(2);
                return;
            }
            QTextStream(stdout) << "Rules exported." << Qt::endl;
        }
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
