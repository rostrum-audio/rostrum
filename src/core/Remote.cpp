#include "core/Remote.h"

#include "core/Model.h"

#include <cmath>

namespace rostrum::remote {

double maxPosition(const QString &busId)
{
    return busId == QLatin1String(kMicBusId) ? 1.5 : 1.0;
}

std::optional<VolumeArg> parseVolumeArg(const QString &text)
{
    const qsizetype eq = text.indexOf(QLatin1Char('='));
    if (eq <= 0) {
        return std::nullopt;
    }
    VolumeArg out;
    out.busId = text.left(eq).trimmed();
    QString value = text.mid(eq + 1).trimmed();
    const bool percent = value.endsWith(QLatin1Char('%'));
    if (percent) {
        value.chop(1);
    }
    bool ok = false;
    out.position = value.toDouble(&ok);
    if (!ok || out.busId.isEmpty() || !std::isfinite(out.position)) {
        return std::nullopt;
    }
    if (percent) {
        out.position /= 100.0;
    }
    return out;
}

} // namespace rostrum::remote
