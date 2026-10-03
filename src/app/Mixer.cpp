#include "app/Mixer.h"

#include "app/AppController.h"
#include "app/Apps.h"
#include "core/Volume.h"
#include "engine/Engine.h"
#include "engine/NodeSpecs.h"

#include <KLocalizedString>

#include <QJSEngine>

namespace rostrum::app {

namespace {
int destinationIndex(Destination d)
{
    switch (d) {
    case Destination::Phones:
        return Mixer::PhonesIndex;
    case Destination::Stream:
        return Mixer::StreamIndex;
    case Destination::Both:
        return Mixer::BothIndex;
    }
    return Mixer::BothIndex;
}
} // namespace

// ---- BusModel ---------------------------------------------------------------------------

BusModel::BusModel(engine::Engine *engine, QObject *parent)
    : QAbstractListModel(parent)
    , m_engine(engine)
{
}

int BusModel::rowCount(const QModelIndex &parent) const
{
    return parent.isValid() ? 0 : int(m_ids.size());
}

QHash<int, QByteArray> BusModel::roleNames() const
{
    return {
        {IdRole, "busId"},     {NameRole, "name"},       {ColorRole, "busColor"},
        {IsInputRole, "isInput"}, {VolumeRole, "volume"}, {MutedRole, "muted"},
        {SoloedRole, "soloed"}, {DimmedRole, "dimmed"},   {DestinationRole, "destination"},
        {AppsRole, "apps"},    {PeakRole, "peak"},       {ClipRole, "clip"},
        {AutoCategoryRole, "autoCategory"}, {BalanceRole, "balance"},
    };
}

QVariant BusModel::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || index.row() >= m_ids.size()) {
        return {};
    }
    const QString &id = m_ids.at(index.row());
    const Bus *b = m_engine->scene().bus(id);
    if (!b) {
        return {};
    }
    switch (role) {
    case IdRole:
        return b->id;
    case NameRole:
        return b->name;
    case ColorRole:
        return b->color;
    case IsInputRole:
        return b->isInput();
    case VolumeRole:
        return b->volume;
    case MutedRole:
        return b->muted;
    case SoloedRole:
        return m_engine->isSoloed(id);
    case DimmedRole:
        return m_engine->dimmedBySolo(id);
    case DestinationRole:
        return destinationIndex(b->destination);
    case AppsRole:
        return m_apps.value(id);
    case PeakRole:
        return m_meters.value(id).fraction;
    case ClipRole:
        return m_meters.value(id).clip;
    case AutoCategoryRole:
        return categoryName(b->autoCategory);
    case BalanceRole:
        return b->balance;
    }
    return {};
}

void BusModel::refresh()
{
    QStringList ids;
    const Scene &s = m_engine->scene();
    if (const Bus *mic = s.micBus()) {
        ids << mic->id;
    }
    for (const auto &b : s.buses) {
        if (!b.isInput()) {
            ids << b.id;
        }
    }

    m_apps.clear();
    QHash<QString, QSet<QString>> seen;
    for (const auto &a : m_engine->appStreams()) {
        if (a.busId.isEmpty()) {
            continue;
        }
        const QString key = a.identity.key.toString();
        if (seen[a.busId].contains(key)) {
            continue;
        }
        seen[a.busId].insert(key);
        m_apps[a.busId].append(QVariantMap{
            {QStringLiteral("key"), key},
            {QStringLiteral("name"), a.identity.displayName},
            {QStringLiteral("session"), a.sessionOnly},
            {QStringLiteral("automatic"), a.automatic},
            {QStringLiteral("reason"), a.automatic ? Apps::reason(a) : QString()},
        });
    }

    if (ids != m_ids) {
        beginResetModel();
        m_ids = ids;
        endResetModel();
        return;
    }
    if (!m_ids.isEmpty()) {
        Q_EMIT dataChanged(index(0), index(int(m_ids.size()) - 1),
                           {NameRole, ColorRole, VolumeRole, MutedRole, SoloedRole, DimmedRole, DestinationRole,
                            AppsRole, AutoCategoryRole, BalanceRole});
    }
}

void BusModel::setMeter(int row, double fraction, bool clip)
{
    if (row < 0 || row >= m_ids.size()) {
        return;
    }
    Meter &m = m_meters[m_ids.at(row)];
    if (qFuzzyCompare(m.fraction + 1.0, fraction + 1.0) && m.clip == clip) {
        return;
    }
    m.fraction = fraction;
    m.clip = clip;
    Q_EMIT dataChanged(index(row), index(row), {PeakRole, ClipRole});
}

// ---- Mixer ------------------------------------------------------------------------------

Mixer *Mixer::s_instance = nullptr;

Mixer::Mixer(AppController *app, QObject *parent)
    : QObject(parent)
    , m_app(app)
    , m_engine(app->engine())
    , m_model(app->engine())
    , m_meters(app->pw())
{
    Q_ASSERT(!s_instance);
    s_instance = this;
    m_clock.start();

    auto levels = [this] {
        m_model.refresh();
        Q_EMIT levelsChanged();
    };
    connect(m_engine, &engine::Engine::sceneChanged, this, [this, levels] {
        levels();
        updateTargets();
        Q_EMIT structureChanged();
    });
    connect(m_engine, &engine::Engine::structureChanged, this, [this, levels] {
        levels();
        updateTargets();
        Q_EMIT structureChanged();
    });
    connect(m_engine, &engine::Engine::levelsChanged, this, levels);
    connect(m_engine, &engine::Engine::soloChanged, this, levels);
    connect(m_engine, &engine::Engine::appsChanged, this, [this] { m_model.refresh(); });
    connect(m_engine, &engine::Engine::devicesChanged, this, &Mixer::updateTargets);

    connect(&m_timer, &QTimer::timeout, this, &Mixer::tick);
    m_model.refresh();
    updateTargets();
}

Mixer::~Mixer()
{
    s_instance = nullptr;
}

Mixer *Mixer::create(QQmlEngine *, QJSEngine *)
{
    QJSEngine::setObjectOwnership(s_instance, QJSEngine::CppOwnership);
    return s_instance;
}

// The mic strip meters the hardware mic, summed to mono like rostrum.mic, and applies the gain
// fader itself: rostrum.mic is muted whenever the mic does not feed Stream, but the strip must
// still show the voice going to sidetone.
void Mixer::updateTargets()
{
    m_micMeterNode = m_engine->resolvedSourceName();
    QStringList targets{QString::fromLatin1(engine::kPhonesNode), QString::fromLatin1(engine::kStreamNode)};
    QStringList summed;
    if (!m_micMeterNode.isEmpty()) {
        targets << m_micMeterNode;
        summed << m_micMeterNode;
    }
    for (const auto &b : m_engine->scene().buses) {
        if (!b.isInput()) {
            targets << b.nodeName();
        }
    }
    m_meters.setTargets(targets, summed);
}

void Mixer::setMetersActive(bool active)
{
    if (active == m_metersActive) {
        return;
    }
    m_metersActive = active;
    m_meters.setActive(active);
    if (active) {
        m_lastTick = m_clock.elapsed();
        m_timer.start(m_app->settings().meterSpeed == QLatin1String("low") ? meters::kLowIntervalMs
                                                                           : meters::kNormalIntervalMs);
    } else {
        m_timer.stop();
        m_busMeters.clear();
        m_phones = {};
        m_stream = {};
        for (int row = 0; row < m_model.rowCount(); ++row) {
            m_model.setMeter(row, 0.0, false);
        }
        Q_EMIT metersChanged();
    }
    Q_EMIT metersActiveChanged();
}

void Mixer::tick()
{
    const qint64 now = m_clock.elapsed();
    const double dt = std::max<qint64>(1, now - m_lastTick) / 1000.0;
    m_lastTick = now;

    const Scene &s = m_engine->scene();
    for (int row = 0; row < m_model.rowCount(); ++row) {
        const QString id = m_model.data(m_model.index(row), BusModel::IdRole).toString();
        const Bus *b = s.bus(id);
        if (!b) {
            continue;
        }
        const float peak = !b->isInput() ? m_meters.takePeak(b->nodeName())
                         : m_micMeterNode.isEmpty() ? 0.0f
                         : m_meters.takePeak(m_micMeterNode) * float(volume::faderToLinear(b->volume));
        MeterState &st = m_busMeters[id];
        meters::advance(st, peak, b->muted || m_engine->dimmedBySolo(id), dt, now);
        m_model.setMeter(row, st.fraction, st.clip);
    }
    meters::advance(m_phones, m_meters.takePeak(QString::fromLatin1(engine::kPhonesNode)), s.masterPhonesMuted, dt, now);
    meters::advance(m_stream, m_meters.takePeak(QString::fromLatin1(engine::kStreamNode)), s.masterStreamMuted, dt, now);
    Q_EMIT metersChanged();
}

double Mixer::masterPhones() const { return m_engine->scene().masterPhones; }
void Mixer::setMasterPhones(double v) { m_engine->setMasterPhones(v); }
bool Mixer::masterPhonesMuted() const { return m_engine->scene().masterPhonesMuted; }
void Mixer::setMasterPhonesMuted(bool m) { m_engine->setMasterPhonesMuted(m); }
double Mixer::masterStream() const { return m_engine->scene().masterStream; }
void Mixer::setMasterStream(double v) { m_engine->setMasterStream(v); }
bool Mixer::masterStreamMuted() const { return m_engine->scene().masterStreamMuted; }
void Mixer::setMasterStreamMuted(bool m) { m_engine->setMasterStreamMuted(m); }
bool Mixer::canAddBus() const { return m_engine->canAddBus(); }
bool Mixer::anySolo() const { return m_engine->anySolo(); }
bool Mixer::showDb() const { return m_app->settings().showDb; }
bool Mixer::scrollToAdjust() const { return m_app->settings().scrollToAdjust; }

QStringList Mixer::palette() const
{
    QStringList out;
    for (const auto &s : defaults::palette()) {
        out << QString::fromLatin1(s.hex);
    }
    return out;
}

QStringList Mixer::paletteNames() const
{
    QStringList out;
    for (const auto &s : defaults::palette()) {
        out << QString::fromLatin1(s.name);
    }
    return out;
}

void Mixer::setVolume(const QString &busId, double position) { m_engine->setBusVolume(busId, position); }
void Mixer::setMuted(const QString &busId, bool muted) { m_engine->setBusMuted(busId, muted); }
void Mixer::setBalance(const QString &busId, double balance) { m_engine->setBusBalance(busId, balance); }

void Mixer::toggleMuted(const QString &busId)
{
    if (const Bus *b = m_engine->scene().bus(busId)) {
        m_engine->setBusMuted(busId, !b->muted);
    }
}

void Mixer::toggleSolo(const QString &busId) { m_engine->setSolo(busId, !m_engine->isSoloed(busId)); }

void Mixer::setDestination(const QString &busId, int index)
{
    static const Destination map[] = {Destination::Phones, Destination::Stream, Destination::Both};
    if (index < 0 || index > 2) {
        return;
    }
    m_engine->setBusDestination(busId, map[index]);
}

void Mixer::rename(const QString &busId, const QString &name)
{
    if (!name.trimmed().isEmpty()) {
        m_engine->renameBus(busId, name.trimmed());
    }
}

void Mixer::recolor(const QString &busId, const QString &color) { m_engine->recolorBus(busId, color); }

void Mixer::setAutoCategory(const QString &busId, const QString &category)
{
    m_engine->setBusAutoCategory(busId, categoryFromString(category).value_or(AppCategory::None));
}
QString Mixer::duplicate(const QString &busId) { return m_engine->duplicateBus(busId); }
bool Mixer::remove(const QString &busId) { return m_engine->removeBus(busId); }

QString Mixer::addBus()
{
    return m_engine->addBus(i18nc("default name of a new bus", "Bus %1", m_engine->scene().buses.size()));
}

int Mixer::assignedCount(const QString &busId) const
{
    QSet<QString> keys;
    for (const auto &r : m_engine->scene().rules) {
        if (r.busId == busId) {
            keys.insert(AppKey{r.key, r.match}.toString().toLower());
        }
    }
    for (const auto &a : m_engine->appStreams()) {
        if (a.busId == busId) {
            keys.insert(a.identity.key.toString().toLower());
        }
    }
    return int(keys.size());
}

void Mixer::assignApp(const QString &appKey, const QString &busId)
{
    m_engine->assignApp(AppKey::fromString(appKey), busId, true);
}

void Mixer::unassignApp(const QString &appKey) { m_engine->unassignApp(AppKey::fromString(appKey)); }

QString Mixer::formatDb(double position) const
{
    if (position <= 0.0) {
        return i18nc("decibels, silent", "−∞ dB");
    }
    const double db = volume::faderToDb(position);
    const QString number = QString::number(std::abs(db), 'f', 1);
    if (db >= 0.05) {
        return i18nc("decibels", "+%1 dB", number);
    }
    if (db <= -0.05) {
        return i18nc("decibels", "−%1 dB", number);
    }
    return i18nc("decibels", "0.0 dB");
}

} // namespace rostrum::app
