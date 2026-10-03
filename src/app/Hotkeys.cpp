#include "app/Hotkeys.h"

#include <KGlobalAccel>
#include <KGlobalShortcutInfo>
#include <KLocalizedString>
#include <QAction>
#include <QDBusArgument>
#include <QDBusConnection>
#include <QDBusConnectionInterface>
#include <QDBusMessage>
#include <QDBusMetaType>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>
#include <QGuiApplication>
#include <QKeySequence>
#include <QLoggingCategory>

Q_LOGGING_CATEGORY(lcHotkeys, "rostrum.hotkeys", QtInfoMsg)

namespace {

const QString kPortalService = QStringLiteral("org.freedesktop.portal.Desktop");
const QString kPortalPath = QStringLiteral("/org/freedesktop/portal/desktop");
const QString kPortalInterface = QStringLiteral("org.freedesktop.portal.GlobalShortcuts");
const QString kRequestInterface = QStringLiteral("org.freedesktop.portal.Request");
// The app id, so System Settings → Shortcuts finds the installed desktop file for name and icon.
const QString kComponent = QStringLiteral(ROSTRUM_APP_ID);

struct PortalShortcut
{
    QString id;
    QVariantMap options;
};
using PortalShortcuts = QList<PortalShortcut>;

QDBusArgument &operator<<(QDBusArgument &arg, const PortalShortcut &s)
{
    arg.beginStructure();
    arg << s.id << s.options;
    arg.endStructure();
    return arg;
}

const QDBusArgument &operator>>(const QDBusArgument &arg, PortalShortcut &s)
{
    arg.beginStructure();
    arg >> s.id >> s.options;
    arg.endStructure();
    return arg;
}

// Qt's "Meta+Alt+PgUp" to the XDG shortcuts format, "LOGO+ALT+Page_Up".
QString toPortalTrigger(const QKeySequence &seq)
{
    if (seq.isEmpty()) {
        return {};
    }
    const QStringList parts =
        QKeySequence(seq[0]).toString(QKeySequence::PortableText).split(QLatin1Char('+'));
    static const QHash<QString, QString> mods = {{QStringLiteral("Ctrl"), QStringLiteral("CTRL")},
                                                 {QStringLiteral("Alt"), QStringLiteral("ALT")},
                                                 {QStringLiteral("Shift"), QStringLiteral("SHIFT")},
                                                 {QStringLiteral("Meta"), QStringLiteral("LOGO")}};
    static const QHash<QString, QString> keys = {{QStringLiteral("PgUp"), QStringLiteral("Page_Up")},
                                                 {QStringLiteral("PgDown"), QStringLiteral("Page_Down")},
                                                 {QStringLiteral("Esc"), QStringLiteral("Escape")},
                                                 {QStringLiteral("Del"), QStringLiteral("Delete")},
                                                 {QStringLiteral("Ins"), QStringLiteral("Insert")}};
    QStringList out;
    for (const QString &p : parts) {
        if (mods.contains(p)) {
            out << mods.value(p);
        } else if (keys.contains(p)) {
            out << keys.value(p);
        } else {
            out << (p.size() == 1 ? p.toLower() : p);
        }
    }
    return out.join(QLatin1Char('+'));
}

bool serviceRunning(const QString &name)
{
    const auto *iface = QDBusConnection::sessionBus().interface();
    return iface && iface->isServiceRegistered(name).value();
}

bool portalHasGlobalShortcuts()
{
    QDBusMessage msg = QDBusMessage::createMethodCall(kPortalService, kPortalPath,
                                                      QStringLiteral("org.freedesktop.DBus.Properties"),
                                                      QStringLiteral("Get"));
    msg << kPortalInterface << QStringLiteral("version");
    const QDBusMessage reply = QDBusConnection::sessionBus().call(msg, QDBus::Block, 2000);
    return reply.type() == QDBusMessage::ReplyMessage;
}

} // namespace

Q_DECLARE_METATYPE(PortalShortcut)

namespace rostrum::app {

Hotkeys::Hotkeys(QObject *parent) : QObject(parent)
{
    // Offscreen runs (screenshots, tests) must not touch the user's desktop shortcuts.
    if (QGuiApplication::platformName() == QLatin1String("offscreen") ||
        qEnvironmentVariableIsSet("ROSTRUM_SCREENSHOT") ||
        qEnvironmentVariableIsSet("ROSTRUM_NO_GLOBAL_SHORTCUTS")) {
        m_disabled = true;
        return;
    }
    if (serviceRunning(QStringLiteral("org.kde.kglobalaccel"))) {
        m_backend = Backend::KGlobalAccel;
    } else if (serviceRunning(kPortalService) && portalHasGlobalShortcuts()) {
        m_backend = Backend::Portal;
        qDBusRegisterMetaType<PortalShortcut>();
        qDBusRegisterMetaType<PortalShortcuts>();
        QDBusConnection::sessionBus().connect(
            kPortalService, kPortalPath, kPortalInterface, QStringLiteral("Activated"), this,
            SLOT(onPortalActivated(QDBusObjectPath, QString, qulonglong, QVariantMap)));
    }
    qCInfo(lcHotkeys) << "global shortcut backend:"
                      << (m_backend == Backend::KGlobalAccel ? "KGlobalAccel"
                          : m_backend == Backend::Portal     ? "portal"
                                                             : "none");
}

Hotkeys::~Hotkeys() = default;

void Hotkeys::setStatus(const QString &id, bool global, const QString &problem)
{
    m_global.insert(id, global);
    m_problem.insert(id, problem);
    qCDebug(lcHotkeys) << id << m_bindings.value(id) << (global ? "global" : "window only") << problem;
}

void Hotkeys::apply(const QMap<QString, QString> &bindings, const QMap<QString, QString> &labels)
{
    if (bindings == m_bindings && labels == m_labels && !m_actions.isEmpty()) {
        return;
    }
    m_bindings = bindings;
    m_labels = labels;
    switch (m_backend) {
    case Backend::KGlobalAccel:
        applyKGlobalAccel();
        break;
    case Backend::Portal:
        applyPortal();
        break;
    case Backend::None:
        for (auto it = bindings.cbegin(); it != bindings.cend(); ++it) {
            setStatus(it.key(), false,
                      it.value().isEmpty() || m_disabled ? QString()
                                                         : i18n("This desktop offers no global shortcuts. It "
                                                                "works while Rostrum's window is focused."));
        }
        Q_EMIT statusChanged();
        break;
    }
}

// ---- KGlobalAccel -----------------------------------------------------------------------

void Hotkeys::applyKGlobalAccel()
{
    m_applying = true;
    auto *kga = KGlobalAccel::self();
    for (auto it = m_bindings.cbegin(); it != m_bindings.cend(); ++it) {
        const QString &id = it.key();
        QAction *action = m_actions.value(id);
        if (!action) {
            action = new QAction(this);
            action->setObjectName(id);
            action->setProperty("componentName", kComponent);
            action->setProperty("componentDisplayName", QStringLiteral("Rostrum"));
            connect(action, &QAction::triggered, this, [this, id] { Q_EMIT triggered(id); });
            m_actions.insert(id, action);
        }
        action->setText(m_labels.value(id, id));

        const QKeySequence seq(it.value(), QKeySequence::PortableText);
        if (seq.isEmpty()) {
            kga->setShortcut(action, {}, KGlobalAccel::NoAutoloading);
            setStatus(id, false, {});
            continue;
        }
        if (!KGlobalAccel::isGlobalShortcutAvailable(seq, kComponent)) {
            const auto owners = KGlobalAccel::globalShortcutsByKey(seq);
            QString owner;
            for (const auto &info : owners) {
                if (info.componentUniqueName() != kComponent) {
                    owner = info.componentFriendlyName().isEmpty() ? info.componentUniqueName()
                                                                   : info.componentFriendlyName();
                    if (!info.friendlyName().isEmpty()) {
                        owner = i18nc("@info shortcut owner: component, action", "%1, %2", owner,
                                      info.friendlyName());
                    }
                    break;
                }
            }
            if (!owner.isEmpty()) {
                kga->setShortcut(action, {}, KGlobalAccel::NoAutoloading);
                setStatus(id, false,
                          i18n("The desktop already uses this shortcut (%1). It works only while Rostrum is "
                               "focused.",
                               owner));
                continue;
            }
        }
        kga->setShortcut(action, {seq}, KGlobalAccel::NoAutoloading);
        const bool ok = kga->shortcut(action).contains(seq);
        setStatus(id, ok,
                  ok ? QString()
                     : i18n("The desktop refused this shortcut. It works only while Rostrum is focused."));
    }
    m_applying = false;

    static bool connected = false;
    if (!connected) {
        connected = true;
        connect(kga, &KGlobalAccel::globalShortcutChanged, this,
                [this](QAction *action, const QKeySequence &seq) {
                    if (m_applying) {
                        return;
                    }
                    const QString id = m_actions.key(action);
                    if (id.isEmpty()) {
                        return;
                    }
                    const QString portable = seq.toString(QKeySequence::PortableText);
                    if (portable != m_bindings.value(id)) {
                        m_bindings.insert(id, portable);
                        setStatus(id, !seq.isEmpty(), {});
                        Q_EMIT changedExternally(id, portable);
                        Q_EMIT statusChanged();
                    }
                });
    }
    Q_EMIT statusChanged();
}

// ---- XDG GlobalShortcuts portal ----------------------------------------------------------

QString Hotkeys::portalRequestPath(const QString &token) const
{
    QString sender = QDBusConnection::sessionBus().baseService().mid(1);
    sender.replace(QLatin1Char('.'), QLatin1Char('_'));
    return QStringLiteral("/org/freedesktop/portal/desktop/request/%1/%2").arg(sender, token);
}

void Hotkeys::applyPortal()
{
    if (m_portalStep != PortalStep::Idle) {
        m_portalRebind = true;
        return;
    }
    if (m_portalSession.isEmpty()) {
        portalCreateSession();
    } else {
        portalBind();
    }
}

void Hotkeys::portalCreateSession()
{
    const QString token = QStringLiteral("rostrum%1").arg(++m_tokenCounter);
    m_portalRequest = portalRequestPath(token);
    QDBusConnection::sessionBus().connect(kPortalService, m_portalRequest, kRequestInterface,
                                          QStringLiteral("Response"), this,
                                          SLOT(onPortalResponse(uint, QVariantMap)));
    QDBusMessage msg = QDBusMessage::createMethodCall(kPortalService, kPortalPath, kPortalInterface,
                                                      QStringLiteral("CreateSession"));
    msg << QVariantMap{{QStringLiteral("handle_token"), token},
                       {QStringLiteral("session_handle_token"), QStringLiteral("rostrum")}};
    m_portalStep = PortalStep::CreatingSession;
    QDBusConnection::sessionBus().asyncCall(msg);
}

void Hotkeys::portalBind()
{
    PortalShortcuts shortcuts;
    for (auto it = m_bindings.cbegin(); it != m_bindings.cend(); ++it) {
        QVariantMap opts{{QStringLiteral("description"), m_labels.value(it.key(), it.key())}};
        const QString trigger = toPortalTrigger(QKeySequence(it.value(), QKeySequence::PortableText));
        if (!trigger.isEmpty()) {
            opts.insert(QStringLiteral("preferred_trigger"), trigger);
        }
        shortcuts.append({it.key(), opts});
    }
    const QString token = QStringLiteral("rostrum%1").arg(++m_tokenCounter);
    m_portalRequest = portalRequestPath(token);
    QDBusConnection::sessionBus().connect(kPortalService, m_portalRequest, kRequestInterface,
                                          QStringLiteral("Response"), this,
                                          SLOT(onPortalResponse(uint, QVariantMap)));
    QDBusMessage msg = QDBusMessage::createMethodCall(kPortalService, kPortalPath, kPortalInterface,
                                                      QStringLiteral("BindShortcuts"));
    msg << QVariant::fromValue(QDBusObjectPath(m_portalSession)) << QVariant::fromValue(shortcuts)
        << QString() << QVariantMap{{QStringLiteral("handle_token"), token}};
    m_portalStep = PortalStep::Binding;
    QDBusConnection::sessionBus().asyncCall(msg);
}

void Hotkeys::onPortalResponse(uint response, const QVariantMap &results)
{
    QDBusConnection::sessionBus().disconnect(kPortalService, m_portalRequest, kRequestInterface,
                                             QStringLiteral("Response"), this,
                                             SLOT(onPortalResponse(uint, QVariantMap)));
    const PortalStep step = m_portalStep;
    m_portalStep = PortalStep::Idle;

    if (step == PortalStep::CreatingSession) {
        if (response != 0) {
            for (auto it = m_bindings.cbegin(); it != m_bindings.cend(); ++it) {
                setStatus(it.key(), false,
                          it.value().isEmpty() ? QString()
                                               : i18n("The desktop did not allow global shortcuts. It works "
                                                      "only while Rostrum is focused."));
            }
            Q_EMIT statusChanged();
            return;
        }
        m_portalSession = results.value(QStringLiteral("session_handle")).toString();
        portalBind();
        return;
    }

    if (step == PortalStep::Binding) {
        QSet<QString> bound;
        if (response == 0) {
            PortalShortcuts list;
            results.value(QStringLiteral("shortcuts")).value<QDBusArgument>() >> list;
            for (const auto &s : std::as_const(list)) {
                bound.insert(s.id);
            }
        }
        for (auto it = m_bindings.cbegin(); it != m_bindings.cend(); ++it) {
            const bool ok = bound.contains(it.key()) && !it.value().isEmpty();
            setStatus(
                it.key(), ok,
                ok || it.value().isEmpty()
                    ? QString()
                    : i18n(
                          "The desktop did not bind this shortcut. It works only while Rostrum is focused."));
        }
        Q_EMIT statusChanged();
        if (m_portalRebind) {
            m_portalRebind = false;
            portalBind();
        }
    }
}

void Hotkeys::onPortalActivated(const QDBusObjectPath &session, const QString &id, qulonglong,
                                const QVariantMap &)
{
    if (session.path() == m_portalSession) {
        Q_EMIT triggered(id);
    }
}

} // namespace rostrum::app
