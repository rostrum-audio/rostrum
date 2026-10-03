#pragma once

#include "core/Volume.h"

#include <QtGlobal>

#include <algorithm>

namespace rostrum::app::meters {

constexpr int kNormalIntervalMs = 40; // 25 fps
constexpr int kLowIntervalMs = 80;
constexpr qint64 kClipHoldMs = 1500;
constexpr double kFalloffPerSecond = 20.0 / -volume::kMeterFloorDb; // 20 dB/s
constexpr float kClipLinear = 0.999f;

struct State
{
    double fraction = 0.0; // 0..1 on the meter's dB scale
    bool clip = false;
    qint64 clipUntil = 0;
};

// One meter tick: jump up to the new peak, fall off at a fixed dB rate, hold the clip mark.
inline void advance(State &state, float linearPeak, bool frozen, double dt, qint64 now)
{
    if (frozen) {
        state = {};
        return;
    }
    const double target = volume::meterFraction(linearPeak);
    state.fraction = std::max(target, state.fraction - kFalloffPerSecond * dt);
    if (linearPeak >= kClipLinear) {
        state.clipUntil = now + kClipHoldMs;
    }
    state.clip = now < state.clipUntil;
}

} // namespace rostrum::app::meters
