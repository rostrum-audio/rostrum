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

QJsonObject sentryEvent()
{
    // What sentry-native 0.17 (inproc) writes for a SIGSEGV, plus fields a scrubber must drop.
    const QJsonObject frameOwn{{QStringLiteral("instruction_addr"), QStringLiteral("0x600ccfb13885")},
                               {QStringLiteral("package"), QStringLiteral("/home/alex/.local/bin/rostrum")},
                               {QStringLiteral("image_addr"), QStringLiteral("0x600ccfb10000")}};
    const QJsonObject frameLibc{{QStringLiteral("instruction_addr"), QStringLiteral("0x76e81e22a718")},
                                {QStringLiteral("function"), QStringLiteral("__libc_start_main")},
                                {QStringLiteral("package"), QStringLiteral("/usr/lib/x86_64-linux-gnu/libc.so.6")},
                                {QStringLiteral("symbol_addr"), QStringLiteral("0x76e81e22a690")},
                                {QStringLiteral("image_addr"), QStringLiteral("0x76e81e200000")},
                                {QStringLiteral("abs_path"), QStringLiteral("/home/alex/src/x.c")}};
    const QJsonObject framePath{{QStringLiteral("instruction_addr"), QStringLiteral("0x76e81e2a61ac")},
                                {QStringLiteral("function"), QStringLiteral("/home/alex/HyperX")}};
    const QJsonObject exception{
        {QStringLiteral("type"), QStringLiteral("SIGSEGV")},
        {QStringLiteral("value"), QStringLiteral("Segfault")},
        {QStringLiteral("mechanism"),
         QJsonObject{{QStringLiteral("type"), QStringLiteral("signalhandler")},
                     {QStringLiteral("synthetic"), true},
                     {QStringLiteral("handled"), false},
                     {QStringLiteral("meta"), QJsonObject{{QStringLiteral("signal"), QJsonObject{{QStringLiteral("name"), QStringLiteral("SIGSEGV")},
                                                                                                  {QStringLiteral("number"), 11}}}}}}},
        {QStringLiteral("stacktrace"),
         QJsonObject{{QStringLiteral("frames"), QJsonArray{frameOwn, frameLibc, framePath}},
                     {QStringLiteral("registers"), QJsonObject{{QStringLiteral("rip"), QStringLiteral("0x76e81e2a61ac")}}}}},
    };
    return QJsonObject{
        {QStringLiteral("event_id"), QStringLiteral("eeb60f0630364806e8952798b6d06283")},
        {QStringLiteral("timestamp"), QStringLiteral("2026-10-03T05:21:54.332753Z")},
        {QStringLiteral("platform"), QStringLiteral("native")},
        {QStringLiteral("level"), QStringLiteral("fatal")},
        {QStringLiteral("exception"), QJsonObject{{QStringLiteral("values"), QJsonArray{exception}}}},
        {QStringLiteral("tags"), QJsonObject{{QStringLiteral("host"), QStringLiteral("alex-desktop")}}},
        {QStringLiteral("extra"), QJsonObject{{QStringLiteral("scene"), QStringLiteral("alex stream")}}},
        {QStringLiteral("release"), QStringLiteral("rostrum@0.1.0")},
        {QStringLiteral("environment"), QStringLiteral("production")},
        {QStringLiteral("server_name"), QStringLiteral("alex-desktop")},
        {QStringLiteral("user"), QJsonObject{{QStringLiteral("id"), QStringLiteral("6daf31dc-4224-4fad-0344-c33cb701c3af")}}},
        {QStringLiteral("sdk"), QJsonObject{{QStringLiteral("name"), QStringLiteral("sentry.native")}, {QStringLiteral("version"), QStringLiteral("0.17.1")}}},
        {QStringLiteral("contexts"),
         QJsonObject{{QStringLiteral("os"), QJsonObject{{QStringLiteral("build"), QStringLiteral("38-generic")},
                                                        {QStringLiteral("name"), QStringLiteral("Linux")},
                                                        {QStringLiteral("version"), QStringLiteral("7.0.0")},
                                                        {QStringLiteral("distribution_name"), QStringLiteral("ubuntu")},
                                                        {QStringLiteral("distribution_version"), QStringLiteral("26.04")},
                                                        {QStringLiteral("distribution_pretty_name"), QStringLiteral("Ubuntu 26.04.1 LTS")}}},
                     {QStringLiteral("rostrum"), QJsonObject{{QStringLiteral("pipewire"), QStringLiteral("1.6.2")},
                                                             {QStringLiteral("headphones"), QStringLiteral("HyperX Cloud")}}},
                     {QStringLiteral("trace"), QJsonObject{{QStringLiteral("trace_id"), QStringLiteral("93d0807898fb48c1d37022284faac6bb")},
                                                           {QStringLiteral("sample_rand"), 0.87}}}}},
        {QStringLiteral("breadcrumbs"), QJsonValue::Null},
        {QStringLiteral("debug_meta"),
         QJsonObject{{QStringLiteral("images"),
                      QJsonArray{QJsonObject{{QStringLiteral("type"), QStringLiteral("elf")},
                                             {QStringLiteral("code_file"), QStringLiteral("/home/alex/.local/bin/rostrum")},
                                             {QStringLiteral("image_addr"), QStringLiteral("0x600ccfb10000")},
                                             {QStringLiteral("image_size"), 454656},
                                             {QStringLiteral("code_id"), QStringLiteral("f1bf217635bcdb532534a6672f25d94e131dae8f")},
                                             {QStringLiteral("debug_id"), QStringLiteral("7621bff1-bc35-53db-2534-a6672f25d94e")}},
                                 QJsonObject{{QStringLiteral("type"), QStringLiteral("elf")},
                                             {QStringLiteral("code_file"), QStringLiteral("/usr/lib/x86_64-linux-gnu/libc.so.6")},
                                             {QStringLiteral("image_addr"), QStringLiteral("0x76e81e200000")},
                                             {QStringLiteral("image_size"), 2179072}},
                                 QJsonObject{{QStringLiteral("type"), QStringLiteral("elf")},
                                             {QStringLiteral("code_file"), QStringLiteral("/usr/lib/libgame-overlay.so")},
                                             {QStringLiteral("image_addr"), QStringLiteral("0x7f0000000000")},
                                             {QStringLiteral("image_size"), 4096}},
                                 QJsonObject{{QStringLiteral("type"), QStringLiteral("elf")},
                                             {QStringLiteral("code_file"), QStringLiteral("/home/alex/no-address.so")}}}}}},
    };
}

} // namespace

class TestPrivacy : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void dsn()
    {
        const crash::Dsn d = crash::parseDsn(QStringLiteral("https://abc123@o1.ingest.us.sentry.io/42"));
        QVERIFY(d.valid());
        QCOMPARE(d.publicKey, QStringLiteral("abc123"));
        QCOMPARE(d.envelopeUrl, QUrl(QStringLiteral("https://o1.ingest.us.sentry.io/api/42/envelope/")));

        const crash::Dsn prefixed = crash::parseDsn(QStringLiteral("http://k@127.0.0.1:8765/sentry/7"));
        QCOMPARE(prefixed.envelopeUrl, QUrl(QStringLiteral("http://127.0.0.1:8765/sentry/api/7/envelope/")));

        QVERIFY(!crash::parseDsn(QString()).valid());
        QVERIFY(!crash::parseDsn(QStringLiteral("https://o1.ingest.us.sentry.io/42")).valid());
        QVERIFY(!crash::parseDsn(QStringLiteral("https://abc@o1.ingest.us.sentry.io/project")).valid());
    }

    void envelopeRoundTrip()
    {
        const QJsonObject event = sentryEvent();
        const QByteArray payload = QJsonDocument(event).toJson(QJsonDocument::Compact);
        // A session item first, then the event with an explicit length, as sentry-native writes them.
        const QByteArray envelope = "{\"event_id\":\"eeb60f0630364806e8952798b6d06283\"}\n"
                                    "{\"type\":\"session\"}\n{\"sid\":\"x\"}\n"
                                    "{\"type\":\"event\",\"length\":" +
                                    QByteArray::number(payload.size()) + "}\n" + payload + "\n";
        QCOMPARE(crash::eventFromEnvelope(envelope), event);
        QVERIFY(crash::eventFromEnvelope("not an envelope").isEmpty());
        QVERIFY(crash::eventFromEnvelope("{}\n{\"type\":\"event\",\"length\":999}\n{}\n").isEmpty());

        const QJsonObject scrubbed = crash::scrubEvent(event);
        QCOMPARE(crash::eventFromEnvelope(crash::toEnvelope(scrubbed)), scrubbed);
        QVERIFY(crash::toEnvelope(scrubbed).startsWith("{\"event_id\":\"eeb60f0630364806e8952798b6d06283\"}\n"));
    }

    void scrubRemovesPersonalData()
    {
        const QJsonObject report = crash::scrubEvent(sentryEvent());
        const QByteArray json = QJsonDocument(report).toJson(QJsonDocument::Compact);

        // The installation ID, the user's folders and the device name never survive.
        QVERIFY(!json.contains("6daf31dc"));
        QVERIFY(!json.contains("/home"));
        QVERIFY(!json.contains("alex"));
        QVERIFY(!json.contains("HyperX"));
        QVERIFY(!json.contains("Ubuntu 26.04.1 LTS"));
        for (const char *key : {"user", "timestamp", "tags", "extra", "breadcrumbs", "server_name", "modules", "request"}) {
            QVERIFY2(!report.contains(QLatin1String(key)), key);
        }
        QVERIFY(!json.contains("registers"));
        QVERIFY(!json.contains("trace_id"));
        QVERIFY(!json.contains("sample_rand"));
        // A library loaded into Rostrum but not in the stack says what else is installed.
        QVERIFY(!json.contains("overlay"));
    }

    void scrubKeepsWhatFindsTheBug()
    {
        const QJsonObject report = crash::scrubEvent(sentryEvent());
        QCOMPARE(report.value(QStringLiteral("event_id")).toString(), QStringLiteral("eeb60f0630364806e8952798b6d06283"));
        QCOMPARE(report.value(QStringLiteral("release")).toString(), QStringLiteral("rostrum@0.1.0"));
        QCOMPARE(report.value(QStringLiteral("level")).toString(), QStringLiteral("fatal"));

        const QJsonObject exception = report.value(QStringLiteral("exception")).toObject().value(QStringLiteral("values")).toArray().first().toObject();
        QCOMPARE(exception.value(QStringLiteral("type")).toString(), QStringLiteral("SIGSEGV"));
        QCOMPARE(exception.value(QStringLiteral("mechanism")).toObject().value(QStringLiteral("meta")).toObject()
                     .value(QStringLiteral("signal")).toObject().value(QStringLiteral("number")).toInt(), 11);
        const QJsonArray frames = exception.value(QStringLiteral("stacktrace")).toObject().value(QStringLiteral("frames")).toArray();
        QCOMPARE(frames.size(), 3);
        const QJsonObject own = frames.at(0).toObject();
        QCOMPARE(own.value(QStringLiteral("package")).toString(), QStringLiteral("rostrum"));
        QCOMPARE(own.value(QStringLiteral("instruction_addr")).toString(), QStringLiteral("0x600ccfb13885"));
        QCOMPARE(own.value(QStringLiteral("image_addr")).toString(), QStringLiteral("0x600ccfb10000"));
        const QJsonObject libc = frames.at(1).toObject();
        QCOMPARE(libc.value(QStringLiteral("package")).toString(), QStringLiteral("libc.so.6"));
        QCOMPARE(libc.value(QStringLiteral("function")).toString(), QStringLiteral("__libc_start_main"));
        // A "function" that is really a path is dropped, the frame kept.
        QVERIFY(!frames.at(2).toObject().contains(QStringLiteral("function")));

        const QJsonArray images = report.value(QStringLiteral("debug_meta")).toObject().value(QStringLiteral("images")).toArray();
        QCOMPARE(images.size(), 2);
        QCOMPARE(images.at(0).toObject().value(QStringLiteral("code_file")).toString(), QStringLiteral("rostrum"));
        QCOMPARE(images.at(0).toObject().value(QStringLiteral("debug_id")).toString(), QStringLiteral("7621bff1-bc35-53db-2534-a6672f25d94e"));
        QCOMPARE(images.at(0).toObject().value(QStringLiteral("image_size")).toInt(), 454656);

        const QJsonObject contexts = report.value(QStringLiteral("contexts")).toObject();
        QCOMPARE(contexts.keys(), (QStringList{QStringLiteral("os"), QStringLiteral("rostrum")}));
        QCOMPARE(contexts.value(QStringLiteral("os")).toObject().value(QStringLiteral("distribution_version")).toString(), QStringLiteral("26.04"));
        const QJsonObject own2 = contexts.value(QStringLiteral("rostrum")).toObject();
        QCOMPARE(own2.value(QStringLiteral("pipewire")).toString(), QStringLiteral("1.6.2"));
        QVERIFY(!own2.contains(QStringLiteral("headphones")));
    }

    void scrubKeepsTheCrashEndOfLongStacks()
    {
        QJsonArray frames;
        for (int i = 0; i < crash::kMaxFrames + 10; ++i) {
            frames.append(QJsonObject{{QStringLiteral("instruction_addr"), QStringLiteral("0x%1").arg(i + 1, 0, 16)}});
        }
        const QJsonObject event{{QStringLiteral("exception"),
                                 QJsonObject{{QStringLiteral("values"),
                                              QJsonArray{QJsonObject{{QStringLiteral("type"), QStringLiteral("SIGABRT")},
                                                                     {QStringLiteral("stacktrace"), QJsonObject{{QStringLiteral("frames"), frames}}}}}}}}};
        const QJsonArray kept = crash::scrubEvent(event).value(QStringLiteral("exception")).toObject().value(QStringLiteral("values"))
                                    .toArray().first().toObject().value(QStringLiteral("stacktrace")).toObject().value(QStringLiteral("frames")).toArray();
        QCOMPARE(kept.size(), crash::kMaxFrames);
        QCOMPARE(kept.last().toObject().value(QStringLiteral("instruction_addr")).toString(), QStringLiteral("0x%1").arg(crash::kMaxFrames + 10, 0, 16));
    }

    void fileNames_data()
    {
        QTest::addColumn<QString>("path");
        QTest::addColumn<QString>("expected");
        QTest::newRow("system library") << QStringLiteral("/usr/lib/x86_64-linux-gnu/libQt6Core.so.6") << QStringLiteral("libQt6Core.so.6");
        QTest::newRow("home folder") << QStringLiteral("/home/alex/.local/bin/rostrum") << QStringLiteral("rostrum");
        QTest::newRow("appimage mount") << QStringLiteral("/tmp/.mount_RostruXk2/usr/lib/libsentry.so") << QStringLiteral("libsentry.so");
        QTest::newRow("relative") << QStringLiteral("./build/src/app/rostrum") << QStringLiteral("rostrum");
        QTest::newRow("odd characters") << QStringLiteral("/home/alex/alex's plugin.so") << QStringLiteral("?");
        QTest::newRow("bare name") << QStringLiteral("linux-gate.so") << QStringLiteral("linux-gate.so");
    }

    void fileNames()
    {
        QFETCH(QString, path);
        QFETCH(QString, expected);
        QCOMPARE(crash::fileName(path), expected);
    }

    void sanitizeValue()
    {
        QCOMPARE(crash::sanitizeValue(QStringLiteral("KDE")), QStringLiteral("KDE"));
        QCOMPARE(crash::sanitizeValue(QStringLiteral("6.24.0\n\"x\"")), QStringLiteral("6.24.0x"));
        QCOMPARE(crash::sanitizeValue(QString(100, QLatin1Char('a'))).size(), 64);
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
