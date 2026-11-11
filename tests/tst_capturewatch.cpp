#include "obs/CaptureWatch.h"
#include "obs/CaptureWatch.h"
#include "obs/ObsConfig.h"

#include <QFile>
#include <QScopeGuard>
#include <QTest>

#include <csignal>
#include <sys/prctl.h>
#include <sys/wait.h>
#include <unistd.h>

using namespace rostrum::obs;

class TestCaptureWatch : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void runningObs_none()
    {
        if (runningObs().has_value()) {
            QSKIP("a real OBS process is running for this user");
        }
        QVERIFY(!runningObs().has_value());
        QVERIFY(!isObsRunning());
    }

    void runningObs_matchesFake()
    {
        // With a real OBS running, runningObs() may return it instead of the fake child.
        const bool realObs = runningObs().has_value();
        const pid_t child = ::fork();
        QVERIFY(child >= 0);
        if (child == 0) {
            ::prctl(PR_SET_NAME, "obs");
            for (;;) {
                ::pause();
            }
        }
        bool reaped = false;
        const auto cleanup = qScopeGuard([&] {
            if (!reaped) {
                ::kill(child, SIGKILL);
                ::waitpid(child, nullptr, 0);
            }
        });

        // The child renames itself after fork; wait for /proc to reflect it.
        std::optional<ObsProcess> first;
        QTRY_VERIFY_WITH_TIMEOUT(procComm(child) == "obs", 5000);
        first = runningObs();
        QVERIFY(first.has_value());
        if (!realObs) {
            QCOMPARE(first->pid, qint64(child));
        }
        QVERIFY(first->startTime > 0);
        QCOMPARE(first->startTime, procStartTime(first->pid));
        QVERIFY(isObsRunning());

        const auto second = runningObs();
        QVERIFY(second.has_value());
        QCOMPARE(second->pid, first->pid);
        QCOMPARE(second->startTime, first->startTime);

        ::kill(child, SIGKILL);
        ::waitpid(child, nullptr, 0);
        reaped = true;
        if (!realObs) {
            QVERIFY(!runningObs().has_value());
            QVERIFY(!isObsRunning());
        } else {
            QVERIFY(runningObs().has_value());
        }
    }

    // ---- CaptureWatch ----

    void watch_baselineDisconnectReconnectGraceAlerts()
    {
        Fixture f;
        f.baseline();
        f.restart();
        f.w.setMix(true, true, false);
        f.at(500);
        f.observe({});
        QVERIFY(!f.w.alertActive());
        f.at(CaptureWatch::kGraceAfterMixReadyMs - 1);
        f.observe({});
        QVERIFY(!f.w.alertActive());
        f.at(CaptureWatch::kGraceAfterMixReadyMs);
        f.observe({});
        QVERIFY(f.w.alertActive());
        QCOMPARE(f.w.missingDevices(), QStringList({"Mic", "Mix"}));
    }

    void watch_uncommittedCandidateDiscarded()
    {
        Fixture f;
        f.observe(Fixture::both());
        f.at(CaptureWatch::kBaselineStableMs - 1);
        f.observe(Fixture::both());
        f.restart();
        f.w.setMix(true, true, false);
        f.at(CaptureWatch::kEpochCapAfterReadyMs + 1);
        f.observe({});
        QVERIFY(!f.w.alertActive());
    }

    void watch_changingCandidateRestartsDebounce()
    {
        Fixture f;
        f.observe({Fixture::mic()});
        f.at(1500);
        f.observe(Fixture::both());
        f.at(1500 + CaptureWatch::kBaselineStableMs - 1);
        f.observe(Fixture::both());
        f.restart();
        f.w.setMix(true, true, false);
        f.at(CaptureWatch::kEpochCapAfterReadyMs + 1);
        f.observe({});
        QVERIFY(!f.w.alertActive());
    }

    void watch_obsGoneOrRestarted_noAlert_data()
    {
        QTest::addColumn<bool>("gone");
        QTest::newRow("closed") << true;
        QTest::newRow("new pid") << false;
    }
    void watch_obsGoneOrRestarted_noAlert()
    {
        QFETCH(bool, gone);
        std::optional<ObsProcess> next;
        if (!gone) {
            next = ObsProcess{999, 5};
        }
        Fixture f;
        f.baseline();
        f.restart();
        f.w.setMix(true, true, false);
        f.at(CaptureWatch::kGraceAfterMixReadyMs);
        f.w.observe({}, next, f.now);
        f.at(CaptureWatch::kEpochCapAfterReadyMs + 1);
        f.w.observe({}, next, f.now);
        QVERIFY(!f.w.alertActive());
    }

    void watch_pidAndStartTimeBothCompared()
    {
        for (const ObsProcess &other : {ObsProcess{100, 8}, ObsProcess{101, 7}}) {
            Fixture f; // OBS is {100, 7}
            f.baseline();
            f.restart();
            f.w.setMix(true, true, false);
            f.at(CaptureWatch::kEpochCapAfterReadyMs);
            f.w.observe({}, other, f.now);
            QVERIFY(!f.w.alertActive());
        }
    }

    void watch_obsRestartedWhileAlerting_clears()
    {
        Fixture f;
        f.baseline();
        f.restart();
        f.w.setMix(true, true, false);
        f.at(CaptureWatch::kGraceAfterMixReadyMs);
        f.observe({});
        QVERIFY(f.w.alertActive());
        f.step(10);
        f.w.observe({}, ObsProcess{100, 8}, f.now);
        QVERIFY(!f.w.alertActive());
    }

    void watch_noBaseline_noAlert()
    {
        Fixture f;
        f.restart();
        f.w.setMix(true, true, false);
        f.at(CaptureWatch::kEpochCapAfterReadyMs + 1);
        f.observe({});
        QVERIFY(!f.w.alertActive());
    }

    void watch_mixOffOrError_noAlert_data()
    {
        QTest::addColumn<bool>("enabled");
        QTest::addColumn<bool>("ready");
        QTest::addColumn<bool>("error");
        QTest::newRow("off") << false << false << false;
        QTest::newRow("error") << true << false << true;
        QTest::newRow("ready but error") << true << true << true;
    }
    void watch_mixOffOrError_noAlert()
    {
        QFETCH(bool, enabled);
        QFETCH(bool, ready);
        QFETCH(bool, error);
        Fixture f;
        f.baseline();
        f.restart();
        f.w.setMix(enabled, ready, error);
        f.at(CaptureWatch::kEpochCapAfterReadyMs + 1000);
        f.observe({});
        QVERIFY(!f.w.alertActive());
    }

    void watch_mixNeverReady_alertsAtCap()
    {
        Fixture f;
        f.baseline();
        f.restart();
        f.w.setMix(true, false, false);
        f.at(CaptureWatch::kEpochCapAfterReadyMs - 1);
        f.observe({});
        QVERIFY(!f.w.alertActive());
        f.at(CaptureWatch::kEpochCapAfterReadyMs);
        f.observe({});
        QVERIFY(f.w.alertActive());
    }

    void watch_captureReturnsDuringGrace_noAlert()
    {
        Fixture f;
        f.baseline();
        f.restart();
        f.w.setMix(true, true, false);
        f.at(3000);
        f.observe(Fixture::both());
        f.at(CaptureWatch::kEpochCapAfterReadyMs + 1);
        f.observe(Fixture::both());
        QVERIFY(!f.w.alertActive());
    }

    void watch_alertClearsWhenCapturesReturn()
    {
        Fixture f;
        f.baseline();
        f.restart();
        f.w.setMix(true, true, false);
        f.at(CaptureWatch::kGraceAfterMixReadyMs);
        f.observe({});
        QVERIFY(f.w.alertActive());
        f.step(100);
        f.observe(Fixture::both());
        QVERIFY(!f.w.alertActive());
        QVERIFY(f.w.missingDevices().isEmpty());
    }

    void watch_dismiss_quietUntilNextEpoch()
    {
        Fixture f;
        f.baseline();
        f.restart();
        f.w.setMix(true, true, false);
        f.at(CaptureWatch::kGraceAfterMixReadyMs);
        f.observe({});
        QVERIFY(f.w.alertActive());
        f.w.dismiss();
        QVERIFY(!f.w.alertActive());
        f.step(CaptureWatch::kBaselineStableMs * 3);
        f.observe({});
        f.w.tick(f.now);
        QVERIFY(!f.w.alertActive());

        // Captures return and hold long enough to be the baseline again; a new restart alerts.
        f.step(100);
        f.observe(Fixture::both());
        f.step(CaptureWatch::kBaselineStableMs);
        f.observe(Fixture::both());
        f.restart();
        f.w.setMix(true, true, false);
        f.at(CaptureWatch::kGraceAfterMixReadyMs);
        f.observe({});
        QVERIFY(f.w.alertActive());
    }

    void watch_dismissThenAnotherReconnect_alertsAgain()
    {
        Fixture f;
        f.baseline();
        f.restart();
        f.w.setMix(true, true, false);
        f.at(CaptureWatch::kGraceAfterMixReadyMs);
        f.observe({});
        f.w.dismiss();
        f.restart();
        f.w.setMix(true, true, false);
        f.at(CaptureWatch::kGraceAfterMixReadyMs);
        f.observe({});
        QVERIFY(f.w.alertActive());
    }

    void watch_partialLoss_alerts()
    {
        Fixture f;
        f.baseline();
        f.restart();
        f.w.setMix(true, true, false);
        f.at(CaptureWatch::kGraceAfterMixReadyMs);
        f.observe({Fixture::mic()});
        QVERIFY(f.w.alertActive());
        QCOMPARE(f.w.missingDevices(), QStringList({"Mix"}));
    }

    void watch_partialReturnNotPromotedToBaseline()
    {
        Fixture f;
        f.baseline();
        f.restart();
        f.w.setMix(true, true, false);
        f.at(100);
        f.observe({Fixture::mic()});
        f.at(100 + CaptureWatch::kBaselineStableMs + 10);
        f.observe({Fixture::mic()}); // stable, but an epoch is pending
        f.at(CaptureWatch::kGraceAfterMixReadyMs);
        f.observe({Fixture::mic()});
        QVERIFY(f.w.alertActive());
    }

    void watch_nonRostrumDevicesIgnored()
    {
        Fixture f;
        const Recording other = Fixture::rec("Game", "alsa_input.foo", "Game");
        f.observe({other});
        f.step(CaptureWatch::kBaselineStableMs);
        f.observe({other});
        f.restart();
        f.w.setMix(true, true, false);
        f.at(CaptureWatch::kEpochCapAfterReadyMs + 1);
        f.observe({});
        QVERIFY(!f.w.alertActive());
    }

    void watch_liveSeverityLatchedAtDecision()
    {
        Fixture f;
        f.baseline();
        f.restart();
        f.w.setMix(true, true, false);
        f.w.setObsLive(true);
        f.at(CaptureWatch::kGraceAfterMixReadyMs);
        f.observe({});
        QVERIFY(f.w.alertActive());
        QVERIFY(f.w.liveSeverity());
    }

    void watch_tickAloneRaisesAlert()
    {
        Fixture f;
        f.baseline();
        f.restart();
        f.w.setMix(true, true, false);
        f.observe({});
        f.w.tick(f.epochBase + CaptureWatch::kGraceAfterMixReadyMs);
        QVERIFY(f.w.alertActive());
    }

private:
    struct Fixture
    {
        CaptureWatch w;
        qint64 now = 0;
        qint64 epochBase = 0;
        ObsProcess obs{100, 7};

        static Recording rec(const QString &source, const QString &device, const QString &what)
        {
            Recording r;
            r.source = source;
            r.device = device;
            r.what = what;
            return r;
        }
        static Recording mic() { return rec("Mic src", "rostrum.mic", "Mic"); }
        static Recording mix() { return rec("Mix src", "rostrum.mix", "Mix"); }
        static QList<Recording> both() { return {mic(), mix()}; }

        void at(qint64 ms) { now = epochBase + ms; } // relative to the last reconnect
        void step(qint64 ms) { now += ms; }
        void observe(const QList<Recording> &r) { w.observe(r, obs, now); }
        // Both captures stable long enough to become the baseline.
        void baseline()
        {
            observe(both());
            step(CaptureWatch::kBaselineStableMs);
            observe(both());
        }
        // PipeWire restarts; times given to at() count from the reconnect.
        void restart()
        {
            w.onDisconnected(now);
            step(1000);
            w.onReconnected(now);
            epochBase = now;
        }
    };

    static QByteArray procComm(pid_t pid)
    {
        QFile f(QStringLiteral("/proc/%1/comm").arg(pid));
        return f.open(QIODevice::ReadOnly) ? f.readAll().trimmed() : QByteArray();
    }

    // Independent parse of /proc/<pid>/stat field 22 (starttime).
    static qint64 procStartTime(qint64 pid)
    {
        QFile f(QStringLiteral("/proc/%1/stat").arg(pid));
        if (!f.open(QIODevice::ReadOnly)) {
            return -1;
        }
        const QByteArray line = f.readAll();
        const QList<QByteArray> rest = line.mid(line.lastIndexOf(')') + 2).split(' ');
        return rest.value(19).toLongLong();
    }
};

QTEST_GUILESS_MAIN(TestCaptureWatch)
#include "tst_capturewatch.moc"
