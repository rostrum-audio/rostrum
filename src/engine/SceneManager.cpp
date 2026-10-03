#include "engine/SceneManager.h"

#include "core/SceneToml.h"
#include "engine/Engine.h"

#include <QLoggingCategory>

Q_DECLARE_LOGGING_CATEGORY(lcEngine)

namespace rostrum::engine {

SceneManager::SceneManager(Engine *engine, const QString &dir, QObject *parent)
    : QObject(parent)
    , m_engine(engine)
    , m_store(dir)
{
    connect(m_engine, &Engine::sceneChanged, this, &SceneManager::onEngineSceneChanged);
    connect(m_engine, &Engine::structureChanged, this, &SceneManager::onStructureChanged);
}

void SceneManager::fail(const QString &message)
{
    m_error = message;
    qCWarning(lcEngine) << message;
    Q_EMIT errorOccurred(message);
}

int SceneManager::indexOf(const QString &name) const
{
    for (int i = 0; i < m_saved.size(); ++i) {
        if (m_saved.at(i).name.compare(name, Qt::CaseInsensitive) == 0) {
            return i;
        }
    }
    return -1;
}

const Scene *SceneManager::saved(const QString &name) const
{
    const int i = indexOf(name);
    return i < 0 ? nullptr : &m_saved.at(i);
}

QStringList SceneManager::names() const
{
    QStringList out;
    for (const auto &s : m_saved) {
        out << s.name;
    }
    return out;
}

QString SceneManager::uniqueName(const QString &base) const
{
    const QString b = base.trimmed().isEmpty() ? QStringLiteral("Scene") : base.trimmed();
    if (indexOf(b) < 0) {
        return b;
    }
    for (int n = 2;; ++n) {
        const QString candidate = QStringLiteral("%1 %2").arg(b).arg(n);
        if (indexOf(candidate) < 0) {
            return candidate;
        }
    }
}

bool SceneManager::write(const Scene &scene)
{
    QString err;
    if (!m_store.save(scene, &err)) {
        fail(err);
        return false;
    }
    return true;
}

void SceneManager::load(const QString &defaultName)
{
    QStringList errors;
    m_saved = m_store.loadAll(&errors);
    for (const auto &e : errors) {
        fail(QStringLiteral("Skipped a scene file that could not be read. %1").arg(e));
    }
    if (m_saved.isEmpty()) {
        Scene live = defaults::scene();
        write(live);
        m_saved.append(live);
    }
    m_default = indexOf(defaultName) >= 0 ? m_saved.at(indexOf(defaultName)).name : m_saved.first().name;
    Q_EMIT scenesChanged();
    Q_EMIT defaultChanged();
    switchTo(m_default);
}

bool SceneManager::switchTo(const QString &name)
{
    const int i = indexOf(name);
    if (i < 0) {
        fail(QStringLiteral("There is no scene called \"%1\".").arg(name));
        return false;
    }
    m_current = m_saved.at(i).name;
    m_engine->setScene(m_saved.at(i));
    m_dirty = false;
    Q_EMIT currentChanged();
    Q_EMIT dirtyChanged();
    return true;
}

bool SceneManager::switchToIndex(int index)
{
    if (index < 0 || index >= m_saved.size()) {
        return false;
    }
    return switchTo(m_saved.at(index).name);
}

bool SceneManager::next()
{
    if (m_saved.isEmpty()) {
        return false;
    }
    return switchToIndex((indexOf(m_current) + 1) % m_saved.size());
}

bool SceneManager::previous()
{
    if (m_saved.isEmpty()) {
        return false;
    }
    const int i = indexOf(m_current);
    return switchToIndex((i - 1 + m_saved.size()) % m_saved.size());
}

void SceneManager::onEngineSceneChanged()
{
    const int i = indexOf(m_current);
    // Solo is not part of Scene, so it can never make a scene dirty.
    const bool dirty = i < 0 || !(m_engine->scene() == m_saved.at(i));
    if (dirty != m_dirty) {
        m_dirty = dirty;
        Q_EMIT dirtyChanged();
    }
}

void SceneManager::onStructureChanged()
{
    const int i = indexOf(m_current);
    if (i < 0) {
        return;
    }
    Scene merged = mergeStructure(m_saved.at(i), m_engine->scene());
    merged.name = m_current;
    if (write(merged)) {
        m_saved[i] = merged;
        Q_EMIT scenesChanged();
    }
    onEngineSceneChanged();
}

bool SceneManager::save()
{
    const int i = indexOf(m_current);
    if (i < 0) {
        return saveAs(m_current);
    }
    Scene s = m_engine->scene();
    s.name = m_current;
    if (!write(s)) {
        return false;
    }
    m_saved[i] = s;
    m_dirty = false;
    Q_EMIT scenesChanged();
    Q_EMIT dirtyChanged();
    return true;
}

bool SceneManager::saveAs(const QString &name)
{
    const QString n = name.trimmed();
    if (n.isEmpty()) {
        fail(QStringLiteral("Give the scene a name."));
        return false;
    }
    Scene s = m_engine->scene();
    s.name = n;
    if (!write(s)) {
        return false;
    }
    const int existing = indexOf(n);
    if (existing >= 0) {
        m_saved[existing] = s;
    } else {
        m_saved.append(s);
    }
    m_current = n;
    m_engine->setScene(s);
    m_dirty = false;
    Q_EMIT scenesChanged();
    Q_EMIT currentChanged();
    Q_EMIT dirtyChanged();
    return true;
}

bool SceneManager::create(const QString &name)
{
    Scene s = defaults::scene(uniqueName(name));
    if (!write(s)) {
        return false;
    }
    m_saved.append(s);
    Q_EMIT scenesChanged();
    return true;
}

bool SceneManager::duplicate(const QString &name, const QString &newName)
{
    const Scene *src = saved(name);
    if (!src) {
        return false;
    }
    Scene s = *src;
    s.name = uniqueName(newName.isEmpty() ? name + QStringLiteral(" copy") : newName);
    if (!write(s)) {
        return false;
    }
    m_saved.append(s);
    Q_EMIT scenesChanged();
    return true;
}

bool SceneManager::rename(const QString &oldName, const QString &newName)
{
    const int i = indexOf(oldName);
    const QString n = newName.trimmed();
    if (i < 0 || n.isEmpty()) {
        return false;
    }
    if (n.compare(oldName, Qt::CaseInsensitive) != 0 && indexOf(n) >= 0) {
        fail(QStringLiteral("A scene called \"%1\" already exists.").arg(n));
        return false;
    }
    Scene s = m_saved.at(i);
    const QString oldFile = m_store.fileFor(s.name);
    s.name = n;
    if (!write(s)) {
        return false;
    }
    if (m_store.fileFor(n) != oldFile) {
        m_store.remove(oldName);
    }
    m_saved[i] = s;
    const bool wasCurrent = m_current.compare(oldName, Qt::CaseInsensitive) == 0;
    if (wasCurrent) {
        m_current = n;
        Scene live = m_engine->scene();
        live.name = n;
        m_engine->setScene(live);
        Q_EMIT currentChanged();
    }
    if (m_default.compare(oldName, Qt::CaseInsensitive) == 0) {
        m_default = n;
        Q_EMIT defaultChanged();
    }
    Q_EMIT scenesChanged();
    onEngineSceneChanged();
    return true;
}

bool SceneManager::remove(const QString &name)
{
    const int i = indexOf(name);
    if (i < 0) {
        return false;
    }
    if (m_saved.size() == 1) {
        fail(QStringLiteral("Keep at least one scene."));
        return false;
    }
    QString err;
    if (!m_store.remove(m_saved.at(i).name, &err)) {
        fail(err);
        return false;
    }
    const bool wasCurrent = m_saved.at(i).name == m_current;
    const bool wasDefault = m_saved.at(i).name == m_default;
    m_saved.removeAt(i);
    if (wasDefault) {
        m_default = m_saved.first().name;
        Q_EMIT defaultChanged();
    }
    Q_EMIT scenesChanged();
    if (wasCurrent) {
        switchTo(m_default);
    }
    return true;
}

void SceneManager::setDefault(const QString &name)
{
    const int i = indexOf(name);
    if (i < 0 || m_saved.at(i).name == m_default) {
        return;
    }
    m_default = m_saved.at(i).name;
    Q_EMIT defaultChanged();
}

bool SceneManager::exportTo(const QString &path)
{
    QString err;
    if (!SceneStore::writeFile(path, toml_io::serializeBundle(m_saved), &err)) {
        fail(err);
        return false;
    }
    return true;
}

int SceneManager::importFrom(const QString &path)
{
    QString err;
    const QString text = SceneStore::readFile(path, &err);
    if (!err.isEmpty()) {
        fail(err);
        return -1;
    }
    const auto scenes = toml_io::parseBundle(text, &err);
    if (scenes.isEmpty()) {
        fail(err.isEmpty() ? QStringLiteral("The file has no scenes in it.") : err);
        return -1;
    }
    int count = 0;
    for (Scene s : scenes) {
        // Never overwrite an existing scene on import.
        s.name = uniqueName(s.name);
        if (write(s)) {
            m_saved.append(s);
            ++count;
        }
    }
    Q_EMIT scenesChanged();
    return count;
}

} // namespace rostrum::engine
