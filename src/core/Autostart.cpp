#include "core/Autostart.h"

#include <QDir>
#include <QFileInfo>
#include <QStandardPaths>
#include <algorithm>

namespace rostrum::autostart {

namespace {

// Percent signs in the executable path cannot be represented in a way that both complies with
// the Desktop Entry field-code rule (`%%`) and resolves in GDesktopAppInfo (which looks up
// argv[0] on disk before expanding `%%`). Control characters are invalid in Desktop Entry
// `string` values.
bool canRepresentLauncher(const QString &path)
{
    return !path.isEmpty() &&
           std::none_of(path.cbegin(), path.cend(), [](QChar c) {
               return c == QLatin1Char('%') || c.unicode() < 0x20 || c.unicode() == 0x7f;
           });
}

// TryExec has Desktop Entry value type `string`: backslashes must be escaped as `\\`, while
// spaces and shell metacharacters remain unquoted.
QString escapeTryExec(const QString &path)
{
    QString escaped = path;
    escaped.replace(QLatin1String("\\"), QLatin1String("\\\\"));
    return escaped;
}

} // namespace

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
    if (!appImage.isEmpty()) {
        if (!canRepresentLauncher(appImage)) {
            return QString();
        }
        if (!appImage.startsWith(QLatin1String("/tmp/.mount_")) &&
            (appImage.endsWith(QLatin1String(".AppImage"), Qt::CaseInsensitive) || QFileInfo::exists(appImage))) {
            return appImage;
        }
    }

    const QString argv0 = qEnvironmentVariable("ARGV0");
    if (!argv0.isEmpty()) {
        if (!canRepresentLauncher(argv0)) {
            return QString();
        }
        if (!argv0.startsWith(QLatin1String("/tmp/.mount_")) &&
            (argv0.endsWith(QLatin1String(".AppImage"), Qt::CaseInsensitive) || QFileInfo::exists(argv0))) {
            return argv0;
        }
    }

    const bool inMount = self.contains(QLatin1String("/.mount_")) || self.startsWith(QLatin1String("/tmp/"));
    const bool isBuild = self.contains(QLatin1String("/build/")) ||
                         self.contains(QLatin1String("/build-")) ||
                         self.contains(QLatin1String("/cmake-build-"));

    if (inMount || isBuild) {
        const QString localBin = QDir::home().filePath(QStringLiteral(".local/bin/rostrum"));
        if (QFileInfo::exists(localBin) && canRepresentLauncher(localBin)) {
            return localBin;
        }
        const QString onPath = QStandardPaths::findExecutable(QStringLiteral("rostrum"));
        if (!onPath.isEmpty() && !onPath.contains(QLatin1String("/.mount_")) &&
            !onPath.contains(QLatin1String("/build/")) && canRepresentLauncher(onPath)) {
            return onPath;
        }
        return QString();
    }

    return canRepresentLauncher(self) ? self : QString();
}

QString execLine(const QString &launcher)
{
    if (!canRepresentLauncher(launcher)) {
        return QString();
    }
    const bool isAppImage = launcher.endsWith(QLatin1String(".AppImage"), Qt::CaseInsensitive);
    return execQuote(launcher, isAppImage) + QStringLiteral(" --autostart");
}

QString entry(const QString &exec, const QString &comment, const QString &tryExec)
{
    if (exec.isEmpty() || (!tryExec.isEmpty() && !canRepresentLauncher(tryExec))) {
        return QString();
    }
    QString safeComment = comment;
    safeComment.replace(QLatin1Char('\n'), QLatin1Char(' '));
    QString result = QStringLiteral("[Desktop Entry]\n"
                                    "Type=Application\n"
                                    "Name=Rostrum\n"
                                    "Comment=%1\n"
                                    "Exec=%2\n")
                         .arg(safeComment, exec);
    if (!tryExec.isEmpty()) {
        result += QStringLiteral("TryExec=%1\n").arg(escapeTryExec(tryExec));
    }
    result += QStringLiteral("Icon=" ROSTRUM_APP_ID "\n"
                             "Terminal=false\n"
                             "StartupWMClass=rostrum\n"
                             "X-GNOME-Autostart-enabled=true\n");
    return result;
}

bool isCurrent(const QString &existing, const QString &launcher)
{
    const QString expectedExec = execLine(launcher);
    if (expectedExec.isEmpty()) {
        return false;
    }
    const QString expectedTryExec = escapeTryExec(launcher);
    bool seenDesktopEntry = false;
    bool inDesktopEntry = false;
    bool seenExec = false;
    bool seenTryExec = false;
    bool seenWmClass = false;
    QString execValue;
    QString tryExecValue;
    QString wmClassValue;

    for (QStringView raw : QStringView(existing).split(QLatin1Char('\n'))) {
        if (raw.endsWith(QLatin1Char('\r'))) {
            raw.chop(1);
        }
        const QStringView trimmed = raw.trimmed();
        if (trimmed.isEmpty() || trimmed.startsWith(QLatin1Char('#'))) {
            continue;
        }
        if (trimmed.startsWith(QLatin1Char('[')) && trimmed.endsWith(QLatin1Char(']'))) {
            if (trimmed == QLatin1String("[Desktop Entry]")) {
                if (seenDesktopEntry) {
                    return false;
                }
                seenDesktopEntry = true;
                inDesktopEntry = true;
            } else {
                if (!seenDesktopEntry) {
                    return false;
                }
                inDesktopEntry = false;
            }
            continue;
        }
        if (!inDesktopEntry) {
            if (!seenDesktopEntry) {
                return false;
            }
            continue;
        }
        const qsizetype eq = raw.indexOf(QLatin1Char('='));
        if (eq <= 0) {
            return false;
        }
        const QStringView key = raw.left(eq).trimmed();
        QStringView value = raw.mid(eq + 1);
        while (!value.isEmpty() && (value.front() == QLatin1Char(' ') || value.front() == QLatin1Char('\t'))) {
            value = value.mid(1);
        }
        if (key == QLatin1String("Exec")) {
            if (seenExec) {
                return false;
            }
            seenExec = true;
            execValue = value.toString();
        } else if (key == QLatin1String("TryExec")) {
            if (seenTryExec) {
                return false;
            }
            seenTryExec = true;
            tryExecValue = value.toString();
        } else if (key == QLatin1String("StartupWMClass")) {
            if (seenWmClass) {
                return false;
            }
            seenWmClass = true;
            wmClassValue = value.toString();
        }
    }

    return seenDesktopEntry && seenExec && seenTryExec && seenWmClass &&
           execValue == expectedExec && tryExecValue == expectedTryExec &&
           wmClassValue == QLatin1String("rostrum");
}

} // namespace rostrum::autostart
