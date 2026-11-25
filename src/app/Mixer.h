#pragma once

#include "app/MeterBallistics.h"
#include "pw/MeterBank.h"

#include <QAbstractListModel>
#include <QElapsedTimer>
#include <QTimer>
#include <QtQml/qqmlregistration.h>

class QQmlEngine;
class QJSEngine;

namespace rostrum::engine {
class Engine;
}

namespace rostrum::app {

class AppController;

// One row per bus of the live scene, mic first. Peak and clip update at the meter rate.
class BusModel : public QAbstractListModel
{
    Q_OBJECT
    QML_ANONYMOUS
public:
    enum Role {
        IdRole = Qt::UserRole + 1,
        NameRole,
        ColorRole,
        IsInputRole,
        VolumeRole,
        PhonesVolumeRole,
        LinkedRole,
        MutedRole,
        SoloedRole,
        DimmedRole,
        DestinationRole,
        AppsRole,
        PeakRole,
        ClipRole,
        PhonesPeakRole,
        PhonesClipRole,
        AutoCategoryRole, // "game", "voice", ... or "none"
        BalanceRole,
        DuckedRole, // turned down by auto-ducking right now
        VodRole,
        ChatRole,        // sends to Rostrum Chat Mic
        ChatMutedRole,   // that send is switched off live
        ChatVolumeRole,
        ChatAllowedRole, // false for the mic and for a bus that plays voice chat
        ChatBlockedRole, // send held off because an app on this bus records the chat mic
    };

    explicit BusModel(engine::Engine *engine, QObject *parent = nullptr);

    int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    void refresh();
    void setMeter(int row, double fraction, bool clip, double phonesFraction, bool phonesClip);

private:
    struct Meter
    {
        double fraction = 0.0;
        bool clip = false;
        double phonesFraction = 0.0;
        bool phonesClip = false;
    };
    engine::Engine *m_engine = nullptr;
    QStringList m_ids;
    QHash<QString, QVariantList> m_apps;
    QHash<QString, Meter> m_meters;
};

// QML's view of the mix: bus strips, masters, meters and the edit actions on them.
class Mixer : public QObject
{
    Q_OBJECT
    QML_ELEMENT
    QML_SINGLETON

    Q_PROPERTY(rostrum::app::BusModel *buses READ buses CONSTANT)
    Q_PROPERTY(double masterPhones READ masterPhones WRITE setMasterPhones NOTIFY levelsChanged)
    Q_PROPERTY(bool masterPhonesMuted READ masterPhonesMuted WRITE setMasterPhonesMuted NOTIFY levelsChanged)
    Q_PROPERTY(double masterStream READ masterStream WRITE setMasterStream NOTIFY levelsChanged)
    Q_PROPERTY(bool masterStreamMuted READ masterStreamMuted WRITE setMasterStreamMuted NOTIFY levelsChanged)
    Q_PROPERTY(double phonesPeak READ phonesPeak NOTIFY metersChanged)
    Q_PROPERTY(bool phonesClip READ phonesClip NOTIFY metersChanged)
    Q_PROPERTY(double streamPeak READ streamPeak NOTIFY metersChanged)
    Q_PROPERTY(bool streamClip READ streamClip NOTIFY metersChanged)
    Q_PROPERTY(bool canAddBus READ canAddBus NOTIFY structureChanged)
    Q_PROPERTY(bool anySolo READ anySolo NOTIFY levelsChanged)
    Q_PROPERTY(QStringList palette READ palette CONSTANT)
    Q_PROPERTY(QStringList paletteNames READ paletteNames CONSTANT)
    Q_PROPERTY(bool metersActive READ metersActive WRITE setMetersActive NOTIFY metersActiveChanged)
    Q_PROPERTY(bool showDb READ showDb NOTIFY settingsChanged)
    Q_PROPERTY(bool scrollToAdjust READ scrollToAdjust NOTIFY settingsChanged)
    Q_PROPERTY(bool perMixLevels READ perMixLevels NOTIFY perMixLevelsChanged)
    Q_PROPERTY(QString perMixUnavailable READ perMixUnavailable NOTIFY perMixLevelsChanged)
    // Empty, or why voice chat cannot run.
    Q_PROPERTY(QString chatUnavailable READ chatUnavailable NOTIFY chatMicChanged)
    // Some playback bus has a voice chat fader; the strips widen to fit three faders.
    Q_PROPERTY(bool anyChat READ anyChat NOTIFY levelsChanged)
    Q_PROPERTY(bool chatMicActive READ chatMicActive NOTIFY chatMicChanged)
    // Apps whose voice chat sounds are off because they record the chat mic from their own bus.
    Q_PROPERTY(QStringList chatBlockedBuses READ chatBlockedBuses NOTIFY chatLoopChanged)
    Q_PROPERTY(QStringList chatLoopApps READ chatLoopApps NOTIFY chatLoopChanged)
    // Names of playback buses set to Stream only; empty when none (for Bluetooth headphones hint).
    Q_PROPERTY(QStringList streamOnlyPlaybackBusNames READ streamOnlyPlaybackBusNames NOTIFY structureChanged)

public:
    // Destination indices used by QML, in the order of the segmented control.
    enum DestinationIndex { PhonesIndex = 0, StreamIndex = 1, BothIndex = 2 };
    Q_ENUM(DestinationIndex)

    Mixer(AppController *app, QObject *parent);
    ~Mixer() override;

    static Mixer *create(QQmlEngine *, QJSEngine *);

    BusModel *buses() { return &m_model; }
    double masterPhones() const;
    void setMasterPhones(double v);
    bool masterPhonesMuted() const;
    void setMasterPhonesMuted(bool m);
    double masterStream() const;
    void setMasterStream(double v);
    bool masterStreamMuted() const;
    void setMasterStreamMuted(bool m);
    double phonesPeak() const { return m_phones.fraction; }
    bool phonesClip() const { return m_phones.clip; }
    double streamPeak() const { return m_stream.fraction; }
    bool streamClip() const { return m_stream.clip; }
    bool canAddBus() const;
    bool anySolo() const;
    QStringList palette() const;
    QStringList paletteNames() const;
    bool metersActive() const { return m_metersActive; }
    void setMetersActive(bool active);
    bool showDb() const;
    bool scrollToAdjust() const;
    bool perMixLevels() const;
    QString perMixUnavailable() const;
    QString chatUnavailable() const;
    bool chatMicActive() const;
    bool anyChat() const;
    QStringList chatLoopApps() const;
    QStringList chatBlockedBuses() const;
    QStringList streamOnlyPlaybackBusNames() const;
    void notifySettingsChanged() { Q_EMIT settingsChanged(); }

    // Set every Stream-only playback bus to Both (Mixer Bluetooth headphones hint).
    Q_INVOKABLE void includeStreamOnlyBusesInHeadphones();
    Q_INVOKABLE void setVolume(const QString &busId, double position);
    // mix is "phones", "stream" or "chat". The chat level is never linked.
    // A linked bus moves the other level by the same amount.
    Q_INVOKABLE void setMixVolume(const QString &busId, const QString &mix, double position);
    Q_INVOKABLE void setLinked(const QString &busId, bool linked);
    // Compact View: move the Stream level to streamPosition and the Headphones level by the same amount.
    Q_INVOKABLE void setVolumeTogether(const QString &busId, double streamPosition);
    Q_INVOKABLE void setMuted(const QString &busId, bool muted);
    Q_INVOKABLE void setBalance(const QString &busId, double balance);
    Q_INVOKABLE void toggleMuted(const QString &busId);
    Q_INVOKABLE void toggleSolo(const QString &busId);
    Q_INVOKABLE void setDestination(const QString &busId, int index);
    Q_INVOKABLE void setVod(const QString &busId, bool on);
    Q_INVOKABLE void setChat(const QString &busId, bool on);
    Q_INVOKABLE void setChatMuted(const QString &busId, bool muted);
    Q_INVOKABLE void rename(const QString &busId, const QString &name);
    Q_INVOKABLE void recolor(const QString &busId, const QString &color);
    // category: "game", "voice", "music", "alerts", "desktop" or "none". One bus per category.
    Q_INVOKABLE void setAutoCategory(const QString &busId, const QString &category);
    Q_INVOKABLE QString duplicate(const QString &busId);
    Q_INVOKABLE bool remove(const QString &busId);
    Q_INVOKABLE QString addBus();
    Q_INVOKABLE int assignedCount(const QString &busId) const; // rules plus running apps
    Q_INVOKABLE void assignApp(const QString &appKey, const QString &busId);
    Q_INVOKABLE void unassignApp(const QString &appKey);
    // Moves app streams another program took off their bus back onto it.
    Q_INVOKABLE void reclaimStreams(const QVariantList &nodeIds);
    Q_INVOKABLE QString formatDb(double position) const;

Q_SIGNALS:
    void levelsChanged();
    void structureChanged();
    void metersChanged();
    void metersActiveChanged();
    void settingsChanged();
    void perMixLevelsChanged();
    void chatMicChanged();
    void chatLoopChanged();

private:
    using MeterState = meters::State;
    void tick();
    void updateTargets();

    static Mixer *s_instance;
    AppController *m_app = nullptr;
    engine::Engine *m_engine = nullptr;
    BusModel m_model;
    pw::MeterBank m_meters;
    QString m_micMeterNode;
    QTimer m_timer;
    QElapsedTimer m_clock;
    qint64 m_lastTick = 0;
    bool m_metersActive = false;
    QHash<QString, MeterState> m_busMeters;
    QHash<QString, MeterState> m_phonesBusMeters;
    MeterState m_phones;
    MeterState m_stream;
};

} // namespace rostrum::app
