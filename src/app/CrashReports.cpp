#include "app/CrashReports.h"

#include "app/AppController.h"
#include "app/CrashCapture.h"
#include "app/Updater.h"
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
#include <QUuid>

#include <algorithm>

Q_LOGGING_CATEGORY(lcCrash, "rostrum.crash", QtInfoMsg)

namespace rostrum::app {

namespace {

constexpr int kKeepDays = 30;
constexpr int kKeepFiles = 10;
constexpr int kTimeoutMs = 15000;

// crash-<seconds>-<n>.envelope, named when sentry handed the envelope over.
qint64 fileTime(const QString &path)
{
    const QStringList parts = QFileInfo(path).completeBaseName().split(QLatin1Char('-'));
    bool ok = false;
    const qint64 secs = parts.size() >= 2 ? parts.at(1).toLongLong(&ok) : 0;
    return ok ? secs : QFileInfo(path).lastModified().toSecsSinceEpoch();
}

QJsonObject rawEvent(const QString &path)
{
    QFile f(path);
    return f.open(QIODevice::ReadOnly) ? crash::eventFromEnvelope(f.read(4 * 1024 * 1024)) : QJsonObject();
}

QJsonObject reportFor(const QString &path)
{
    const QJsonObject event = rawEvent(path);
    return event.isEmpty() ? QJsonObject() : crash::scrubEvent(event);
}

QString hexAddress(quint64 value) { return QStringLiteral("0x") + QString::number(value, 16); }

} // namespace

CrashReports *CrashReports::s_instance = nullptr;

CrashReports::CrashReports(AppController *app, QObject *parent)
    : QObject(parent)
    , m_app(app)
{
    Q_ASSERT(!s_instance);
    s_instance = this;

    const QString override = qEnvironmentVariable("ROSTRUM_SENTRY_DSN");
    m_dsnText = override.isEmpty() ? QStringLiteral(ROSTRUM_SENTRY_DSN) : override;
    m_dsn = crash::parseDsn(m_dsnText);
    m_available = crashcapture::compiledIn() && m_dsn.valid() && updates::trustedUrl(m_dsn.envelopeUrl);
    // Screenshot and test runs never send anything unless a test DSN is given.
    const bool testRun = QGuiApplication::platformName() == QLatin1String("offscreen") ||
                         !qEnvironmentVariableIsEmpty("ROSTRUM_SCREENSHOT");
    m_networkAllowed = m_available && (!testRun || !override.isEmpty());

    connect(app, &AppController::statusChanged, this, [this] { crashcapture::setContext(contextFields()); });
    connect(app, &AppController::settingsChanged, this, &CrashReports::changed);
}

CrashReports::~CrashReports()
{
    crashcapture::stop();
    s_instance = nullptr;
}

CrashReports *CrashReports::create(QQmlEngine *, QJSEngine *)
{
    QJSEngine::setObjectOwnership(s_instance, QJSEngine::CppOwnership);
    return s_instance;
}

QMap<QString, QString> CrashReports::contextFields() const
{
    return {
        {QStringLiteral("install"), updates::kindName(Updater::installKind())},
        {QStringLiteral("arch"), QSysInfo::currentCpuArchitecture()},
        {QStringLiteral("desktop"), qEnvironmentVariable("XDG_CURRENT_DESKTOP")},
        {QStringLiteral("session"), qEnvironmentVariable("XDG_SESSION_TYPE")},
        {QStringLiteral("qt"), QString::fromLatin1(qVersion())},
        {QStringLiteral("kf"), KCoreAddons::versionString()},
        {QStringLiteral("pipewire"), m_app->pipewireVersion()},
        {QStringLiteral("wireplumber"), m_app->wireplumberVersion()},
    };
}

void CrashReports::start()
{
    if (!m_available) {
        return;
    }
    applyMode();
    scan();
    if (!m_pending.isEmpty()) {
        qCInfo(lcCrash) << m_pending.size() << "crash report(s) from earlier runs, mode" << mode();
    }
    if (mode() == QLatin1String(crashmode::kSend)) {
        sendPending();
    }
    Q_EMIT changed();
}

void CrashReports::applyMode()
{
    if (mode() == QLatin1String(crashmode::kNever)) {
        crashcapture::stop();
        // Nothing is captured and nothing is kept, including sentry's own database.
        QDir(paths::sentryDir()).removeRecursively();
        discardPending();
        return;
    }
    if (!crashcapture::running()) {
        if (crashcapture::start(m_dsnText, paths::sentryDir(), paths::crashDir())) {
            qCInfo(lcCrash) << "Capturing crashes for reports to" << destination();
        } else {
            qCWarning(lcCrash) << "Could not start crash capture";
        }
    }
    crashcapture::setContext(contextFields());
}

void CrashReports::scan()
{
    QDir dir(paths::crashDir());
    QStringList files;
    for (const QString &name : dir.entryList({QStringLiteral("crash-*.envelope")}, QDir::Files)) {
        files << dir.filePath(name);
    }
    std::sort(files.begin(), files.end(), [](const QString &a, const QString &b) {
        return fileTime(a) != fileTime(b) ? fileTime(a) < fileTime(b) : a < b;
    });
    const qint64 cutoff = QDateTime::currentSecsSinceEpoch() - qint64(kKeepDays) * 24 * 3600;
    QStringList kept;
    for (qsizetype i = 0; i < files.size(); ++i) {
        const bool tooOld = fileTime(files.at(i)) < cutoff;
        const bool tooMany = files.size() - i > kKeepFiles;
        const bool noCrash = rawEvent(files.at(i)).isEmpty();
        if ((tooOld || tooMany || noCrash) && !m_inFlight.contains(files.at(i))) {
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
    if (m_available) {
        applyMode();
        if (mode == QLatin1String(crashmode::kSend)) {
            sendPending();
        }
    }
    Q_EMIT m_app->settingsChanged();
}

bool CrashReports::askNow() const
{
    return m_available && !m_pending.isEmpty() && m_app->setupComplete() && mode() == QLatin1String(crashmode::kAsk);
}

QString CrashReports::lastCrashDate() const
{
    if (m_pending.isEmpty()) {
        return {};
    }
    QDateTime when = crash::eventTime(rawEvent(m_pending.constLast()));
    if (!when.isValid()) {
        when = QDateTime::fromSecsSinceEpoch(fileTime(m_pending.constLast()));
    }
    return QLocale().toString(when.toLocalTime(), QLocale::ShortFormat);
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
    // Shaped like what sentry-native captures, including the parts the scrubber removes.
    const QString self = QCoreApplication::applicationFilePath();
    const QString qtCore = QStringLiteral("/usr/lib/x86_64-linux-gnu/libQt6Core.so.6");
    const quint64 selfBase = 0x55d1c8400000;
    const quint64 qtBase = 0x7f3a2c000000;
    const auto frame = [](quint64 base, quint64 offset, const QString &package, const QString &function) {
        QJsonObject f{{QStringLiteral("instruction_addr"), hexAddress(base + offset)},
                      {QStringLiteral("image_addr"), hexAddress(base)},
                      {QStringLiteral("package"), package}};
        if (!function.isEmpty()) {
            f.insert(QStringLiteral("function"), function);
        }
        return f;
    };
    QJsonObject own;
    const auto fields = contextFields();
    for (auto it = fields.cbegin(); it != fields.cend(); ++it) {
        own.insert(it.key(), it.value());
    }
    const QJsonObject event{
        {QStringLiteral("event_id"), QUuid::createUuid().toString(QUuid::Id128)},
        {QStringLiteral("timestamp"), QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs)},
        {QStringLiteral("level"), QStringLiteral("fatal")},
        {QStringLiteral("release"), QStringLiteral("rostrum@" ROSTRUM_VERSION)},
        {QStringLiteral("environment"), QStringLiteral("production")},
        {QStringLiteral("user"), QJsonObject{{QStringLiteral("id"), QUuid::createUuid().toString(QUuid::WithoutBraces)}}},
        {QStringLiteral("sdk"), QJsonObject{{QStringLiteral("name"), QStringLiteral("sentry.native")},
                                            {QStringLiteral("version"), QStringLiteral("0.17.1")}}},
        {QStringLiteral("exception"),
         QJsonObject{{QStringLiteral("values"),
                      QJsonArray{QJsonObject{
                          {QStringLiteral("type"), QStringLiteral("SIGSEGV")},
                          {QStringLiteral("value"), QStringLiteral("Segfault")},
                          {QStringLiteral("mechanism"),
                           QJsonObject{{QStringLiteral("type"), QStringLiteral("signalhandler")},
                                       {QStringLiteral("handled"), false},
                                       {QStringLiteral("meta"),
                                        QJsonObject{{QStringLiteral("signal"),
                                                     QJsonObject{{QStringLiteral("name"), QStringLiteral("SIGSEGV")},
                                                                 {QStringLiteral("number"), 11}}}}}}},
                          {QStringLiteral("stacktrace"),
                           QJsonObject{{QStringLiteral("frames"),
                                        QJsonArray{frame(selfBase, 0x2c1b0, self, {}),
                                                   frame(qtBase, 0x1d01c, qtCore, QStringLiteral("_ZN7QObject5eventEP6QEvent")),
                                                   frame(selfBase, 0x4f2a1, self, {})}},
                                       {QStringLiteral("registers"), QJsonObject{{QStringLiteral("rip"), hexAddress(selfBase + 0x4f2a1)}}}}},
                      }}}}},
        {QStringLiteral("contexts"),
         QJsonObject{{QStringLiteral("os"),
                      QJsonObject{{QStringLiteral("name"), QSysInfo::kernelType()},
                                  {QStringLiteral("version"), QSysInfo::kernelVersion()},
                                  {QStringLiteral("distribution_name"), QSysInfo::productType()},
                                  {QStringLiteral("distribution_version"), QSysInfo::productVersion()}}},
                     {QStringLiteral("rostrum"), own}}},
        {QStringLiteral("debug_meta"),
         QJsonObject{{QStringLiteral("images"),
                      QJsonArray{QJsonObject{{QStringLiteral("type"), QStringLiteral("elf")},
                                             {QStringLiteral("code_file"), self},
                                             {QStringLiteral("image_addr"), hexAddress(selfBase)},
                                             {QStringLiteral("image_size"), 1048576},
                                             {QStringLiteral("debug_id"), QStringLiteral("7621bff1-bc35-53db-2534-a6672f25d94e")}},
                                 QJsonObject{{QStringLiteral("type"), QStringLiteral("elf")},
                                             {QStringLiteral("code_file"), qtCore},
                                             {QStringLiteral("image_addr"), hexAddress(qtBase)},
                                             {QStringLiteral("image_size"), 8388608},
                                             {QStringLiteral("debug_id"), QStringLiteral("e4276506-a330-6827-d827-41e00b81eebb")}}}}}},
    };
    return QString::fromUtf8(QJsonDocument(crash::scrubEvent(event)).toJson(QJsonDocument::Indented)).trimmed();
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
    QNetworkRequest request(m_dsn.envelopeUrl);
    request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/x-sentry-envelope"));
    request.setHeader(QNetworkRequest::UserAgentHeader, QStringLiteral("Rostrum/" ROSTRUM_VERSION));
    request.setRawHeader("X-Sentry-Auth", "Sentry sentry_version=7, sentry_client=rostrum/" ROSTRUM_VERSION
                                          ", sentry_key=" + m_dsn.publicKey.toUtf8());
    request.setAttribute(QNetworkRequest::CookieLoadControlAttribute, QNetworkRequest::Manual);
    request.setAttribute(QNetworkRequest::CookieSaveControlAttribute, QNetworkRequest::Manual);
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
    request.setTransferTimeout(kTimeoutMs);

    m_inFlight << path;
    QNetworkReply *reply = m_net.post(request, crash::toEnvelope(report));
    connect(reply, &QNetworkReply::finished, this, [this, reply, path] {
        reply->deleteLater();
        m_inFlight.removeAll(path);
        const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        if (status >= 200 && status < 300) {
            qCInfo(lcCrash) << "Crash report sent";
            QFile::remove(path);
        } else if (status >= 400 && status < 500 && status != 429) {
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
    QDir dir(paths::crashDir());
    for (const QString &name : dir.entryList({QStringLiteral("crash-*.envelope")}, QDir::Files)) {
        if (!m_inFlight.contains(dir.filePath(name))) {
            dir.remove(name);
        }
    }
    scan();
}

} // namespace rostrum::app
