#pragma once

#include "core/MicFilters.h"

#include <QObject>
#include <QVariantList>
#include <QtQml/qqmlregistration.h>

#include <functional>

class QQmlEngine;
class QJSEngine;

namespace rostrum::app {

class AppController;

// The Mic Filters page and the mixer's FX button: the [mic_filters] settings, what the engine is
// doing with them, and which apps record the filtered mic.
class MicFilters : public QObject
{
    Q_OBJECT
    QML_ELEMENT
    QML_SINGLETON

    // False with a reason when this system or build cannot run them.
    Q_PROPERTY(bool available READ available NOTIFY stateChanged)
    Q_PROPERTY(QString unavailableReason READ unavailableReason NOTIFY stateChanged)
    Q_PROPERTY(bool hasDenoise READ hasDenoise NOTIFY stateChanged)
    // "off", "starting", "active" or "failed"
    Q_PROPERTY(QString state READ state NOTIFY stateChanged)
    Q_PROPERTY(QString error READ error NOTIFY stateChanged)
    Q_PROPERTY(bool enabled READ enabled WRITE setEnabled NOTIFY settingsChanged)
    // "all" (every app) or "stream" (only the stream mic)
    Q_PROPERTY(QString scope READ scope WRITE setScope NOTIFY settingsChanged)
    // A preset id, or "custom"
    Q_PROPERTY(QString preset READ preset NOTIFY settingsChanged)
    // Rows: {id, label, description}
    Q_PROPERTY(QVariantList presets READ presets CONSTANT)
    // Rows: {id, label, description, needsDenoise, switches: [{key, label, description}],
    //        params: [{key, label, description, min, max, step, advanced}]}, in processing order.
    Q_PROPERTY(QVariantList modules READ modules CONSTANT)
    // Bumped on every settings change; value() and isOn() bindings list it to update.
    Q_PROPERTY(int revision READ revision NOTIFY settingsChanged)
    // Rows: {key, name, icon, running, filtered, choice, excludedByDefault, wantsFiltered}.
    // choice: "default", "filtered" or "raw". filtered: records the filtered mic right now.
    // wantsFiltered: what the settings ask for. Saved choices for apps not running are included.
    Q_PROPERTY(QVariantList apps READ apps NOTIFY appsChanged)
    // Mic gain above 100 % with the limiter on: the limiter works harder than it should.
    Q_PROPERTY(bool gainWarning READ gainWarning NOTIFY gainWarningChanged)

public:
    MicFilters(AppController *app, QObject *parent);
    ~MicFilters() override;

    static MicFilters *create(QQmlEngine *, QJSEngine *);

    bool available() const;
    QString unavailableReason() const;
    bool hasDenoise() const;
    QString state() const;
    QString error() const;
    bool enabled() const;
    void setEnabled(bool on);
    QString scope() const;
    void setScope(const QString &scope);
    QString preset() const;
    QVariantList presets() const;
    QVariantList modules() const;
    int revision() const { return m_revision; }
    QVariantList apps() const { return m_apps; }
    bool gainWarning() const;

    Q_INVOKABLE double value(const QString &module, const QString &key) const;
    Q_INVOKABLE void setValue(const QString &module, const QString &key, double value);
    Q_INVOKABLE bool isOn(const QString &module, const QString &key = QStringLiteral("enabled")) const;
    Q_INVOKABLE void setOn(const QString &module, bool on, const QString &key = QStringLiteral("enabled"));
    // A value as shown next to its slider: "80 Hz", "4.0 kHz", "-20 dB", "3.0:1", "Off".
    Q_INVOKABLE QString format(const QString &module, const QString &key, double value) const;
    Q_INVOKABLE void applyPreset(const QString &id);
    // One module's switches and values back to the Streaming preset's.
    Q_INVOKABLE void resetModule(const QString &module);
    // filtered: the app gets the filtered mic. Matching what it would get anyway clears the choice.
    Q_INVOKABLE void setAppFiltered(const QString &appKey, bool filtered);
    Q_INVOKABLE void forgetApp(const QString &appKey);

Q_SIGNALS:
    void stateChanged();
    void settingsChanged();
    void appsChanged();
    void gainWarningChanged();

private:
    void update(const std::function<void(micfx::Settings &)> &change);
    void rebuildApps();
    const micfx::Settings &settings() const;

    static MicFilters *s_instance;
    AppController *m_app = nullptr;
    int m_revision = 0;
    QVariantList m_apps;
    bool m_gainWarning = false;
};

} // namespace rostrum::app
