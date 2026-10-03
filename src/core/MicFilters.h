#pragma once

#include <QList>
#include <QString>
#include <QStringList>

#include <optional>

namespace rostrum::micfx {

// Which apps record the filtered mic. The stream mic is filtered either way.
enum class Scope { AllApps, StreamOnly };

QString scopeName(Scope s);
std::optional<Scope> scopeFromString(const QString &s);

// A per-app choice; Default follows the scope and Rostrum's sense of what the app is.
enum class AppChoice { Default, Filtered, Raw };

// The processing chain, in signal order. Each is one plugin in librostrum-dsp.
enum class Module { Highpass, Denoise, Gate, Eq, Compressor, Limiter };

inline constexpr Module kModules[] = {Module::Highpass, Module::Denoise, Module::Gate,
                                      Module::Eq,       Module::Compressor, Module::Limiter};

QString moduleName(Module m);              // "highpass": TOML table and UI id
QString moduleNode(Module m);              // "rostrum_highpass": graph node and plugin label

struct Settings
{
    bool enabled = false;
    Scope scope = Scope::AllApps;
    QStringList filteredApps; // app keys ("name:Discord") the user moved to the filtered mic
    QStringList rawApps;      // app keys the user moved to the raw mic

    bool highpass = true;
    double highpassHz = 80;
    bool highpassSteep = false;

    bool denoise = true;
    double denoiseStrength = 100; // percent
    double voiceThreshold = 0;    // percent of RNNoise voice probability; 0 = off

    bool gate = false;
    double gateThresholdDb = -50;
    double gateRangeDb = -20;
    double gateAttackMs = 2;
    double gateHoldMs = 200;
    double gateReleaseMs = 150;

    bool eq = true;
    double lowGainDb = 0;
    double lowHz = 120;
    double mudGainDb = -2;
    double mudHz = 300;
    double presenceGainDb = 2;
    double presenceHz = 4000;
    double airGainDb = 1.5;
    double airHz = 10000;

    bool compressor = true;
    double compThresholdDb = -20;
    double compRatio = 3;
    double compKneeDb = 6;
    double compAttackMs = 8;
    double compReleaseMs = 150;
    double compMakeupDb = 4;

    bool limiter = true;
    double limiterCeilingDb = -1;
    double limiterReleaseMs = 60;

    bool operator==(const Settings &) const = default;
};

// One adjustable value: where it lives in settings.toml, which plugin port it drives and the
// range the UI offers. Port value = setting * scale.
struct Param
{
    Module module;
    const char *key;  // TOML key and UI id
    const char *port; // plugin control port
    double Settings::*field;
    double min;
    double max;
    double step;
    double scale = 1.0;
};

struct Switch
{
    Module module;
    const char *key;
    const char *port;
    bool Settings::*field;
};

const QList<Param> &params();
const QList<Switch> &switches(); // each module's "enabled", plus the steep rumble filter
const Param *findParam(const QString &module, const QString &key);
const Switch *findSwitch(const QString &module, const QString &key);

// Clamps and snaps every value to its range and step, and tidies the app lists.
Settings sanitize(Settings s);

// Presets set every module value; the scope, app choices and the master switch are kept.
QStringList presetIds(); // "light", "streaming", "noisy", "broadcast"
QString presetLabel(const QString &id);
Settings applyPreset(Settings s, const QString &id);
// The preset the module values match, or "custom".
QString matchingPreset(const Settings &s);

AppChoice appChoice(const Settings &s, const QString &appKey);
Settings setAppChoice(Settings s, const QString &appKey, AppChoice choice);
// Whether a capture stream from an app records the filtered mic. excludedByDefault: audio tools
// and recorders, which get the untouched mic unless the user says otherwise.
bool useFiltered(const Settings &s, AppChoice choice, bool excludedByDefault);

struct Control
{
    QString key; // "rostrum_gate:Threshold"
    float value;
    bool operator==(const Control &) const = default;
};

// Every control value for the current settings. denoise: the plugin was built with RNNoise.
QList<Control> controls(const Settings &s, bool denoise);
// The filter graph for an audioconvert node, with the current values as its initial controls.
// The node layout depends only on the plugin path and denoise, so later changes are controls.
QString graphJson(const Settings &s, const QString &pluginPath, bool denoise);
// True once a node's Props list the graph's controls, i.e. the graph loaded.
bool graphLoaded(const QStringList &propKeys);

// The audioconvert Props key the graph is loaded into.
inline constexpr const char *kGraphKey = "audioconvert.filter-graph.0";

} // namespace rostrum::micfx
