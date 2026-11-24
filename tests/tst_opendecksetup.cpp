#include "IsolatedHome.h"
#include "app/AppController.h"
#include "app/OpenDeckSetup.h"
#include "core/OpenDeck.h"

#include <KLocalizedString>
#include <QDesktopServices>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTest>
#include <QUrl>

using namespace rostrum;

// Catches the OpenDeck website link, so no browser opens during the test.
class UrlCatcher : public QObject
{
    Q_OBJECT
public:
    QList<QUrl> urls;
public Q_SLOTS:
    void open(const QUrl &url) { urls << url; }
};

// The notice's Update runs activate(); Desktop reports its `finished`. Every activate() must end
// in exactly one `finished` that tells the truth, or the notice says the wrong thing or a later
// install from Settings pops up a desktop notification.
class TestOpenDeckSetup : public QObject
{
    Q_OBJECT
    test::IsolatedHome m_home;
    UrlCatcher m_urls;

    static void setVersion(const QString &plugins, const QString &version)
    {
        const QString path = plugins + QLatin1Char('/') + opendeck::pluginId() + QStringLiteral("/manifest.json");
        QFile file(path);
        QVERIFY(file.open(QIODevice::ReadOnly));
        auto object = QJsonDocument::fromJson(file.readAll()).object();
        file.close();
        object.insert(QStringLiteral("Version"), version);
        QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
        file.write(QJsonDocument(object).toJson());
    }

private Q_SLOTS:
    void initTestCase()
    {
        // Nothing here may reach the user's home, config, audio session or browser.
        const QString leak = m_home.leak();
        QVERIFY2(leak.isEmpty(), qPrintable(QStringLiteral("would write outside the test folder: ") + leak));
        KLocalizedString::setApplicationDomain("rostrum");
        QStandardPaths::setTestModeEnabled(true);
        QDesktopServices::setUrlHandler(QStringLiteral("https"), &m_urls, "open");
    }

    void updateOnACurrentPluginReportsSuccess()
    {
        // Updated from Settings meanwhile: the notice's Update finds nothing to do.
        QTemporaryDir plugins;
        QCOMPARE(opendeck::install({QStringLiteral("custom"), plugins.path()}, true).error, opendeck::Error::None);
        app::AppController app(nullptr);
        app.settings().openDeckPluginsFolder = plugins.path();
        app::OpenDeckSetup setup(&app, nullptr);
        QSignalSpy finished(&setup, &app::OpenDeckSetup::finished);
        setup.activate();
        QCOMPARE(finished.count(), 1);
        QCOMPARE(finished.at(0).at(0).toBool(), true);
    }

    void updateWithoutOpenDeckStillFinishes()
    {
        app::AppController app(nullptr);
        app::OpenDeckSetup setup(&app, nullptr);
        QSignalSpy finished(&setup, &app::OpenDeckSetup::finished);
        m_urls.urls.clear();
        setup.activate();
        QCOMPARE(m_urls.urls.size(), 1); // the website, caught above
        QCOMPARE(finished.count(), 1);
        QCOMPARE(finished.at(0).at(0).toBool(), false);
    }

    void updateWhileInstallingFinishesOnce()
    {
        // Update in the notice while an install from Settings runs: that install's result is the one.
        QTemporaryDir plugins;
        QCOMPARE(opendeck::install({QStringLiteral("custom"), plugins.path()}, true).error, opendeck::Error::None);
        setVersion(plugins.path(), QStringLiteral("1.3.0"));
        app::AppController app(nullptr);
        app.settings().openDeckPluginsFolder = plugins.path();
        app::OpenDeckSetup setup(&app, nullptr);
        QSignalSpy finished(&setup, &app::OpenDeckSetup::finished);
        setup.activate();
        setup.activate();
        QTRY_COMPARE(finished.count(), 1);
        QCOMPARE(finished.at(0).at(0).toBool(), true);
        QTest::qWait(50);
        QCOMPARE(finished.count(), 1);
        QCOMPARE(opendeck::installedVersion({QStringLiteral("custom"), plugins.path()}), opendeck::bundledVersion());
    }
};

QTEST_MAIN(TestOpenDeckSetup)
#include "tst_opendecksetup.moc"
