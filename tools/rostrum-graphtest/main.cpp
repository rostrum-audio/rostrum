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
    parser.addOptions({seconds, teardown});
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
        engine.createMix();
    });

    QObject::connect(&engine, &engine::Engine::mixStateChanged, &app, [&] {
        if (engine.mixReady()) {
            QTextStream(stdout) << "Mix ready.\n";
            printState(pw);
        } else if (!engine.mixError().isEmpty()) {
            QTextStream(stderr) << "Mix error: " << engine.mixError() << "\n";
        }
    });

    if (const int s = parser.value(seconds).toInt(); s > 0) {
        QTimer::singleShot(s * 1000, &app, &QCoreApplication::quit);
    }
    return app.exec();
}
