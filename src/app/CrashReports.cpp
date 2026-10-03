#include "app/CrashReports.h"

#include "app/AppController.h"
#include "app/Logging.h"
#include "app/Updater.h"
#include "core/CrashReport.h"
#include "core/Paths.h"
#include "core/UpdateFeed.h"

#include <KCoreAddons>

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QGuiApplication>
#include <QJSEngine>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLocale>
#include <QLoggingCategory>
#include <QNetworkReply>
#include <QSysInfo>
#include <QTimeZone>

#include <algorithm>

Q_LOGGING_CATEGORY(lcCrash, "rostrum.crash", QtInfoMsg)

namespace rostrum::app {

namespace {

constexpr int kKeepDays = 30;
constexpr int kKeepFiles = 10;
constexpr int kTimeoutMs = 15000;

qint64 crashTime(const QString &path)
{
    const QString name = QFileInfo(path).completeBaseName();
    bool ok = false;
    const qint64 secs = name.mid(name.indexOf(QLatin1Char('-')) + 1).toLongLong(&ok);
    return ok ? secs : QFileInfo(path).lastModified().toSecsSinceEpoch();
}

QDate crashDate(const QString &path)
{
    return QDateTime::fromSecsSinceEpoch(crashTime(path), QTimeZone::UTC).date();
}

QByteArray readAll(const QString &path)
{
    QFile f(path);
    return f.open(QIODevice::ReadOnly) ? f.read(256 * 1024) : QByteArray();
}

QMap<QString, QString> systemFields()
{
    QByteArray osRelease = readAll(QStringLiteral("/etc/os-release"));
    if (osRelease.isEmpty()) {
        osRelease = readAll(QStringLiteral("/usr/lib/os-release"));
    }
    return {
        {QStringLiteral("install"), updates::kindName(Updater::installKind())},
        {QStringLiteral("os"), crash::osName(QString::fromUtf8(osRelease))},
        {QStringLiteral("kernel"), QSysInfo::kernelVersion()},
        {QStringLiteral("arch"), QSysInfo::currentCpuArchitecture()},
        {QStringLiteral("desktop"), qEnvironmentVariable("XDG_CURRENT_DESKTOP")},
        {QStringLiteral("session"), qEnvironmentVariable("XDG_SESSION_TYPE")},
        {QStringLiteral("qt"), QString::fromLatin1(qVersion())},
        {QStringLiteral("kf"), KCoreAddons::versionString()},
    };
}

QJsonObject reportFor(const QString &path)
{
    const crash::CrashFile file = crash::parseCrashFile(readAll(path));
    return file.valid ? crash::buildReport(file, crashDate(path)) : QJsonObject();
}

} // namespace

CrashReports *CrashReports::s_instance = nullptr;

CrashReports::CrashReports(AppController *app, QObject *parent)
    : QObject(parent)
    , m_app(app)
{
    Q_ASSERT(!s_instance);
    s_instance = this;

    const QString override = qEnvironmentVariable("ROSTRUM_CRASH_URL");
    m_endpoint = QUrl(override.isEmpty() ? QStringLiteral(ROSTRUM_CRASH_URL) : override);
    // Screenshot and test runs never send anything unless a test endpoint is given.
    const bool testRun = QGuiApplication::platformName() == QLatin1String("offscreen") ||
                         !qEnvironmentVariableIsEmpty("ROSTRUM_SCREENSHOT");
    m_networkAllowed = updates::trustedUrl(m_endpoint) && (!testRun || !override.isEmpty());

    updateSystemContext();
    connect(app, &AppController::statusChanged, this, [this] {
        logging::setCrashContext(QStringLiteral("pipewire"), m_app->pipewireVersion());
        logging::setCrashContext(QStringLiteral("wireplumber"), m_app->wireplumberVersion());
    });
    connect(app, &AppController::settingsChanged, this, &CrashReports::changed);
}

CrashReports::~CrashReports()
{
    s_instance = nullptr;
}

CrashReports *CrashReports::create(QQmlEngine *, QJSEngine *)
{
    QJSEngine::setObjectOwnership(s_instance, QJSEngine::CppOwnership);
    return s_instance;
}

void CrashReports::updateSystemContext()
{
    const auto fields = systemFields();
    for (auto it = fields.cbegin(); it != fields.cend(); ++it) {
        logging::setCrashContext(it.key(), it.value());
    }
}

void CrashReports::start()
{
    scan();
    if (!m_pending.isEmpty()) {
        qCInfo(lcCrash) << m_pending.size() << "crash report(s) from earlier runs, mode" << mode();
    }
    if (mode() == QLatin1String(crashmode::kNever)) {
        discardPending();
    } else if (mode() == QLatin1String(crashmode::kSend)) {
        sendPending();
    }
    Q_EMIT changed();
}

void CrashReports::scan()
{
    QDir dir(paths::crashDir());
    QStringList files;
    for (const QString &name : dir.entryList({QStringLiteral("crash-*.txt")}, QDir::Files)) {
        files << dir.filePath(name);
    }
    std::sort(files.begin(), files.end(), [](const QString &a, const QString &b) { return crashTime(a) < crashTime(b); });
    const qint64 cutoff = QDateTime::currentSecsSinceEpoch() - qint64(kKeepDays) * 24 * 3600;
    QStringList kept;
    for (qsizetype i = 0; i < files.size(); ++i) {
        const bool tooOld = crashTime(files.at(i)) < cutoff;
        const bool tooMany = files.size() - i > kKeepFiles;
        if ((tooOld || tooMany) && !m_inFlight.contains(files.at(i))) {
            QFile::remove(files.at(i));
        } else {
            kept << files.at(i);
        }
    }
    if (kept != m_pending) {
        m_pending = kept;
        Q_EMIT changed();
    }
}

QString CrashReports::mode() const { return m_app->settings().crashReports; }

void CrashReports::setMode(const QString &mode)
{
    if (mode != QLatin1String(crashmode::kSend) && mode != QLatin1String(crashmode::kAsk) &&
        mode != QLatin1String(crashmode::kNever)) {
        return;
    }
    if (m_app->settings().crashReports == mode) {
        return;
    }
    m_app->settings().crashReports = mode;
    m_app->saveSettingsSoon();
    Q_EMIT m_app->settingsChanged();
}

bool CrashReports::askNow() const
{
    return !m_pending.isEmpty() && m_app->setupComplete() && mode() == QLatin1String(crashmode::kAsk);
}

QString CrashReports::lastCrashDate() const
{
    if (m_pending.isEmpty()) {
        return {};
    }
    const QDateTime when = QDateTime::fromSecsSinceEpoch(crashTime(m_pending.constLast()));
    return QLocale().toString(when, QLocale::ShortFormat);
}

QString CrashReports::pendingReportText() const
{
    QJsonArray reports;
    for (const QString &path : m_pending) {
        if (const QJsonObject r = reportFor(path); !r.isEmpty()) {
            reports.append(r);
        }
    }
    const QJsonDocument doc = reports.size() == 1 ? QJsonDocument(reports.first().toObject()) : QJsonDocument(reports);
    return QString::fromUtf8(doc.toJson(QJsonDocument::Indented)).trimmed();
}

QString CrashReports::exampleReportText() const
{
    crash::CrashFile file;
    file.valid = true;
    file.fields = systemFields();
    file.fields.insert(QStringLiteral("version"), QStringLiteral(ROSTRUM_VERSION));
    file.fields.insert(QStringLiteral("build_id"), logging::buildId());
    file.fields.insert(QStringLiteral("signal"), QStringLiteral("11"));
    file.fields.insert(QStringLiteral("uptime"), QStringLiteral("5400"));
    file.fields.insert(QStringLiteral("pipewire"), m_app->pipewireVersion());
    file.fields.insert(QStringLiteral("wireplumber"), m_app->wireplumberVersion());
    const QString self = QCoreApplication::applicationFilePath();
    file.frames = {
        self + QStringLiteral("(+0x4f2a1) [0x55d1c84f2a1]"),
        QStringLiteral("/lib/x86_64-linux-gnu/libQt6Core.so.6(_ZN7QObject5eventEP6QEvent+0x1c) [0x7f3a2c11d01c]"),
        QStringLiteral("/lib/x86_64-linux-gnu/libQt6Core.so.6(_ZN16QCoreApplication6notifyEP7QObjectP6QEvent+0x5e) [0x7f3a2c0e1f5e]"),
        self + QStringLiteral("(+0x2c1b0) [0x55d1c82c1b0]"),
    };
    const QJsonObject report = crash::buildReport(file, QDate::currentDate());
    return QString::fromUtf8(QJsonDocument(report).toJson(QJsonDocument::Indented)).trimmed();
}

void CrashReports::sendPending()
{
    if (!m_networkAllowed) {
        qCInfo(lcCrash) << "Not sending crash reports from a test run";
        return;
    }
    for (const QString &path : std::as_const(m_pending)) {
        if (!m_inFlight.contains(path)) {
            upload(path);
        }
    }
}

void CrashReports::upload(const QString &path)
{
    const QJsonObject report = reportFor(path);
    if (report.isEmpty()) {
        QFile::remove(path);
        scan();
        return;
    }
    QNetworkRequest request(m_endpoint);
    request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
    request.setHeader(QNetworkRequest::UserAgentHeader, QStringLiteral("Rostrum/" ROSTRUM_VERSION));
    request.setAttribute(QNetworkRequest::CookieLoadControlAttribute, QNetworkRequest::Manual);
    request.setAttribute(QNetworkRequest::CookieSaveControlAttribute, QNetworkRequest::Manual);
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
    request.setTransferTimeout(kTimeoutMs);

    m_inFlight << path;
    QNetworkReply *reply = m_net.post(request, QJsonDocument(report).toJson(QJsonDocument::Compact));
    connect(reply, &QNetworkReply::finished, this, [this, reply, path] {
        reply->deleteLater();
        m_inFlight.removeAll(path);
        const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        if (status >= 200 && status < 300) {
            qCInfo(lcCrash) << "Crash report sent";
            QFile::remove(path);
        } else if (status >= 400 && status < 500) {
            qCWarning(lcCrash) << "Crash report refused with HTTP" << status << "- dropped";
            QFile::remove(path);
        } else {
            qCInfo(lcCrash) << "Crash report not sent, will retry next start:" << reply->errorString();
        }
        scan();
    });
}

void CrashReports::discardPending()
{
    for (const QString &path : std::as_const(m_pending)) {
        if (!m_inFlight.contains(path)) {
            QFile::remove(path);
        }
    }
    scan();
}

} // namespace rostrum::app
