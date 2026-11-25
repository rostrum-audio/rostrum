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

    // The chat mic carries the voice with Studio Voice on or off, so the switch is not asked.
    void chatMicIgnoresFiltersSwitch()
    {
        Settings s;
        s.enabled = false;
        s.scope = Scope::AllApps;
        QVERIFY(useChatMic(s, AppChoice::Default, false));
        QVERIFY(!useFiltered(s, AppChoice::Default, false));
        s.enabled = true;
        QVERIFY(useChatMic(s, AppChoice::Default, false));
    }

    void chatMicKeepsRawChoice()
    {
        Settings s;
        QVERIFY(!useChatMic(s, AppChoice::Raw, false));
        s.enabled = true;
        QVERIFY(!useChatMic(s, AppChoice::Raw, false));
    }

    void chatMicKeepsExclusions()
    {
        Settings s;
        QVERIFY(!useChatMic(s, AppChoice::Default, true)); // recorders get the untouched mic
        QVERIFY(useChatMic(s, AppChoice::Filtered, true));
    }

    void chatMicScopeStream()
    {
        Settings s;
        s.scope = Scope::StreamOnly;
        QVERIFY(!useChatMic(s, AppChoice::Default, false));
        QVERIFY(useChatMic(s, AppChoice::Filtered, false));
    }

    // Removing the chat mic waits for the apps moved off it, at most 3 s.
    void releaseWaitsForAppsToLeaveChatMic()
    {
        QVERIFY(chatMicReleaseHeld(true, 0));
        QVERIFY(chatMicReleaseHeld(true, 2999));
        QVERIFY(!chatMicReleaseHeld(true, 3000));
        QVERIFY(!chatMicReleaseHeld(true, 10000));
        QVERIFY(!chatMicReleaseHeld(false, 0));
        QVERIFY(!chatMicReleaseHeld(false, 2999));
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

    // What Rostrum 0.2.0 wrote: every key of the time, no de-esser, no sidechain filter, no revision.
    static QString savedBy020(const Settings &module)
    {
        rostrum::Settings settings;
        settings.micFilters = module;
        settings.micFilters.enabled = true;
        settings.micFilters.scope = Scope::StreamOnly;
        settings.micFilters.rawApps = {QStringLiteral("name:Audacity")};
        QStringList out;
        bool skip = false;
        for (const QString &raw : serializeSettings(settings).split(QLatin1Char('\n'))) {
            const QString line = raw.trimmed(); // sub-tables are written indented
            if (line.startsWith(QLatin1Char('['))) {
                skip = line == QLatin1String("[mic_filters.deesser]");
            }
            if (!skip && !line.startsWith(QLatin1String("sidechain_hpf")) && !line.startsWith(QLatin1String("revision"))) {
                out << line;
            }
        }
        return out.join(QLatin1Char('\n'));
    }

    static Settings preset020(const QString &id)
    {
        Settings s = defaultsForRevision(0);
        if (id == QLatin1String("light")) {
            s.denoiseStrength = 70;
            s.eq = false;
            s.lowGainDb = s.mudGainDb = s.presenceGainDb = s.airGainDb = 0;
            s.compThresholdDb = -18;
            s.compRatio = 2;
            s.compAttackMs = 10;
            s.compMakeupDb = 2;
        } else if (id == QLatin1String("noisy")) {
            s.highpassHz = 100;
            s.highpassSteep = true;
            s.voiceThreshold = 50;
            s.gate = true;
            s.gateThresholdDb = -45;
            s.gateRangeDb = -30;
        } else if (id == QLatin1String("broadcast")) {
            s.highpassHz = 90;
            s.highpassSteep = true;
            s.gate = true;
            s.gateRangeDb = -15;
            s.lowGainDb = 2;
            s.mudGainDb = -3;
            s.mudHz = 350;
            s.presenceGainDb = 3;
            s.presenceHz = 4500;
            s.airGainDb = 2;
            s.airHz = 11000;
            s.compThresholdDb = -24;
            s.compRatio = 4;
            s.compAttackMs = 5;
            s.compReleaseMs = 120;
            s.compMakeupDb = 7;
        }
        return s;
    }

    void presetSavedBy020StaysThatPreset()
    {
        for (const QString &id : presetIds()) {
            const QString text = savedBy020(preset020(id));
            QVERIFY(!text.contains(QStringLiteral("deesser")));
            const Settings m = parseSettings(text).micFilters;
            QCOMPARE(matchingPreset(m), id);
            // The user's own choices are untouched.
            QVERIFY(m.enabled);
            QCOMPARE(m.scope, Scope::StreamOnly);
            QCOMPARE(m.rawApps, QStringList{QStringLiteral("name:Audacity")});
        }
        // Natural never had a de-esser and still has none.
        QVERIFY(!parseSettings(savedBy020(preset020(QStringLiteral("light")))).micFilters.deesser);
    }

    void ownValuesSavedBy020KeepTheirSound()
    {
        Settings own = preset020(QStringLiteral("streaming"));
        own.compRatio = 6;
        own.presenceGainDb = 5;
        const Settings m = parseSettings(savedBy020(own)).micFilters;
        QCOMPARE(matchingPreset(m), QStringLiteral("custom"));
        QCOMPARE(m.compRatio, 6.0);
        QCOMPARE(m.presenceGainDb, 5.0);
        QCOMPARE(m.mudHz, 300.0);   // their saved value, not the new default
        QVERIFY(!m.deesser);        // never switched on by them
        QCOMPARE(m.compSidechainHz, 20.0); // the compressor hears the whole signal, as before
    }

    void firstDeesserThresholdFollowsTheDetector()
    {
        // Saved by a build with the de-esser but before the revision: its detector read 8 dB hot.
        rostrum::Settings settings;
        settings.micFilters.deesserThresholdDb = -28;
        QString text = serializeSettings(settings);
        QVERIFY(text.contains(QStringLiteral("revision = 2")));
        text.remove(QStringLiteral("revision = 2"));
        const Settings m = parseSettings(text).micFilters;
        QCOMPARE(m.deesserThresholdDb, -36.0);
        QCOMPARE(matchingPreset(m), QStringLiteral("streaming"));
        // A current file is read as written, however often.
        const Settings again = parseSettings(serializeSettings(settings)).micFilters;
        QCOMPARE(again.deesserThresholdDb, -28.0);
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
        QVERIFY(graphLoaded(keys, list));
        QVERIFY(!graphLoaded({QStringLiteral("audioconvert.filter-graph.disable")}, list));
        // A graph an older plugin left behind: no de-esser, no sidechain filter.
        QStringList older = keys;
        older.removeAll(QStringLiteral("rostrum_compressor:Sidechain HPF"));
        QVERIFY(!graphLoaded(older, list));
        older = keys;
        older.removeIf([](const QString &k) { return k.startsWith(QLatin1String("rostrum_deesser:")); });
        QVERIFY(!graphLoaded(older, list));
        // Without RNNoise the denoise controls are not expected.
        older = keys;
        older.removeIf([](const QString &k) { return k.startsWith(QLatin1String("rostrum_denoise:")); });
        QVERIFY(graphLoaded(older, controls(s, false)));
        QVERIFY(!graphLoaded(older, list));

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
        QVERIFY(json.contains(QStringLiteral("{ output = \"rostrum_eq:Out\" input = \"rostrum_deesser:In\" }")));
        QVERIFY(json.contains(QStringLiteral("{ output = \"rostrum_deesser:Out\" input = \"rostrum_compressor:In\" }")));
        QVERIFY(json.contains(QStringLiteral("{ output = \"rostrum_compressor:Out\" input = \"rostrum_limiter:In\" }")));
        QVERIFY(json.contains(QStringLiteral("\"Voice threshold\" = 0")));
        QVERIFY(json.contains(QStringLiteral("\"Sidechain HPF\" = 120")));
        QVERIFY(json.contains(QStringLiteral("\"Ceiling\" = -1")));

        const QString noDenoise = graphJson(Settings{}, path, false);
        QVERIFY(!noDenoise.contains(QStringLiteral("rostrum_denoise")));
        QVERIFY(noDenoise.contains(QStringLiteral("{ output = \"rostrum_highpass:Out\" input = \"rostrum_gate:In\" }")));
        QVERIFY(noDenoise.contains(QStringLiteral("{ output = \"rostrum_eq:Out\" input = \"rostrum_deesser:In\" }")));
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
