#include "app/QuitSignals.h"

#include <QCoreApplication>
#include <QLoggingCategory>
#include <QSocketNotifier>

#include <cerrno>
#include <cstring>
#include <csignal>
#include <sys/socket.h>
#include <unistd.h>

namespace rostrum::app {

namespace {

Q_LOGGING_CATEGORY(lcQuit, "rostrum.quit")

int g_pipe[2] = {-1, -1};

void onSignal(int sig)
{
    // Only async-signal-safe calls. The byte carries the signal number to the event loop.
    const int saved = errno;
    const unsigned char byte = static_cast<unsigned char>(sig);
    [[maybe_unused]] const ssize_t n = ::write(g_pipe[0], &byte, 1);
    errno = saved;
}

} // namespace

void installQuitSignals(QObject *parent)
{
    if (g_pipe[0] >= 0) {
        return;
    }
    if (::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0, g_pipe) != 0) {
        qCWarning(lcQuit) << "could not watch quit signals:" << strerror(errno);
        return;
    }
    auto *notifier = new QSocketNotifier(g_pipe[1], QSocketNotifier::Read, parent);
    QObject::connect(notifier, &QSocketNotifier::activated, notifier, [] {
        unsigned char byte = 0;
        while (::read(g_pipe[1], &byte, 1) == 1) {
            qCInfo(lcQuit) << "quitting on" << strsignal(byte);
        }
        QCoreApplication::quit();
    });
    for (int sig : {SIGTERM, SIGINT, SIGHUP}) {
        struct sigaction sa = {};
        sa.sa_handler = onSignal;
        sigemptyset(&sa.sa_mask);
        // The first signal asks for a normal quit; the handler then reverts, so a second one
        // ends a quit that hangs.
        sa.sa_flags = SA_RESTART | SA_RESETHAND;
        sigaction(sig, &sa, nullptr);
    }
}

} // namespace rostrum::app
