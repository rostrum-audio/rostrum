// rostrum-obs: shows what the OBS page sees, from the terminal. --plan only reads OBS.

#include "obs/ObsCaptures.h"
#include "obs/ObsClient.h"
#include "obs/ObsConfig.h"
#include "obs/ObsLive.h"
#include "obs/ObsPlan.h"
#include "obs/SceneCollection.h"
#include "pw/PwContext.h"

#include <QCommandLineParser>
#include <QCoreApplication>
#include <QFile>
#include <QJsonDocument>
#include <QTextStream>
#include <QTimer>

using namespace rostrum;

namespace {

QString captureText(obs::Capture c)
{
    switch (c) {
    case obs::Capture::RostrumMic: return QStringLiteral("ok: Rostrum Mic");
    case obs::Capture::RostrumStream: return QStringLiteral("ok: Rostrum Stream Mix");
    case obs::Capture::RostrumBus: return QStringLiteral("doubles: a bus already in the Stream Mix");
    case obs::Capture::Mic: return QStringLiteral("bypasses Rostrum: a mic, directly");
    case obs::Capture::Output: return QStringLiteral("bypasses Rostrum: everything you hear");
    case obs::Capture::App: return QStringLiteral("bypasses Rostrum: one app, directly");
    case obs::Capture::None: break;
    }
    return QStringLiteral("-");
}

void printPlan(const obs::Plan &plan)
{
    QTextStream out(stdout);
    out << "Plan (" << (plan.mode == obs::Mode::Live ? "live" : "offline") << "):\n";
    if (plan.isEmpty()) {
        out << "  nothing to do, OBS is set up\n";
    }
    for (const auto &a : plan.actions) {
        switch (a.type) {
        case obs::Action::Type::SetDevice: out << "  point \"" << a.input << "\" at " << a.device << "\n"; break;
        case obs::Action::Type::Unmute: out << "  unmute \"" << a.input << "\"\n"; break;
        case obs::Action::Type::Mute: out << "  mute \"" << a.input << "\" (" << captureText(a.capture) << ")\n"; break;
        case obs::Action::Type::CreateInput:
            out << "  add \"" << a.input << "\" (" << a.device << ") to " << a.scenes.size() << " scenes: "
                << a.scenes.join(QStringLiteral(", ")) << ", tracks mask " << a.tracks << "\n";
            break;
        case obs::Action::Type::CreateGlobal:
            out << "  global " << a.channel << " \"" << a.input << "\" = " << a.device << ", tracks mask " << a.tracks << "\n";
            break;
        }
    }
}

} // namespace

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    QCommandLineParser parser;
    parser.setApplicationDescription(QStringLiteral("Show what Rostrum's OBS page sees. Read-only."));
    parser.addHelpOption();
    QCommandLineOption plan(QStringLiteral("plan"), QStringLiteral("Print the setup plan without applying it."));
    parser.addOption(plan);
    parser.process(app);

    QTextStream out(stdout);
    const auto installs = obs::findInstalls();
    if (installs.isEmpty()) {
        out << "No OBS configuration found." << (obs::obsBinaryInstalled() ? " Start OBS once." : "") << "\n";
        return 1;
    }
    const obs::Install install = installs.first();
    const bool running = obs::isObsRunning();
    const obs::WebSocketConfig ws = obs::readWebSocketConfig(install.configDir);
    out << "OBS: " << install.flavor << " at " << install.configDir << "\n"
        << "Running: " << (running ? "yes" : "no") << ", WebSocket: "
        << (ws.enabled ? QStringLiteral("on, port %1").arg(ws.port) : QStringLiteral("off")) << "\n";

    pw::PwContext pw;
    if (!pw.start()) {
        out << pw.errorString() << "\n";
        return 2;
    }
    auto *client = new obs::Client(&app);
    bool started = false;
    QObject::connect(&pw, &pw::PwContext::stateChanged, &app, [&] {
        if (pw.state() != pw::PwContext::State::Ready || started) {
            return;
        }
        started = true;
        // Let the registry settle so links and defaults are known.
        QTimer::singleShot(500, &app, [&] {
            out << "OBS records:\n";
            const auto recs = obs::obsRecordings(pw.graph());
            if (recs.isEmpty()) {
                out << "  nothing\n";
            }
            for (const auto &r : recs) {
                out << "  " << r.source << " <- " << r.what << "  [" << captureText(r.capture) << "]\n";
            }
            out.flush();
            if (!parser.isSet(plan)) {
                QCoreApplication::quit();
                return;
            }
            const obs::Facts facts = obs::factsFrom(pw);
            if (!running) {
                QFile f(obs::sceneCollectionFile(install.configDir));
                if (!f.open(QIODevice::ReadOnly)) {
                    out << "No scene collection file.\n";
                    QCoreApplication::exit(1);
                    return;
                }
                printPlan(obs::makePlan(obs::stateFromCollection(QJsonDocument::fromJson(f.readAll()).object()), facts,
                                        obs::Mode::Offline));
                QCoreApplication::quit();
                return;
            }
            if (!ws.enabled) {
                out << "Turn on OBS's WebSocket server, or close OBS, to see the plan.\n";
                QCoreApplication::exit(1);
                return;
            }
            QObject::connect(client, &obs::Client::statusChanged, &app, [&, client, facts] {
                if (client->status() == obs::Client::Status::Connected && client->obsVersion().isEmpty()) {
                    obs::fetchState(client, [&, facts](const obs::State &state, const QString &error) {
                        if (!error.isEmpty()) {
                            out << "OBS: " << error << "\n";
                            QCoreApplication::exit(1);
                            return;
                        }
                        printPlan(obs::makePlan(state, facts, obs::Mode::Live));
                        out.flush();
                        QCoreApplication::quit();
                    });
                } else if (client->status() == obs::Client::Status::AuthFailed ||
                           client->status() == obs::Client::Status::Failed) {
                    out << "OBS: " << client->errorString() << "\n";
                    QCoreApplication::exit(1);
                }
            });
            client->open(ws.port, ws.password);
        });
    });
    QTimer::singleShot(10000, &app, [] { QCoreApplication::exit(3); });
    return app.exec();
}
