#pragma once

#include <QString>
#include <QStringList>

#include <optional>

namespace rostrum::ducking {

// What makes the target buses duck: the stream mic, the bus that receives voice chat, or either.
enum class Trigger { Mic, Voice, Either };

QString triggerName(Trigger t);
std::optional<Trigger> triggerFromString(const QString &s);

// Offered in Settings; hand-edited values snap to the nearest.
inline constexpr int kAmountChoicesDb[] = {-6, -9, -12, -18, -24};
inline constexpr int kAttackChoicesMs[] = {20, 50, 100, 250, 500};
inline constexpr int kReleaseChoicesMs[] = {250, 500, 800, 1500, 3000};

// Session behaviour only: the ducked level is never written to a scene.
struct Settings
{
    bool enabled = false;
    Trigger trigger = Trigger::Mic;
    QStringList buses{QStringLiteral("music")}; // bus ids
    int amountDb = -12;
    int attackMs = 100;
    int releaseMs = 800;

    bool operator==(const Settings &) const = default;
};

Settings sanitize(Settings s);

// Signal above this counts as speech.
inline constexpr double kThresholdDb = -40.0;
// Speech pauses shorter than this keep the buses ducked, so they do not pump between words.
inline constexpr double kHoldMs = 500.0;

// Gain envelope in dB: moves toward the amount at amount/attack dB per ms while the trigger is
// above the threshold (or was within the hold time), and back to 0 dB at amount/release dB per ms.
class Envelope
{
public:
    // peak: linear peak of the trigger since the last call; dtMs: time since the last call.
    // Returns the gain to multiply the target buses' linear volume by.
    double advance(double peak, double dtMs, const Settings &s);
    double gainDb() const { return m_gainDb; }
    double gain() const;
    bool ducked() const { return m_gainDb < -1.0; }
    void reset();

private:
    double m_gainDb = 0.0;
    double m_holdLeftMs = 0.0;
};

} // namespace rostrum::ducking
