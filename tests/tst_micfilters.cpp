#include "core/DspPlugin.h"
#include "core/MicFilters.h"
#include "core/Settings.h"

#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <QTest>

using namespace rostrum::micfx;
using rostrum::defaultSettings;
using rostrum::parseSettings;
using rostrum::serializeSettings;
namespace actions = rostrum::actions;
namespace dsp = rostrum::dsp;

class TestMicFilters : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void defaultsAreTheStreamingPresetAndOff()
    {
        const Settings s;
        QVERIFY(!s.enabled);
        QCOMPARE(s.scope, Scope::AllApps);
        QCOMPARE(matchingPreset(s), QStringLiteral("streaming"));
        QCOMPARE(sanitize(s), s);
    }

    void presetsRoundTripAndEditsBecomeCustom()
    {
        for (const QString &id : presetIds()) {
            Settings s = applyPreset(Settings{}, id);
            QCOMPARE(matchingPreset(s), id);
            QCOMPARE(sanitize(s), s); // presets sit on the offered steps
        }
        Settings s = applyPreset(Settings{}, QStringLiteral("broadcast"));
        s.compRatio = 6;
        QCOMPARE(matchingPreset(s), QStringLiteral("custom"));
        // A switched-off module's details do not break the match.
        Settings light = applyPreset(Settings{}, QStringLiteral("light"));
        light.gateThresholdDb = -30;
        QCOMPARE(matchingPreset(light), QStringLiteral("light"));
    }

    void presetsKeepScopeAndAppChoices()
    {
        Settings s;
        s.enabled = true;
        s.scope = Scope::StreamOnly;
        s.rawApps = {QStringLiteral("name:Audacity")};
        const Settings after = applyPreset(s, QStringLiteral("noisy"));
        QVERIFY(after.enabled);
        QCOMPARE(after.scope, Scope::StreamOnly);
        QCOMPARE(after.rawApps, s.rawApps);
        QVERIFY(after.gate);
    }

    void sanitizeClampsSnapsAndTidies()
    {
        Settings s;
        s.highpassHz = 1000;
        s.compRatio = 3.04;
        s.gateThresholdDb = std::numeric_limits<double>::quiet_NaN();
        s.limiterCeilingDb = 3;
        s.filteredApps = {QStringLiteral("name:Discord"), QStringLiteral(" "), QStringLiteral("name:Discord")};
        s.rawApps = {QStringLiteral("name:Discord"), QStringLiteral("binary:audacity")};
        s = sanitize(s);
        QCOMPARE(s.highpassHz, 200.0);
        QCOMPARE(s.compRatio, 3.0);
        QCOMPARE(s.gateThresholdDb, Settings{}.gateThresholdDb);
        QCOMPARE(s.limiterCeilingDb, 0.0);
        QCOMPARE(s.filteredApps, QStringList{QStringLiteral("name:Discord")});
        QCOMPARE(s.rawApps, QStringList{QStringLiteral("binary:audacity")});
    }

    void appChoicesDecideTheMic()
    {
        Settings s;
        s.enabled = true;
        QVERIFY(useFiltered(s, AppChoice::Default, false));
        QVERIFY(!useFiltered(s, AppChoice::Default, true)); // recorders get the untouched mic
        QVERIFY(useFiltered(s, AppChoice::Filtered, true));
        QVERIFY(!useFiltered(s, AppChoice::Raw, false));

        s.scope = Scope::StreamOnly;
        QVERIFY(!useFiltered(s, AppChoice::Default, false));
        QVERIFY(useFiltered(s, AppChoice::Filtered, false));

        s.enabled = false;
        QVERIFY(!useFiltered(s, AppChoice::Filtered, false));

        const QString discord = QStringLiteral("name:Discord");
        s = setAppChoice(s, discord, AppChoice::Raw);
        QCOMPARE(appChoice(s, discord), AppChoice::Raw);
        s = setAppChoice(s, discord, AppChoice::Filtered);
        QCOMPARE(appChoice(s, discord), AppChoice::Filtered);
        QVERIFY(s.rawApps.isEmpty());
        s = setAppChoice(s, discord, AppChoice::Default);
        QCOMPARE(appChoice(s, discord), AppChoice::Default);
        QVERIFY(s.filteredApps.isEmpty());
    }

    void settingsTomlRoundTrip()
    {
        rostrum::Settings settings = defaultSettings();
        settings.micFilters = applyPreset(settings.micFilters, QStringLiteral("broadcast"));
        settings.micFilters.enabled = true;
        settings.micFilters.scope = Scope::StreamOnly;
        settings.micFilters.voiceThreshold = 35;
        settings.micFilters.rawApps = {QStringLiteral("name:OBS")};
        settings.micFilters.filteredApps = {QStringLiteral("binary:discord")};
        const QString text = serializeSettings(settings);
        QVERIFY(text.contains(QStringLiteral("[mic_filters.compressor]")));
        QString error;
        const rostrum::Settings back = parseSettings(text, &error);
        QVERIFY2(error.isEmpty(), qPrintable(error));
        QCOMPARE(back.micFilters, settings.micFilters);
    }

    void handEditedTomlIsSanitized()
    {
        const QString text = QStringLiteral(
            "[mic_filters]\nenabled = true\nscope = \"bogus\"\n"
            "[mic_filters.compressor]\nratio = 100\nthreshold = -12\n"
            "[mic_filters.highpass]\nenabled = false\nfrequency = 97\n");
        const auto m = parseSettings(text).micFilters;
        QVERIFY(m.enabled);
        QCOMPARE(m.scope, Scope::AllApps);
        QCOMPARE(m.compRatio, 20.0);
        QCOMPARE(m.compThresholdDb, -12.0);
        QVERIFY(!m.highpass);
        QCOMPARE(m.highpassHz, 95.0);
        QVERIFY(m.denoise); // missing keys keep their defaults
    }

    void controlsCoverEveryPortOnce()
    {
        Settings s;
        s.denoiseStrength = 50;
        const auto list = controls(s, true);
        QCOMPARE(list.size(), params().size() + switches().size());
        QStringList keys;
        for (const auto &c : list) {
            QVERIFY(!keys.contains(c.key));
            keys << c.key;
        }
        QVERIFY(keys.contains(QStringLiteral("rostrum_denoise:Strength")));
        for (const auto &c : list) {
            if (c.key == QLatin1String("rostrum_denoise:Strength")) {
                QCOMPARE(c.value, 0.5f);
            }
        }
        QVERIFY(graphLoaded(keys));
        QVERIFY(!graphLoaded({QStringLiteral("audioconvert.filter-graph.disable")}));

        for (const auto &c : controls(s, false)) {
            QVERIFY(!c.key.startsWith(QLatin1String("rostrum_denoise:")));
        }
    }

    void graphChainsTheModulesInOrder()
    {
        const QString path = QStringLiteral("/opt/My \"Apps\"/librostrum-dsp.so");
        const QString json = graphJson(Settings{}, path, true);
        QVERIFY(json.contains(QStringLiteral("plugin = \"/opt/My \\\"Apps\\\"/librostrum-dsp.so\"")));
        QVERIFY(json.contains(QStringLiteral("inputs = [ \"rostrum_highpass:In\" ]")));
        QVERIFY(json.contains(QStringLiteral("outputs = [ \"rostrum_limiter:Out\" ]")));
        QVERIFY(json.contains(QStringLiteral("{ output = \"rostrum_highpass:Out\" input = \"rostrum_denoise:In\" }")));
        QVERIFY(json.contains(QStringLiteral("{ output = \"rostrum_compressor:Out\" input = \"rostrum_limiter:In\" }")));
        QVERIFY(json.contains(QStringLiteral("\"Voice threshold\" = 0")));
        QVERIFY(json.contains(QStringLiteral("\"Ceiling\" = -1")));

        const QString noDenoise = graphJson(Settings{}, path, false);
        QVERIFY(!noDenoise.contains(QStringLiteral("rostrum_denoise")));
        QVERIFY(noDenoise.contains(QStringLiteral("{ output = \"rostrum_highpass:Out\" input = \"rostrum_gate:In\" }")));
    }

    void toggleActionIsAMicShortcut()
    {
        const QString id = QString::fromLatin1(actions::kToggleMicFilters);
        QVERIFY(actions::all().contains(id));
        QCOMPARE(actions::group(id), actions::Group::Mic);
        QVERIFY(actions::defaultShortcut(id).isEmpty());
    }

    void stableCopyNeverRewritesInPlace()
    {
        QTemporaryDir tmp;
        const QString source = tmp.filePath(QStringLiteral("librostrum-dsp.so"));
        auto write = [&](const QByteArray &data) {
            QFile f(source);
            QVERIFY(f.open(QIODevice::WriteOnly));
            f.write(data);
        };
        write("version one");
        const QString dir = tmp.filePath(QStringLiteral("dsp"));
        QString error;
        const QString first = dsp::stableCopy(source, dir, &error);
        QVERIFY2(!first.isEmpty(), qPrintable(error));
        QVERIFY(first.endsWith(QStringLiteral("/librostrum-dsp.so")));
        QCOMPARE(dsp::stableCopy(source, dir), first);

        write("version two");
        const QString second = dsp::stableCopy(source, dir);
        QVERIFY(second != first);
        QVERIFY(!QFile::exists(first)); // older copies are cleared out
        QFile copy(second);
        QVERIFY(copy.open(QIODevice::ReadOnly));
        QCOMPARE(copy.readAll(), QByteArray("version two"));
        QCOMPARE(QDir(dir).entryList(QDir::Dirs | QDir::NoDotAndDotDot).size(), 1);
    }

    void pluginOverrideMustExist()
    {
        qputenv("ROSTRUM_DSP_PLUGIN", "/nonexistent/librostrum-dsp.so");
        QString error;
        QVERIFY(dsp::pluginPath(&error).isEmpty());
        QVERIFY(!error.isEmpty());
        qunsetenv("ROSTRUM_DSP_PLUGIN");
    }
};

QTEST_GUILESS_MAIN(TestMicFilters)
#include "tst_micfilters.moc"
