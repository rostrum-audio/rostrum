#pragma once

#include <QMap>
#include <QString>

// sentry-native, when Rostrum is built with ROSTRUM_WITH_SENTRY. Its transport never sends:
// it writes each envelope to the crash folder, and CrashReports decides what happens next.
namespace rostrum::app::crashcapture {

bool compiledIn();

// Starts capturing crashes. Crashes from earlier runs are written to crashDir as
// crash-*.envelope before this returns.
bool start(const QString &dsn, const QString &databaseDir, const QString &crashDir);
void stop();
bool running();

// Attached to the next crash as the "rostrum" context.
void setContext(const QMap<QString, QString> &fields);

} // namespace rostrum::app::crashcapture
