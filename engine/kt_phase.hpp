// Kontur: what the natural-phase and linear-phase modes need.
//
// An FFT, a windowed frequency-sampling FIR designer for 2x2 filter matrices, a direct
// FIR (short kernels, no added delay) and a partitioned convolver (long kernels).
#pragma once
#include <cmath>
#include <complex>
#include <functional>
#include <vector>

namespace kt {

using cplx = std::complex<double>;

// In-place radix-2 FFT. n must be a power of two. inverse = true also divides by n.
inline void fft(cplx* a, int n, bool inverse) {
    for (int i = 1, j = 0; i < n; ++i) {
        int bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) std::swap(a[i], a[j]);
    }
    const double pi = 3.14159265358979323846;
    for (int len = 2; len <= n; len <<= 1) {
        const double ang = 2.0 * pi / len * (inverse ? 1.0 : -1.0);
        const cplx wl(std::cos(ang), std::sin(ang));
        for (int i = 0; i < n; i += len) {
            cplx w(1.0, 0.0);
            for (int k = 0; k < len / 2; ++k) {
                const cplx u = a[i + k], v = a[i + k + len / 2] * w;
                a[i + k] = u + v;
                a[i + k + len / 2] = u - v;
                w *= wl;
            }
        }
    }
    if (inverse)
        for (int i = 0; i < n; ++i) a[i] /= (double) n;
}

// A 2x2 matrix of responses: out = [[ll, lr], [rl, rr]] * [left, right].
struct Mat2 {
    cplx ll{1, 0}, lr{0, 0}, rl{0, 0}, rr{1, 0};
    Mat2 operator*(const Mat2& o) const {
        return {ll * o.ll + lr * o.rl, ll * o.lr + lr * o.rr, rl * o.ll + rr * o.rl, rl * o.lr + rr * o.rr};
    }
};

// Four FIR kernels (ll, lr, rl, rr), each `taps` long, centred on tap `delay`.
struct Kernels {
    std::vector<double> h[4];
    int taps = 0, delay = 0;
    bool cross = false;     // false: lr and rl are zero and can be skipped
};

inline double besselI0(double x) {
    double s = 1.0, t = 1.0;
    for (int k = 1; k < 60; ++k) { t *= (x / (2.0 * k)) * (x / (2.0 * k)); s += t; if (t < 1e-17 * s) break; }
    return s;
}

// Samples `target` (a response matrix at a frequency in Hz) on an FFT grid of `grid` points,
// turns each entry into an impulse response delayed by `delay` samples, and windows it
// (Kaiser) to `taps` = 2 * delay + 1 taps.
inline Kernels designKernels(const std::function<Mat2(double)>& target, double fs, int grid, int delay, double beta = 9.0) {
    Kernels k;
    k.delay = delay;
    k.taps = 2 * delay + 1;
    std::vector<cplx> buf[4];
    for (auto& b : buf) b.assign(grid, cplx(0, 0));
    const double pi = 3.14159265358979323846;
    for (int i = 0; i <= grid / 2; ++i) {
        const Mat2 m = target(fs * i / grid);
        const cplx rot = std::polar(1.0, -2.0 * pi * i * delay / grid);
        const cplx v[4] = {m.ll * rot, m.lr * rot, m.rl * rot, m.rr * rot};
        for (int e = 0; e < 4; ++e) {
            buf[e][i] = v[e];
            if (i > 0 && i < grid / 2) buf[e][grid - i] = std::conj(v[e]);
        }
    }
    const double i0b = besselI0(beta);
    double crossPeak = 0.0;
    for (int e = 0; e < 4; ++e) {
        buf[e][grid / 2] = cplx(buf[e][grid / 2].real(), 0.0);
        fft(buf[e].data(), grid, true);
        k.h[e].resize(k.taps);
        for (int n = 0; n < k.taps; ++n) {
            const double r = delay > 0 ? (double) (n - delay) / delay : 0.0;
            const double w = besselI0(beta * std::sqrt(std::max(0.0, 1.0 - r * r))) / i0b;
            k.h[e][n] = buf[e][n].real() * w;
            if (e == 1 || e == 2) crossPeak = std::max(crossPeak, std::fabs(k.h[e][n]));
        }
    }
    k.cross = crossPeak > 1e-12;
    return k;
}

// A short FIR matrix run directly: no delay beyond the kernel's own.
class DirectFir {
public:
    void set(const Kernels& kn) {
        k = kn;
        for (auto& b : hist) b.assign(2 * k.taps, 0.0);
        pos = 0;
    }
    void reset() { for (auto& b : hist) std::fill(b.begin(), b.end(), 0.0); pos = 0; }
    int taps() const { return k.taps; }
    // Takes the other's kernel without allocating. Same length: the history stays, so the
    // sound carries straight on. Different length: the two swap whole.
    void adopt(DirectFir& o) {
        if (o.k.taps == k.taps) { for (int e = 0; e < 4; ++e) k.h[e].swap(o.k.h[e]); std::swap(k.cross, o.k.cross); }
        else std::swap(*this, o);
    }

    void process(double* l, double* r, int n) {
        const int T = k.taps;
        if (T == 0) return;
        for (int i = 0; i < n; ++i) {
            // Each sample is written twice, so the last T samples are always contiguous.
            hist[0][pos] = hist[0][pos + T] = l[i];
            hist[1][pos] = hist[1][pos + T] = r[i];
            const double* xl = &hist[0][pos + 1];   // oldest .. newest
            const double* xr = &hist[1][pos + 1];
            double yl = 0, yr = 0;
            const double *hll = k.h[0].data(), *hrr = k.h[3].data();
            for (int j = 0; j < T; ++j) { yl += hll[T - 1 - j] * xl[j]; yr += hrr[T - 1 - j] * xr[j]; }
            if (k.cross) {
                const double *hlr = k.h[1].data(), *hrl = k.h[2].data();
                for (int j = 0; j < T; ++j) { yl += hlr[T - 1 - j] * xr[j]; yr += hrl[T - 1 - j] * xl[j]; }
            }
            l[i] = yl; r[i] = yr;
            if (++pos == T) pos = 0;
        }
    }

private:
    Kernels k;
    std::vector<double> hist[2];
    int pos = 0;
};

// A long FIR matrix run as a uniformly partitioned frequency-domain convolution.
// Adds `partition` samples of delay on top of the kernel's own.
class PartConv {
public:
    void set(const Kernels& kn, int partition) {
        P = partition; F = 2 * P; bins = P + 1;
        K = (kn.taps + P - 1) / P;
        cross = kn.cross;
        std::vector<cplx> tmp(F);
        for (int e = 0; e < 4; ++e) {
            H[e].assign((size_t) K * bins, cplx(0, 0));
            if ((e == 1 || e == 2) && !cross) continue;
            for (int p = 0; p < K; ++p) {
                for (int i = 0; i < F; ++i) {
                    const int n = p * P + i;
                    tmp[i] = (i < P && n < kn.taps) ? cplx(kn.h[e][n], 0.0) : cplx(0, 0);
                }
                fft(tmp.data(), F, false);
                for (int b = 0; b < bins; ++b) H[e][(size_t) p * bins + b] = tmp[b];
            }
        }
        for (int c = 0; c < 2; ++c) {
            X[c].assign((size_t) K * bins, cplx(0, 0));
            prev[c].assign(P, 0.0); in[c].assign(P, 0.0); out[c].assign(P, 0.0);
        }
        work.assign(F, cplx(0, 0)); acc.assign(bins, cplx(0, 0));
        head = 0; fill = 0;
    }
    void reset() {
        for (int c = 0; c < 2; ++c) {
            std::fill(X[c].begin(), X[c].end(), cplx(0, 0));
            std::fill(prev[c].begin(), prev[c].end(), 0.0);
            std::fill(in[c].begin(), in[c].end(), 0.0);
            std::fill(out[c].begin(), out[c].end(), 0.0);
        }
        head = 0; fill = 0;
    }

    bool ready() const { return P > 0; }
    // As DirectFir::adopt: same shape, swap the kernel's spectra and keep the history.
    void adopt(PartConv& o) {
        if (o.P == P && o.K == K) { for (int e = 0; e < 4; ++e) H[e].swap(o.H[e]); std::swap(cross, o.cross); }
        else std::swap(*this, o);
    }

    void process(double* l, double* r, int n) {
        if (P == 0) return;
        for (int i = 0; i < n; ++i) {
            in[0][fill] = l[i]; in[1][fill] = r[i];
            l[i] = out[0][fill]; r[i] = out[1][fill];
            if (++fill == P) { block(); fill = 0; }
        }
    }

private:
    void block() {
        head = (head + K - 1) % K;   // the newest spectrum goes in at head
        for (int c = 0; c < 2; ++c) {
            for (int i = 0; i < P; ++i) { work[i] = prev[c][i]; work[P + i] = in[c][i]; prev[c][i] = in[c][i]; }
            fft(work.data(), F, false);
            cplx* x = &X[c][(size_t) head * bins];
            for (int b = 0; b < bins; ++b) x[b] = work[b];
        }
        for (int o = 0; o < 2; ++o) {
            std::fill(acc.begin(), acc.end(), cplx(0, 0));
            for (int c = 0; c < 2; ++c) {
                if (c != o && !cross) continue;
                const std::vector<cplx>& h = H[o * 2 + c];
                for (int p = 0; p < K; ++p) {
                    const cplx* x = &X[c][(size_t) ((head + p) % K) * bins];
                    const cplx* hp = &h[(size_t) p * bins];
                    for (int b = 0; b < bins; ++b) acc[b] += x[b] * hp[b];
                }
            }
            for (int b = 0; b < bins; ++b) work[b] = acc[b];
            for (int b = 1; b < P; ++b) work[F - b] = std::conj(acc[b]);
            fft(work.data(), F, true);
            for (int i = 0; i < P; ++i) out[o][i] = work[P + i].real();
        }
    }

    int P = 0, F = 0, bins = 0, K = 0, head = 0, fill = 0;
    bool cross = false;
    std::vector<cplx> H[4], X[2], work, acc;
    std::vector<double> prev[2], in[2], out[2];
};

}  // namespace kt
