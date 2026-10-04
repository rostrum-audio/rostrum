#pragma once

#include <QHash>
#include <QJsonObject>
#include <QList>
#include <QSet>
#include <QString>
#include <QStringList>

namespace rostrum::obs {

// Device ids OBS's built-in PulseAudio sources use for Rostrum's nodes.
inline constexpr char kMicDevice[] = "rostrum.mic";
// The filtered mic for apps: the voice again, not part of the Stream Mix.
inline constexpr char kFilteredMicDevice[] = "rostrum.filtered";
inline constexpr char kStreamDevice[] = "rostrum.stream.monitor";
// Names Rostrum gives the sources it creates.
inline constexpr char kMicInputName[] = "Rostrum Mic";
inline constexpr char kStreamInputName[] = "Rostrum Stream Mix";

inline constexpr char kPulseInput[] = "pulse_input_capture";
inline constexpr char kPulseOutput[] = "pulse_output_capture";
inline constexpr char kPwInput[] = "pipewire_audio_input_capture";
inline constexpr char kPwOutput[] = "pipewire_audio_output_capture";
inline constexpr char kPwApp[] = "pipewire_audio_application_capture";

// Tracks are a bit mask, bit 0 = track 1. OBS has six.
inline constexpr quint32 kAllTracks = 0x3F;

// One OBS input, as far as audio routing is concerned.
struct Input
{
    QString name;
    QString kind; // unversioned input kind
    QJsonObject settings;
    bool muted = false;
    quint32 tracks = 0;
    QString channel; // "desktop1", "mic1"... for OBS's global audio devices, else empty
    bool settingsKnown = false, muteKnown = false, tracksKnown = false, gainKnown = false;
    double gain = 1.0;
};

struct State
{
    QList<Input> inputs;
    QStringList scenes; // top-level scenes
    bool scopeKnown = false;
    QString programScene;
    QSet<QString> programInputs; // enabled direct program-scene items; groups/nesting not supported

    const Input *input(const QString &name) const;
    const Input *channel(const QString &channel) const;
    // The free global channel of a family ("desktop" or "mic"), or empty.
    QString freeChannel(const QString &family) const;
};

bool isAudioCaptureKind(const QString &kind);

// What Rostrum knows about the PipeWire side, for resolving "default" and PipeWire serials.
struct Facts
{
    QString defaultSource; // node.name
    QString defaultSink;   // node.name
    QHash<quint32, QString> serials; // object.serial -> node.name
};

// What an input records, from its settings.
enum class Capture {
    None,          // not an audio capture, or not one Rostrum can judge (JACK)
    RostrumMic,    // rostrum.mic
    RostrumStream, // rostrum.stream's monitor
    RostrumBus,    // another Rostrum node: its audio is already in the Stream Mix
    Mic,           // a hardware mic or the filtered mic: the voice doubles with Rostrum Mic
    Output,        // a hardware output's monitor: everything you hear, Phones-only buses too
    App,           // one app captured directly, bypassing its bus
};

Capture classify(const Input &input, const Facts &facts);

} // namespace rostrum::obs
