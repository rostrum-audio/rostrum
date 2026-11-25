#include "core/JavaApps.h"

#include "core/AppIdentity.h"

#include <QHash>

namespace rostrum {

namespace {

const QHash<QString, JavaApp> &known()
{
    static const QHash<QString, JavaApp> table = [] {
        QHash<QString, JavaApp> t;
        const auto add = [&t](const JavaApp &app, std::initializer_list<const char *> programs) {
            t.insert(app.name.toLower(), app);
            for (const char *p : programs) {
                t.insert(QString::fromLatin1(p).toLower(), app);
            }
        };
        add({QStringLiteral("RuneLite"), AppCategory::Game},
            {"net.runelite.client.RuneLite", "net.runelite.launcher.Launcher", "RuneLite.jar"});
        // Vanilla, Fabric (new and old), Quilt, Forge and NeoForge, LaunchWrapper, and the entry
        // points Prism Launcher and MultiMC start the game through.
        add({QStringLiteral("Minecraft"), AppCategory::Game},
            {"net.minecraft.client.main.Main", "net.fabricmc.loader.impl.launch.knot.KnotClient",
             "net.fabricmc.loader.launch.knot.KnotClient", "org.quiltmc.loader.impl.launch.knot.KnotClient",
             "cpw.mods.bootstraplauncher.BootstrapLauncher", "net.minecraft.launchwrapper.Launch",
             "org.prismlauncher.EntryPoint", "org.multimc.EntryPoint"});
        return t;
    }();
    return table;
}

// Launcher options whose value is the next argument. Every other option is one argument, and the
// long ones may also be written --name=value.
bool takesValue(const QString &arg)
{
    static const QStringList options = {
        QStringLiteral("-cp"),           QStringLiteral("-classpath"),          QStringLiteral("--class-path"),
        QStringLiteral("-p"),            QStringLiteral("--module-path"),       QStringLiteral("--upgrade-module-path"),
        QStringLiteral("--add-modules"), QStringLiteral("--limit-modules"),     QStringLiteral("--add-reads"),
        QStringLiteral("--add-exports"), QStringLiteral("--add-opens"),         QStringLiteral("--patch-module"),
        QStringLiteral("--enable-native-access"), QStringLiteral("--source"),
    };
    return options.contains(arg);
}

QString fileName(const QString &path)
{
    return path.mid(path.lastIndexOf(QLatin1Char('/')) + 1);
}

} // namespace

bool isJavaBinary(const QString &binary)
{
    return cleanBinary(binary).compare(QLatin1String("java"), Qt::CaseInsensitive) == 0;
}

QString javaProgram(const QStringList &commandLine)
{
    for (qsizetype i = 1; i < commandLine.size(); ++i) {
        const QString &arg = commandLine.at(i);
        if (arg.startsWith(QLatin1Char('@'))) {
            return {};
        }
        if (arg == QLatin1String("-jar")) {
            return i + 1 < commandLine.size() ? fileName(commandLine.at(i + 1)) : QString();
        }
        if (arg == QLatin1String("-m") || arg == QLatin1String("--module")) {
            return i + 1 < commandLine.size() ? commandLine.at(i + 1).section(QLatin1Char('/'), 0, 0) : QString();
        }
        if (arg.startsWith(QLatin1Char('-'))) {
            if (takesValue(arg)) {
                ++i;
            }
            continue;
        }
        return arg;
    }
    return {};
}

std::optional<JavaApp> knownJavaApp(const QString &programOrName)
{
    if (auto it = known().constFind(programOrName.trimmed().toLower()); it != known().cend()) {
        return it.value();
    }
    return std::nullopt;
}

QString javaRuleMatch(const QString &program)
{
    const auto app = knownJavaApp(program);
    return app ? app->name : program;
}

} // namespace rostrum
