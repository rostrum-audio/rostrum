#include "pw/TestTone.h"

#include <QTest>

#include <cmath>

using namespace rostrum::pw;

namespace {

// Sum of squares per channel over [from, to) seconds.
void energy(double from, double to, double &left, double &right)
{
    left = right = 0.0;
    const int rate = chime::sampleRate();
    for (int f = int(from * rate); f < int(to * rate); ++f) {
        float l = 0.0f;
        float r = 0.0f;
        chime::sample(f, l, r);
        left += double(l) * l;
        right += double(r) * r;
    }
}

} // namespace

class TestTestTone : public QObject
{
    Q_OBJECT
private Q_SLOTS:
    void levelsAreSafe()
    {
        float peak = 0.0f;
        for (int f = 0; f < chime::totalFrames(); ++f) {
            float l = 0.0f;
            float r = 0.0f;
            chime::sample(f, l, r);
            QVERIFY(std::isfinite(l) && std::isfinite(r));
            peak = std::max({peak, std::abs(l), std::abs(r)});
        }
        QVERIFY2(peak > 0.1f, "audible");
        QVERIFY2(peak < 0.5f, "well below clipping");
    }

    void startsAndEndsSilent()
    {
        float l = 1.0f;
        float r = 1.0f;
        chime::sample(0, l, r);
        QCOMPARE(l, 0.0f);
        QCOMPARE(r, 0.0f);
        chime::sample(chime::totalFrames() - 1, l, r);
        QVERIFY(std::abs(l) < 1e-3f && std::abs(r) < 1e-3f);
        chime::sample(chime::totalFrames() + 10, l, r);
        QCOMPARE(l, 0.0f);
        QCOMPARE(r, 0.0f);
        QVERIFY(chime::totalFrames() < 2 * chime::sampleRate());
    }

    void leansLeftThenRight()
    {
        double l = 0.0;
        double r = 0.0;
        energy(0.0, 0.15, l, r);
        QVERIFY(l > 2.0 * r);
        energy(0.17, 0.31, l, r);
        QVERIFY(r > 1.5 * l);
    }
};

QTEST_GUILESS_MAIN(TestTestTone)
#include "tst_testtone.moc"
