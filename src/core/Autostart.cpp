#include "core/Autostart.h"

#include <algorithm>

namespace rostrum::autostart {

QString execQuote(const QString &arg)
{
    static const QString reserved = QStringLiteral(" \t\n\"'\\><~|&;$*?#()`");
    if (!arg.isEmpty() &&
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

QString entry(const QString &exec, const QString &comment)
{
    QString safeComment = comment;
    safeComment.replace(QLatin1Char('\n'), QLatin1Char(' '));
    return QStringLiteral("[Desktop Entry]\n"
                          "Type=Application\n"
                          "Name=Rostrum\n"
                          "Comment=%1\n"
                          "Exec=%2\n"
                          "Icon=" ROSTRUM_APP_ID "\n"
                          "Terminal=false\n"
                          "X-GNOME-Autostart-enabled=true\n")
        .arg(safeComment, exec);
}

} // namespace rostrum::autostart
