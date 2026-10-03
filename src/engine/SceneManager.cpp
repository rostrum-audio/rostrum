#include "engine/SceneManager.h"

#include "core/RuleExport.h"
#include "core/SceneToml.h"
#include "engine/Engine.h"

#include <QFile>
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
    // Long enough that a fader drag is one write, short enough that a crash loses little.
    m_autoSaveTimer.setSingleShot(true);
    m_autoSaveTimer.setInterval(1000);
    connect(&m_autoSaveTimer, &QTimer::timeout, this, &SceneManager::flush);
}

void SceneManager::setAutoSave(bool on)
{
    m_autoSave = on;
    if (!on) {
        m_autoSaveTimer.stop();
    } else if (m_dirty) {
        m_autoSaveTimer.start();
    }
}

void SceneManager::flush()
{
    m_autoSaveTimer.stop();
    if (m_autoSave && m_dirty && indexOf(m_current) >= 0) {
        save();
    }
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
    m_saved = SceneStore::applyOrder(m_store.loadAll(&errors), m_order);
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
    // The nodes may already be playing at their lingering levels; a fade would start from defaults.
    activate(m_default, false);
}

void SceneManager::setOrder(const QStringList &names)
{
    m_order = names;
    const QList<Scene> ordered = SceneStore::applyOrder(m_saved, names);
    if (ordered != m_saved) {
        m_saved = ordered;
        Q_EMIT scenesChanged();
    }
}

bool SceneManager::move(const QString &name, int toIndex)
{
    const int from = indexOf(name);
    if (from < 0 || m_saved.isEmpty()) {
        return false;
    }
    const int to = std::clamp(toIndex, 0, int(m_saved.size()) - 1);
    if (to == from) {
        return false;
    }
    m_saved.move(from, to);
    m_order = names();
    Q_EMIT scenesChanged();
    return true;
}

bool SceneManager::switchTo(const QString &name) { return activate(name, true); }

bool SceneManager::activate(const QString &name, bool fade)
{
    const int i = indexOf(name);
    if (i < 0) {
        fail(QStringLiteral("There is no scene called \"%1\".").arg(name));
        return false;
    }
    flush();
    m_current = m_saved.at(i).name;
    m_engine->setScene(m_saved.at(i), fade);
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
    if (m_autoSave && m_dirty) {
        m_autoSaveTimer.start();
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

bool SceneManager::create(const QString &name) { return create(defaults::scene(name)); }

bool SceneManager::create(const Scene &scene)
{
    Scene s = scene;
    s.name = uniqueName(scene.name);
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
    const QString was = m_saved.at(i).name;
    m_saved[i] = s;
    Q_EMIT renamed(was, n);
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
    QString trashFile;
    if (m_trash) {
        trashFile = m_trash->put(m_saved.at(i), QDateTime::currentDateTimeUtc(), &err);
        if (trashFile.isEmpty()) {
            fail(err);
            return false;
        }
    }
    if (!m_store.remove(m_saved.at(i).name, &err)) {
        if (!trashFile.isEmpty()) {
            QFile::remove(trashFile);
        }
        fail(err);
        return false;
    }
    const bool wasCurrent = m_saved.at(i).name == m_current;
    const bool wasDefault = m_saved.at(i).name == m_default;
    m_saved.removeAt(i);
    if (!trashFile.isEmpty()) {
        m_lastTrashed = {trashFile, i, wasDefault};
        Q_EMIT trashChanged();
    }
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

void SceneManager::setTrashDir(const QString &dir)
{
    m_trash.emplace(dir);
    m_lastTrashed = {};
    if (m_trash->purge(QDateTime::currentDateTimeUtc()) > 0) {
        Q_EMIT trashChanged();
    }
}

QList<SceneTrash::Entry> SceneManager::trash() const
{
    return m_trash ? m_trash->entries() : QList<SceneTrash::Entry>{};
}

QString SceneManager::restore(const QString &trashFile)
{
    if (!m_trash) {
        return {};
    }
    QString err;
    auto scene = m_trash->take(trashFile, &err);
    if (!scene) {
        fail(err);
        return {};
    }
    Scene s = *scene;
    s.name = uniqueName(s.name);
    if (!write(s)) {
        m_trash->put(*scene, QDateTime::currentDateTimeUtc());
        return {};
    }
    const bool wasLast = trashFile == m_lastTrashed.file;
    if (wasLast && m_lastTrashed.index >= 0 && m_lastTrashed.index <= m_saved.size()) {
        m_saved.insert(m_lastTrashed.index, s);
    } else {
        m_saved.append(s);
    }
    m_order = names();
    Q_EMIT scenesChanged();
    if (wasLast && m_lastTrashed.wasDefault) {
        setDefault(s.name);
    }
    if (wasLast) {
        m_lastTrashed = {};
    }
    Q_EMIT trashChanged();
    return s.name;
}

void SceneManager::enableRuleExport(const QString &pulseFragment, const QString &clientFragment)
{
    const bool first = m_pulseFragment.isEmpty();
    m_pulseFragment = pulseFragment;
    m_clientFragment = clientFragment;
    if (first) {
        connect(this, &SceneManager::scenesChanged, this, &SceneManager::exportRules);
        connect(this, &SceneManager::defaultChanged, this, &SceneManager::exportRules);
    }
    exportRules();
}

void SceneManager::exportRules()
{
    const Scene *scene = saved(m_default);
    if (m_pulseFragment.isEmpty() || !scene) {
        return;
    }
    QString err;
    if (!rule_export::apply(*scene, m_pulseFragment, m_clientFragment, &err)) {
        fail(QStringLiteral("Could not update the PipeWire app rules. %1").arg(err));
    }
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

int SceneManager::restoreScenes(const QList<Scene> &scenes)
{
    flush();
    int count = 0;
    bool trashed = false;
    bool liveReplaced = false;
    for (Scene s : scenes) {
        s.name = s.name.trimmed();
        if (s.name.isEmpty()) {
            continue;
        }
        const int i = indexOf(s.name);
        if (i >= 0 && m_saved.at(i) == s) {
            ++count;
            continue;
        }
        if (i >= 0 && m_trash) {
            trashed = !m_trash->put(m_saved.at(i), QDateTime::currentDateTimeUtc()).isEmpty() || trashed;
        }
        if (i >= 0 && m_store.fileFor(m_saved.at(i).name) != m_store.fileFor(s.name)) {
            m_store.remove(m_saved.at(i).name);
        }
        if (!write(s)) {
            continue;
        }
        if (i >= 0) {
            liveReplaced = liveReplaced || m_saved.at(i).name == m_current;
            if (m_saved.at(i).name == m_current) {
                m_current = s.name;
            }
            if (m_saved.at(i).name == m_default) {
                m_default = s.name;
            }
            m_saved[i] = s;
        } else {
            m_saved.append(s);
        }
        ++count;
    }
    if (trashed) {
        Q_EMIT trashChanged();
    }
    Q_EMIT scenesChanged();
    if (liveReplaced) {
        switchTo(m_current);
    }
    return count;
}

} // namespace rostrum::engine
