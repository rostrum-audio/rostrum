#include "core/Autostart.h"

#include <QDir>
#include <QFileInfo>
#include <QStandardPaths>
#include <algorithm>

namespace rostrum::autostart {

QString execQuote(const QString &arg, bool forceQuote)
{
    static const QString reserved = QStringLiteral(" \t\n\"'\\><~|&;$*?#()`");
    if (!forceQuote && !arg.isEmpty() &&
        std::none_of(arg.cbegin(), arg.cend(), [](QChar c) { return reserved.contains(c); })) {
        return arg;
    }
    QString quoted = arg;
    for (const char *c : {"\\", "\"", "`", "$"}) {
        quoted.replace(QLatin1String(c), QLatin1String("\\") + QLatin1String(c));
    }
    // The string-type escape for backslashes applies on top of the Exec quoting rule.
    quoted.replace(QLatin1String("\\"), QLatin1String("\\\\"));
    return QLatin1Char('"') + quoted + QLatin1Char('"');
}

QString launcherPath(const QString &self, const QString &appImage)
{
    if (!appImage.isEmpty() && !appImage.startsWith(QLatin1String("/tmp/.mount_")) &&
        (appImage.endsWith(QLatin1String(".AppImage"), Qt::CaseInsensitive) || QFileInfo::exists(appImage))) {
        return appImage;
    }

    const QString argv0 = qEnvironmentVariable("ARGV0");
    if (!argv0.isEmpty() && !argv0.startsWith(QLatin1String("/tmp/.mount_")) &&
        (argv0.endsWith(QLatin1String(".AppImage"), Qt::CaseInsensitive) || QFileInfo::exists(argv0))) {
        return argv0;
    }

    const bool inMount = self.contains(QLatin1String("/.mount_")) || self.startsWith(QLatin1String("/tmp/"));
    const bool isBuild = self.contains(QLatin1String("/build/")) ||
                         self.contains(QLatin1String("/build-")) ||
                         self.contains(QLatin1String("/cmake-build-"));

    if (inMount || isBuild) {
        const QString localBin = QDir::home().filePath(QStringLiteral(".local/bin/rostrum"));
        if (QFileInfo::exists(localBin)) {
            return localBin;
        }
        const QString onPath = QStandardPaths::findExecutable(QStringLiteral("rostrum"));
        if (!onPath.isEmpty() && !onPath.contains(QLatin1String("/.mount_")) &&
            !onPath.contains(QLatin1String("/build/"))) {
            return onPath;
        }
        return QString();
    }

    return self;
}

QString execLine(const QString &launcher)
{
    if (launcher.isEmpty()) {
        return QString();
    }
    const bool isAppImage = launcher.endsWith(QLatin1String(".AppImage"), Qt::CaseInsensitive);
    return execQuote(launcher, isAppImage) + QStringLiteral(" --autostart");
}

QString entry(const QString &exec, const QString &comment, const QString &tryExec)
{
    QString safeComment = comment;
    safeComment.replace(QLatin1Char('\n'), QLatin1Char(' '));
    QString result = QStringLiteral("[Desktop Entry]\n"
                                    "Type=Application\n"
                                    "Name=Rostrum\n"
                                    "Comment=%1\n"
                                    "Exec=%2\n")
                         .arg(safeComment, exec);
    if (!tryExec.isEmpty()) {
        result += QStringLiteral("TryExec=%1\n").arg(tryExec);
    }
    result += QStringLiteral("Icon=" ROSTRUM_APP_ID "\n"
                             "Terminal=false\n"
                             "StartupWMClass=rostrum\n"
                             "X-GNOME-Autostart-enabled=true\n");
    return result;
}

} // namespace rostrum::autostart
