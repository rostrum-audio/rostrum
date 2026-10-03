#include "core/CrashReport.h"
#include "core/UpdateFeed.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QTest>

using namespace rostrum;

namespace {

QByteArray toJson(const QJsonObject &o)
{
    return QJsonDocument(o).toJson(QJsonDocument::Compact);
}

} // namespace

class TestPrivacy : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void sanitizeFrame_data()
    {
        QTest::addColumn<QString>("line");
        QTest::addColumn<QString>("expected");
        QTest::newRow("own binary, offset only")
            << QStringLiteral("/home/alex/.local/bin/rostrum(+0x4f2a1) [0x55d1c84f2a1]") << QStringLiteral("rostrum +0x4f2a1");
        QTest::newRow("library with symbol")
            << QStringLiteral("/usr/lib/x86_64-linux-gnu/libQt6Core.so.6(_ZN7QObject5eventEP6QEvent+0x1c) [0x7f3a2c11d01c]")
            << QStringLiteral("libQt6Core.so.6 _ZN7QObject5eventEP6QEvent+0x1c");
        QTest::newRow("folder with spaces and parentheses")
            << QStringLiteral("/home/alex/My Stuff (old)/build/rostrum(+0x12) [0x1]") << QStringLiteral("rostrum +0x12");
        QTest::newRow("relative build path") << QStringLiteral("./build/src/app/rostrum(+0x5aa4b) [0x585a499efa4b]")
                                            << QStringLiteral("rostrum +0x5aa4b");
        QTest::newRow("no symbol info") << QStringLiteral("/usr/lib/libfoo.so.1 [0x7f00]") << QStringLiteral("libfoo.so.1");
        QTest::newRow("empty parentheses") << QStringLiteral("/usr/lib/libfoo.so.1() [0x7f00]") << QStringLiteral("libfoo.so.1");
        QTest::newRow("module name with odd characters")
            << QStringLiteral("/home/alex/alex's plugin.so(+0x10) [0x1]") << QStringLiteral("? +0x10");
        QTest::newRow("symbol that is not a symbol")
            << QStringLiteral("/usr/lib/libfoo.so(/home/alex/secret+0x1) [0x1]") << QStringLiteral("libfoo.so ?");
        QTest::newRow("unparseable") << QStringLiteral("something /home/alex wrote") << QStringLiteral("?");
    }

    void sanitizeFrame()
    {
        QFETCH(QString, line);
        QFETCH(QString, expected);
        QCOMPARE(crash::sanitizeFrame(line), expected);
    }

    void sanitizeValue()
    {
        QCOMPARE(crash::sanitizeValue(QStringLiteral("6.10.2")), QStringLiteral("6.10.2"));
        QCOMPARE(crash::sanitizeValue(QStringLiteral("KDE")), QStringLiteral("KDE"));
        QCOMPARE(crash::sanitizeValue(QStringLiteral("a\"b<c>@d")), QStringLiteral("abcd"));
        QCOMPARE(crash::sanitizeValue(QString(100, QLatin1Char('x'))).size(), 64);
    }

    void parseCrashFile()
    {
        const QByteArray text = "rostrum-crash 1\nsignal: 11\nuptime: 42\nversion: 0.1.0\nos: Ubuntu 26.04\n"
                                "frames:\n/usr/bin/rostrum(+0x10) [0x1]\n/lib/libc.so.6(abort+0x5) [0x2]\n";
        const crash::CrashFile file = crash::parseCrashFile(text);
        QVERIFY(file.valid);
        QCOMPARE(file.fields.value(QStringLiteral("signal")), QStringLiteral("11"));
        QCOMPARE(file.fields.value(QStringLiteral("os")), QStringLiteral("Ubuntu 26.04"));
        QCOMPARE(file.frames.size(), 2);

        QVERIFY(!crash::parseCrashFile("not a crash file\nsignal: 11\n").valid);
        QVERIFY(!crash::parseCrashFile("rostrum-crash 1\nversion: 0.1.0\n").valid);
    }

    void reportHoldsOnlyWhitelistedFields()
    {
        crash::CrashFile file;
        file.valid = true;
        file.fields = {
            {QStringLiteral("signal"), QStringLiteral("6")},
            {QStringLiteral("uptime"), QStringLiteral("90")},
            {QStringLiteral("version"), QStringLiteral("0.1.0")},
            {QStringLiteral("build_id"), QStringLiteral("2615ebc32d3f3d30705b9729536fae62112f393d")},
            {QStringLiteral("desktop"), QStringLiteral("KDE")},
            {QStringLiteral("user"), QStringLiteral("alex")},
            {QStringLiteral("home"), QStringLiteral("/home/alex")},
            {QStringLiteral("headphones"), QStringLiteral("HyperX Cloud")},
        };
        file.frames = {QStringLiteral("/home/alex/.local/bin/rostrum(+0x10) [0x55d1]")};
        const QJsonObject report = crash::buildReport(file, QDate(2026, 10, 3));
        const QByteArray json = QJsonDocument(report).toJson(QJsonDocument::Compact);

        QVERIFY(!json.contains("alex"));
        QVERIFY(!json.contains("HyperX"));
        QVERIFY(!json.contains("0x55d1"));
        QCOMPARE(report.value(QStringLiteral("schema")).toInt(), crash::kSchema);
        QCOMPARE(report.value(QStringLiteral("date")).toString(), QStringLiteral("2026-10-03"));
        const QJsonObject crashSection = report.value(QStringLiteral("crash")).toObject();
        QCOMPARE(crashSection.value(QStringLiteral("signal")).toString(), QStringLiteral("SIGABRT"));
        QCOMPARE(crashSection.value(QStringLiteral("uptime_seconds")).toInt(), 90);
        QCOMPARE(crashSection.value(QStringLiteral("frames")).toArray().first().toString(), QStringLiteral("rostrum +0x10"));
        QCOMPARE(report.value(QStringLiteral("app")).toObject().value(QStringLiteral("build_id")).toString().size(), 40);
        QCOMPARE(report.value(QStringLiteral("system")).toObject().value(QStringLiteral("desktop")).toString(), QStringLiteral("KDE"));

        // Every key in the report comes from the whitelist.
        const QStringList allowed = crash::reportFields() << QStringLiteral("uptime_seconds") << QStringLiteral("frames");
        for (const char *section : {"app", "crash", "system"}) {
            for (const QString &key : report.value(QLatin1String(section)).toObject().keys()) {
                QVERIFY2(allowed.contains(key), qPrintable(key));
            }
        }
    }

    void buildIdMustBeHex()
    {
        crash::CrashFile file;
        file.valid = true;
        file.fields = {{QStringLiteral("signal"), QStringLiteral("11")}, {QStringLiteral("build_id"), QStringLiteral("/home/alex")}};
        const QJsonObject app = crash::buildReport(file, QDate(2026, 1, 1)).value(QStringLiteral("app")).toObject();
        QVERIFY(!app.contains(QStringLiteral("build_id")));
    }

    void osName()
    {
        QCOMPARE(crash::osName(QStringLiteral("PRETTY_NAME=\"Ubuntu 26.04 LTS\"\nNAME=\"Ubuntu\"\nVERSION_ID=\"26.04\"\n")),
                 QStringLiteral("Ubuntu 26.04"));
        QCOMPARE(crash::osName(QStringLiteral("NAME='Arch Linux'\nID=arch\n")), QStringLiteral("Arch Linux"));
        QCOMPARE(crash::osName(QString()), QString());
    }

    void rostrumFeed()
    {
        const QByteArray json = toJson(QJsonObject{
            {QStringLiteral("version"), QStringLiteral("0.2.0")},
            {QStringLiteral("date"), QStringLiteral("2026-10-20")},
            {QStringLiteral("notes"), QStringLiteral("Fixes")},
            {QStringLiteral("url"), QStringLiteral("https://getrostrum.dev/releases/0.2.0")},
            {QStringLiteral("appimage"),
             QJsonObject{{QStringLiteral("x86_64"),
                          QJsonObject{{QStringLiteral("url"), QStringLiteral("https://getrostrum.dev/dl/Rostrum-0.2.0-x86_64.AppImage")},
                                      {QStringLiteral("sha256"),
                                       QStringLiteral("E6D10587827F7A9BA1DFA3D1BF5BA7CE8F99F7A30EB4555DE5E1B90D42600A6D")},
                                      {QStringLiteral("size"), 42}}}}},
        });
        const updates::Release r = updates::parseFeed(json, QStringLiteral("x86_64"));
        QVERIFY(r.valid());
        QCOMPARE(r.version, QStringLiteral("0.2.0"));
        QCOMPARE(r.date, QDate(2026, 10, 20));
        QCOMPARE(r.page, QUrl(QStringLiteral("https://getrostrum.dev/releases/0.2.0")));
        QVERIFY(r.appImage.valid());
        QCOMPARE(r.appImage.sha256, QByteArray("e6d10587827f7a9ba1dfa3d1bf5ba7ce8f99f7a30eb4555de5e1b90d42600a6d"));
        QCOMPARE(r.appImage.size, 42);

        // No build for this CPU: the release is still reported, just not installable.
        const updates::Release arm = updates::parseFeed(json, QStringLiteral("aarch64"));
        QVERIFY(arm.valid());
        QVERIFY(!arm.appImage.valid());
    }

    void gitHubFeed()
    {
        const auto asset = [](const QString &name, const QString &url, char digit, int size) {
            QJsonObject a{{QStringLiteral("name"), name},
                          {QStringLiteral("browser_download_url"), url},
                          {QStringLiteral("digest"), QStringLiteral("sha256:") + QString(64, QLatin1Char(digit))}};
            if (size > 0) {
                a.insert(QStringLiteral("size"), size);
            }
            return a;
        };
        const QByteArray json = toJson(QJsonObject{
            {QStringLiteral("tag_name"), QStringLiteral("v0.3.1")},
            {QStringLiteral("html_url"), QStringLiteral("https://github.com/rostrum-audio/rostrum/releases/tag/v0.3.1")},
            {QStringLiteral("body"), QStringLiteral("Notes")},
            {QStringLiteral("published_at"), QStringLiteral("2026-11-02T10:00:00Z")},
            {QStringLiteral("draft"), false},
            {QStringLiteral("prerelease"), false},
            {QStringLiteral("assets"),
             QJsonArray{asset(QStringLiteral("Rostrum-0.3.1-aarch64.AppImage"), QStringLiteral("https://github.com/a"), 'a', 0),
                        asset(QStringLiteral("Rostrum-0.3.1-x86_64.AppImage"), QStringLiteral("https://github.com/b"), 'b', 7)}},
        });
        const updates::Release r = updates::parseFeed(json, QStringLiteral("x86_64"));
        QCOMPARE(r.version, QStringLiteral("0.3.1"));
        QCOMPARE(r.date, QDate(2026, 11, 2));
        QCOMPARE(r.appImage.url, QUrl(QStringLiteral("https://github.com/b")));
        QCOMPARE(r.appImage.sha256, QByteArray(64, 'b'));

        const QByteArray pre =
            toJson(QJsonObject{{QStringLiteral("tag_name"), QStringLiteral("v0.4.0")}, {QStringLiteral("prerelease"), true}});
        QVERIFY(!updates::parseFeed(pre, QStringLiteral("x86_64")).valid());
    }

    void feedRejects()
    {
        QString err;
        QVERIFY(!updates::parseFeed("not json", QStringLiteral("x86_64"), &err).valid());
        QVERIFY(!err.isEmpty());
        QVERIFY(!updates::parseFeed(toJson(QJsonObject{{QStringLiteral("version"), QStringLiteral("0.3.0-rc1")}}),
                                    QStringLiteral("x86_64"))
                     .valid());
        const auto feed = [](const QString &page, const QString &download, const QString &sha) {
            return toJson(QJsonObject{
                {QStringLiteral("version"), QStringLiteral("0.2.0")},
                {QStringLiteral("url"), page},
                {QStringLiteral("appimage"),
                 QJsonObject{{QStringLiteral("x86_64"), QJsonObject{{QStringLiteral("url"), download}, {QStringLiteral("sha256"), sha}}}}},
            });
        };
        // Downloads only over HTTPS (or loopback, for testing), and only with a full checksum.
        const updates::Release http =
            updates::parseFeed(feed(QStringLiteral("http://example.com/r"), QStringLiteral("http://example.com/x"), QString(64, u'c')),
                               QStringLiteral("x86_64"));
        QVERIFY(http.valid());
        QVERIFY(!http.appImage.valid());
        QVERIFY(http.page.isEmpty());
        const updates::Release shortSha = updates::parseFeed(
            feed(QStringLiteral("https://example.com/r"), QStringLiteral("https://example.com/x"), QStringLiteral("abc")),
            QStringLiteral("x86_64"));
        QVERIFY(shortSha.valid());
        QVERIFY(!shortSha.appImage.valid());
    }

    void versions()
    {
        QVERIFY(updates::isNewer(QStringLiteral("0.2.0"), QStringLiteral("0.1.0")));
        QVERIFY(updates::isNewer(QStringLiteral("v0.10.0"), QStringLiteral("0.9.9")));
        QVERIFY(updates::isNewer(QStringLiteral("1.0"), QStringLiteral("0.9")));
        QVERIFY(!updates::isNewer(QStringLiteral("0.1.0"), QStringLiteral("0.1.0")));
        QVERIFY(!updates::isNewer(QStringLiteral("0.0.9"), QStringLiteral("0.1.0")));
        QVERIFY(!updates::isNewer(QStringLiteral("0.2.0-beta"), QStringLiteral("0.1.0")));
        QVERIFY(!updates::isNewer(QString(), QStringLiteral("0.1.0")));
    }

    void installKind()
    {
        using updates::InstallKind;
        QCOMPARE(updates::detectInstallKind(QStringLiteral("/app/bin/rostrum"), {}, true, false), InstallKind::Flatpak);
        QCOMPARE(updates::detectInstallKind(QStringLiteral("/tmp/.mount_Rostru/usr/bin/rostrum"),
                                            QStringLiteral("/home/alex/Apps/Rostrum.AppImage"), false, false),
                 InstallKind::AppImage);
        QCOMPARE(updates::detectInstallKind(QStringLiteral("/usr/bin/rostrum"), {}, false, false), InstallKind::Package);
        QCOMPARE(updates::detectInstallKind(QStringLiteral("/snap/rostrum/1/usr/bin/rostrum"), {}, false, true), InstallKind::Package);
        QCOMPARE(updates::detectInstallKind(QStringLiteral("/usr/local/bin/rostrum"), {}, false, false), InstallKind::Source);
        QCOMPARE(updates::detectInstallKind(QStringLiteral("/home/alex/.local/bin/rostrum"), {}, false, false), InstallKind::Source);
    }

    void trustedUrls()
    {
        QVERIFY(updates::trustedUrl(QUrl(QStringLiteral("https://getrostrum.dev/releases/latest.json"))));
        QVERIFY(updates::trustedUrl(QUrl(QStringLiteral("http://127.0.0.1:8765/latest.json"))));
        QVERIFY(updates::trustedUrl(QUrl(QStringLiteral("http://localhost/x"))));
        QVERIFY(!updates::trustedUrl(QUrl(QStringLiteral("http://getrostrum.dev/x"))));
        QVERIFY(!updates::trustedUrl(QUrl(QStringLiteral("file:///tmp/x"))));
        QVERIFY(!updates::trustedUrl(QUrl()));
        QCOMPARE(updates::feedArch(QStringLiteral("arm64")), QStringLiteral("aarch64"));
        QCOMPARE(updates::feedArch(QStringLiteral("x86_64")), QStringLiteral("x86_64"));
    }
};

QTEST_GUILESS_MAIN(TestPrivacy)
#include "tst_privacy.moc"
