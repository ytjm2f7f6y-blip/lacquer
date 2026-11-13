#pragma once
// A synthetic song for the tests: kick, snare, hats, bass and a pad, in 4/4,
// with quiet sections (sparse, narrower) and loud ones (busier, wider).
// Deterministic, so every run hears the same thing.

#include <cmath>
#include <cstdint>
#include <utility>
#include <vector>

namespace testsig {

struct Song {
    std::vector<float> L, R;
    double fs = 48000, bpm = 120;
    int bars = 0;
    double secondsPerBar() const { return 240.0 / bpm; }
};

struct Noise {
    uint64_t s = 0x2545F4914F6CDD1Dull;
    double next() { s ^= s << 13; s ^= s >> 7; s ^= s << 17; return double(s >> 11) * (2.0 / 9007199254740992.0) - 1.0; }
};

// sections: (bars, loud?)
inline Song song(double fs, double bpm, const std::vector<std::pair<int, bool>>& sections, double gain = 1.0) {
    Song out;
    out.fs = fs; out.bpm = bpm;
    for (const auto& s : sections) out.bars += s.first;
    const double bar = 240.0 / bpm, beat = 60.0 / bpm;
    const size_t total = size_t(out.bars * bar * fs);
    out.L.assign(total, 0.0f);
    out.R.assign(total, 0.0f);
    Noise nz;
    const double pi = 3.14159265358979323846;
    static const double roots[4] = {55.0, 43.65, 65.41, 49.0};   // A, F, C, G
    auto addMono = [&](size_t at, const std::vector<double>& v, double pan) {
        const double gl = std::cos((pan + 1) * pi / 4), gr = std::sin((pan + 1) * pi / 4);
        for (size_t i = 0; i < v.size() && at + i < total; ++i) { out.L[at + i] += float(v[i] * gl); out.R[at + i] += float(v[i] * gr); }
    };
    int barIndex = 0;
    for (const auto& sec : sections) {
        const bool loud = sec.second;
        for (int b = 0; b < sec.first; ++b, ++barIndex) {
            const size_t b0 = size_t(barIndex * bar * fs);
            const double root = roots[barIndex % 4];
            // Kick.
            for (int k = 0; k < 4; ++k) {
                if (!loud && (k % 2)) continue;
                std::vector<double> v(size_t(0.3 * fs));
                double ph = 0;
                for (size_t i = 0; i < v.size(); ++i) {
                    const double t = i / fs;
                    ph += 2 * pi * (45 + 75 * std::exp(-t * 30)) / fs;
                    v[i] = std::sin(ph) * std::exp(-t * 9) * (loud ? 0.55 : 0.4);
                }
                addMono(b0 + size_t(k * beat * fs), v, 0);
            }
            // Snare on 2 and 4.
            for (int k : {1, 3}) {
                std::vector<double> v(size_t(0.25 * fs));
                double lp = 0;
                for (size_t i = 0; i < v.size(); ++i) {
                    const double t = i / fs;
                    lp += 0.35 * (nz.next() - lp);
                    v[i] = (lp * 0.8 + 0.4 * std::sin(2 * pi * 185 * t)) * std::exp(-t * 18) * 0.32;
                }
                addMono(b0 + size_t(k * beat * fs), v, 0.05);
            }
            // Hats: eighths (sixteenths when loud).
            const int hats = loud ? 16 : 8;
            for (int k = 0; k < hats; ++k) {
                std::vector<double> v(size_t(0.05 * fs));
                double prev = 0;
                for (size_t i = 0; i < v.size(); ++i) {
                    const double x = nz.next();
                    v[i] = (x - prev) * std::exp(-double(i) / fs * 70) * (loud ? 0.09 : 0.06);
                    prev = x;
                }
                addMono(b0 + size_t(k * bar / hats * fs), v, (k % 2 ? 0.6 : -0.6) * (loud ? 1.0 : 0.5));
            }
            // Bass: eighth notes, a filtered saw.
            for (int k = 0; k < 8; ++k) {
                std::vector<double> v(size_t(beat / 2 * fs * 0.9));
                double ph = 0, lp = 0;
                const double f = root * (k == 7 ? 1.5 : 1.0);
                for (size_t i = 0; i < v.size(); ++i) {
                    ph += f / fs; if (ph >= 1) ph -= 1;
                    lp += 0.06 * ((2 * ph - 1) - lp);
                    const double env = std::min(1.0, i / (0.004 * fs)) * std::exp(-double(i) / fs * 3);
                    v[i] = lp * env * (loud ? 0.32 : 0.2);
                }
                addMono(b0 + size_t(k * beat / 2 * fs), v, 0);
            }
            // Pad: three detuned saws per chord note, spread, quieter when not loud.
            const double chord[3] = {root * 4, root * 4 * 1.25992, root * 4 * 1.49831};
            const size_t len = size_t(bar * fs);
            std::vector<double> pl(len, 0.0), pr(len, 0.0);
            for (int nidx = 0; nidx < 3; ++nidx)
                for (int d = -1; d <= 1; ++d) {
                    const double f = chord[nidx] * (1 + 0.004 * d);
                    double ph = 0.31 * (nidx * 3 + d + 1), lp = 0;
                    const double pan = 0.7 * d * (loud ? 1.0 : 0.4);
                    const double gl = std::cos((pan + 1) * pi / 4), gr = std::sin((pan + 1) * pi / 4);
                    for (size_t i = 0; i < len; ++i) {
                        ph += f / fs; if (ph >= 1) ph -= 1;
                        lp += 0.12 * ((2 * ph - 1) - lp);
                        const double env = std::min(1.0, i / (0.05 * fs)) * std::min(1.0, (len - i) / (0.05 * fs));
                        const double v = lp * env * (loud ? 0.035 : 0.018);
                        pl[i] += v * gl; pr[i] += v * gr;
                    }
                }
            for (size_t i = 0; i < len && b0 + i < total; ++i) { out.L[b0 + i] += float(pl[i]); out.R[b0 + i] += float(pr[i]); }
        }
    }
    for (size_t i = 0; i < total; ++i) { out.L[i] *= float(gain); out.R[i] *= float(gain); }
    return out;
}

}  // namespace testsig
