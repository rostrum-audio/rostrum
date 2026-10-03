#include "app/Cli.h"

#include "app/DBusControl.h"
#include "app/Preferences.h"
#include "core/Paths.h"
#include "core/SceneStore.h"
#include "core/Settings.h"

#include <KLocalizedString>
#include <QCommandLineParser>
#include <QCoreApplication>
#include <QDBusArgument>
#include <QDBusConnection>
#include <QDBusConnectionInterface>
#include <QDBusMessage>
#include <QElapsedTimer>
#include <QLoggingCategory>
#include <QThread>
#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <signal.h>

Q_DECLARE_LOGGING_CATEGORY(lcDBus)

namespace rostrum::app::cli {

namespace {

constexpr int kCallTimeoutMs = 5000;

const QStringList kControlOptions = {QStringLiteral("action"),     QStringLiteral("scene"),
                                     QStringLiteral("mute-mic"),   QStringLiteral("unmute-mic"),
                                     QStringLiteral("toggle-mic"), QStringLiteral("set-volume")};
const QStringList kQueryOptions = {QStringLiteral("list-scenes"), QStringLiteral("list-actions"),
                                   QStringLiteral("list-buses")};

void print(const QString &line)
{
    std::fputs(line.toLocal8Bit().constData(), stdout);
    std::fputc('\n', stdout);
}

void printError(const QString &message)
{
    std::fputs(QStringLiteral("rostrum: %1\n").arg(message).toLocal8Bit().constData(), stderr);
}

void printBuses(const BusInfoList &buses)
{
    for (const auto &b : buses) {
        print(QStringLiteral("%1\t%2\t%3\t%4")
                  .arg(b.id, QString::number(b.position, 'f', 2),
                       b.muted ? QStringLiteral("muted") : QStringLiteral("unmuted"), b.name));
    }
}

void printActions(const ActionInfoList &list)
{
    for (const auto &a : list) {
        print(QStringLiteral("%1\t%2").arg(a.id, a.label));
    }
}

} // namespace

bool Request::hasControls() const
{
    return !scene.isEmpty() || micMuted.has_value() || toggleMic || !volumes.isEmpty() || !actions.isEmpty();
}

bool Request::hasQueries() const
{
    return listScenes || listActions || listBuses;
}

bool needsGui(int argc, char **argv)
{
    static const QStringList printOnly = {QStringLiteral("-h"),         QStringLiteral("--help"),
                                          QStringLiteral("--help-all"), QStringLiteral("-v"),
                                          QStringLiteral("--version"),  QStringLiteral("--author"),
                                          QStringLiteral("--license")};
    bool query = false;
    for (int i = 1; i < argc; ++i) {
        const QString arg = QString::fromLocal8Bit(argv[i]);
        const QString name = arg.section(QLatin1Char('='), 0, 0).mid(2);
        if (arg.startsWith(QLatin1String("--")) && kControlOptions.contains(name)) {
            return true;
        }
        query = query || printOnly.contains(arg) ||
                (arg.startsWith(QLatin1String("--")) && kQueryOptions.contains(name));
    }
    return !query;
}

void addOptions(QCommandLineParser &parser)
{
    parser.addOptions({
        {QStringLiteral("autostart"), i18n("Started at login by the autostart entry.")},
        {QStringLiteral("scene"), i18n("Switch to the scene called <name>."),
         i18nc("@info:shell value name", "name")},
        {QStringLiteral("action"),
         i18n("Run a hotkey action by id, as if its shortcut was pressed. Can be repeated."),
         i18nc("@info:shell value name", "id")},
        {QStringLiteral("mute-mic"), i18n("Mute the mic.")},
        {QStringLiteral("unmute-mic"), i18n("Unmute the mic.")},
        {QStringLiteral("toggle-mic"), i18n("Mute the mic if it is live, unmute it if it is muted.")},
        {QStringLiteral("set-volume"),
         i18n(
             "Set a fader. <bus> is a bus id or name, or stream or phones for the masters. <level> is 0 to 1 "
             "or a percentage; mic gain goes to 1.5. Can be repeated."),
         i18nc("@info:shell value name", "bus=level")},
        {QStringLiteral("list-scenes"), i18n("Print the scene names, one per line.")},
        {QStringLiteral("list-actions"), i18n("Print the action ids and their names, separated by a tab.")},
        {QStringLiteral("list-buses"),
         i18n("Print id, level, muted or unmuted, and name of each fader, separated by tabs.")},
    });
    QCommandLineOption restartAfter(QString::fromLatin1(kRestartAfter), QString(), QStringLiteral("pid"));
    restartAfter.setFlags(QCommandLineOption::HiddenFromHelp);
    QCommandLineOption startHidden(QString::fromLatin1(kStartHidden));
    startHidden.setFlags(QCommandLineOption::HiddenFromHelp);
    parser.addOption(restartAfter);
    parser.addOption(startHidden);
}

QString restartProgram()
{
    if (const QString appImage = qEnvironmentVariable("APPIMAGE"); !appImage.isEmpty()) {
        return appImage;
    }
    QString program = QCoreApplication::applicationFilePath();
    const QLatin1String deleted(" (deleted)");
    if (program.endsWith(deleted)) {
        program.chop(deleted.size());
    }
    return program;
}

QStringList restartArguments(bool hidden)
{
    QStringList args{QStringLiteral("--%1=%2").arg(QLatin1String(kRestartAfter)).arg(QCoreApplication::applicationPid())};
    if (hidden) {
        args << QStringLiteral("--%1").arg(QLatin1String(kStartHidden));
    }
    return args;
}

bool waitForExit(qint64 pid, int timeoutMs)
{
    if (pid <= 0) {
        return true;
    }
    QElapsedTimer timer;
    timer.start();
    // kill(pid, 0) only asks whether the process exists; EPERM means it does, under another user.
    while (::kill(pid_t(pid), 0) == 0 || errno == EPERM) {
        if (timer.elapsed() >= timeoutMs) {
            return false;
        }
        QThread::msleep(50);
    }
    return true;
}

Request parse(const QCommandLineParser &parser)
{
    Request r;
    r.scene = parser.value(QStringLiteral("scene"));
    if (parser.isSet(QStringLiteral("scene")) && r.scene.isEmpty()) {
        r.errors << i18n("--scene needs a scene name.");
    }
    const int micOptions = int(parser.isSet(QStringLiteral("mute-mic"))) +
                           int(parser.isSet(QStringLiteral("unmute-mic"))) +
                           int(parser.isSet(QStringLiteral("toggle-mic")));
    if (micOptions > 1) {
        r.errors << i18n("Use only one of --mute-mic, --unmute-mic and --toggle-mic.");
    } else if (parser.isSet(QStringLiteral("mute-mic"))) {
        r.micMuted = true;
    } else if (parser.isSet(QStringLiteral("unmute-mic"))) {
        r.micMuted = false;
    }
    r.toggleMic = micOptions == 1 && parser.isSet(QStringLiteral("toggle-mic"));
    for (const QString &v : parser.values(QStringLiteral("set-volume"))) {
        if (const auto arg = remote::parseVolumeArg(v)) {
            r.volumes << *arg;
        } else {
            r.errors << i18n("“%1” is not bus=level, for example game=0.8 or game=80%.", v);
        }
    }
    r.actions = parser.values(QStringLiteral("action"));
    r.listScenes = parser.isSet(QStringLiteral("list-scenes"));
    r.listActions = parser.isSet(QStringLiteral("list-actions"));
    r.listBuses = parser.isSet(QStringLiteral("list-buses"));
    return r;
}

bool instanceRunning()
{
    const auto *bus = QDBusConnection::sessionBus().interface();
    return bus && bus->isServiceRegistered(QStringLiteral(ROSTRUM_APP_ID)).value();
}

int forward(const Request &r)
{
    int status = kExitOk;
    // Returns the reply, or an invalid message after printing why the call failed.
    auto call = [&status](const QString &method, const QVariantList &args = {}) {
        QDBusMessage msg =
            QDBusMessage::createMethodCall(QStringLiteral(ROSTRUM_APP_ID), QString::fromLatin1(remote::kPath),
                                           QString::fromLatin1(remote::kInterface), method);
        msg.setArguments(args);
        const QDBusMessage reply = QDBusConnection::sessionBus().call(msg, QDBus::Block, kCallTimeoutMs);
        if (reply.type() == QDBusMessage::ErrorMessage) {
            const bool refused = reply.errorName().startsWith(QString::fromLatin1(remote::kInterface));
            printError(refused ? reply.errorMessage()
                               : i18n("Could not reach Rostrum: %1", reply.errorMessage()));
            status = refused ? kExitRefused : kExitUnreachable;
            return QDBusMessage();
        }
        return reply;
    };
    auto ok = [&status] {
        return status == kExitOk;
    };

    if (!r.scene.isEmpty()) {
        call(QStringLiteral("SwitchScene"), {r.scene});
    }
    if (ok() && r.micMuted) {
        call(QStringLiteral("SetMicMuted"), {*r.micMuted});
    }
    if (ok() && r.toggleMic) {
        call(QStringLiteral("ToggleMicMute"));
    }
    for (const auto &v : r.volumes) {
        if (ok()) {
            call(QStringLiteral("SetBusVolume"), {v.busId, v.position});
        }
    }
    for (const QString &id : r.actions) {
        if (ok()) {
            call(QStringLiteral("TriggerAction"), {id});
        }
    }
    if (ok() && r.listScenes) {
        const QDBusMessage reply = call(QStringLiteral("ListScenes"));
        if (!reply.arguments().isEmpty()) {
            for (const QString &name : reply.arguments().constFirst().toStringList()) {
                print(name);
            }
        }
    }
    if (ok() && r.listActions) {
        const QDBusMessage reply = call(QStringLiteral("ListActions"));
        if (!reply.arguments().isEmpty()) {
            printActions(qdbus_cast<ActionInfoList>(reply.arguments().constFirst()));
        }
    }
    if (ok() && r.listBuses) {
        const QDBusMessage reply = call(QStringLiteral("ListBuses"));
        if (!reply.arguments().isEmpty()) {
            printBuses(qdbus_cast<BusInfoList>(reply.arguments().constFirst()));
        }
    }
    return status;
}

int answerOffline(const Request &r)
{
    QList<Scene> scenes = SceneStore(paths::scenesDir()).loadAll();
    if (scenes.isEmpty()) {
        scenes << defaults::scene(); // what Rostrum creates on its first start
    }
    const QString defaultName = loadSettings(paths::settingsFile()).defaultScene;
    const auto def = std::find_if(scenes.cbegin(), scenes.cend(), [&](const Scene &s) {
        return s.name.compare(defaultName, Qt::CaseInsensitive) == 0;
    });
    const Scene &start = def != scenes.cend() ? *def : scenes.constFirst();

    if (r.listScenes) {
        for (const auto &s : std::as_const(scenes)) {
            print(s.name);
        }
    }
    if (r.listActions) {
        ActionInfoList list;
        for (const QString &id : actions::all()) {
            list.append({id, Preferences::actionLabel(id)});
        }
        QStringList seen;
        auto addBuses = [&](const Scene &s) {
            for (const auto &b : s.buses) {
                if (!b.isInput() && !seen.contains(b.id)) {
                    seen << b.id;
                    list.append({actions::muteBusAction(b.id),
                                 Preferences::actionLabel(actions::muteBusAction(b.id), b.name)});
                }
            }
        };
        addBuses(start);
        for (const auto &s : std::as_const(scenes)) {
            addBuses(s);
        }
        printActions(list);
    }
    if (r.listBuses) {
        printBuses(DBusControl::busesOf(start));
    }
    return kExitOk;
}

void apply(const Request &r, DBusControl &control)
{
    auto report = [](const DBusControl::Error &e) {
        if (e) {
            qCWarning(lcDBus).noquote() << "Command line:" << e.message;
        }
    };
    if (!r.scene.isEmpty()) {
        report(control.switchScene(r.scene));
    }
    if (r.micMuted) {
        control.setMicMuted(*r.micMuted);
    }
    if (r.toggleMic) {
        control.toggleMicMute();
    }
    for (const auto &v : r.volumes) {
        report(control.setBusVolume(v.busId, v.position));
    }
    for (const QString &id : r.actions) {
        report(control.triggerAction(id));
    }
}

} // namespace rostrum::app::cli
