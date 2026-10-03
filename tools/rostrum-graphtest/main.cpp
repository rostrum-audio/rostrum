// rostrum-graphtest: headless driver for the manual graph tests in docs/manual-tests.md.
// It runs the same Engine as the app, without any UI.

#include "engine/Engine.h"
#include "pw/PwContext.h"

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
    QCommandLineOption sidetone(QStringLiteral("sidetone"), QStringLiteral("Sidetone volume 0..1 (turns it on)."),
                                QStringLiteral("v"));
    QCommandLineOption solo(QStringLiteral("solo"), QStringLiteral("Solo a bus (session only)."),
                            QStringLiteral("bus"));
    parser.addOptions({seconds, teardown, rule, session, unassignAfter, listApps, dest, headphones, mic, micMuted,
                       sidetone, solo});
    parser.process(app);

    pw::PwContext pw;
    if (!pw.start()) {
        QTextStream(stderr) << pw.errorString() << "\n";
        return 2;
    }
    engine::Engine engine(&pw);

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
        if (parser.isSet(teardown)) {
            engine.destroyMix();
            QTimer::singleShot(500, &app, [&] {
                printState(pw);
                QCoreApplication::quit();
            });
            return;
        }
        engine.setHeadphoneDevice(parser.value(headphones));
        engine.setMicDevice(parser.value(mic));
        for (const auto &spec : parser.values(dest)) {
            const auto parts = spec.split(QLatin1Char('='));
            if (const auto d = destinationFromString(parts.value(1))) {
                engine.setBusDestination(parts.value(0), *d);
            }
        }
        engine.setMicMuted(parser.isSet(micMuted));
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

    if (const int s = parser.value(seconds).toInt(); s > 0) {
        QTimer::singleShot(s * 1000, &app, &QCoreApplication::quit);
    }
    return app.exec();
}
