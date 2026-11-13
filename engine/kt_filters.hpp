// Kontur: filter design.
//
// Every band starts as an analog prototype (a cascade of first- and second-order
// sections in s). Each section becomes a digital section whose poles are the
// analog poles mapped by z = exp(sT), and whose zeros are chosen so the
// magnitude equals the analog magnitude at DC, at Nyquist and at one frequency
// in between. That keeps the analog shape right up to the top of the band,
// where a bilinear-transform EQ squashes ("cramps") its curves.
// After Martin Vicanek, "Matched Second Order Digital Filters" (2016).
//
// Header-only, no dependencies, double precision.
#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <complex>
#include <vector>

namespace kt {

constexpr double kPi = 3.14159265358979323846;

enum class Shape { Bell, LowShelf, LowCut, HighShelf, HighCut, Notch, BandPass, TiltShelf, FlatTilt, AllPass };
constexpr int kNumShapes = 10;

// One analog section: (b2 s^2 + b1 s + b0) / (a2 s^2 + a1 s + a0), s in rad/s.
// a2 == 0 means a first-order section.
struct AnalogSection {
    double b2 = 0, b1 = 0, b0 = 1, a2 = 0, a1 = 0, a0 = 1;
    bool allpass = false;

    std::complex<double> at(double w) const {
        const std::complex<double> s(0.0, w);
        return (b2 * s * s + b1 * s + b0) / (a2 * s * s + a1 * s + a0);
    }
    double mag2(double w) const { return std::norm(at(w)); }
};

// One digital section: (b0 + b1 z^-1 + b2 z^-2) / (1 + a1 z^-1 + a2 z^-2).
struct Biquad {
    double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0;

    std::complex<double> at(double wT) const {
        const std::complex<double> z1 = std::polar(1.0, -wT), z2 = z1 * z1;
        return (b0 + b1 * z1 + b2 * z2) / (1.0 + a1 * z1 + a2 * z2);
    }
};

namespace detail {

// Minimum-phase numerator from its squared magnitude, written in the basis
// |N|^2 = B0 (1-x) + B1 x + 4 B2 x (1-x), x = sin^2(wT/2).
inline void factor(double B0, double B1, double B2, Biquad& q) {
    const double r0 = std::sqrt(std::max(0.0, B0)), r1 = std::sqrt(std::max(0.0, B1));
    const double W = 0.5 * (r0 + r1);
    q.b1 = 0.5 * (r0 - r1);
    q.b0 = 0.5 * (W + std::sqrt(std::max(0.0, W * W + B2)));
    q.b2 = q.b0 > 1e-300 ? -B2 / (4.0 * q.b0) : 0.0;
}

// Least squares for up to three unknowns by modified Gram-Schmidt (rows: a[k][i], target t[i]).
inline void solve(int n, int m, const double (*a)[64], const double* t, double* out) {
    double q[3][64], r[3][3] = {}, qt[3] = {};
    for (int k = 0; k < n; ++k) {
        for (int i = 0; i < m; ++i) q[k][i] = a[k][i];
        for (int j = 0; j < k; ++j) {
            double d = 0; for (int i = 0; i < m; ++i) d += q[j][i] * q[k][i];
            r[j][k] = d; for (int i = 0; i < m; ++i) q[k][i] -= d * q[j][i];
        }
        double nn = 0; for (int i = 0; i < m; ++i) nn += q[k][i] * q[k][i];
        nn = std::sqrt(nn); r[k][k] = nn;
        if (nn > 0) for (int i = 0; i < m; ++i) q[k][i] /= nn;
        double d = 0; for (int i = 0; i < m; ++i) d += q[k][i] * t[i];
        qt[k] = d;
    }
    for (int k = n - 1; k >= 0; --k) {
        double v = qt[k];
        for (int j = k + 1; j < n; ++j) v -= r[k][j] * out[j];
        out[k] = r[k][k] > 0 ? v / r[k][k] : 0.0;
    }
}

}  // namespace detail

// The analog section as a digital one at sample rate fs.
//
// Poles: the analog poles mapped by z = exp(sT). Zeros: whatever second-order numerator
// brings the magnitude closest (least squares, in relative terms, so in dB) to the analog
// magnitude from DC to Nyquist, weighted toward the audible band and toward the section's
// own corner. Zeros that belong at DC or on the unit circle are put exactly there.
inline Biquad matched(const AnalogSection& s, double fs) {
    using namespace detail;
    Biquad q;
    const double T = 1.0 / fs, wN = kPi * fs;
    const bool first = s.a2 == 0.0;
    double wp;
    if (first) {
        wp = s.a0 / s.a1;
        q.a1 = -std::exp(-std::min(wp * T, 40.0));
        q.a2 = 0.0;
        if (s.allpass) { q.b0 = q.a1; q.b1 = 1.0; q.b2 = 0.0; return q; }
    } else {
        wp = std::sqrt(s.a0 / s.a2);
        const double d = s.a1 / (2.0 * std::sqrt(s.a0 * s.a2));
        // Keep the pole angle below Nyquist; above that the mapping would alias back into the band.
        const double wd = std::min(wp * T, 0.985 * kPi);
        const double e = std::exp(-d * std::min(wp * T, 3.0 * kPi));   // a pole beyond Nyquist is damped as far as it is away
        q.a1 = d <= 1.0 ? -2.0 * e * std::cos(std::sqrt(1.0 - d * d) * wd)
                        : -2.0 * e * std::cosh(std::sqrt(d * d - 1.0) * wd);
        q.a2 = e * e;
        if (s.allpass) { q.b0 = q.a2; q.b1 = q.a1; q.b2 = 1.0; return q; }
    }
    const bool z2 = s.b2 != 0.0, z1 = s.b1 != 0.0, z0 = s.b0 != 0.0;
    if (z0 && (z2 || (first && z1))) {
        // Bell, shelf, notch: the zeros are mapped exactly like the poles, so sharp features
        // land where they belong, and the DC gain is set. What little is left to fix near
        // Nyquist is smooth, and the band's correction section (see design) takes care of it.
        double n1, n2;
        if (first) { n1 = -std::exp(-std::min(s.b0 / s.b1 * T, 40.0)); n2 = 0.0; }
        else {
            const double wzT = std::min(std::sqrt(s.b0 / s.b2) * T, 0.985 * kPi);
            const double dz = s.b1 / (2.0 * std::sqrt(s.b0 * s.b2));
            const double ez = std::exp(-dz * std::min(std::sqrt(s.b0 / s.b2) * T, 3.0 * kPi));
            n1 = dz <= 1.0 ? -2.0 * ez * std::cos(std::sqrt(1.0 - dz * dz) * wzT)
                           : -2.0 * ez * std::cosh(std::sqrt(dz * dz - 1.0) * wzT);
            n2 = ez * ez;
        }
        const double g = (s.b0 / s.a0) * (1.0 + q.a1 + q.a2) / (1.0 + n1 + n2);
        q.b0 = g; q.b1 = g * n1; q.b2 = g * n2;
        return q;
    }
    const bool notch = false;
    const double wz = 0.0;

    // Where to compare: a log grid from 10 Hz to just under Nyquist, plus points round the corners.
    double w[64], wt[64];
    int m = 0;
    const double fTop = 0.499 * fs, audible = std::min(20000.0, 0.47 * fs) * 2.0 * kPi;
    for (int i = 0; i < 40; ++i) w[m++] = 2.0 * kPi * 10.0 * std::pow(fTop / 10.0, i / 39.0);
    for (double c : {wp, wz})
        if (c > 0.0)
            for (double k : {0.8, 0.93, 1.0, 1.07, 1.25})
                if (c * k < 0.995 * wN) w[m++] = c * k;
    for (int i = 0; i < m; ++i) wt[i] = w[i] <= audible ? 1.0 : 0.15;
    if (z0) { w[m] = 0.0; wt[m] = 30.0; ++m; }           // hold the DC gain
    else    { w[m] = wN;  wt[m] = 0.3;  ++m; }

    double tgt[64], x[64], peak = 0.0;
    for (int i = 0; i < m; ++i) {
        const std::complex<double> z1c = std::polar(1.0, -w[i] * T);
        tgt[i] = s.mag2(w[i]) * std::norm(1.0 + q.a1 * z1c + q.a2 * z1c * z1c);
        x[i] = std::pow(std::sin(0.5 * w[i] * T), 2.0);
        peak = std::max(peak, tgt[i]);
    }
    // Basis for |N|^2 in x.
    double a[3][64] = {}, t[64] = {}, B[3] = {0, 0, 0};
    int n = 0;
    enum { Full, NoDc, DoubleDc, Circle } kind = Full;
    if (first) kind = z0 ? Full : NoDc;
    else if (notch) kind = Circle;
    else if (!z0 && z2) kind = DoubleDc;
    else if (!z0) kind = NoDc;
    const double c1 = notch ? -2.0 * std::cos(std::min(wz * T, 0.985 * kPi)) : 0.0;
    for (int i = 0; i < m; ++i) {
        // Relative error, but not chasing the curve below -60 dB of the section's peak.
        const double sc = std::sqrt(wt[i]) / (tgt[i] + 1e-6 * peak);
        const double f0 = 1.0 - x[i], f1 = x[i], f2 = 4.0 * x[i] * (1.0 - x[i]);
        t[i] = sc * tgt[i];
        switch (kind) {
            case Full:     a[0][i] = sc * f0; a[1][i] = sc * f1; if (!first) a[2][i] = sc * f2; n = first ? 2 : 3; break;
            case NoDc:     a[0][i] = sc * f1; if (!first) a[1][i] = sc * f2; n = first ? 1 : 2; break;
            case DoubleDc: a[0][i] = sc * 16.0 * x[i] * x[i]; n = 1; break;
            case Circle: {   // |1 + c1 z^-1 + z^-2|^2 = (2 cos wT + c1)^2
                const double v = 2.0 * std::cos(w[i] * T) + c1;
                a[0][i] = sc * v * v; n = 1; break;
            }
        }
    }
    solve(n, m, a, t, B);
    switch (kind) {
        case Full:
            if (first) { const double r0 = std::sqrt(std::max(0.0, B[0])), r1 = std::sqrt(std::max(0.0, B[1])); q.b0 = 0.5 * (r0 + r1); q.b1 = 0.5 * (r0 - r1); q.b2 = 0.0; }
            else factor(B[0], B[1], B[2], q);
            break;
        case NoDc:
            if (first) { const double r1 = std::sqrt(std::max(0.0, B[0])); q.b0 = 0.5 * r1; q.b1 = -0.5 * r1; q.b2 = 0.0; }
            else factor(0.0, B[0], B[1], q);
            break;
        case DoubleDc: { const double c = std::sqrt(std::max(0.0, B[0])); q.b0 = c; q.b1 = -2.0 * c; q.b2 = c; break; }
        case Circle:   { const double c = std::sqrt(std::max(0.0, B[0])); q.b0 = c; q.b1 = c * c1; q.b2 = c; break; }
    }
    return q;
}

// ---------------------------------------------------------------------------
// A band's settings and its analog prototype.
struct BandSpec {
    Shape shape = Shape::Bell;
    double freq = 1000.0;   // Hz
    double gainDb = 0.0;    // bell, shelves, tilts
    double q = 1.0;         // bell, notch, band pass, all pass; resonance of shelves and cuts
    int order = 2;          // cuts and shelves: 1..16 (6..96 dB/oct); others ignore it
};

struct Prototype {
    std::vector<AnalogSection> sections;
    double gain = 1.0;      // a flat gain on top (flat tilt)

    std::complex<double> at(double f) const {
        std::complex<double> h(gain, 0.0);
        for (const auto& s : sections) h *= s.at(2.0 * kPi * f);
        return h;
    }
    double magDb(double f) const {
        double m2 = gain * gain;
        for (const auto& s : sections) m2 *= s.mag2(2.0 * kPi * f);
        return 10.0 * std::log10(std::max(m2, 1e-300));
    }
};

namespace detail {

inline AnalogSection lowShelf2(double w0, double g, double Q) {   // g: linear gain at DC
    const double A = std::sqrt(g), k = std::sqrt(A) / Q;
    return {A / (w0 * w0), A * k / w0, A * A, A / (w0 * w0), k / w0, 1.0};
}
inline AnalogSection highShelf2(double w0, double g, double Q) {  // g: linear gain at the top
    const double A = std::sqrt(g), k = std::sqrt(A) / Q;
    return {A * A / (w0 * w0), A * k / w0, A, 1.0 / (w0 * w0), k / w0, A};
}
inline AnalogSection lowShelf1(double w0, double g) {
    const double r = std::sqrt(g);
    return {0, 1.0, w0 * r, 0, 1.0, w0 / r};
}
inline AnalogSection highShelf1(double w0, double g) {
    const double r = std::sqrt(g);
    return {0, r, w0, 0, 1.0 / r, w0};
}
// Butterworth section Qs for an order-n filter; `first` says whether there is a first-order section too.
inline std::vector<double> butterQs(int n, bool& first) {
    std::vector<double> qs;
    for (int k = 0; k < n / 2; ++k) qs.push_back(1.0 / (2.0 * std::sin(kPi * (2 * k + 1) / (2.0 * n))));
    first = (n % 2) == 1;
    return qs;
}

}  // namespace detail

inline Prototype prototype(const BandSpec& b) {
    using namespace detail;
    Prototype p;
    const double w0 = 2.0 * kPi * b.freq;
    const double g = std::pow(10.0, b.gainDb / 20.0);
    const double Q = std::max(b.q, 0.01);
    const int n = std::clamp(b.order, 1, 16);
    switch (b.shape) {
        case Shape::Bell: {
            const double A = std::sqrt(g);
            p.sections.push_back({1.0 / (w0 * w0), A / (Q * w0), 1.0, 1.0 / (w0 * w0), 1.0 / (A * Q * w0), 1.0});
            break;
        }
        case Shape::Notch:
            p.sections.push_back({1.0 / (w0 * w0), 0.0, 1.0, 1.0 / (w0 * w0), 1.0 / (Q * w0), 1.0});
            break;
        case Shape::BandPass:
            p.sections.push_back({0.0, 1.0 / (Q * w0), 0.0, 1.0 / (w0 * w0), 1.0 / (Q * w0), 1.0});
            break;
        case Shape::AllPass: {
            AnalogSection s{1.0 / (w0 * w0), -1.0 / (Q * w0), 1.0, 1.0 / (w0 * w0), 1.0 / (Q * w0), 1.0};
            s.allpass = true;
            p.sections.push_back(s);
            break;
        }
        case Shape::LowCut:
        case Shape::HighCut: {
            bool first = false;
            auto qs = butterQs(n, first);
            // The Q control sets the resonance at the corner: 0.71 is maximally flat.
            if (!qs.empty()) qs[0] *= Q / std::sqrt(0.5);
            const bool lc = b.shape == Shape::LowCut;
            for (double qk : qs)
                p.sections.push_back(lc ? AnalogSection{1.0 / (w0 * w0), 0, 0, 1.0 / (w0 * w0), 1.0 / (qk * w0), 1.0}
                                        : AnalogSection{0, 0, 1.0, 1.0 / (w0 * w0), 1.0 / (qk * w0), 1.0});
            if (first)
                p.sections.push_back(lc ? AnalogSection{0, 1.0 / w0, 0, 0, 1.0 / w0, 1.0}
                                        : AnalogSection{0, 0, 1.0, 0, 1.0 / w0, 1.0});
            break;
        }
        case Shape::LowShelf:
        case Shape::HighShelf: {
            bool first = false;
            auto qs = butterQs(n, first);
            if (!qs.empty()) qs[0] *= Q / std::sqrt(0.5);
            const bool ls = b.shape == Shape::LowShelf;
            const double g2 = std::pow(g, 2.0 / n), g1 = std::pow(g, 1.0 / n);
            for (double qk : qs) p.sections.push_back(ls ? lowShelf2(w0, g2, qk) : highShelf2(w0, g2, qk));
            if (first) p.sections.push_back(ls ? lowShelf1(w0, g1) : highShelf1(w0, g1));
            break;
        }
        case Shape::TiltShelf: {
            // The lows go down by half the gain and the highs up by half, pivoting at the frequency.
            const double h = std::sqrt(g);
            p.sections.push_back(lowShelf2(w0, 1.0 / h, Q));
            p.sections.push_back(highShelf2(w0, h, Q));
            break;
        }
        case Shape::FlatTilt: {
            // A straight line on a log axis: gain/10 dB per octave, 0 dB at the frequency.
            // Seven first-order shelves two octaves apart, each worth two octaves of tilt.
            const double perOct = b.gainDb / 10.0;
            const double gs = std::pow(10.0, 2.0 * perOct / 20.0);
            for (int k = -3; k <= 3; ++k) p.sections.push_back(highShelf1(w0 * std::pow(4.0, k), gs));
            p.gain = std::pow(10.0, -7.0 * perOct / 20.0);
            break;
        }
    }
    return p;
}

// The digital filter for a band.
struct Design {
    std::vector<Biquad> sections;
    double gain = 1.0;

    std::complex<double> at(double f, double fs) const {
        std::complex<double> h(gain, 0.0);
        for (const auto& q : sections) h *= q.at(2.0 * kPi * f / fs);
        return h;
    }
    double magDb(double f, double fs) const { return 20.0 * std::log10(std::max(std::abs(at(f, fs)), 1e-150)); }
};

// A last section for the band that takes up what the others leave near Nyquist: a smooth
// second-order correction whose numerator is fitted (least squares, in relative terms) to the
// ratio of the analog curve to the digital one. Returns false if there is nothing worth fixing.
inline bool correction(const Prototype& p, const std::vector<Biquad>& secs, double fs, Biquad& out) {
    using namespace detail;
    const double T = 1.0 / fs, audible = std::min(20000.0, 0.47 * fs);
    double f[64], want[64], wt[64], peak = 0.0, worstDb = 0.0;
    int m = 0;
    for (int i = 0; i < 48; ++i) {
        const double fi = 20.0 * std::pow(0.499 * fs / 20.0, i / 47.0);
        double an = 1.0, dg = 1.0;
        for (const auto& s : p.sections) an *= s.mag2(2.0 * kPi * fi);
        for (const auto& q : secs) dg *= std::norm(q.at(2.0 * kPi * fi * T));
        if (!(dg > 1e-30) || !(an > 1e-30)) continue;
        f[m] = fi; want[m] = an / dg; wt[m] = an; peak = std::max(peak, an); ++m;
    }
    for (int i = 0; i < m; ++i) {
        // Count a point for less the further the curve itself is below its peak (inaudible there),
        // and for much less above the audible band.
        if (wt[i] < 1e-6 * peak) { want[i] = 1.0; wt[i] = 0.02; continue; }   // inaudible there: just stay near unity
        if (f[i] < 0.02 * fs) { want[i] = 1.0; wt[i] = 1.0; continue; }        // far below Nyquist there is nothing to fix
        wt[i] = (wt[i] / (wt[i] + 1e-5 * peak)) * (f[i] <= audible ? 1.0 : 0.02);
        if (f[i] <= audible) worstDb = std::max(worstDb, wt[i] * std::fabs(10.0 * std::log10(want[i])));
    }
    if (worstDb < 0.003 || m < 8) return false;
    f[m] = 0.0; want[m] = 1.0; wt[m] = 50.0; ++m;      // leave DC alone
    double best = 1e300;
    for (double rho : {0.0, 0.25, 0.5, 0.7, 0.85}) {    // the correction's (fixed, real, double) pole at z = -rho
        double a[3][64] = {}, t[64] = {}, B[3] = {0, 0, 0};
        for (int i = 0; i < m; ++i) {
            const double x = std::pow(std::sin(kPi * f[i] * T), 2.0);
            const double den = std::pow(std::norm(1.0 + rho * std::polar(1.0, -2.0 * kPi * f[i] * T)), 2.0);
            const double tg = want[i] * den, sc = std::sqrt(wt[i]) / tg;
            a[0][i] = sc * (1 - x); a[1][i] = sc * x; a[2][i] = sc * 4.0 * x * (1 - x); t[i] = sc * tg;
        }
        solve(3, m, a, t, B);
        Biquad q; factor(B[0], B[1], B[2], q);
        q.a1 = 2.0 * rho; q.a2 = rho * rho;
        double err = 0.0;
        for (int i = 0; i < m; ++i) {
            const double e = 10.0 * std::log10(std::max(std::norm(q.at(2.0 * kPi * f[i] * T)), 1e-30) / want[i]);
            err += wt[i] * e * e;
        }
        if (err < best) { best = err; out = q; }
    }
    return true;
}

inline Design design(const BandSpec& b, double fs) {
    const Prototype p = prototype(b);
    Design d;
    d.gain = p.gain;
    d.sections.reserve(p.sections.size() + 1);
    for (const auto& s : p.sections) d.sections.push_back(matched(s, fs));
    if (b.shape != Shape::AllPass) {
        Biquad c;
        if (correction(p, d.sections, fs, c)) d.sections.push_back(c);
    }
    return d;
}

}  // namespace kt
