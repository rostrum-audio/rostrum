#include "app/Logging.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QMutex>

#include <csignal>
#include <cstdio>
#include <cstring>
#include <execinfo.h>
#include <fcntl.h>
#include <unistd.h>

namespace rostrum::logging {

namespace {

constexpr qint64 kMaxLogBytes = 1024 * 1024;

QMutex g_mutex;
QFile *g_file = nullptr;
int g_fd = -1; // raw descriptor for the crash handler
QtMessageHandler g_previous = nullptr;

const char *levelName(QtMsgType type)
{
    switch (type) {
    case QtDebugMsg:
        return "debug";
    case QtInfoMsg:
        return "info";
    case QtWarningMsg:
        return "warning";
    case QtCriticalMsg:
        return "critical";
    case QtFatalMsg:
        return "fatal";
    }
    return "?";
}

void handler(QtMsgType type, const QMessageLogContext &context, const QString &message)
{
    if (g_previous) {
        g_previous(type, context, message);
    } else {
        std::fprintf(stderr, "%s\n", qPrintable(qFormatLogMessage(type, context, message)));
    }
    if (type == QtDebugMsg) {
        return; // debug output stays on stderr
    }
    QMutexLocker lock(&g_mutex);
    if (!g_file) {
        return;
    }
    const QByteArray line = QDateTime::currentDateTime().toString(Qt::ISODateWithMs).toUtf8() + ' ' +
                            levelName(type) + ' ' + (context.category ? context.category : "default") + ": " +
                            message.toUtf8() + '\n';
    g_file->write(line);
    g_file->flush();
}

void writeRaw(const char *s)
{
    if (g_fd >= 0) {
        [[maybe_unused]] auto n = ::write(g_fd, s, std::strlen(s));
    }
    [[maybe_unused]] auto m = ::write(STDERR_FILENO, s, std::strlen(s));
}

void crashHandler(int sig)
{
    // Only async-signal-safe calls from here on.
    writeRaw("\n==== Rostrum crashed: ");
    writeRaw(strsignal(sig));
    writeRaw(" ====\n");
    void *frames[64];
    const int n = backtrace(frames, 64);
    if (g_fd >= 0) {
        backtrace_symbols_fd(frames, n, g_fd);
    }
    backtrace_symbols_fd(frames, n, STDERR_FILENO);
    writeRaw("==== end of backtrace ====\n");
    std::signal(sig, SIG_DFL);
    std::raise(sig);
}

} // namespace

void install(const QString &logFile)
{
    QDir().mkpath(QFileInfo(logFile).absolutePath());
    if (QFileInfo(logFile).size() > kMaxLogBytes) {
        const QString old = logFile + QStringLiteral(".1");
        QFile::remove(old);
        QFile::rename(logFile, old);
    }
    auto *file = new QFile(logFile);
    if (file->open(QIODevice::Append | QIODevice::Text)) {
        g_file = file;
        g_fd = file->handle();
        const QByteArray start = "\n" + QDateTime::currentDateTime().toString(Qt::ISODate).toUtf8() +
                                 " Rostrum " ROSTRUM_VERSION " started\n";
        file->write(start);
        file->flush();
    } else {
        delete file;
    }
    g_previous = qInstallMessageHandler(handler);

    // Load libgcc's unwinder now; backtrace() may allocate on its first call.
    void *dummy[1];
    backtrace(dummy, 1);
    for (int sig : {SIGSEGV, SIGABRT, SIGBUS, SIGFPE, SIGILL}) {
        struct sigaction sa = {};
        sa.sa_handler = crashHandler;
        sigemptyset(&sa.sa_mask);
        sa.sa_flags = SA_RESETHAND;
        sigaction(sig, &sa, nullptr);
    }
}

} // namespace rostrum::logging
