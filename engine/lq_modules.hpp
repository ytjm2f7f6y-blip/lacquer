#pragma once
// Lacquer's own processing: clean, tone, punch, even, iron, the loudness stage and the listening aids.
// Everything runs in sub-blocks of at most lp::kSub samples, with settings ramped across each one.
// The modules here add no latency; the ones borrowed from long player keep theirs constant.

#include "lp_modules.hpp"

#include <array>
#include <complex>

namespace lq {

using lp::Biquad;
using lp::clamp;
using lp::dbToGain;
using lp::gainToDb;
using lp::kSub;
using lp::Ramp;
using lp::tcCoef;
constexpr double kPi = lp::kPi;

// A band-pass with 0 dB at its centre. dry + (g - 1) * bp is an exact peaking band
// (Regalia-Mitra), so the gain can move every sample without touching the filter.
inline void bandpass0(Biquad& b, double f, double q, double fs) {
    const double w = 2 * kPi * clamp(f, 10.0, fs * 0.45) / fs, al = std::sin(w) / (2 * q), c = std::cos(w);
    b.set(al, 0, -al, 1 + al, -2 * c, 1 - al);
}

// First-order low and high pass (bilinear, prewarped): dry + (g - 1) * lp is a gentle shelf.
struct FirstOrder {
    double b0 = 1, b1 = 0, a1 = 0, z = 0;
    void lowpass(double f, double fs) {
        const double k = std::tan(kPi * clamp(f, 5.0, fs * 0.45) / fs);
        b0 = b1 = k / (1 + k); a1 = (k - 1) / (k + 1);
    }
    void highpass(double f, double fs) {
        const double k = std::tan(kPi * clamp(f, 5.0, fs * 0.45) / fs);
        b0 = 1 / (1 + k); b1 = -b0; a1 = (k - 1) / (k + 1);
    }
    inline double process(double x) { const double y = b0 * x + z; z = b1 * x - a1 * y; return y; }
    void reset() { z = 0; }
};

// An envelope in dB: fast attack, slower release, from a mean square.
struct EnvDb {
    double a = 0, r = 0, ms = 0;
    void set(double attackS, double releaseS, double fs) { a = tcCoef(attackS, fs); r = tcCoef(releaseS, fs); }
    inline double push(double x2) { ms = x2 > ms ? x2 + (ms - x2) * a : x2 + (ms - x2) * r; return 10 * std::log10(ms + 1e-20); }
    void reset(double v = 0) { ms = v; }
};

// ---------------------------------------------------------------------------
// CLEAN: rumble cut, a harshness cut that only works when it has to, and a resonance tamer.
struct CleanParams { float res = 0, harsh = 0, rumbleHz = 0, weight = 0; bool removed = false; };

class Clean {
public:
    static constexpr int kBands = 18;   // third-octave resonance bands, 200 Hz to 10 kHz
    void prepare(double fs) {
        fs_ = fs;
        for (int b = 0; b < kBands; ++b) {
            fc_[b] = 200.0 * std::pow(2.0, b / 3.0);
            bandpass0(det_[b], fc_[b], 4.3, fs);
            for (int c = 0; c < 2; ++c) bandpass0(app_[b][c], fc_[b], 4.3, fs);
            fast_[b].set(0.004, 0.06, fs);
            slow_[b].set(0.25, 0.25, fs);
        }
        bandpass0(hDet_, 3600, 0.9, fs);
        for (int c = 0; c < 2; ++c) bandpass0(hApp_[c], 3200, 1.0, fs);
        hFast_.set(0.005, 0.08, fs);
        hSlow_.set(1.5, 1.5, fs);
        wide_.set(0.005, 0.08, fs);
        gSmooth_ = tcCoef(0.02, fs);
        reset();
    }
    void reset() {
        for (int b = 0; b < kBands; ++b) { det_[b].reset(); for (auto& a : app_[b]) a.reset(); fast_[b].reset(); slow_[b].reset(); gain_[b] = 1; }
        hDet_.reset(); for (auto& a : hApp_) a.reset(); hFast_.reset(); hSlow_.reset(); wide_.reset(); hGain_ = 1;
        for (auto& c : hp_) for (auto& s : c) s.reset();
        lastHp_ = -1; wR_.reset();
    }
    float harshCut() const { return float(gainToDb(hGain_)); }   // dB, for the meters and checks

    void process(float* L, float* R, int n, const CleanParams& p) {
        float w[kSub];
        wR_.set(p.weight, n, w);
        if (std::abs(p.rumbleHz - lastHp_) > 0.01f) {
            lastHp_ = p.rumbleHz;
            for (int c = 0; c < 2; ++c) {
                if (p.rumbleHz > 1) { hp_[c][0].highpass(p.rumbleHz, 0.7071, fs_); }
                else hp_[c][0].setIdentity();
            }
        }
        const double resDepth = p.res / 10.0, harshDepth = p.harsh / 10.0;
        for (int i = 0; i < n; ++i) {
            const double l = L[i], r = R[i];
            double x[2] = {hp_[0][0].process(l), hp_[1][0].process(r)};
            const double m = 0.5 * (x[0] + x[1]);
            // Harshness: the 2-6 kHz region against the whole, against its own usual level.
            const double wide = wide_.push(m * m);
            const double hb = hDet_.process(m);
            const double rel = hFast_.push(hb * hb) - wide;
            const double usual = hSlow_.push(std::pow(10.0, rel / 10.0));
            const double over = rel - usual - (6.0 - 4.0 * harshDepth);
            const double hg = over > 0 ? dbToGain(-std::min(10.0 * harshDepth + 0.5, over * (0.4 + 0.6 * harshDepth))) : 1.0;
            hGain_ = hg + (hGain_ - hg) * gSmooth_;
            // Resonances: a band standing above its neighbours, against how it usually stands.
            double lev[kBands];
            if (resDepth > 0.001) {
                for (int b = 0; b < kBands; ++b) { const double d = det_[b].process(m); lev[b] = fast_[b].push(d * d); }
                for (int b = 0; b < kBands; ++b) {
                    const double nb = 0.5 * (lev[b > 0 ? b - 1 : b + 1] + lev[b < kBands - 1 ? b + 1 : b - 1]);
                    const double stand = lev[b] - nb;
                    const double usualStand = 10 * std::log10(slow_[b].push(std::pow(10.0, clamp(stand, -40.0, 40.0) / 10.0)) + 1e-20);
                    const double ex = stand - std::max(usualStand, 0.0) - (5.0 - 3.0 * resDepth);
                    const double g = ex > 0 ? dbToGain(-std::min(10.0 * resDepth, ex * 0.8 * resDepth + 0.2)) : 1.0;
                    gain_[b] = g + (gain_[b] - g) * gSmooth_;
                }
            }
            for (int c = 0; c < 2; ++c) {
                double y = x[c];
                if (resDepth > 0.001)
                    for (int b = 0; b < kBands; ++b) y += (gain_[b] - 1.0) * app_[b][c].process(y);
                y += (hGain_ - 1.0) * hApp_[c].process(y);
                const double in = c == 0 ? l : r;
                const double out = in + w[i] * (y - in);
                const double o = p.removed ? (in - out) : out;
                (c == 0 ? L : R)[i] = float(o);
            }
        }
    }

private:
    double fs_ = 48000;
    double fc_[kBands]{};
    Biquad det_[kBands], app_[kBands][2], hDet_, hApp_[2], hp_[2][1];
    EnvDb fast_[kBands], slow_[kBands], hFast_, hSlow_, wide_;
    double gain_[kBands]{}, hGain_ = 1, gSmooth_ = 0;
    float lastHp_ = -1;
    Ramp wR_;
};

// ---------------------------------------------------------------------------
// TONE: four bands (a low shelf, a mud bell, a presence bell, an air shelf), each at its own
// frequency and each working on the stereo signal, the mid or the side. In dynamic mode each one
// works only as hard as the music needs it: a boost backs off while its band runs hot, a cut leans in.
// Every band is dry + (g - 1) * filter, so its gain can move every sample without touching the filter.
constexpr int kToneBands = 4;
struct ToneBand { float db = 0, hz = 1000, place = 0; };   // place 0 stereo, 1 mid, 2 side (in between while gliding)
struct ToneParams { ToneBand b[kToneBands]; float dynamic = 0, weight = 0; };

inline double toneQ(int band) { return band == 1 ? 1.0 : 0.7; }
// How much of a band reaches the mid and the side, from its placement.
inline double placeMid(double p) { return p <= 1 ? 1.0 : clamp(2.0 - p, 0.0, 1.0); }
inline double placeSide(double p) { return p <= 1 ? clamp(1.0 - p, 0.0, 1.0) : clamp(p - 1.0, 0.0, 1.0); }

// The filter a band adds in (F in dry + (g - 1) * F), as a response at frequency f.
inline std::complex<double> toneFilterAt(int band, double hz, double f, double fs) {
    const std::complex<double> z1 = std::polar(1.0, -2 * kPi * f / fs);   // z^-1
    if (band == 0 || band == 3) {
        const double k = std::tan(kPi * clamp(hz, 5.0, fs * 0.45) / fs);
        if (band == 0) { const double b0 = k / (1 + k), a1 = (k - 1) / (k + 1); return b0 * (1.0 + z1) / (1.0 + a1 * z1); }
        const double b0 = 1 / (1 + k), a1 = (k - 1) / (k + 1);
        return b0 * (1.0 - z1) / (1.0 + a1 * z1);
    }
    const double w = 2 * kPi * clamp(hz, 10.0, fs * 0.45) / fs, al = std::sin(w) / (2 * toneQ(band)), c = std::cos(w);
    return (al - al * z1 * z1) / ((1 + al) - 2 * c * z1 + (1 - al) * z1 * z1);
}
// The whole of TONE (static, as set) on the mid and the side at frequency f.
inline void toneResponseMS(const ToneParams& p, double f, double fs, std::complex<double>& hm, std::complex<double>& hs) {
    hm = hs = 1.0;
    for (int b = 0; b < kToneBands; ++b) {
        const double g = dbToGain(p.b[b].db) - 1.0;
        const std::complex<double> F = toneFilterAt(b, p.b[b].hz, f, fs);
        hm *= 1.0 + placeMid(p.b[b].place) * g * F;
        hs *= 1.0 + placeSide(p.b[b].place) * g * F;
    }
    hm = 1.0 + double(p.weight) * (hm - 1.0);
    hs = 1.0 + double(p.weight) * (hs - 1.0);
}

class Tone {
public:
    void prepare(double fs) {
        fs_ = fs;
        for (auto& f : hz_) f = -1;
        for (auto& e : fast_) e.set(0.03, 0.3, fs);
        for (auto& e : slow_) e.set(4.0, 4.0, fs);
        k_ = tcCoef(0.08, fs);
        reset();
    }
    void reset() {
        for (int b = 0; b < kToneBands; ++b) { for (int c = 0; c < 2; ++c) { fo_[b][c].reset(); bq_[b][c].reset(); } dFo_[b].reset(); dBq_[b].reset(); }
        for (auto& e : fast_) e.reset();
        for (auto& e : slow_) e.reset();
        for (auto& f : fac_) f = 1;
        for (auto& r : gR_) r.reset();
        for (auto& r : mR_) r.reset();
        for (auto& r : sR_) r.reset();
        wR_.reset();
    }
    void process(float* L, float* R, int n, const ToneParams& p) {
        for (int b = 0; b < kToneBands; ++b) tune(b, p.b[b].hz);
        float w[kSub], g[kToneBands][kSub], wm[kToneBands][kSub], ws[kToneBands][kSub];
        wR_.set(p.weight, n, w);
        // Dynamic factors from each band's level against its own usual level (on the signal it works on).
        for (int i = 0; i < n; ++i) {
            const double m = 0.5 * (L[i] + R[i]), sd = 0.5 * (L[i] - R[i]);
            for (int b = 0; b < kToneBands; ++b) {
                const double x = placeMid(p.b[b].place) > 0.5 ? m : sd;
                const double d = isShelf(b) ? dFo_[b].process(x) : dBq_[b].process(x);
                const double f = fast_[b].push(d * d), u = slow_[b].push(d * d);
                const double hot = clamp(f - u, -12.0, 12.0);
                double fac = p.b[b].db >= 0 ? 1.0 - hot / 8.0 : 1.0 + hot / 8.0;
                fac = clamp(fac, 0.25, 1.5);
                fac_[b] = fac + (fac_[b] - fac) * k_;
            }
        }
        for (int b = 0; b < kToneBands; ++b) {
            const double eff = p.b[b].db * (1.0 + p.dynamic * (fac_[b] - 1.0));
            gR_[b].set(float(dbToGain(eff) - 1.0), n, g[b]);
            mR_[b].set(float(placeMid(p.b[b].place)), n, wm[b]);
            sR_[b].set(float(placeSide(p.b[b].place)), n, ws[b]);
        }
        for (int i = 0; i < n; ++i) {
            double ch[2] = {0.5 * (L[i] + R[i]), 0.5 * (L[i] - R[i])};
            for (int c = 0; c < 2; ++c) {
                double y = ch[c];
                for (int b = 0; b < kToneBands; ++b) {
                    const double f = isShelf(b) ? fo_[b][c].process(y) : bq_[b][c].process(y);
                    y += double(g[b][i]) * double(c == 0 ? wm[b][i] : ws[b][i]) * f;
                }
                ch[c] = y;
            }
            const double yl = ch[0] + ch[1], yr = ch[0] - ch[1];
            L[i] = float(L[i] + w[i] * (yl - L[i]));
            R[i] = float(R[i] + w[i] * (yr - R[i]));
        }
    }

private:
    static bool isShelf(int b) { return b == 0 || b == 3; }
    void tune(int b, double hz) {
        if (hz_[b] > 0 && std::abs(hz - hz_[b]) < 1e-4 * hz_[b]) return;
        hz_[b] = hz;
        for (int c = 0; c < 2; ++c) {
            if (b == 0) fo_[b][c].lowpass(hz, fs_);
            else if (b == 3) fo_[b][c].highpass(hz, fs_);
            else bandpass0(bq_[b][c], hz, toneQ(b), fs_);
        }
        if (b == 0) dFo_[b].lowpass(hz, fs_);
        else if (b == 3) dFo_[b].highpass(hz, fs_);
        else bandpass0(dBq_[b], hz, toneQ(b), fs_);
    }

    double fs_ = 48000, k_ = 0;
    double hz_[kToneBands]{};
    FirstOrder fo_[kToneBands][2], dFo_[kToneBands];
    Biquad bq_[kToneBands][2], dBq_[kToneBands];
    EnvDb fast_[kToneBands], slow_[kToneBands];
    double fac_[kToneBands]{1, 1, 1, 1};
    Ramp gR_[kToneBands], mR_[kToneBands], sR_[kToneBands], wR_;
};

// ---------------------------------------------------------------------------
// PUNCH: a transient shaper. A fast envelope over a slow one marks the hits; they are lifted or
// rounded off by up to 6 dB, and the long-term level is left where it was.
class Punch {
public:
    void prepare(double fs) { fast_.set(0.0008, 0.03, fs); slow_.set(0.025, 0.2, fs); k_ = tcCoef(0.002, fs); reset(); }
    void reset() { fast_.reset(); slow_.reset(); g_ = 0; aR_.reset(); }
    void process(float* L, float* R, int n, float amount /* -5..5 */, float weight) {
        float a[kSub];
        aR_.set(amount * weight, n, a);
        for (int i = 0; i < n; ++i) {
            const double m = std::max(std::abs(L[i]), std::abs(R[i]));
            const double f = fast_.push(m * m), s = slow_.push(m * m);
            const double tr = clamp(f - s, 0.0, 12.0);
            const double gdb = clamp(a[i] * 0.32 * tr, -9.0, 9.0);
            g_ = gdb + (g_ - gdb) * k_;
            const double g = dbToGain(g_);
            L[i] = float(L[i] * g);
            R[i] = float(R[i] * g);
        }
    }
private:
    EnvDb fast_, slow_;
    double g_ = 0, k_ = 0;
    Ramp aR_;
};

// ---------------------------------------------------------------------------
// EVEN: three bands (LR4 at 200 Hz and 3 kHz) held near their usual share of the whole, so the
// balance stays put from section to section. Gentle: at most 4 dB either way.
class Even {
public:
    void prepare(double fs) {
        for (int c = 0; c < 2; ++c) { x1_[c].set(200, fs); x2_[c].set(3000, fs); ap_[c].set(3000, fs); }
        for (int b = 0; b < 3; ++b) { short_[b].set(0.15, 0.3, fs); long_[b].set(8, 8, fs); }
        all_.set(0.15, 0.3, fs); allLong_.set(8, 8, fs);
        k_ = tcCoef(0.05, fs);
        reset();
    }
    void reset() {
        for (int c = 0; c < 2; ++c) { x1_[c].reset(); x2_[c].reset(); ap_[c].reset(); }
        for (int b = 0; b < 3; ++b) { short_[b].reset(); long_[b].reset(); g_[b] = 0; }
        all_.reset(); allLong_.reset(); aR_.reset();
    }
    void process(float* L, float* R, int n, float even /* 0..10 */, float weight) {
        float a[kSub];
        aR_.set(even / 10.0f * weight, n, a);
        for (int i = 0; i < n; ++i) {
            double band[2][3];
            for (int c = 0; c < 2; ++c) {
                const double x = c == 0 ? L[i] : R[i];
                double lo, rest, mid, hi;
                x1_[c].split(x, lo, rest);
                x2_[c].split(rest, mid, hi);
                double a1, a2;
                ap_[c].split(lo, a1, a2);   // the low band through the same 3 kHz all-pass, so the sum stays flat
                band[c][0] = a1 + a2; band[c][1] = mid; band[c][2] = hi;
            }
            double tot = 0;
            for (int b = 0; b < 3; ++b) {
                const double e = 0.5 * (band[0][b] * band[0][b] + band[1][b] * band[1][b]);
                tot += e;
                const double s = short_[b].push(e), l = long_[b].push(e);
                rel_[b][0] = s; rel_[b][1] = l;
            }
            const double ts = all_.push(tot), tl = allLong_.push(tot);
            for (int b = 0; b < 3; ++b) {
                const double dev = (rel_[b][0] - ts) - (rel_[b][1] - tl);
                const double gdb = clamp(-dev * 0.8 * a[i], -8.0, 8.0);
                g_[b] = gdb + (g_[b] - gdb) * k_;
            }
            for (int c = 0; c < 2; ++c) {
                double y = 0;
                for (int b = 0; b < 3; ++b) y += band[c][b] * dbToGain(g_[b]);
                (c == 0 ? L : R)[i] = float(y);
            }
        }
    }
private:
    struct Split {   // LR4: low and high that sum to an all-pass
        Biquad l1, l2, h1, h2;
        void set(double f, double fs) { l1.lowpass(f, 0.7071, fs); l2 = l1; h1.highpass(f, 0.7071, fs); h2 = h1; }
        void reset() { l1.reset(); l2.reset(); h1.reset(); h2.reset(); }
        inline void split(double x, double& lo, double& hi) { lo = l2.process(l1.process(x)); hi = h2.process(h1.process(x)); }
    };
    Split x1_[2], x2_[2], ap_[2];
    EnvDb short_[3], long_[3], all_, allLong_;
    double rel_[3][2]{}, g_[3]{}, k_ = 0;
    Ramp aR_;
};

// ---------------------------------------------------------------------------
// IRON: an output transformer pushed hard. The lows saturate (the core runs out of flux first),
// with a little loss at the top as the drive rises.
class Iron {
public:
    void prepare(double fs) { fs_ = fs; for (int c = 0; c < 2; ++c) { split_[c].lowpass(160, fs); top_[c].lowpass(18000, fs); } reset(); }
    void reset() { for (int c = 0; c < 2; ++c) { split_[c].reset(); top_[c].reset(); } }
    void process(float* L, float* R, int n, float drive /* 0..10 */, float shade /* 0..10 */) {
        const double k = 0.6 + drive * 0.55;
        const double topHz = 20000 - drive * 900 + (shade - 5) * 900;
        for (int c = 0; c < 2; ++c) {
            top_[c].lowpass(clamp(topHz, 9000.0, 20000.0), fs_);
            float* x = c == 0 ? L : R;
            for (int i = 0; i < n; ++i) {
                const double lo = split_[c].process(x[i]);
                const double hi = x[i] - lo;
                const double sat = std::tanh(lo * k) / k;
                x[i] = float(top_[c].process(hi + sat));
            }
        }
    }
private:
    double fs_ = 48000;
    FirstOrder split_[2], top_[2];
};

// ---------------------------------------------------------------------------
// Short-term K-weighted level of a signal (for matched comparisons and the loudness meter).
struct KLevel {
    Biquad pre[2], rlb[2];
    EnvDb env;
    double db = -120;
    void prepare(double fs) {
        // BS.1770 K-weighting, the usual analog-matched coefficients worked out for this rate.
        const double f0 = 1681.974450955533, G = 3.999843853973347, Q = 0.7071752369554196;
        const double K = std::tan(kPi * f0 / fs), Vh = std::pow(10.0, G / 20.0), Vb = std::pow(Vh, 0.4996667741545416);
        const double a0 = 1.0 + K / Q + K * K;
        for (int c = 0; c < 2; ++c)
            pre[c].set((Vh + Vb * K / Q + K * K), 2.0 * (K * K - Vh), (Vh - Vb * K / Q + K * K), a0, 2.0 * (K * K - 1.0), 1.0 - K / Q + K * K);
        const double f1 = 38.13547087602444, Q1 = 0.5003270373238773, K1 = std::tan(kPi * f1 / fs);
        for (int c = 0; c < 2; ++c) rlb[c].set(1.0, -2.0, 1.0, 1.0 + K1 / Q1 + K1 * K1, 2.0 * (K1 * K1 - 1.0), 1.0 - K1 / Q1 + K1 * K1);
        env.set(0.4, 0.4, fs);
        reset();
    }
    void reset() { for (int c = 0; c < 2; ++c) { pre[c].reset(); rlb[c].reset(); } env.reset(); db = -120; }
    void push(const float* L, const float* R, int n) {
        for (int i = 0; i < n; ++i) {
            const double l = rlb[0].process(pre[0].process(L[i])), r = rlb[1].process(pre[1].process(R[i]));
            db = env.push(l * l + r * r) - 0.691;
        }
    }
};

// ---------------------------------------------------------------------------
// LISTEN ON: what the master might sound like elsewhere. For listening only.
class ListenOn {
public:
    enum Where { kDesk, kPhone, kBuds, kCar, kClub };
    void prepare(double fs) { fs_ = fs; where_ = -1; reset(); }
    void reset() { for (auto& c : f_) for (auto& b : c) b.reset(); }
    void process(float* L, float* R, int n, int where) {
        if (where == kDesk) return;
        if (where != where_) configure(where);
        for (int i = 0; i < n; ++i) {
            double l = L[i], r = R[i];
            if (where == kPhone) { const double m = 0.5 * (l + r); l = r = m; }
            for (int b = 0; b < 4; ++b) { l = f_[0][b].process(l); r = f_[1][b].process(r); }
            if (where == kPhone) { l = std::tanh(l * 1.4) / 1.4; r = std::tanh(r * 1.4) / 1.4; }
            L[i] = float(l * trim_); R[i] = float(r * trim_);
        }
    }
private:
    void configure(int where) {
        where_ = where;
        for (int c = 0; c < 2; ++c) {
            auto& f = f_[c];
            for (auto& b : f) b.setIdentity();
            switch (where) {
                case kPhone:   // a small speaker: nothing below 300 Hz, a peaky upper mid, little top
                    f[0].highpass(320, 0.9, fs_); f[1].highpass(320, 0.9, fs_); f[2].peak(2200, 1.2, 5, fs_); f[3].lowpass(9000, 0.7, fs_); trim_ = 0.8; break;
                case kBuds:    // earbuds: a lifted low end, a dip in the presence region, bright top
                    f[0].lowShelf(90, 4, fs_); f[1].peak(3000, 1.5, -3, fs_); f[2].highShelf(9000, 2.5, fs_); f[3].highpass(25, 0.7, fs_); trim_ = 0.85; break;
                case kCar:     // a car: a big low bump from the cabin, a congested low-mid, softer top
                    f[0].peak(60, 0.9, 6, fs_); f[1].peak(250, 1.2, 2.5, fs_); f[2].highShelf(6000, -3, fs_); f[3].highpass(28, 0.7, fs_); trim_ = 0.75; break;
                case kClub:    // a club system: sub weight, a forward low-mid, a softer top
                    f[0].peak(50, 0.8, 5, fs_); f[1].peak(120, 1.0, 2, fs_); f[2].highShelf(8000, -4, fs_); f[3].highpass(30, 0.7, fs_); trim_ = 0.7; break;
                default: trim_ = 1; break;
            }
        }
        reset();
    }
    double fs_ = 48000, trim_ = 1;
    int where_ = -1;
    Biquad f_[2][4];
};

}  // namespace lq
