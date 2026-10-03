#include "app/CrashCapture.h"

#ifdef ROSTRUM_WITH_SENTRY

#include <QDir>
#include <QFile>

#include <sentry.h>

#include <atomic>
#include <climits>
#include <cstdio>
#include <ctime>
#include <sys/stat.h>

namespace rostrum::app::crashcapture {

namespace {

// Filled before sentry_init so the transport, which may run while crashing, only formats a name.
char g_crashDir[PATH_MAX] = {};
std::atomic<unsigned> g_counter{0};
bool g_running = false;

void writeEnvelope(sentry_envelope_t *envelope, void *)
{
    // Crashes from earlier runs arrive as raw bytes with no parsed event, so everything is written
    // and CrashReports drops files without an event item.
    if (g_crashDir[0] != '\0') {
        char path[PATH_MAX + 64];
        std::snprintf(path, sizeof(path), "%s/crash-%lld-%u.envelope", g_crashDir, static_cast<long long>(std::time(nullptr)),
                      g_counter.fetch_add(1));
        if (sentry_envelope_write_to_file(envelope, path) == 0) {
            ::chmod(path, 0600);
        }
    }
    sentry_envelope_free(envelope);
}

} // namespace

bool compiledIn() { return true; }

bool start(const QString &dsn, const QString &databaseDir, const QString &crashDir)
{
    if (g_running) {
        return true;
    }
    if (!QDir().mkpath(crashDir) || !QDir().mkpath(databaseDir)) {
        return false;
    }
    QFile::setPermissions(crashDir, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
    QFile::setPermissions(databaseDir, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
    const QByteArray dir = QFile::encodeName(crashDir);
    if (dir.size() >= PATH_MAX) {
        return false;
    }
    std::snprintf(g_crashDir, sizeof(g_crashDir), "%s", dir.constData());

    sentry_options_t *options = sentry_options_new();
    sentry_options_set_dsn(options, dsn.toUtf8().constData());
    sentry_options_set_database_path(options, QFile::encodeName(databaseDir).constData());
    sentry_options_set_release(options, "rostrum@" ROSTRUM_VERSION);
    sentry_options_set_environment(options, "production");
    // One report per crash and nothing else: no launch pings, breadcrumbs or client reports.
    sentry_options_set_auto_session_tracking(options, 0);
    sentry_options_set_max_breadcrumbs(options, 0);
    sentry_options_set_send_client_reports(options, 0);
    sentry_options_set_symbolize_stacktraces(options, 1);
    sentry_options_set_debug(options, qEnvironmentVariableIsSet("ROSTRUM_SENTRY_DEBUG") ? 1 : 0);
    sentry_options_set_transport(options, sentry_transport_new(writeEnvelope));
    g_running = sentry_init(options) == 0;
    return g_running;
}

void stop()
{
    if (g_running) {
        sentry_close();
        g_running = false;
    }
}

bool running() { return g_running; }

void setContext(const QMap<QString, QString> &fields)
{
    if (!g_running) {
        return;
    }
    sentry_value_t context = sentry_value_new_object();
    for (auto it = fields.cbegin(); it != fields.cend(); ++it) {
        if (!it.value().isEmpty()) {
            sentry_value_set_by_key(context, it.key().toUtf8().constData(),
                                    sentry_value_new_string(it.value().toUtf8().constData()));
        }
    }
    sentry_set_context("rostrum", context);
}

} // namespace rostrum::app::crashcapture

#else

namespace rostrum::app::crashcapture {

bool compiledIn() { return false; }
bool start(const QString &, const QString &, const QString &) { return false; }
void stop() {}
bool running() { return false; }
void setContext(const QMap<QString, QString> &) {}

} // namespace rostrum::app::crashcapture

#endif
