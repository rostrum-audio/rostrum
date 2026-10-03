#pragma once

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
