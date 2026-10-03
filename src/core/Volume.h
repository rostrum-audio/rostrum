#pragma once

#include <algorithm>
#include <cmath>

namespace rostrum::volume {

// Fader positions are perceptual (like pavucontrol): linear gain = position^3.
inline double faderToLinear(double position)
{
    if (position <= 0.0) {
        return 0.0;
    }
    return position * position * position;
}

inline double linearToFader(double linear)
{
    return linear <= 0.0 ? 0.0 : std::cbrt(linear);
}

inline double linearToDb(double linear)
{
    return linear <= 1e-9 ? -180.0 : 20.0 * std::log10(linear);
}

inline double faderToDb(double position) { return linearToDb(faderToLinear(position)); }

// Balance: -1 = left only, 0 = centre, 1 = right only. Anything else (NaN included) is centre.
inline double clampBalance(double balance)
{
    if (!std::isfinite(balance)) {
        return 0.0;
    }
    // Snap near-centre so a dragged-back control saves no balance at all.
    return std::abs(balance) < 0.005 ? 0.0 : std::clamp(balance, -1.0, 1.0);
}

// Per-channel fader positions for a stereo bus. Like PulseAudio's balance, the far side is turned
// down in fader space and the near side stays at the fader.
inline double balancedPosition(double position, double balance, bool right)
{
    const double b = clampBalance(balance);
    return position * (right ? 1.0 + std::min(b, 0.0) : 1.0 - std::max(b, 0.0));
}

// A scene fade at progress t (0..1), linear in fader space, which is already perceptual.
inline double fadePosition(double from, double to, double t)
{
    return from + (to - from) * std::clamp(t, 0.0, 1.0);
}

// Meter scale: -60 dB .. 0 dB mapped to 0..1.
inline constexpr double kMeterFloorDb = -60.0;
inline constexpr double kMeterAmberDb = -12.0;
inline constexpr double kMeterRedDb = -6.0;

inline double meterFraction(double linearPeak)
{
    const double db = linearToDb(linearPeak);
    if (db <= kMeterFloorDb) {
        return 0.0;
    }
    if (db >= 0.0) {
        return 1.0;
    }
    return (db - kMeterFloorDb) / -kMeterFloorDb;
}

} // namespace rostrum::volume
