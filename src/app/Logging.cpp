#include "app/Logging.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QMap>
#include <QMutex>

#include <atomic>
#include <climits>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <elf.h>
#include <execinfo.h>
#include <fcntl.h>
#include <link.h>
#include <unistd.h>

namespace rostrum::logging {

namespace {

constexpr qint64 kMaxLogBytes = 1024 * 1024;

QMutex g_mutex;
QFile *g_file = nullptr;
int g_fd = -1; // raw descriptor for the crash handler
QtMessageHandler g_previous = nullptr;

// Crash file state, prepared up front so the handler only copies bytes.
char g_crashDir[PATH_MAX] = {};
timespec g_started = {};
constexpr size_t kContextBytes = 4096;
char g_context[2][kContextBytes] = {};
std::atomic<int> g_contextIndex{0};
QMap<QString, QString> g_contextFields;
QString g_buildId;

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

void writeFd(int fd, const char *s)
{
    [[maybe_unused]] auto n = ::write(fd, s, std::strlen(s));
}

// Async-signal-safe integer to text.
const char *number(long long value, char (&buf)[24])
{
    char *p = buf + sizeof(buf) - 1;
    *p = '\0';
    const bool negative = value < 0;
    unsigned long long v = negative ? 0ULL - static_cast<unsigned long long>(value) : value;
    do {
        *--p = char('0' + v % 10);
        v /= 10;
    } while (v != 0 && p > buf + 1);
    if (negative) {
        *--p = '-';
    }
    return p;
}

void writeCrashFile(int sig, void *const *frames, int count)
{
    if (g_crashDir[0] == '\0') {
        return;
    }
    timespec now = {};
    clock_gettime(CLOCK_REALTIME, &now);
    timespec mono = {};
    clock_gettime(CLOCK_MONOTONIC, &mono);

    char num[24];
    char path[PATH_MAX + 48];
    path[0] = '\0';
    std::strncat(path, g_crashDir, PATH_MAX - 1);
    std::strcat(path, "/crash-");
    std::strcat(path, number(now.tv_sec, num));
    std::strcat(path, ".txt");
    const int fd = ::open(path, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    if (fd < 0) {
        return;
    }
    writeFd(fd, "rostrum-crash 1\nsignal: ");
    writeFd(fd, number(sig, num));
    writeFd(fd, "\nuptime: ");
    writeFd(fd, number(mono.tv_sec - g_started.tv_sec, num));
    writeFd(fd, "\n");
    writeFd(fd, g_context[g_contextIndex.load(std::memory_order_acquire)]);
    writeFd(fd, "frames:\n");
    backtrace_symbols_fd(frames, count, fd);
    ::close(fd);
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
    writeCrashFile(sig, frames, n);
    std::signal(sig, SIG_DFL);
    std::raise(sig);
}

int findBuildId(dl_phdr_info *info, size_t, void *out)
{
    // The executable is the first object and the only one without a name.
    for (int i = 0; i < info->dlpi_phnum; ++i) {
        const ElfW(Phdr) &ph = info->dlpi_phdr[i];
        if (ph.p_type != PT_NOTE) {
            continue;
        }
        const char *p = reinterpret_cast<const char *>(info->dlpi_addr + ph.p_vaddr);
        const char *end = p + ph.p_memsz;
        while (p + sizeof(ElfW(Nhdr)) <= end) {
            const auto *note = reinterpret_cast<const ElfW(Nhdr) *>(p);
            const char *name = p + sizeof(ElfW(Nhdr));
            const char *desc = name + ((note->n_namesz + 3) & ~3u);
            if (note->n_type == NT_GNU_BUILD_ID && note->n_namesz == 4 && std::memcmp(name, "GNU", 4) == 0 &&
                desc + note->n_descsz <= end) {
                *static_cast<QString *>(out) =
                    QString::fromLatin1(QByteArray(desc, int(note->n_descsz)).toHex());
                return 1;
            }
            p = desc + ((note->n_descsz + 3) & ~3u);
        }
    }
    return 1;
}

void publishContext()
{
    QByteArray text = "version: " ROSTRUM_VERSION "\n";
    if (!g_buildId.isEmpty()) {
        text += "build_id: " + g_buildId.toLatin1() + '\n';
    }
    for (auto it = g_contextFields.cbegin(); it != g_contextFields.cend(); ++it) {
        QString value = it.value();
        value.replace(QLatin1Char('\n'), QLatin1Char(' '));
        text += it.key().toUtf8() + ": " + value.toUtf8() + '\n';
    }
    const int next = 1 - g_contextIndex.load(std::memory_order_relaxed);
    const qsizetype len = std::min<qsizetype>(text.size(), kContextBytes - 1);
    std::memcpy(g_context[next], text.constData(), size_t(len));
    g_context[next][len] = '\0';
    g_contextIndex.store(next, std::memory_order_release);
}

} // namespace

void install(const QString &logFile, const QString &crashDir)
{
    clock_gettime(CLOCK_MONOTONIC, &g_started);
    dl_iterate_phdr(findBuildId, &g_buildId);
    if (QDir().mkpath(crashDir)) {
        const QByteArray dir = QFile::encodeName(crashDir);
        if (dir.size() < PATH_MAX) {
            std::memcpy(g_crashDir, dir.constData(), size_t(dir.size()));
            g_crashDir[dir.size()] = '\0';
        }
    }
    publishContext();

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

void setCrashContext(const QString &key, const QString &value)
{
    if (g_contextFields.value(key) == value && g_contextFields.contains(key)) {
        return;
    }
    g_contextFields.insert(key, value);
    publishContext();
}

QString buildId() { return g_buildId; }

} // namespace rostrum::logging
