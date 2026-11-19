#include "app/AppController.h"
#include "app/Updater.h"

#include <KLocalizedString>
#include <QCryptographicHash>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QSysInfo>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QTest>

using namespace rostrum;

namespace {

// Serves the release feed and one AppImage over plain HTTP on localhost, counting image downloads.
class FeedServer : public QObject
{
public:
    QByteArray image = QByteArrayLiteral("new appimage bytes");
    QByteArray advertised = image; // what the feed's checksum and size describe
    int imageGets = 0;

    bool listen() { return m_server.listen(QHostAddress::LocalHost, 0); }
    QString base() const { return QStringLiteral("http://127.0.0.1:%1").arg(m_server.serverPort()); }

    QByteArray feed() const
    {
        const QString arch = updates::feedArch(QSysInfo::currentCpuArchitecture());
        const QJsonObject asset{
            {QStringLiteral("url"), base() + QStringLiteral("/Rostrum.AppImage")},
            {QStringLiteral("sha256"),
             QString::fromLatin1(QCryptographicHash::hash(advertised, QCryptographicHash::Sha256).toHex())},
            {QStringLiteral("size"), advertised.size()},
        };
        return QJsonDocument(QJsonObject{{QStringLiteral("version"), QStringLiteral("999.0.0")},
                                         {QStringLiteral("appimage"), QJsonObject{{arch, asset}}}})
            .toJson(QJsonDocument::Compact);
    }

    FeedServer()
    {
        connect(&m_server, &QTcpServer::newConnection, this, [this] {
            while (QTcpSocket *sock = m_server.nextPendingConnection()) {
                connect(sock, &QTcpSocket::readyRead, sock, [this, sock] {
                    const QByteArray req = sock->readAll();
                    const bool wantsImage = req.startsWith("GET /Rostrum.AppImage");
                    if (wantsImage) {
                        ++imageGets;
                    }
                    const QByteArray body = wantsImage ? image : feed();
                    sock->write("HTTP/1.1 200 OK\r\nConnection: close\r\nContent-Length: " +
                                QByteArray::number(body.size()) + "\r\n\r\n" + body);
                    sock->disconnectFromHost();
                });
                connect(sock, &QTcpSocket::disconnected, sock, &QObject::deleteLater);
            }
        });
    }

private:
    QTcpServer m_server;
};

} // namespace

class TestUpdater : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void initTestCase()
    {
        KLocalizedString::setApplicationDomain("rostrum");
        QStandardPaths::setTestModeEnabled(true);
    }

    // Once an update is installed and waits for a restart, Check now must not download it again
    // (a failed second download reported "could not be installed") or drop the restart.
    void checkNowWhileReadyKeepsTheInstalledUpdate()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString image = dir.filePath(QStringLiteral("Rostrum.AppImage"));
        {
            QFile f(image);
            QVERIFY(f.open(QIODevice::WriteOnly));
            f.write("old appimage bytes");
        }
        FeedServer server;
        QVERIFY(server.listen());
        qputenv("APPIMAGE", image.toUtf8());
        qputenv("ROSTRUM_UPDATE_URL", (server.base() + QStringLiteral("/latest.json")).toUtf8());
        qputenv("XDG_CONFIG_HOME", dir.filePath(QStringLiteral("config")).toUtf8());

        app::AppController app(nullptr);
        app::Updater updater(&app, nullptr);
        QVERIFY(updater.canInstall());
        updater.setAutoInstall(true);

        updater.checkNow();
        QTRY_COMPARE_WITH_TIMEOUT(updater.state(), QStringLiteral("ready"), 10000);
        QCOMPARE(server.imageGets, 1);

        // A second download would replace the installed file; make it fail if it is attempted.
        server.image = QByteArrayLiteral("bytes that do not match");
        updater.checkNow();
        QCOMPARE(updater.state(), QStringLiteral("ready"));
        QTest::qWait(1000);
        QCOMPARE(updater.state(), QStringLiteral("ready"));
        QCOMPARE(updater.errorText(), QString());
        QCOMPARE(server.imageGets, 1);
        QFile installed(image);
        QVERIFY(installed.open(QIODevice::ReadOnly));
        QCOMPARE(installed.readAll(), QByteArrayLiteral("new appimage bytes"));

        // Restart Now still works.
        QSignalSpy restart(&updater, &app::Updater::restartRequested);
        updater.restart();
        QCOMPARE(restart.count(), 1);
        QCOMPARE(restart.first().first().toString(), image);
    }
};

QTEST_GUILESS_MAIN(TestUpdater)
#include "tst_updater.moc"
