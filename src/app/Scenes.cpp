#include "app/Scenes.h"

#include "app/AppController.h"
#include "core/ScenePresets.h"

#include <KLocalizedString>

#include <QJSEngine>
#include <QKeySequence>

namespace rostrum::app {

namespace {
constexpr int kMaxNameLength = 64;
}

Scenes *Scenes::s_instance = nullptr;

Scenes::Scenes(AppController *app, QObject *parent)
    : QObject(parent)
    , m_app(app)
{
    Q_ASSERT(!s_instance);
    s_instance = this;
    engine::SceneManager *sm = app->scenes();
    for (auto sig : {&engine::SceneManager::scenesChanged, &engine::SceneManager::currentChanged,
                     &engine::SceneManager::defaultChanged}) {
        connect(sm, sig, this, &Scenes::rebuild);
    }
    connect(app, &AppController::settingsChanged, this, &Scenes::rebuild);
    rebuild();
}

Scenes::~Scenes()
{
    s_instance = nullptr;
}

Scenes *Scenes::create(QQmlEngine *, QJSEngine *)
{
    QJSEngine::setObjectOwnership(s_instance, QJSEngine::CppOwnership);
    return s_instance;
}

QString Scenes::summary(const Scene &scene)
{
    QStringList parts;
    parts << i18ncp("@info scene summary", "%1 bus", "%1 buses", scene.buses.size());
    for (const auto &b : scene.buses) {
        if (b.isInput() || b.destination == Destination::Both) {
            continue;
        }
        parts << (b.destination == Destination::Stream ? i18nc("@info scene summary", "%1 → Stream", b.name)
                                                       : i18nc("@info scene summary", "%1 → Headphones", b.name));
    }
    if (const Bus *mic = scene.micBus(); mic && mic->muted) {
        parts << i18nc("@info scene summary", "mic muted");
    }
    if (scene.masterStreamMuted) {
        parts << i18nc("@info scene summary", "stream muted");
    }
    if (!scene.rules.isEmpty()) {
        parts << i18ncp("@info scene summary", "%1 app rule", "%1 app rules", scene.rules.size());
    }
    return parts.join(i18nc("@info scene summary separator", ", "));
}

void Scenes::rebuild()
{
    const engine::SceneManager *sm = m_app->scenes();
    const int slots = actions::sceneSlotCount();
    QVariantList rows;
    for (int i = 0; i < sm->scenes().size(); ++i) {
        const Scene &s = sm->scenes().at(i);
        const int slot = i < slots ? i + 1 : 0;
        const QString key =
            slot > 0 ? m_app->settings().hotkeys.value(actions::sceneSlotAction(slot)) : QString();
        rows << QVariantMap{
            {QStringLiteral("name"), s.name},
            {QStringLiteral("summary"), summary(s)},
            {QStringLiteral("isDefault"), s.name == sm->defaultName()},
            {QStringLiteral("isCurrent"), s.name == sm->currentName()},
            {QStringLiteral("slot"), slot},
            {QStringLiteral("shortcut"),
             QKeySequence(key, QKeySequence::PortableText).toString(QKeySequence::NativeText)},
        };
    }
    if (rows != m_rows) {
        m_rows = rows;
        Q_EMIT changed();
    }
}

QString Scenes::nameProblem(const QString &name, const QString &except) const
{
    const QString n = name.trimmed();
    if (n.isEmpty()) {
        return i18n("Give the scene a name.");
    }
    if (n.size() > kMaxNameLength) {
        return i18n("Use a shorter name.");
    }
    if (n.compare(except.trimmed(), Qt::CaseInsensitive) != 0 && m_app->scenes()->saved(n)) {
        return i18n("A scene called “%1” already exists.", n);
    }
    return {};
}

QString Scenes::uniqueName(const QString &base) const { return m_app->scenes()->uniqueName(base); }

QString Scenes::createScene(const QString &name)
{
    const QString n = uniqueName(name.trimmed().isEmpty() ? i18nc("default name for a new scene", "New scene") : name);
    return m_app->scenes()->create(n) ? n : QString();
}

QVariantList Scenes::presets() const
{
    struct Text
    {
        KLocalizedString name;
        KLocalizedString description;
        const char *icon;
    };
    static const QMap<QString, Text> texts = {
        {QString::fromLatin1(rostrum::presets::kGaming),
         {ki18nc("@item scene preset", "Gaming"),
          ki18n("Game and voice chat on stream, music low under your voice."), "input-gaming"}},
        {QString::fromLatin1(rostrum::presets::kChatting),
         {ki18nc("@item scene preset", "Just Chatting"),
          ki18n("Your voice up front with music as a bed. Game muted."), "dialog-messages"}},
        {QString::fromLatin1(rostrum::presets::kMusic),
         {ki18nc("@item scene preset", "Music Stream"),
          ki18n("Music at full level on stream. Voice chat only in your headphones."), "media-playlist-audio"}},
        {QString::fromLatin1(rostrum::presets::kPodcast),
         {ki18nc("@item scene preset", "Podcast"),
          ki18n("Guests on Voice at full level. Game, music and alerts muted."), "audio-input-microphone"}},
        {QString::fromLatin1(rostrum::presets::kBreak),
         {ki18nc("@item scene preset", "Be Right Back"),
          ki18n("Mic muted and music on stream. Voice chat stays in your headphones."), "media-playback-pause"}},
    };
    QVariantList out;
    for (const QString &id : rostrum::presets::ids()) {
        const Text t = texts.value(id);
        out << QVariantMap{
            {QStringLiteral("id"), id},
            {QStringLiteral("name"), t.name.toString()},
            {QStringLiteral("description"), t.description.toString()},
            {QStringLiteral("icon"), QString::fromLatin1(t.icon)},
        };
    }
    return out;
}

QString Scenes::createFromPreset(const QString &presetId, const QString &name)
{
    if (!rostrum::presets::ids().contains(presetId)) {
        return {};
    }
    if (const QString problem = nameProblem(name); !problem.isEmpty()) {
        Q_EMIT m_app->toast(problem);
        return {};
    }
    Scene s = rostrum::presets::apply(presetId, m_app->engine()->scene());
    s.name = name.trimmed();
    return m_app->scenes()->create(s) ? s.name : QString();
}

QString Scenes::duplicate(const QString &name)
{
    const QString n = uniqueName(i18nc("name of a duplicated scene", "%1 copy", name));
    return m_app->scenes()->duplicate(name, n) ? n : QString();
}

QString Scenes::rename(const QString &oldName, const QString &newName)
{
    if (const QString problem = nameProblem(newName, oldName); !problem.isEmpty()) {
        Q_EMIT m_app->toast(problem);
        return {};
    }
    return m_app->scenes()->rename(oldName, newName.trimmed()) ? newName.trimmed() : QString();
}

QString Scenes::saveAs(const QString &name)
{
    if (const QString problem = nameProblem(name, m_app->scenes()->currentName()); !problem.isEmpty()) {
        Q_EMIT m_app->toast(problem);
        return {};
    }
    if (!m_app->scenes()->saveAs(name.trimmed())) {
        return {};
    }
    Q_EMIT m_app->toast(i18n("Scene saved"));
    return name.trimmed();
}

bool Scenes::remove(const QString &name) { return m_app->scenes()->remove(name); }
void Scenes::setDefault(const QString &name) { m_app->scenes()->setDefault(name); }

bool Scenes::exportTo(const QUrl &file)
{
    if (!file.isLocalFile()) {
        Q_EMIT m_app->toast(i18n("Choose a file on this computer."));
        return false;
    }
    if (!m_app->scenes()->exportTo(file.toLocalFile())) {
        return false;
    }
    Q_EMIT m_app->toast(i18np("Exported %1 scene", "Exported %1 scenes", m_app->scenes()->scenes().size()));
    return true;
}

bool Scenes::move(const QString &name, int toIndex)
{
    return m_app->scenes()->move(name, toIndex);
}

bool Scenes::moveBy(const QString &name, int delta)
{
    const int from = m_app->scenes()->names().indexOf(name);
    return from >= 0 && move(name, from + delta);
}

int Scenes::importFrom(const QUrl &file)
{
    if (!file.isLocalFile()) {
        Q_EMIT m_app->toast(i18n("Choose a file on this computer."));
        return -1;
    }
    const int n = m_app->scenes()->importFrom(file.toLocalFile());
    if (n >= 0) {
        Q_EMIT m_app->toast(i18np("Imported %1 scene", "Imported %1 scenes", n));
    }
    return n;
}

} // namespace rostrum::app
