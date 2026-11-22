#pragma once

#include "pw/Graph.h"

#include <QList>
#include <QString>

#include <algorithm>
#include <cstdint>

namespace rostrum::pw {

// Higher is better. Unavailable or non-A2DP/BAP sink profiles score 0.
// Order: AAC > SBC-XQ > SBC > other A2DP (aptX/vendor) > BAP.
inline int highQualityProfileScore(const QString &profileName)
{
    const QString name = profileName.toLower();
    if (name.contains(QLatin1String("a2dp")) && name.contains(QLatin1String("aac"))) {
        return 50;
    }
    if (name.contains(QLatin1String("sbc_xq")) || name.contains(QLatin1String("sbc-xq"))) {
        return 40;
    }
    if (name.contains(QLatin1String("a2dp")) && name.contains(QLatin1String("sbc"))) {
        return 30;
    }
    if (name.contains(QLatin1String("a2dp")) && name.contains(QLatin1String("sink"))) {
        return 20;
    }
    if (name.contains(QLatin1String("bap")) && name.contains(QLatin1String("sink"))) {
        return 10;
    }
    return 0;
}

// Available high-quality sink profiles, best first. Empty when none score above 0.
inline QList<uint32_t> rankedHighQualityProfiles(const Device &device)
{
    struct Scored {
        uint32_t index = 0;
        int score = 0;
    };
    QList<Scored> scored;
    for (const auto &p : device.profiles) {
        if (!p.available) {
            continue;
        }
        const int score = highQualityProfileScore(p.name);
        if (score <= 0) {
            continue;
        }
        scored.append({p.index, score});
    }
    std::sort(scored.begin(), scored.end(), [](const Scored &a, const Scored &b) {
        return a.score > b.score;
    });
    QList<uint32_t> out;
    out.reserve(scored.size());
    for (const auto &s : scored) {
        out.append(s.index);
    }
    return out;
}

} // namespace rostrum::pw
