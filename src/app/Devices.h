#pragma once

#include "app/MeterBallistics.h"
#include "pw/MeterBank.h"
#include "pw/TestTone.h"

#include <QElapsedTimer>
#include <QObject>
#include <QTimer>
#include <QVariantList>
#include <QtQml/qqmlregistration.h>

class QQmlEngine;
class QJSEngine;

namespace rostrum::app {

class AppController;

// Hardware outputs and inputs for the Devices page and the wizard: pickers, test tone, live
// input meters while a page that shows them is visible, and Rostrum's own nodes.
class Devices : public QObject
{
    Q_OBJECT
    QML_ELEMENT
    QML_SINGLETON

    // Rows: {name, description, subtitle, isDefault, bluetooth, headsetProfile, id}
    Q_PROPERTY(QVariantList outputs READ outputs NOTIFY listsChanged)
    Q_PROPERTY(QVariantList inputs READ inputs NOTIFY listsChanged)
    // Rows: {name, description, id}
    Q_PROPERTY(QVariantList virtualNodes READ virtualNodes NOTIFY listsChanged)
    Q_PROPERTY(bool anyHeadsetProfile READ anyHeadsetProfile NOTIFY listsChanged)
    // Saved choice (node.name); empty = follow the system default.
    Q_PROPERTY(QString headphones READ headphones WRITE setHeadphones NOTIFY choiceChanged)
    Q_PROPERTY(QString mic READ mic WRITE setMic NOTIFY choiceChanged)
    // The node actually in use after fallback.
    Q_PROPERTY(QString headphonesInUse READ headphonesInUse NOTIFY choiceChanged)
    Q_PROPERTY(QString micInUse READ micInUse NOTIFY choiceChanged)
    // Off: while the saved mic is unplugged, the stream mic is silent. On: another mic stands in.
    Q_PROPERTY(bool micFallback READ micFallback WRITE setMicFallback NOTIFY choiceChanged)
    Q_PROPERTY(bool monoHeadphones READ monoHeadphones WRITE setMonoHeadphones NOTIFY choiceChanged)
    Q_PROPERTY(bool metersActive READ metersActive WRITE setMetersActive NOTIFY metersActiveChanged)
    // node.name -> 0..1 meter fraction, for every input while meters are active.
    Q_PROPERTY(QVariantMap inputLevels READ inputLevels NOTIFY levelsChanged)
    // node.name of the output playing the test tone, empty when silent.
    Q_PROPERTY(QString toneTarget READ toneTarget NOTIFY toneChanged)

public:
    Devices(AppController *app, QObject *parent);
    ~Devices() override;

    static Devices *create(QQmlEngine *, QJSEngine *);

    QVariantList outputs() const { return m_outputs; }
    QVariantList inputs() const { return m_inputs; }
    QVariantList virtualNodes() const { return m_virtual; }
    bool anyHeadsetProfile() const { return m_anyHeadset; }
    QString headphones() const;
    void setHeadphones(const QString &nodeName);
    QString mic() const;
    void setMic(const QString &nodeName);
    QString headphonesInUse() const;
    QString micInUse() const;
    bool micFallback() const;
    void setMicFallback(bool on);
    bool monoHeadphones() const;
    void setMonoHeadphones(bool on);
    bool metersActive() const { return m_metersActive; }
    void setMetersActive(bool active);
    QVariantMap inputLevels() const { return m_levels; }
    QString toneTarget() const;

    Q_INVOKABLE void refresh();
    Q_INVOKABLE bool playTone(const QString &nodeName);
    Q_INVOKABLE void openSoundSettings();

Q_SIGNALS:
    void listsChanged();
    void choiceChanged();
    void metersActiveChanged();
    void levelsChanged();
    void toneChanged();

private:
    void rebuild();
    void tick();

    static Devices *s_instance;
    AppController *m_app = nullptr;
    pw::MeterBank m_meters;
    pw::TestTone m_tone;
    QTimer m_timer;
    QElapsedTimer m_clock;
    qint64 m_lastTick = 0;
    bool m_metersActive = false;
    QVariantList m_outputs;
    QVariantList m_inputs;
    QVariantList m_virtual;
    bool m_anyHeadset = false;
    QStringList m_inputNames;
    QHash<QString, meters::State> m_state;
    QVariantMap m_levels;
};

} // namespace rostrum::app
