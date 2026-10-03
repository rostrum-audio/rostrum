#include "dsp/ladspa_abi.h"

#include <QTest>

#include <dlfcn.h>

#include <atomic>
#include <cmath>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

// Counts heap use while a plugin's run() is on the stack; realtime code must not allocate.
static std::atomic<bool> g_inRun{false};
static std::atomic<int> g_runAllocations{0};

extern "C" {
void *__libc_malloc(size_t);
void *__libc_calloc(size_t, size_t);
void *__libc_realloc(void *, size_t);

void *malloc(size_t n)
{
    if (g_inRun.load(std::memory_order_relaxed))
        g_runAllocations++;
    return __libc_malloc(n);
}

void *calloc(size_t n, size_t size)
{
    if (g_inRun.load(std::memory_order_relaxed))
        g_runAllocations++;
    return __libc_calloc(n, size);
}

void *realloc(void *p, size_t n)
{
    if (g_inRun.load(std::memory_order_relaxed))
        g_runAllocations++;
    return __libc_realloc(p, n);
}
}

namespace {

constexpr unsigned long kRate = 48000;

const LADSPA_Descriptor *findDescriptor(const char *label)
{
    static void *module = dlopen(ROSTRUM_DSP_PLUGIN, RTLD_NOW | RTLD_LOCAL);
    if (!module)
        return nullptr;
    auto entry = reinterpret_cast<LADSPA_Descriptor_Function>(dlsym(module, "ladspa_descriptor"));
    if (!entry)
        return nullptr;
    for (unsigned long i = 0;; i++) {
        const LADSPA_Descriptor *d = entry(i);
        if (!d)
            return nullptr;
        if (!label || std::string(d->Label) == label)
            return d;
    }
}

std::vector<const LADSPA_Descriptor *> allDescriptors()
{
    std::vector<const LADSPA_Descriptor *> out;
    void *module = dlopen(ROSTRUM_DSP_PLUGIN, RTLD_NOW | RTLD_LOCAL);
    auto entry = module ? reinterpret_cast<LADSPA_Descriptor_Function>(dlsym(module, "ladspa_descriptor")) : nullptr;
    for (unsigned long i = 0; entry && entry(i); i++)
        out.push_back(entry(i));
    return out;
}

// The same default rules PipeWire's LADSPA loader applies.
float hintDefault(const LADSPA_PortRangeHint &h)
{
    const float lo = h.LowerBound, hi = h.UpperBound;
    const bool log = h.HintDescriptor & LADSPA_HINT_LOGARITHMIC;
    auto blend = [&](float a) {
        return log ? std::exp(std::log(lo) * a + std::log(hi) * (1 - a)) : lo * a + hi * (1 - a);
    };
    switch (h.HintDescriptor & LADSPA_HINT_DEFAULT_MASK) {
    case LADSPA_HINT_DEFAULT_MINIMUM: return lo;
    case LADSPA_HINT_DEFAULT_MAXIMUM: return hi;
    case LADSPA_HINT_DEFAULT_LOW: return blend(0.75f);
    case LADSPA_HINT_DEFAULT_MIDDLE: return blend(0.5f);
    case LADSPA_HINT_DEFAULT_HIGH: return blend(0.25f);
    case LADSPA_HINT_DEFAULT_0: return 0;
    case LADSPA_HINT_DEFAULT_1: return 1;
    case LADSPA_HINT_DEFAULT_100: return 100;
    case LADSPA_HINT_DEFAULT_440: return 440;
    default: return lo;
    }
}

class Host
{
public:
    Host(const char *label, unsigned long rate = kRate, std::map<std::string, float> controls = {})
        : m_desc(findDescriptor(label))
    {
        if (!m_desc)
            return;
        m_values.resize(m_desc->PortCount);
        for (unsigned long p = 0; p < m_desc->PortCount; p++)
            m_values[p] = hintDefault(m_desc->PortRangeHints[p]);
        for (const auto &[name, value] : controls)
            set(name, value);
        m_handle = m_desc->instantiate(m_desc, rate);
        for (unsigned long p = 0; p < m_desc->PortCount; p++) {
            if (m_desc->PortDescriptors[p] & LADSPA_PORT_CONTROL)
                m_desc->connect_port(m_handle, p, &m_values[p]);
        }
        if (m_desc->activate)
            m_desc->activate(m_handle);
    }
    ~Host()
    {
        if (m_handle)
            m_desc->cleanup(m_handle);
    }
    Host(const Host &) = delete;
    Host &operator=(const Host &) = delete;

    bool valid() const { return m_handle != nullptr; }

    void set(const std::string &name, float value) { m_values[port(name)] = value; }
    float get(const std::string &name) const { return m_values[port(name)]; }

    std::vector<float> process(const std::vector<float> &in, size_t block = 1024)
    {
        std::vector<float> out(in.size());
        std::vector<float> inBuf(block), outBuf(block);
        m_desc->connect_port(m_handle, port("In"), inBuf.data());
        m_desc->connect_port(m_handle, port("Out"), outBuf.data());
        for (size_t at = 0; at < in.size(); at += block) {
            const size_t n = std::min(block, in.size() - at);
            std::copy(in.begin() + long(at), in.begin() + long(at + n), inBuf.begin());
            g_inRun = true;
            m_desc->run(m_handle, n);
            g_inRun = false;
            std::copy(outBuf.begin(), outBuf.begin() + long(n), out.begin() + long(at));
        }
        return out;
    }

private:
    unsigned long port(const std::string &name) const
    {
        for (unsigned long p = 0; p < m_desc->PortCount; p++) {
            if (name == m_desc->PortNames[p])
                return p;
        }
        qFatal("no port %s on %s", name.c_str(), m_desc->Label);
    }

    const LADSPA_Descriptor *m_desc = nullptr;
    LADSPA_Handle m_handle = nullptr;
    std::vector<float> m_values;
};

std::vector<float> sine(float freq, float amplitude, double seconds, unsigned long rate = kRate)
{
    std::vector<float> out(size_t(seconds * rate));
    for (size_t i = 0; i < out.size(); i++)
        out[i] = amplitude * float(std::sin(2 * M_PI * freq * double(i) / rate));
    return out;
}

std::vector<float> noise(float amplitude, double seconds, unsigned seed = 1)
{
    std::vector<float> out(size_t(seconds * kRate));
    uint32_t s = seed;
    for (float &v : out) {
        s = s * 1664525u + 1013904223u;
        v = amplitude * (float(s >> 8) / float(1 << 23) - 1.0f);
    }
    return out;
}

// A signal with speech-like dynamics: noise floor, bursts and loud peaks.
std::vector<float> program()
{
    auto out = noise(0.01f, 2.0);
    const auto tone = sine(220, 0.6f, 2.0);
    const auto loud = sine(1000, 1.8f, 2.0);
    for (size_t i = 0; i < out.size(); i++) {
        const double t = double(i) / kRate;
        if (std::fmod(t, 0.5) < 0.2)
            out[i] += tone[i];
        if (t > 1.2 && t < 1.3)
            out[i] += loud[i];
    }
    return out;
}

double rmsDb(const std::vector<float> &v, size_t from, size_t to)
{
    double sum = 0;
    for (size_t i = from; i < to; i++)
        sum += double(v[i]) * v[i];
    return 10 * std::log10(sum / double(to - from) + 1e-30);
}

double rmsDb(const std::vector<float> &v, double fromSeconds)
{
    return rmsDb(v, size_t(fromSeconds * kRate), v.size());
}

float peak(const std::vector<float> &v)
{
    float p = 0;
    for (float x : v)
        p = std::max(p, std::fabs(x));
    return p;
}

std::vector<std::string> labels()
{
    std::vector<std::string> out = { "rostrum_highpass", "rostrum_eq", "rostrum_gate", "rostrum_compressor",
                                     "rostrum_limiter" };
#ifdef ROSTRUM_DSP_DENOISE
    out.push_back("rostrum_denoise");
#endif
    return out;
}

} // namespace

class TestDsp : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void descriptorsAreWellFormed()
    {
        const auto all = allDescriptors();
        QCOMPARE(all.size(), labels().size());
        std::set<unsigned long> ids;
        for (const LADSPA_Descriptor *d : all) {
            QVERIFY2(ids.insert(d->UniqueID).second, d->Label);
            QVERIFY(d->Properties & LADSPA_PROPERTY_HARD_RT_CAPABLE);
            std::set<std::string> names;
            int audioIn = 0, audioOut = 0;
            for (unsigned long p = 0; p < d->PortCount; p++) {
                const std::string name = d->PortNames[p];
                QVERIFY2(names.insert(name).second, name.c_str());
                QVERIFY2(name.find(':') == std::string::npos, name.c_str());
                const auto pd = d->PortDescriptors[p];
                if (pd & LADSPA_PORT_AUDIO) {
                    audioIn += (pd & LADSPA_PORT_INPUT) ? 1 : 0;
                    audioOut += (pd & LADSPA_PORT_OUTPUT) ? 1 : 0;
                    continue;
                }
                if ((pd & LADSPA_PORT_INPUT) && (pd & LADSPA_PORT_CONTROL)) {
                    const auto &h = d->PortRangeHints[p];
                    QVERIFY2((h.HintDescriptor & (LADSPA_HINT_BOUNDED_BELOW | LADSPA_HINT_BOUNDED_ABOVE))
                                 || (h.HintDescriptor & LADSPA_HINT_TOGGLED),
                             name.c_str());
                    const float def = hintDefault(h);
                    QVERIFY2(def >= h.LowerBound && def <= h.UpperBound, name.c_str());
                }
            }
            QCOMPARE(audioIn, 1);
            QCOMPARE(audioOut, 1);
            QVERIFY(names.count("In") && names.count("Out") && names.count("Enabled"));
        }
    }

    void outputDoesNotDependOnBlockSize_data()
    {
        QTest::addColumn<QString>("label");
        for (const auto &label : labels())
            QTest::newRow(label.c_str()) << QString::fromStdString(label);
    }

    void outputDoesNotDependOnBlockSize()
    {
        QFETCH(QString, label);
        const auto in = program();
        const std::string l = label.toStdString();
        Host whole(l.c_str());
        QVERIFY(whole.valid());
        const auto reference = whole.process(in, 8192);
        for (size_t block : { size_t(1), size_t(7), size_t(333), size_t(1024) }) {
            Host split(l.c_str());
            const auto out = split.process(in, block);
            for (size_t i = 0; i < in.size(); i++) {
                if (out[i] != reference[i])
                    QFAIL(qPrintable(QStringLiteral("block %1 differs at sample %2").arg(block).arg(i)));
            }
        }
        for (float v : reference)
            QVERIFY(std::isfinite(v));
    }

    void runNeverAllocates()
    {
        const auto in = program();
        g_runAllocations = 0;
        for (const auto &label : labels()) {
            Host host(label.c_str());
            host.process(in, 256);
            host.set("Enabled", 0);
            host.process(in, 256);
        }
        QCOMPARE(g_runAllocations.load(), 0);

        // The counter itself works.
        g_inRun = true;
        free(malloc(16));
        g_inRun = false;
        QCOMPARE(g_runAllocations.load(), 1);
    }

    void disabledIsBitTransparent_data() { outputDoesNotDependOnBlockSize_data(); }

    void disabledIsBitTransparent()
    {
        QFETCH(QString, label);
        const std::string l = label.toStdString();
        Host host(l.c_str(), kRate, { { "Enabled", 0 } });
        const auto in = program();
        const auto out = host.process(in);
        size_t latency = 0;
        if (l == "rostrum_limiter" || l == "rostrum_denoise")
            latency = size_t(host.get("latency"));
        for (size_t i = latency; i < in.size(); i++)
            QCOMPARE(out[i], in[i - latency]);
    }

    void highpassRemovesRumble()
    {
        Host gentle("rostrum_highpass", kRate, { { "Frequency", 120 } });
        Host steep("rostrum_highpass", kRate, { { "Frequency", 120 }, { "Steep", 1 } });
        const auto rumble = sine(30, 0.5f, 1.0);
        const double in = rmsDb(rumble, 0.5);
        const double outGentle = rmsDb(gentle.process(rumble), 0.5);
        const double outSteep = rmsDb(steep.process(rumble), 0.5);
        QVERIFY2(in - outGentle > 20, qPrintable(QString::number(in - outGentle)));
        QVERIFY2(in - outSteep > 40, qPrintable(QString::number(in - outSteep)));

        Host voice("rostrum_highpass", kRate, { { "Frequency", 120 }, { "Steep", 1 } });
        const auto tone = sine(1000, 0.5f, 1.0);
        QVERIFY(std::fabs(rmsDb(voice.process(tone), 0.5) - rmsDb(tone, 0.5)) < 0.1);
    }

    void eqIsFlatAtZeroAndHitsItsGain()
    {
        Host flat("rostrum_eq");
        const auto in = program();
        const auto out = flat.process(in);
        for (size_t i = 0; i < in.size(); i++)
            QVERIFY(std::fabs(out[i] - in[i]) < 1e-4f);

        Host presence("rostrum_eq", kRate, { { "Presence gain", 6 }, { "Presence frequency", 4000 } });
        const auto tone = sine(4000, 0.25f, 1.0);
        QVERIFY(std::fabs(rmsDb(presence.process(tone), 0.5) - rmsDb(tone, 0.5) - 6.0) < 0.3);

        Host mud("rostrum_eq", kRate, { { "Mud gain", -6 }, { "Mud frequency", 350 } });
        const auto low = sine(350, 0.25f, 1.0);
        QVERIFY(std::fabs(rmsDb(mud.process(low), 0.5) - rmsDb(low, 0.5) + 6.0) < 0.3);
    }

    void gateClosesOnQuietAndPassesSpeech()
    {
        Host gate("rostrum_gate", kRate, { { "Threshold", -50 }, { "Range", -30 }, { "Release", 50 } });
        const auto quiet = sine(300, 0.001f, 1.5); // -60 dBFS peak
        const double reduction = rmsDb(quiet, 1.0) - rmsDb(gate.process(quiet), 1.0);
        QVERIFY2(std::fabs(reduction - 30) < 1, qPrintable(QString::number(reduction)));
        QVERIFY(std::fabs(gate.get("Reduction") - 30) < 1);

        Host open("rostrum_gate", kRate, { { "Threshold", -50 }, { "Range", -30 } });
        const auto speech = sine(300, 0.1f, 1.0);
        QVERIFY(std::fabs(rmsDb(open.process(speech), 0.2) - rmsDb(speech, 0.2)) < 0.01);
    }

    void compressorFollowsItsCurve()
    {
        const std::vector<float> dc(kRate, 0.5f); // -6.02 dBFS
        Host comp("rostrum_compressor", kRate,
                  { { "Threshold", -18 }, { "Ratio", 3 }, { "Knee", 0 }, { "Makeup", 0 } });
        const auto out = comp.process(dc);
        const double expected = -18 + (20 * std::log10(0.5) + 18) / 3;
        QVERIFY2(std::fabs(rmsDb(out, 0.5) - expected) < 0.05, qPrintable(QString::number(rmsDb(out, 0.5))));
        QVERIFY(std::fabs(comp.get("Reduction") - (-6.0206 - expected)) < 0.05);

        Host makeup("rostrum_compressor", kRate,
                    { { "Threshold", -18 }, { "Ratio", 3 }, { "Knee", 0 }, { "Makeup", 4 } });
        QVERIFY(std::fabs(rmsDb(makeup.process(dc), 0.5) - expected - 4) < 0.05);

        Host below("rostrum_compressor", kRate, { { "Threshold", -18 }, { "Knee", 6 }, { "Makeup", 0 } });
        const auto quiet = sine(500, 0.05f, 0.5);
        const auto quietOut = below.process(quiet);
        for (size_t i = 0; i < quiet.size(); i++)
            QCOMPARE(quietOut[i], quiet[i]);
    }

    void limiterNeverPassesTheCeiling()
    {
        Host limiter("rostrum_limiter", kRate, { { "Ceiling", -1 } });
        const float ceiling = std::pow(10.0f, -1.0f / 20);
        QCOMPARE(limiter.get("latency"), 72.0f);

        auto hot = sine(1000, 2.0f, 1.0);
        hot[24000] = 6.0f;
        hot[24001] = -6.0f;
        const auto out = limiter.process(hot);
        QVERIFY2(peak(out) <= ceiling, qPrintable(QString::number(peak(out))));
        QVERIFY(peak(out) > ceiling * 0.95f);
        QVERIFY(limiter.get("Reduction") > 6);

        Host clean("rostrum_limiter");
        const auto quiet = sine(1000, 0.3f, 0.5);
        const auto quietOut = clean.process(quiet);
        for (size_t i = 72; i < quiet.size(); i++)
            QCOMPARE(quietOut[i], quiet[i - 72]);
    }

    void denoiseSuppressesNoise()
    {
#ifndef ROSTRUM_DSP_DENOISE
        QSKIP("built without RNNoise");
#else
        Host denoise("rostrum_denoise");
        QCOMPARE(denoise.get("latency"), 480.0f);
        QCOMPARE(denoise.get("Active"), 1.0f);
        const auto hiss = noise(0.03f, 3.0);
        const double reduction = rmsDb(hiss, 2.0) - rmsDb(denoise.process(hiss), 2.0);
        QVERIFY2(reduction > 15, qPrintable(QString::number(reduction)));

        Host half("rostrum_denoise", kRate, { { "Strength", 0.5f } });
        const double partial = rmsDb(hiss, 2.0) - rmsDb(half.process(hiss), 2.0);
        QVERIFY(partial > 3 && partial < reduction);
#endif
    }

    void denoisePassesThroughAtOtherRates()
    {
#ifndef ROSTRUM_DSP_DENOISE
        QSKIP("built without RNNoise");
#else
        Host denoise("rostrum_denoise", 44100);
        QCOMPARE(denoise.get("Active"), 0.0f);
        QCOMPARE(denoise.get("latency"), 0.0f);
        const auto in = sine(440, 0.3f, 0.5, 44100);
        QCOMPARE(denoise.process(in), in);
#endif
    }
};

QTEST_GUILESS_MAIN(TestDsp)
#include "tst_dsp.moc"
