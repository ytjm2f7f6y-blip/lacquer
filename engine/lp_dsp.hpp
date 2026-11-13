#pragma once
// long player: the building blocks. Header-only, no dependencies, so the engine
// can be tested without JUCE. Everything runs in double inside the filters and
// float in the buffers.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

namespace lp {

constexpr double kPi = 3.14159265358979323846;

inline double dbToGain(double db) { return std::pow(10.0, db / 20.0); }
inline double gainToDb(double g) { return 20.0 * std::log10(std::max(g, 1e-12)); }
inline double powToDb(double p) { return 10.0 * std::log10(std::max(p, 1e-20)); }
template <typename T> inline T clamp(T v, T lo, T hi) { return std::min(hi, std::max(lo, v)); }
inline double lerp(double a, double b, double t) { return a + (b - a) * t; }
// Time constant in samples to a one-pole coefficient (reaches 63% in that time).
inline double tcCoef(double seconds, double fs) { return seconds <= 0.0 ? 0.0 : std::exp(-1.0 / (seconds * fs)); }

// Flush denormals for the scope (the plugin also uses JUCE's guard; this covers the tests).
struct NoDenormals {
#if defined(__SSE__) || defined(_M_X64) || defined(__x86_64__)
    unsigned int saved;
    NoDenormals() {
        unsigned int csr;
        __asm__ volatile("stmxcsr %0" : "=m"(csr));
        saved = csr;
        csr |= 0x8040;
        __asm__ volatile("ldmxcsr %0" : : "m"(csr));
    }
    ~NoDenormals() { __asm__ volatile("ldmxcsr %0" : : "m"(saved)); }
#elif defined(__aarch64__)
    uint64_t saved;
    NoDenormals() {
        uint64_t fpcr;
        __asm__ volatile("mrs %0, fpcr" : "=r"(fpcr));
        saved = fpcr;
        fpcr |= (1ull << 24);
        __asm__ volatile("msr fpcr, %0" : : "r"(fpcr));
    }
    ~NoDenormals() { __asm__ volatile("msr fpcr, %0" : : "r"(saved)); }
#else
    NoDenormals() {}
#endif
};

// ---------------------------------------------------------------------------
// Biquad (transposed direct form II), with the RBJ cookbook designs.
struct Biquad {
    double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0;
    double z1 = 0, z2 = 0;

    inline double process(double x) {
        const double y = b0 * x + z1;
        z1 = b1 * x - a1 * y + z2;
        z2 = b2 * x - a2 * y;
        return y;
    }
    void reset() { z1 = z2 = 0; }
    void setIdentity() { b0 = 1; b1 = b2 = a1 = a2 = 0; }

    void set(double nb0, double nb1, double nb2, double na0, double na1, double na2) {
        b0 = nb0 / na0; b1 = nb1 / na0; b2 = nb2 / na0; a1 = na1 / na0; a2 = na2 / na0;
    }
    void lowpass(double f, double q, double fs) {
        const double w = 2 * kPi * clamp(f, 1.0, fs * 0.49) / fs, c = std::cos(w), al = std::sin(w) / (2 * q);
        set((1 - c) / 2, 1 - c, (1 - c) / 2, 1 + al, -2 * c, 1 - al);
    }
    void highpass(double f, double q, double fs) {
        const double w = 2 * kPi * clamp(f, 1.0, fs * 0.49) / fs, c = std::cos(w), al = std::sin(w) / (2 * q);
        set((1 + c) / 2, -(1 + c), (1 + c) / 2, 1 + al, -2 * c, 1 - al);
    }
    void peak(double f, double q, double gainDb, double fs) {
        const double A = std::pow(10.0, gainDb / 40), w = 2 * kPi * clamp(f, 1.0, fs * 0.49) / fs, c = std::cos(w), al = std::sin(w) / (2 * q);
        set(1 + al * A, -2 * c, 1 - al * A, 1 + al / A, -2 * c, 1 - al / A);
    }
    void lowShelf(double f, double gainDb, double fs, double slope = 1.0) {
        const double A = std::pow(10.0, gainDb / 40), w = 2 * kPi * clamp(f, 1.0, fs * 0.49) / fs, c = std::cos(w), s = std::sin(w);
        const double al = s / 2 * std::sqrt((A + 1 / A) * (1 / slope - 1) + 2), sa = 2 * std::sqrt(A) * al;
        set(A * ((A + 1) - (A - 1) * c + sa), 2 * A * ((A - 1) - (A + 1) * c), A * ((A + 1) - (A - 1) * c - sa),
            (A + 1) + (A - 1) * c + sa, -2 * ((A - 1) + (A + 1) * c), (A + 1) + (A - 1) * c - sa);
    }
    void highShelf(double f, double gainDb, double fs, double slope = 1.0) {
        const double A = std::pow(10.0, gainDb / 40), w = 2 * kPi * clamp(f, 1.0, fs * 0.49) / fs, c = std::cos(w), s = std::sin(w);
        const double al = s / 2 * std::sqrt((A + 1 / A) * (1 / slope - 1) + 2), sa = 2 * std::sqrt(A) * al;
        set(A * ((A + 1) + (A - 1) * c + sa), -2 * A * ((A - 1) + (A + 1) * c), A * ((A + 1) + (A - 1) * c - sa),
            (A + 1) - (A - 1) * c + sa, 2 * ((A - 1) - (A + 1) * c), (A + 1) - (A - 1) * c - sa);
    }
};

// One-pole filters.
struct OnePole {
    double a = 0, z = 0;   // y = (1-a) x + a y
    void setTime(double seconds, double fs) { a = tcCoef(seconds, fs); }
    void setCutoff(double f, double fs) { a = std::exp(-2 * kPi * clamp(f, 1.0, fs * 0.49) / fs); }
    inline double lp(double x) { z = (1 - a) * x + a * z; return z; }
    inline double hp(double x) { return x - lp(x); }
    void reset(double v = 0) { z = v; }
};

// DC blocker (first-order high-pass near 5 Hz).
struct DcBlock {
    double r = 0.9995, x1 = 0, y1 = 0;
    void prepare(double fs, double f = 5.0) { r = std::exp(-2 * kPi * f / fs); x1 = y1 = 0; }
    inline double process(double x) { const double y = x - x1 + r * y1; x1 = x; y1 = y; return y; }
    void reset() { x1 = y1 = 0; }
};

// Linkwitz-Riley crossover (fourth order): low + high sum to an allpass.
struct LR4 {
    Biquad lp1, lp2, hp1, hp2;
    void set(double f, double fs) {
        lp1.lowpass(f, 0.7071067811865476, fs); lp2 = lp1;
        hp1.highpass(f, 0.7071067811865476, fs); hp2 = hp1;
        // keep states
    }
    void setKeep(double f, double fs) {
        Biquad a, b; a.lowpass(f, 0.7071067811865476, fs); b.highpass(f, 0.7071067811865476, fs);
        copyCoefs(lp1, a); copyCoefs(lp2, a); copyCoefs(hp1, b); copyCoefs(hp2, b);
    }
    static void copyCoefs(Biquad& d, const Biquad& s) { d.b0 = s.b0; d.b1 = s.b1; d.b2 = s.b2; d.a1 = s.a1; d.a2 = s.a2; }
    inline void split(double x, double& lo, double& hi) {
        lo = lp2.process(lp1.process(x));
        hi = hp2.process(hp1.process(x));
    }
    void reset() { lp1.reset(); lp2.reset(); hp1.reset(); hp2.reset(); }
};

// ---------------------------------------------------------------------------
// Integer delay line (power-of-two ring).
struct Delay {
    std::vector<float> buf;
    size_t mask = 0, pos = 0;
    void prepare(size_t maxDelay) {
        size_t n = 1;
        while (n < maxDelay + 2) n <<= 1;
        buf.assign(n, 0.0f);
        mask = n - 1;
        pos = 0;
    }
    inline void push(float x) { buf[pos] = x; pos = (pos + 1) & mask; }
    // d = 0 is the sample just pushed.
    inline float tap(size_t d) const { return buf[(pos - 1 - d) & mask]; }
    // Fractional read, cubic (Hermite) interpolation.
    inline float tapFrac(double d) const {
        const double fl = std::floor(d);
        const size_t i = size_t(std::max(0.0, fl));
        const float t = float(d - fl);
        const float xm1 = tap(i == 0 ? 0 : i - 1), x0 = tap(i), x1 = tap(i + 1), x2 = tap(i + 2);
        const float c1 = 0.5f * (x1 - xm1), c2 = xm1 - 2.5f * x0 + 2.0f * x1 - 0.5f * x2, c3 = 0.5f * (x2 - xm1) + 1.5f * (x0 - x1);
        return ((c3 * t + c2) * t + c1) * t + x0;
    }
    void clear() { std::fill(buf.begin(), buf.end(), 0.0f); pos = 0; }
    void fill(float v) { std::fill(buf.begin(), buf.end(), v); pos = 0; }
};

// ---------------------------------------------------------------------------
// Linear-phase halfband oversampling, 2x, 4x or 8x, with the same whole-sample
// latency whichever factor runs (the shorter path is padded), so switching
// factors never moves the audio.

// Kaiser window helpers.
inline double besselI0(double x) {
    double s = 1, t = 1;
    for (int k = 1; k < 40; ++k) { t *= (x / (2 * k)) * (x / (2 * k)); s += t; if (t < 1e-14 * s) break; }
    return s;
}
inline std::vector<double> halfbandTaps(int n, double beta) {
    // n = 4k+3; cutoff at a quarter of the (high) rate. Gain 1 at DC.
    std::vector<double> h(size_t(n), 0.0);
    const int m = (n - 1) / 2;
    double sum = 0;
    for (int i = 0; i < n; ++i) {
        const int k = i - m;
        const double sinc = k == 0 ? 0.5 : std::sin(kPi * k / 2.0) / (kPi * k);
        const double r = double(k) / m;
        const double w = besselI0(beta * std::sqrt(std::max(0.0, 1 - r * r))) / besselI0(beta);
        h[size_t(i)] = sinc * w;
        sum += h[size_t(i)];
    }
    for (auto& v : h) v /= sum;
    // Exact zeros where the halfband has them.
    for (int i = 0; i < n; ++i) if ((i - m) != 0 && ((i - m) % 2) == 0) h[size_t(i)] = 0.0;
    return h;
}

// One 2x stage: up (zero-stuff + filter, gain 2) and down (filter + decimate).
struct HalfbandStage {
    std::vector<double> taps;   // full symmetric set
    std::vector<int> nz;        // indices of the non-zero taps other than the centre
    int n = 0, centre = 0;
    // Histories (double length rings for contiguous reads).
    std::vector<double> upHist, downHist;
    int upPos = 0, downPos = 0;

    void design(int length, double beta) {
        n = length;
        centre = (n - 1) / 2;
        taps = halfbandTaps(n, beta);
        nz.clear();
        for (int i = 0; i < n; ++i) if (i != centre && taps[size_t(i)] != 0.0) nz.push_back(i);
        upHist.assign(size_t(2 * n), 0.0);
        downHist.assign(size_t(2 * n), 0.0);
        upPos = downPos = 0;
    }
    void reset() { std::fill(upHist.begin(), upHist.end(), 0.0); std::fill(downHist.begin(), downHist.end(), 0.0); upPos = downPos = 0; }
    // Latency of up + down at this stage's high rate: n - 1 samples.

    inline void pushHist(std::vector<double>& h, int& p, double x) {
        h[size_t(p)] = x;
        h[size_t(p + n)] = x;
        p = (p + 1) % n;
    }
    // Upsample one input sample into two outputs.
    inline void up(double x, double& o0, double& o1) {
        // Zero-stuffed stream: x, 0. Filter the two samples; gain 2.
        pushHist(upHist, upPos, x);
        o0 = convolve(upHist, upPos) * 2.0;
        pushHist(upHist, upPos, 0.0);
        o1 = convolve(upHist, upPos) * 2.0;
    }
    // Downsample two inputs into one output.
    inline double down(double x0, double x1) {
        // Keep the even phase, so each stage's delay is exactly (n - 1) / 2 high-rate samples each way.
        pushHist(downHist, downPos, x0);
        const double y = convolve(downHist, downPos);
        pushHist(downHist, downPos, x1);
        return y;
    }
    inline double convolve(const std::vector<double>& h, int p) const {
        // h[p .. p+n-1] holds oldest .. newest.
        const double* base = h.data() + p;
        double s = taps[size_t(centre)] * base[n - 1 - centre];
        for (int i : nz) s += taps[size_t(i)] * base[n - 1 - i];
        return s;
    }
};

class Oversampler {
public:
    static constexpr int kMaxFactor = 8;

    // Design: stage 1 carries the steep transition; later stages are short.
    void prepare(int maxBlock) {
        stage_[0].design(127, 9.0);
        stage_[1].design(31, 8.0);
        stage_[2].design(15, 7.0);
        // Latency at the base rate: sum over stages s (1-based) of (n_s - 1) / 2^s.
        const double l4 = 126.0 / 2 + 30.0 / 4;
        const double l8 = l4 + 14.0 / 8;
        latency_ = int(std::ceil(l8));
        pad2_ = int(std::lround((latency_ - 63.0) * 2));   // samples at 2x
        pad4_ = int(std::lround((latency_ - l4) * 4));     // samples at 4x
        pad8_ = int(std::lround((latency_ - l8) * 8));     // samples at 8x
        padLine2_.prepare(size_t(pad2_ + 4));
        padLine4_.prepare(size_t(pad4_ + 4));
        padLine8_.prepare(size_t(pad8_ + 8));
        buf_.assign(size_t(maxBlock * kMaxFactor), 0.0);
        for (auto& s : stage_) s.reset();
    }
    void reset() { for (auto& s : stage_) s.reset(); padLine2_.clear(); padLine4_.clear(); padLine8_.clear(); }
    int latency() const { return latency_; }

    // Upsample n samples into the internal buffer (factor 4 or 8); returns it.
    double* up(const float* in, int n, int factor) {
        factor_ = factor;
        double* o = buf_.data();
        for (int i = 0; i < n; ++i) {
            double a0, a1;
            stage_[0].up(in[i], a0, a1);
            if (factor == 2) { *o++ = a0; *o++ = a1; continue; }
            double b[4];
            stage_[1].up(a0, b[0], b[1]);
            stage_[1].up(a1, b[2], b[3]);
            if (factor == 8) {
                for (int k = 0; k < 4; ++k) {
                    double c0, c1;
                    stage_[2].up(b[k], c0, c1);
                    *o++ = c0; *o++ = c1;
                }
            } else {
                for (int k = 0; k < 4; ++k) *o++ = b[k];
            }
        }
        return buf_.data();
    }
    // Downsample the internal buffer (as modified in place) back to n samples.
    void down(float* out, int n) {
        const double* x = buf_.data();
        if (factor_ == 2) {
            for (int i = 0; i < n; ++i) {
                padLine2_.push(float(x[0])); const double p0 = pad2_ ? padLine2_.tap(size_t(pad2_)) : x[0];
                padLine2_.push(float(x[1])); const double p1 = pad2_ ? padLine2_.tap(size_t(pad2_)) : x[1];
                out[i] = float(stage_[0].down(p0, p1));
                x += 2;
            }
            return;
        }
        for (int i = 0; i < n; ++i) {
            double b[4];
            if (factor_ == 8) {
                for (int k = 0; k < 4; ++k) {
                    padLine8_.push(float(x[0])); const double p0 = pad8_ ? padLine8_.tap(size_t(pad8_)) : x[0];
                    padLine8_.push(float(x[1])); const double p1 = pad8_ ? padLine8_.tap(size_t(pad8_)) : x[1];
                    b[k] = stage_[2].down(p0, p1);
                    x += 2;
                }
            } else {
                for (int k = 0; k < 4; ++k) {
                    padLine4_.push(float(x[k])); b[k] = pad4_ ? padLine4_.tap(size_t(pad4_)) : x[k];
                }
                x += 4;
            }
            const double a0 = stage_[1].down(b[0], b[1]);
            const double a1 = stage_[1].down(b[2], b[3]);
            out[i] = float(stage_[0].down(a0, a1));
        }
    }

private:
    HalfbandStage stage_[3];
    std::vector<double> buf_;
    Delay padLine2_, padLine4_, padLine8_;
    int latency_ = 0, pad2_ = 0, pad4_ = 0, pad8_ = 0, factor_ = 4;
};

// ---------------------------------------------------------------------------
// True-peak estimate: 4x polyphase interpolation (48 taps), as a meter or a
// limiter detector. Reports the largest absolute value among the sample and
// the three points between it and the one before. Its delay is kDelay samples.
class TruePeak {
public:
    static constexpr int kTaps = 48, kPhases = 4, kPerPhase = kTaps / kPhases, kDelay = kPerPhase / 2;
    TruePeak() {
        // Windowed sinc at a quarter of the 4x rate (the base Nyquist), Kaiser beta 8.
        const int m = kTaps;
        const double c = (m - 1) / 2.0;
        for (int i = 0; i < m; ++i) {
            const double t = (i - c) / kPhases;
            const double s = std::abs(t) < 1e-12 ? 1.0 : std::sin(kPi * t) / (kPi * t);
            const double r = (i - c) / c;
            const double w = besselI0(8.0 * std::sqrt(std::max(0.0, 1 - r * r))) / besselI0(8.0);
            coef_[size_t(i % kPhases)][size_t(i / kPhases)] = s * w;
        }
        // Normalise each phase to unity DC gain.
        for (auto& ph : coef_) {
            double s = 0;
            for (double v : ph) s += v;
            for (double& v : ph) v /= s;
        }
    }
    void reset() { hist_.fill(0.0); pos_ = 0; }
    inline double process(double x) {
        hist_[size_t(pos_)] = x;
        hist_[size_t(pos_ + kPerPhase)] = x;
        pos_ = (pos_ + 1) % kPerPhase;
        const double* h = hist_.data() + pos_;   // oldest .. newest
        double best = 0;
        for (int p = 0; p < kPhases; ++p) {
            double s = 0;
            for (int k = 0; k < kPerPhase; ++k) s += coef_[size_t(p)][size_t(k)] * h[kPerPhase - 1 - k];
            best = std::max(best, std::abs(s));
        }
        return best;
    }

private:
    std::array<std::array<double, kPerPhase>, kPhases> coef_{};
    std::array<double, 2 * kPerPhase> hist_{};
    int pos_ = 0;
};

// Deterministic noise.
struct Rng {
    uint64_t s = 0x9E3779B97F4A7C15ull;
    inline uint64_t next() { s ^= s << 13; s ^= s >> 7; s ^= s << 17; return s; }
    inline double uniform() { return double(next() >> 11) * (1.0 / 9007199254740992.0); }   // [0,1)
    inline double bipolar() { return uniform() * 2.0 - 1.0; }
};

}  // namespace lp
