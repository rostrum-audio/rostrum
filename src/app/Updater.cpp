#include "app/Updater.h"

#include "app/AppController.h"

#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QGuiApplication>
#include <QJSEngine>
#include <QLocale>
#include <QLoggingCategory>
#include <QNetworkReply>
#include <QSysInfo>

#include <cerrno>
#include <cstdio>
#include <cstring>

Q_LOGGING_CATEGORY(lcUpdates, "rostrum.updates", QtInfoMsg)

namespace rostrum::app {

namespace {

constexpr qint64 kCheckEverySecs = 24 * 3600;
constexpr int kTickMs = 3600 * 1000;
constexpr int kFirstCheckMs = 20 * 1000;
constexpr int kTimeoutMs = 20 * 1000;
constexpr qint64 kMaxDownloadBytes = 512LL * 1024 * 1024;

QNetworkRequest request(const QUrl &url)
{
    QNetworkRequest r(url);
    r.setHeader(QNetworkRequest::UserAgentHeader, QStringLiteral("Rostrum/" ROSTRUM_VERSION));
    r.setAttribute(QNetworkRequest::CookieLoadControlAttribute, QNetworkRequest::Manual);
    r.setAttribute(QNetworkRequest::CookieSaveControlAttribute, QNetworkRequest::Manual);
    r.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
    r.setTransferTimeout(kTimeoutMs);
    return r;
}

} // namespace

Updater *Updater::s_instance = nullptr;

updates::InstallKind Updater::installKind()
{
    return updates::detectInstallKind(QCoreApplication::applicationFilePath(), qEnvironmentVariable("APPIMAGE"),
                                      !qEnvironmentVariableIsEmpty("FLATPAK_ID") ||
                                          QFileInfo::exists(QStringLiteral("/.flatpak-info")),
                                      !qEnvironmentVariableIsEmpty("SNAP"));
}

Updater::Updater(AppController *app, QObject *parent)
    : QObject(parent)
    , m_app(app)
    , m_kind(installKind())
    , m_appImage(qEnvironmentVariable("APPIMAGE"))
{
    Q_ASSERT(!s_instance);
    s_instance = this;

    const QString override = qEnvironmentVariable("ROSTRUM_UPDATE_URL");
    m_feed = QUrl(override.isEmpty() ? QStringLiteral(ROSTRUM_UPDATE_URL) : override);
    m_checkSoon = !override.isEmpty();
    const bool testRun = QGuiApplication::platformName() == QLatin1String("offscreen") ||
                         !qEnvironmentVariableIsEmpty("ROSTRUM_SCREENSHOT");
    m_networkAllowed = updates::trustedUrl(m_feed) && (!testRun || !override.isEmpty());

    if (m_kind == updates::InstallKind::AppImage) {
        const QFileInfo image(m_appImage);
        m_canInstall = image.isFile() && image.isWritable() && QFileInfo(image.absolutePath()).isWritable();
    }

    m_timer.setInterval(kTickMs);
    connect(&m_timer, &QTimer::timeout, this, &Updater::maybeCheck);
    connect(app, &AppController::settingsChanged, this, [this] {
        Q_EMIT changed();
        maybeCheck();
    });
}

Updater::~Updater()
{
    if (m_reply) {
        m_reply->abort();
    }
    s_instance = nullptr;
}

Updater *Updater::create(QQmlEngine *, QJSEngine *)
{
    QJSEngine::setObjectOwnership(s_instance, QJSEngine::CppOwnership);
    return s_instance;
}

void Updater::start()
{
    qCInfo(lcUpdates) << "Installed as" << installKindName() << (m_canInstall ? "(can update itself)" : "");
    if (!canCheck() || !m_networkAllowed) {
        return;
    }
    m_timer.start();
    QTimer::singleShot(m_checkSoon ? 0 : kFirstCheckMs, this, &Updater::maybeCheck);
}

bool Updater::checkEnabled() const { return m_app->settings().checkUpdates; }

void Updater::setCheckEnabled(bool on)
{
    if (m_app->settings().checkUpdates == on) {
        return;
    }
    m_app->settings().checkUpdates = on;
    m_app->saveSettingsSoon();
    Q_EMIT m_app->settingsChanged();
}

bool Updater::autoInstall() const { return m_app->settings().installUpdates; }

void Updater::setAutoInstall(bool on)
{
    if (m_app->settings().installUpdates == on) {
        return;
    }
    m_app->settings().installUpdates = on;
    m_app->saveSettingsSoon();
    Q_EMIT m_app->settingsChanged();
}

QString Updater::lastChecked() const
{
    const qint64 last = m_app->settings().lastUpdateCheck;
    return last > 0 ? QLocale().toString(QDateTime::fromSecsSinceEpoch(last), QLocale::ShortFormat) : QString();
}

bool Updater::showBanner() const
{
    return m_state == QLatin1String("ready") ||
           (m_state == QLatin1String("available") && m_release.version != m_app->settings().skippedVersion);
}

void Updater::setState(const QString &state, const QString &error)
{
    if (state == m_state && error == m_error) {
        return;
    }
    m_state = state;
    m_error = error;
    Q_EMIT changed();
}

void Updater::maybeCheck()
{
    if (!m_networkAllowed || !canCheck() || !checkEnabled() || !m_app->setupComplete() || !m_timer.isActive()) {
        return;
    }
    if (m_state == QLatin1String("checking") || m_state == QLatin1String("downloading") ||
        m_state == QLatin1String("ready")) {
        return;
    }
    const qint64 since = QDateTime::currentSecsSinceEpoch() - m_app->settings().lastUpdateCheck;
    if (since < kCheckEverySecs && !m_checkSoon) {
        return;
    }
    m_checkSoon = false;
    QNetworkReply *reply = m_net.get(request(m_feed));
    setState(QStringLiteral("checking"));
    connect(reply, &QNetworkReply::finished, this, [this, reply] { finishCheck(reply, false); });
}

void Updater::checkNow()
{
    if (!canCheck() || m_state == QLatin1String("checking") || m_state == QLatin1String("downloading")) {
        return;
    }
    if (!m_networkAllowed) {
        setState(QStringLiteral("error"), QStringLiteral("Update checks are off in test runs."));
        return;
    }
    QNetworkReply *reply = m_net.get(request(m_feed));
    setState(QStringLiteral("checking"));
    connect(reply, &QNetworkReply::finished, this, [this, reply] { finishCheck(reply, true); });
}

void Updater::finishCheck(QNetworkReply *reply, bool manual)
{
    reply->deleteLater();
    if (reply->error() != QNetworkReply::NoError) {
        qCInfo(lcUpdates) << "Update check failed:" << reply->errorString();
        setState(QStringLiteral("error"), reply->errorString());
        return;
    }
    QString error;
    const updates::Release release =
        updates::parseFeed(reply->readAll(), updates::feedArch(QSysInfo::currentCpuArchitecture()), &error);
    if (!release.valid()) {
        qCInfo(lcUpdates) << "Update feed unusable:" << error;
        setState(QStringLiteral("error"), error);
        return;
    }
    m_app->settings().lastUpdateCheck = QDateTime::currentSecsSinceEpoch();
    m_app->saveSettingsSoon();
    m_release = release;

    if (!updates::isNewer(release.version, QStringLiteral(ROSTRUM_VERSION))) {
        qCInfo(lcUpdates) << "Up to date; latest is" << release.version;
        setState(QStringLiteral("upToDate"));
        return;
    }
    qCInfo(lcUpdates) << "Rostrum" << release.version << "is available";
    setState(QStringLiteral("available"));
    const bool skipped = release.version == m_app->settings().skippedVersion && !manual;
    if (autoInstall() && m_canInstall && release.appImage.valid() && !skipped) {
        install();
    }
}

void Updater::install()
{
    if (!m_canInstall || !m_release.appImage.valid() || m_state == QLatin1String("downloading") ||
        m_state == QLatin1String("ready")) {
        return;
    }
    const QFileInfo image(m_appImage);
    m_part.setFileName(image.absolutePath() + QStringLiteral("/.") + image.fileName() + QStringLiteral(".part"));
    if (!m_part.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        failDownload(m_part.errorString());
        return;
    }
    m_hash.reset();
    m_progress = 0.0;
    setState(QStringLiteral("downloading"));
    qCInfo(lcUpdates) << "Downloading" << m_release.appImage.url.toString();

    m_reply = m_net.get(request(m_release.appImage.url));
    connect(m_reply, &QNetworkReply::readyRead, this, [this] {
        const QByteArray chunk = m_reply->readAll();
        m_hash.addData(chunk);
        if (m_part.write(chunk) != chunk.size()) {
            failDownload(m_part.errorString());
        } else if (m_part.size() > kMaxDownloadBytes ||
                   (m_release.appImage.size > 0 && m_part.size() > m_release.appImage.size)) {
            failDownload(QStringLiteral("The download is larger than the release says."));
        }
    });
    connect(m_reply, &QNetworkReply::downloadProgress, this, [this](qint64 received, qint64 total) {
        if (total <= 0) {
            total = m_release.appImage.size;
        }
        if (total > 0) {
            m_progress = double(received) / double(total);
            Q_EMIT changed();
        }
    });
    connect(m_reply, &QNetworkReply::finished, this, &Updater::finishDownload);
}

void Updater::failDownload(const QString &error)
{
    qCWarning(lcUpdates) << "Update not installed:" << error;
    if (m_reply) {
        m_reply->disconnect(this);
        m_reply->abort();
        m_reply->deleteLater();
        m_reply = nullptr;
    }
    if (m_part.isOpen()) {
        m_part.close();
    }
    m_part.remove();
    setState(QStringLiteral("available"), error);
}

void Updater::finishDownload()
{
    if (!m_reply) {
        return;
    }
    QNetworkReply *reply = m_reply;
    if (reply->error() != QNetworkReply::NoError) {
        failDownload(reply->errorString());
        return;
    }
    m_part.write(reply->readAll());
    reply->deleteLater();
    m_reply = nullptr;
    m_part.close();
    if (m_hash.result().toHex() != m_release.appImage.sha256) {
        failDownload(QStringLiteral("The download does not match the release checksum."));
        return;
    }
    m_part.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner |
                          QFileDevice::ReadGroup | QFileDevice::ExeGroup | QFileDevice::ReadOther |
                          QFileDevice::ExeOther);
    // rename() swaps the file in one step; the running copy keeps its old inode.
    if (std::rename(QFile::encodeName(m_part.fileName()).constData(), QFile::encodeName(m_appImage).constData()) != 0) {
        failDownload(QString::fromLocal8Bit(std::strerror(errno)));
        return;
    }
    qCInfo(lcUpdates) << "Rostrum" << m_release.version << "installed to" << m_appImage;
    m_progress = 1.0;
    setState(QStringLiteral("ready"));
}

void Updater::skipThisVersion()
{
    if (m_release.version.isEmpty() || m_app->settings().skippedVersion == m_release.version) {
        return;
    }
    m_app->settings().skippedVersion = m_release.version;
    m_app->saveSettingsSoon();
    Q_EMIT m_app->settingsChanged();
}

void Updater::restart()
{
    if (m_state == QLatin1String("ready")) {
        Q_EMIT restartRequested(m_appImage);
    }
}

} // namespace rostrum::app
