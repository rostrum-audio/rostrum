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

private:
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
