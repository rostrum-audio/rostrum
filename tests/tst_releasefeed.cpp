// The release workflow's feed generator must write exactly what the updater reads.

#include "core/UpdateFeed.h"

#include <QCryptographicHash>
#include <QFile>
#include <QProcess>
#include <QTemporaryDir>
#include <QTest>

using namespace rostrum;

class TestReleaseFeed : public QObject
{
    Q_OBJECT

private:
    QByteArray runGenerator(const QStringList &args, int *exitCode)
    {
        QProcess p;
        p.start(QStringLiteral(ROSTRUM_PYTHON),
                QStringList{QStringLiteral(ROSTRUM_SOURCE_DIR "/tools/appimage/make-feed.py")} + args);
        if (!p.waitForFinished(30000)) {
            *exitCode = -1;
            return {};
        }
        *exitCode = p.exitStatus() == QProcess::NormalExit ? p.exitCode() : -1;
        return p.readAllStandardOutput();
    }

private Q_SLOTS:
    void generatedFeedParses()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString image = dir.filePath(QStringLiteral("Rostrum-0.2.0-x86_64.AppImage"));
        const QByteArray content(4096, 'r');
        QFile f(image);
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write(content);
        f.close();
        const QString notes = dir.filePath(QStringLiteral("notes.txt"));
        QFile n(notes);
        QVERIFY(n.open(QIODevice::WriteOnly));
        n.write("Scenes keep their order.\n");
        n.close();

        int code = 0;
        const QByteArray json =
            runGenerator({QStringLiteral("--version"), QStringLiteral("0.2.0"), QStringLiteral("--page"),
                          QStringLiteral("https://github.com/rostrum-audio/rostrum/releases/tag/v0.2.0"),
                          QStringLiteral("--download-base"),
                          QStringLiteral("https://github.com/rostrum-audio/rostrum/releases/download/v0.2.0"),
                          QStringLiteral("--notes-file"), notes, QStringLiteral("--date"),
                          QStringLiteral("2026-10-20"), image},
                         &code);
        QCOMPARE(code, 0);

        QString err;
        const updates::Release r = updates::parseFeed(json, QStringLiteral("x86_64"), &err);
        QVERIFY2(r.valid(), qPrintable(err));
        QCOMPARE(r.version, QStringLiteral("0.2.0"));
        QCOMPARE(r.date, QDate(2026, 10, 20));
        QCOMPARE(r.notes, QStringLiteral("Scenes keep their order."));
        QCOMPARE(r.page,
                 QUrl(QStringLiteral("https://github.com/rostrum-audio/rostrum/releases/tag/v0.2.0")));
        QVERIFY(r.appImage.valid());
        QCOMPARE(r.appImage.url,
                 QUrl(QStringLiteral("https://github.com/rostrum-audio/rostrum/releases/download/"
                                     "v0.2.0/Rostrum-0.2.0-x86_64.AppImage")));
        QCOMPARE(r.appImage.sha256, QCryptographicHash::hash(content, QCryptographicHash::Sha256).toHex());
        QCOMPARE(r.appImage.size, qint64(content.size()));
        QVERIFY(updates::isNewer(r.version, QStringLiteral("0.1.0")));
        // No aarch64 build was given, so an aarch64 machine is told about it but gets no download.
        QVERIFY(!updates::parseFeed(json, QStringLiteral("aarch64")).appImage.valid());
    }

    void generatorRefusesWhatTheUpdaterWouldIgnore()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        auto touch = [&](const QString &name) {
            QFile f(dir.filePath(name));
            QVERIFY(f.open(QIODevice::WriteOnly));
            f.write("x");
        };
        touch(QStringLiteral("Rostrum-0.2.0-rc1-x86_64.AppImage"));
        touch(QStringLiteral("rostrum.AppImage"));
        touch(QStringLiteral("Rostrum-0.2.0-x86_64.AppImage"));
        const QStringList base{QStringLiteral("--page"), QStringLiteral("https://example.org/r"),
                               QStringLiteral("--download-base"), QStringLiteral("https://example.org/d")};
        int code = 0;
        runGenerator(QStringList{QStringLiteral("--version"), QStringLiteral("0.2.0-rc1")} + base +
                         QStringList{dir.filePath(QStringLiteral("Rostrum-0.2.0-rc1-x86_64.AppImage"))},
                     &code);
        QVERIFY(code != 0);
        runGenerator(QStringList{QStringLiteral("--version"), QStringLiteral("0.2.0")} + base +
                         QStringList{dir.filePath(QStringLiteral("rostrum.AppImage"))},
                     &code);
        QVERIFY(code != 0);
        runGenerator({QStringLiteral("--version"), QStringLiteral("0.2.0"), QStringLiteral("--page"),
                      QStringLiteral("http://example.org/r"), QStringLiteral("--download-base"),
                      QStringLiteral("https://example.org/d"),
                      dir.filePath(QStringLiteral("Rostrum-0.2.0-x86_64.AppImage"))},
                     &code);
        QVERIFY(code != 0);
    }
};

QTEST_GUILESS_MAIN(TestReleaseFeed)
#include "tst_releasefeed.moc"
