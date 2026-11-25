#include "core/JavaApps.h"

#include <QTest>

using namespace rostrum;

class TestJavaApps : public QObject
{
    Q_OBJECT
private Q_SLOTS:
    void programFromMainClass()
    {
        // RuneLite as Bolt starts it.
        QCOMPARE(javaProgram({QStringLiteral("/app/jre/bin/java"), QStringLiteral("-XX:+DisableAttachMechanism"),
                              QStringLiteral("-Xmx768m"), QStringLiteral("--add-opens=java.base/java.net=ALL-UNNAMED"),
                              QStringLiteral("-Drunelite.launcher.version=2.8.1"), QStringLiteral("-cp"),
                              QStringLiteral("/x/client-1.13.8.jar:/x/okhttp-3.14.9.jar"),
                              QStringLiteral("net.runelite.client.RuneLite"), QStringLiteral("--debug")}),
                 QStringLiteral("net.runelite.client.RuneLite"));
    }

    void programIgnoresTheClassPath()
    {
        // A client update changes the jar versions, never the key.
        const QString a = javaProgram({QStringLiteral("java"), QStringLiteral("-cp"), QStringLiteral("client-1.13.8.jar"),
                                       QStringLiteral("net.runelite.client.RuneLite")});
        const QString b = javaProgram({QStringLiteral("java"), QStringLiteral("--class-path"),
                                       QStringLiteral("client-1.14.0.jar"), QStringLiteral("net.runelite.client.RuneLite")});
        QCOMPARE(a, QStringLiteral("net.runelite.client.RuneLite"));
        QCOMPARE(a, b);
    }

    void optionsWithSeparateValues()
    {
        for (const char *opt : {"-cp", "-classpath", "--class-path", "-p", "--module-path", "--upgrade-module-path",
                                "--add-modules", "--limit-modules", "--add-reads", "--add-exports", "--add-opens",
                                "--patch-module", "--enable-native-access", "--source"}) {
            QCOMPARE(javaProgram({QStringLiteral("java"), QString::fromLatin1(opt), QStringLiteral("value"),
                                  QStringLiteral("com.example.Game")}),
                     QStringLiteral("com.example.Game"));
        }
    }

    void programFromJarAndModule()
    {
        QCOMPARE(javaProgram({QStringLiteral("java"), QStringLiteral("-Xss2m"), QStringLiteral("-jar"),
                              QStringLiteral("/opt/runelite/RuneLite.jar"), QStringLiteral("--mode=OFF")}),
                 QStringLiteral("RuneLite.jar"));
        QCOMPARE(javaProgram({QStringLiteral("java"), QStringLiteral("-m"), QStringLiteral("com.example.app/com.example.Main")}),
                 QStringLiteral("com.example.app"));
        QCOMPARE(javaProgram({QStringLiteral("java"), QStringLiteral("--module"), QStringLiteral("com.example.app")}),
                 QStringLiteral("com.example.app"));
    }

    void noProgram_data()
    {
        QTest::addColumn<QStringList>("commandLine");
        QTest::newRow("empty") << QStringList{};
        QTest::newRow("bare java") << QStringList{QStringLiteral("java")};
        QTest::newRow("version") << QStringList{QStringLiteral("java"), QStringLiteral("-version")};
        QTest::newRow("jar without a file") << QStringList{QStringLiteral("java"), QStringLiteral("-jar")};
        QTest::newRow("argfile") << QStringList{QStringLiteral("java"), QStringLiteral("@args.txt")};
        QTest::newRow("class path only") << QStringList{QStringLiteral("java"), QStringLiteral("-cp"), QStringLiteral("a.jar")};
    }

    void noProgram()
    {
        QFETCH(QStringList, commandLine);
        QVERIFY(javaProgram(commandLine).isEmpty());
    }

    void knownApps_data()
    {
        QTest::addColumn<QString>("program");
        QTest::addColumn<QString>("name");
        for (const char *p : {"net.runelite.client.RuneLite", "net.runelite.launcher.Launcher", "RuneLite.jar",
                              "runelite.jar", "RuneLite"}) {
            QTest::newRow(p) << QString::fromLatin1(p) << QStringLiteral("RuneLite");
        }
        for (const char *p : {"net.minecraft.client.main.Main", "net.fabricmc.loader.impl.launch.knot.KnotClient",
                              "net.fabricmc.loader.launch.knot.KnotClient",
                              "org.quiltmc.loader.impl.launch.knot.KnotClient",
                              "cpw.mods.bootstraplauncher.BootstrapLauncher", "net.minecraft.launchwrapper.Launch",
                              "org.prismlauncher.EntryPoint", "org.multimc.EntryPoint", "minecraft"}) {
            QTest::newRow(p) << QString::fromLatin1(p) << QStringLiteral("Minecraft");
        }
    }

    void knownApps()
    {
        QFETCH(QString, program);
        QFETCH(QString, name);
        const auto app = knownJavaApp(program);
        QVERIFY(app);
        QCOMPARE(app->name, name);
        QCOMPARE(app->category, AppCategory::Game);
        QCOMPARE(javaRuleMatch(program), name);
    }

    void unknownStaysRaw()
    {
        QVERIFY(!knownJavaApp(QStringLiteral("com.example.Game")));
        QVERIFY(!knownJavaApp(QString()));
        QCOMPARE(javaRuleMatch(QStringLiteral("com.example.Game")), QStringLiteral("com.example.Game"));
        QCOMPARE(javaRuleMatch(QStringLiteral("org.prismlauncher.EntryPoint")), QStringLiteral("Minecraft"));
        QVERIFY(isJavaBinary(QStringLiteral("/app/jre/bin/java")));
        QVERIFY(isJavaBinary(QStringLiteral("java")));
        QVERIFY(!isJavaBinary(QStringLiteral("javaws")));
        QVERIFY(!isJavaBinary(QString()));
    }
};

QTEST_GUILESS_MAIN(TestJavaApps)
#include "tst_javaapps.moc"
