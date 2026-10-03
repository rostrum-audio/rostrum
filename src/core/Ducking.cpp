#include "core/Ducking.h"

#include "core/Volume.h"

#include <QRegularExpression>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <iterator>

namespace rostrum::ducking {

namespace {

template<std::size_t N>
int nearest(int value, const int (&choices)[N])
{
    return *std::min_element(std::begin(choices), std::end(choices),
                             [value](int a, int b) { return std::abs(a - value) < std::abs(b - value); });
}

} // namespace

QString triggerName(Trigger t)
{
    switch (t) {
    case Trigger::Mic:
        return QStringLiteral("mic");
    case Trigger::Voice:
        return QStringLiteral("voice");
    case Trigger::Either:
        return QStringLiteral("either");
    }
    return QStringLiteral("mic");
}

std::optional<Trigger> triggerFromString(const QString &s)
{
    for (const Trigger t : {Trigger::Mic, Trigger::Voice, Trigger::Either}) {
        if (s == triggerName(t)) {
            return t;
        }
    }
    return std::nullopt;
}

Settings sanitize(Settings s)
{
    static const QRegularExpression idChars(QStringLiteral("^[a-z0-9][a-z0-9-]{0,31}$"));
    QStringList buses;
    for (const QString &id : std::as_const(s.buses)) {
        if (idChars.match(id).hasMatch() && id != QLatin1String("mic") && !buses.contains(id)) {
            buses << id;
        }
    }
    s.buses = buses;
    s.amountDb = nearest(s.amountDb, kAmountChoicesDb);
    s.attackMs = nearest(s.attackMs, kAttackChoicesMs);
    s.releaseMs = nearest(s.releaseMs, kReleaseChoicesMs);
    return s;
}

double Envelope::advance(double peak, double dtMs, const Settings &s)
{
    dtMs = std::max(0.0, dtMs);
    if (volume::linearToDb(peak) >= kThresholdDb) {
        m_holdLeftMs = kHoldMs;
    } else {
        m_holdLeftMs = std::max(0.0, m_holdLeftMs - dtMs);
    }
    const double amount = std::min(0.0, double(s.amountDb));
    const double target = m_holdLeftMs > 0.0 ? amount : 0.0;
    if (m_gainDb > target) {
        const double step = s.attackMs > 0 ? -amount / s.attackMs * dtMs : -amount;
        m_gainDb = std::max(target, m_gainDb - step);
    } else if (m_gainDb < target) {
        const double step = s.releaseMs > 0 ? -amount / s.releaseMs * dtMs : -amount;
        m_gainDb = std::min(target, m_gainDb + step);
    }
    return gain();
}

double Envelope::gain() const
{
    return m_gainDb >= 0.0 ? 1.0 : std::pow(10.0, m_gainDb / 20.0);
}

void Envelope::reset()
{
    m_gainDb = 0.0;
    m_holdLeftMs = 0.0;
}

} // namespace rostrum::ducking
