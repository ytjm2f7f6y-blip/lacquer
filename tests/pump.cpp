// How much each stage pumps: a steady tone under a loud kick and bass, and how far the tone's level
// moves at the output once things settle. Pumping is the tone ducking on every kick.
//
//   pump [song.wav-free: synthetic only]
#include "lq_engine.hpp"
#include "lq_assistant.hpp"

#include <cstdio>
#include <functional>
#include <string>
#include <vector>

using namespace lq;

namespace {
constexpr double kFs = 48000, kPi = 3.14159265358979;

struct Sig { std::vector<float> L, R; };

// 120 bpm: a kick on every beat (55 Hz, 0 dBFS-ish peaks), a sub bass note, hats, and a steady
// 1 kHz tone 20 dB under the kick that we listen to for pumping.
Sig mix(double secs, double kickGain, double toneDb) {
    const size_t n = size_t(secs * kFs);
    Sig s{std::vector<float>(n), std::vector<float>(n)};
    uint32_t seed = 9;
    const double tone = dbToGain(toneDb);
    for (size_t i = 0; i < n; ++i) {
        const double t = double(i) / kFs, beat = std::fmod(t, 0.5);
        double k = 0;
        if (beat < 0.25) {
            const double f = 55 + 90 * std::exp(-beat * 40);
            k = std::sin(2 * kPi * (55 * beat + 90 / 40.0 * (1 - std::exp(-beat * 40)))) * std::exp(-beat * 9) * kickGain;
            (void)f;
        }
        const double bass = 0.18 * std::sin(2 * kPi * 41.2 * t) * (std::fmod(t, 2.0) < 1.8 ? 1 : 0.2);
        seed = seed * 1664525u + 1013904223u;
        const double hat = (std::fmod(t + 0.25, 0.5) < 0.03 ? 1 : 0) * double(int32_t(seed)) / 2147483648.0 * 0.05;
        const double pad = tone * std::sin(2 * kPi * 1000 * t);
        s.L[i] = float(k + bass + hat + pad);
        s.R[i] = float(k + bass + hat * 0.6 + pad);
    }
    return s;
}

// The 1 kHz tone's level over time: a narrow band-pass and a 10 ms RMS, in dB.
std::vector<double> toneLevel(const std::vector<float>& x) {
    // Two cascaded RBJ band-passes at 1 kHz, Q 8.
    const double w = 2 * kPi * 1000 / kFs, q = 8, alpha = std::sin(w) / (2 * q);
    const double b0 = alpha, b2 = -alpha, a0 = 1 + alpha, a1 = -2 * std::cos(w), a2 = 1 - alpha;
    double z[2][4]{};
    const size_t win = size_t(0.01 * kFs);
    std::vector<double> out;
    double acc = 0;
    size_t cnt = 0;
    for (float v : x) {
        double y = v;
        for (int s = 0; s < 2; ++s) {
            const double o = (b0 * y + b2 * z[s][1] - a1 * z[s][2] - a2 * z[s][3]) / a0;
            z[s][1] = z[s][0]; z[s][0] = y; z[s][3] = z[s][2]; z[s][2] = o;
            y = o;
        }
        acc += y * y;
        if (++cnt == win) { out.push_back(10 * std::log10(acc / double(win) + 1e-20)); acc = 0; cnt = 0; }
    }
    return out;
}

// Ducking: over the last 8 s, how far the tone dips below its own median (95th percentile of the
// dips, dB). A steady tone through a clean gain reads 0.
double pumping(const std::vector<float>& L) {
    const auto lv = toneLevel(L);
    std::vector<double> tail(lv.end() - 800, lv.end());
    std::vector<double> s = tail;
    std::sort(s.begin(), s.end());
    const double med = s[s.size() / 2];
    std::vector<double> dips;
    for (double v : tail) dips.push_back(med - v);
    std::sort(dips.begin(), dips.end());
    return dips[size_t(0.95 * double(dips.size() - 1))];
}

struct Out { double pump, lufs, tp; };

Out run(const Sig& in, const Values& v, double measured, bool kneeKnown = true) {
    Engine e;
    e.prepare(kFs, 512);
    e.setMeasuredInput(measured, true);
    if (kneeKnown) e.setKnee(-8, true);
    Sig s = in;
    Transport t;
    t.playing = true; t.hasPpq = true; t.bpm = 120;
    for (size_t i = 0; i < s.L.size(); i += 512) {
        const int n = int(std::min<size_t>(512, s.L.size() - i));
        t.ppq = double(i) / kFs * 2.0;
        e.process(s.L.data() + i, s.R.data() + i, n, v, t);
    }
    return {pumping(s.L), e.passOutputLufs(), e.peakTpDb()};
}

double lufsOf(const Sig& s) {
    KeyStats ks;
    KLevel k;
    k.prepare(kFs);
    const int len = int(0.1 * kFs);
    double sub = 0, q[4]{};
    int n = 0, qi = 0;
    for (size_t i = 0; i < s.L.size(); ++i) {
        const double l = k.rlb[0].process(k.pre[0].process(s.L[i])), r = k.rlb[1].process(k.pre[1].process(s.R[i]));
        sub += l * l + r * r;
        if (++n == len) { q[qi & 3] = sub / len; if (++qi >= 4) ks.blocks.push_back(-0.691 + 10 * std::log10(0.25 * (q[0] + q[1] + q[2] + q[3]) + 1e-20)); sub = 0; n = 0; }
    }
    return ks.integrated();
}
}  // namespace

int main() {
    const Sig s = mix(20, 0.6, -26);
    const double in = lufsOf(s);
    std::printf("the mix: %.1f LUFS; the tone's own dips through nothing: %.2f dB\n\n", in, pumping(s.L));
    Values def;
    struct Case { const char* name; std::function<void(Values&)> set; };
    const std::vector<Case> cases = {
        {"opening settings", [](Values&) {}},
        {"GLUE, PUNCH, EVEN at 0", [](Values& v) { v[dGlue] = 0; v[dPunch] = 0; v[dEven] = 0; }},
        {"  and CLEAN out", [](Values& v) { v[dGlue] = 0; v[dPunch] = 0; v[dEven] = 0; v[cleanIn] = 0; }},
        {"  and TONE static", [](Values& v) { v[dGlue] = 0; v[dPunch] = 0; v[dEven] = 0; v[tMode] = 0; }},
        {"  and COLOUR out", [](Values& v) { v[dGlue] = 0; v[dPunch] = 0; v[dEven] = 0; v[colIn] = 0; }},
        {"  and LOUDNESS out", [](Values& v) { v[dGlue] = 0; v[dPunch] = 0; v[dEven] = 0; v[loudIn] = 0; }},
        {"only EVEN (2)", [](Values& v) { v[dGlue] = 0; v[dPunch] = 0; v[cleanIn] = 0; v[colIn] = 0; v[loudIn] = 0; }},
        {"only GLUE (3)", [](Values& v) { v[dPunch] = 0; v[dEven] = 0; v[cleanIn] = 0; v[colIn] = 0; v[loudIn] = 0; }},
        {"only PUNCH (0.5)", [](Values& v) { v[dGlue] = 0; v[dEven] = 0; v[cleanIn] = 0; v[colIn] = 0; v[loudIn] = 0; }},
        {"only CLEAN", [](Values& v) { v[dynIn] = 0; v[colIn] = 0; v[loudIn] = 0; }},
        {"only LOUDNESS at -13", [](Values& v) { v[dynIn] = 0; v[cleanIn] = 0; v[colIn] = 0; v[toneIn] = 0; }},
        {"only LOUDNESS at -10", [](Values& v) { v[dynIn] = 0; v[cleanIn] = 0; v[colIn] = 0; v[toneIn] = 0; v[lTarget] = -10; v[lGuard] = 0; }},
        {"only LOUDNESS at -10, PUNCHY", [](Values& v) { v[dynIn] = 0; v[cleanIn] = 0; v[colIn] = 0; v[toneIn] = 0; v[lTarget] = -10; v[lGuard] = 0; v[lStyle] = 1; }},
        {"only LOUDNESS at -10, DENSE", [](Values& v) { v[dynIn] = 0; v[cleanIn] = 0; v[colIn] = 0; v[toneIn] = 0; v[lTarget] = -10; v[lGuard] = 0; v[lStyle] = 2; }},
        {"only LOUDNESS at -10, clipper 0", [](Values& v) { v[dynIn] = 0; v[cleanIn] = 0; v[colIn] = 0; v[toneIn] = 0; v[lTarget] = -10; v[lGuard] = 0; v[lClip] = 0; }},
    };
    for (const auto& c : cases) {
        Values v = def;
        v[oDither] = 0;
        c.set(v);
        const Out o = run(s, v, in);
        std::printf("%-34s tone ducks %5.2f dB   (out %5.1f LUFS, %5.1f dBTP)\n", c.name, o.pump, o.lufs, o.tp);
    }
    return 0;
}
