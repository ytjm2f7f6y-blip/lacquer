#pragma once
// long player: the processors. Each runs on a stereo sub-block (at most
// kSub samples) with that sub-block's settings, keeps a constant latency
// whatever its settings, and blends from the open signal to its full effect
// by its weight, with the open signal lined up exactly, so a module at weight
// 0 passes the audio untouched.

#include "lp_dsp.hpp"

namespace lp {

constexpr int kSub = 32;   // settings are updated every 32 samples

// Mid/side helpers. Which channels a module works on.
enum Proc { kStereo, kMid, kSide };

// A value ramped linearly across a sub-block.
struct Ramp {
    float cur = 0;
    bool init = false;
    void set(float target, int n, float* out) {
        if (!init) { cur = target; init = true; }
        const float step = (target - cur) / float(std::max(1, n));
        for (int i = 0; i < n; ++i) { cur += step; out[i] = cur; }
        cur = target;
    }
    void reset() { init = false; }
};

// ---------------------------------------------------------------------------
// Saturation: tube (asymmetric soft curve) or tape (soft peak compression,
// low-end bump, a top that softens as it is pushed). Generic designs.
struct SatParams {
    int mode = 0, proc = kStereo, osFactor = 4;
    float drive = 0, toneDb = 0, mix = 0, bias = 0.4f, bumpDb = 1, bumpHz = 60, rolloffKhz = 14;
    float weight = 0;
};

class Saturation {
public:
    void prepare(double fs, int maxBlock) {
        fs_ = fs;
        for (int c = 0; c < 2; ++c) {
            os_[c].prepare(maxBlock);
            dry_[c].prepare(size_t(os_[c].latency() + 4));
            dc_[c].prepare(fs);
        }
        reset();
    }
    void reset() {
        for (int c = 0; c < 2; ++c) {
            os_[c].reset(); dry_[c].clear(); dc_[c].reset();
            pre_[c].reset(); de_[c].reset(); bump_[c].reset(); roll_[c].reset(); tiltLo_[c].reset(); tiltHi_[c].reset();
            env_[c] = 0;
        }
        wR_.reset(); gR_.reset();
        lastCfg_ = -1;
    }
    int latency() const { return os_[0].latency(); }

    void process(float* L, float* R, int n, const SatParams& p) {
        float a[kSub], b[kSub], w[kSub], wet[2][kSub];
        // Work channels: L/R, or M/S.
        const bool ms = p.proc != kStereo;
        for (int i = 0; i < n; ++i) {
            if (ms) { a[i] = 0.5f * (L[i] + R[i]); b[i] = 0.5f * (L[i] - R[i]); }
            else { a[i] = L[i]; b[i] = R[i]; }
        }
        float* ch[2] = {a, b};
        const bool doCh[2] = {p.proc != kSide, p.proc != kMid};

        updateFilters(p);
        const double g = std::pow(10.0, p.drive * 2.4 / 20.0) * 0.35;   // drive into the curve
        float gain[kSub];
        gR_.set(float(g), n, gain);
        const double bias = p.bias * 0.4;
        const double tb = std::tanh(bias), norm = 1.0 - tb * tb;

        for (int c = 0; c < 2; ++c) {
            float* x = ch[c];
            if (!doCh[c]) { for (int i = 0; i < n; ++i) wet[c][i] = 0; continue; }
            float pre[kSub];
            for (int i = 0; i < n; ++i) pre[i] = float(p.mode == 1 ? pre_[c].process(x[i]) : x[i]);
            double* u = os_[c].up(pre, n, p.osFactor);
            const int f = p.osFactor;
            for (int i = 0; i < n; ++i) {
                const double gi = gain[i];
                for (int k = 0; k < f; ++k) {
                    double& s = u[i * f + k];
                    const double v = s * gi;
                    double y;
                    if (p.mode == 0) {
                        y = (std::tanh(v + bias) - tb) / norm;
                    } else {
                        // Soft peak compression with a touch of asymmetry from the bias.
                        const double vb = v + bias * 0.25 * v * v;
                        y = vb / std::pow(1.0 + std::pow(std::abs(vb), 2.5), 0.4);
                        env_[c] = std::max(std::abs(v), env_[c] * 0.99995);
                    }
                    s = y / gi;
                }
            }
            os_[c].down(wet[c], n);
            for (int i = 0; i < n; ++i) {
                double y = wet[c][i];
                if (p.mode == 1) y = roll_[c].process(bump_[c].process(de_[c].process(y)));
                y = tiltHi_[c].process(tiltLo_[c].process(y));
                wet[c][i] = float(dc_[c].process(y));
            }
        }
        // Mix against the open signal, lined up.
        wR_.set(p.weight * p.mix, n, w);
        const size_t lat = size_t(latency());
        for (int c = 0; c < 2; ++c) {
            float* x = ch[c];
            for (int i = 0; i < n; ++i) {
                dry_[c].push(x[i]);
                const float d = dry_[c].tap(lat);
                x[i] = doCh[c] ? d + w[i] * (wet[c][i] - d) : d;
            }
        }
        for (int i = 0; i < n; ++i) {
            if (ms) { L[i] = a[i] + b[i]; R[i] = a[i] - b[i]; }
            else { L[i] = a[i]; R[i] = b[i]; }
        }
    }

private:
    void updateFilters(const SatParams& p) {
        for (int c = 0; c < 2; ++c) {
            // Tape: a gentle pre-emphasis so the highs saturate a little earlier, undone after.
            Biquad t; t.highShelf(3000, 3.0, fs_, 0.7); copy(pre_[c], t);
            t.highShelf(3000, -3.0, fs_, 0.7); copy(de_[c], t);
            t.peak(p.bumpHz, 0.9, p.bumpDb, fs_); copy(bump_[c], t);
            // The top softens as the tape is pushed.
            const double push = clamp(env_[c] / 2.0, 0.0, 1.0);
            t.lowpass(p.rolloffKhz * 1000.0 * (1.0 - 0.35 * push), 0.6, fs_); copy(roll_[c], t);
            t.lowShelf(900, -p.toneDb * 0.5, fs_, 0.6); copy(tiltLo_[c], t);
            t.highShelf(1100, p.toneDb * 0.5, fs_, 0.6); copy(tiltHi_[c], t);
        }
    }
    static void copy(Biquad& d, const Biquad& s) { LR4::copyCoefs(d, s); }

    double fs_ = 48000;
    Oversampler os_[2];
    Delay dry_[2];
    DcBlock dc_[2];
    Biquad pre_[2], de_[2], bump_[2], roll_[2], tiltLo_[2], tiltHi_[2];
    double env_[2]{};
    Ramp wR_, gR_;
    int lastCfg_ = -1;
};

// ---------------------------------------------------------------------------
// Compressor: feed-forward, soft knee, sidechain high-pass, lookahead, stereo
// link, two-stage program release, parallel mix, stereo or mid/side.
struct CompParams {
    int proc = 0;   // 0 stereo, 1 m/s
    float threshDb = 0, ratio = 1.1f, mix = 0, attackMs = 30, releaseMs = 250, autoRel = 1, kneeDb = 6, scHz = 90, lookMs = 0, link = 1, autoMakeup = 1;
    float weight = 0;
};

class Compressor {
public:
    void prepare(double fs, int) {
        fs_ = fs;
        maxLook_ = int(std::lround(0.005 * fs));
        for (int c = 0; c < 2; ++c) line_[c].prepare(size_t(maxLook_ + 4));
        reset();
    }
    void reset() {
        for (int c = 0; c < 2; ++c) { line_[c].clear(); sc_[c].reset(); fast_[c] = slow_[c] = 0; }
        wR_.reset(); mR_.reset();
        grMax_ = 0;
    }
    int latency() const { return maxLook_; }
    float takeGr() { const float g = grMax_; grMax_ = 0; return g; }

    void process(float* L, float* R, int n, const CompParams& p) {
        float a[kSub], b[kSub], w[kSub], mk[kSub];
        const bool ms = p.proc == 1;
        for (int i = 0; i < n; ++i) {
            if (ms) { a[i] = 0.5f * (L[i] + R[i]); b[i] = 0.5f * (L[i] - R[i]); }
            else { a[i] = L[i]; b[i] = R[i]; }
        }
        float* ch[2] = {a, b};
        for (int c = 0; c < 2; ++c) sc_[c].highpass(p.scHz, 0.7071, fs_);
        const double atk = tcCoef(p.attackMs * 0.001, fs_), rel = tcCoef(p.releaseMs * 0.001, fs_);
        const double slowAtk = tcCoef(0.4, fs_), slowRel = tcCoef(p.releaseMs * 0.003, fs_);
        const double T = p.threshDb, Rt = std::max(1.0, double(p.ratio)), W = p.kneeDb, slope = 1.0 / Rt - 1.0;
        const double makeup = p.autoMakeup * (-T) * (1.0 - 1.0 / Rt) * 0.4;
        mR_.set(float(makeup), n, mk);
        wR_.set(p.weight * p.mix, n, w);
        const size_t tapLook = size_t(clamp(maxLook_ - int(std::lround(p.lookMs * 0.001 * fs_)), 0, maxLook_));

        for (int i = 0; i < n; ++i) {
            double target[2];
            for (int c = 0; c < 2; ++c) {
                line_[c].push(ch[c][i]);
                const double d = sc_[c].process(line_[c].tap(tapLook));
                const double lev = gainToDb(std::abs(d));
                const double over = lev - T;
                double gr = 0;
                if (2 * over < -W) gr = 0;
                else if (W > 0 && 2 * std::abs(over) <= W) gr = -slope * (over + W / 2) * (over + W / 2) / (2 * W);
                else gr = -slope * over;
                target[c] = std::max(0.0, gr);
            }
            const double linked = std::max(target[0], target[1]);
            for (int c = 0; c < 2; ++c) {
                const double t = p.link * linked + (1 - p.link) * target[c];
                fast_[c] = t > fast_[c] ? t + (fast_[c] - t) * atk : t + (fast_[c] - t) * rel;
                slow_[c] = fast_[c] > slow_[c] ? fast_[c] + (slow_[c] - fast_[c]) * slowAtk : fast_[c] + (slow_[c] - fast_[c]) * slowRel;
                const double gr = fast_[c] + p.autoRel * (std::max(fast_[c], slow_[c]) - fast_[c]);
                grMax_ = std::max(grMax_, float(gr));
                const float dly = line_[c].tap(size_t(maxLook_));
                const float comp = float(dly * dbToGain(-gr + mk[i]));
                ch[c][i] = dly + w[i] * (comp - dly);
            }
        }
        for (int i = 0; i < n; ++i) {
            if (ms) { L[i] = a[i] + b[i]; R[i] = a[i] - b[i]; }
            else { L[i] = a[i]; R[i] = b[i]; }
        }
    }

private:
    double fs_ = 48000;
    int maxLook_ = 240;
    Delay line_[2];
    Biquad sc_[2];
    double fast_[2]{}, slow_[2]{};
    Ramp wR_, mR_;
    float grMax_ = 0;
};

// ---------------------------------------------------------------------------
// Exciter: harmonics made from four bands (low, mid, high, air) at 2x, added
// to the untouched signal. Chebyshev shapes on an envelope-normalised band
// give clean second and third harmonics at the band's own level.
struct ExcParams {
    int proc = kStereo;
    float low = 0, mid = 0, high = 0, air = 0, lowSplit = 150, highSplit = 5000, airFrom = 10000, evenOdd = 0.5f, dynamic = 0.6f;
    float weight = 0;
};

class Exciter {
public:
    void prepare(double fs, int maxBlock) {
        fs_ = fs;
        for (int c = 0; c < 2; ++c) {
            os_[c].prepare(maxBlock);
            dry_[c].prepare(size_t(os_[c].latency() + 4));
        }
        reset();
    }
    void reset() {
        for (int c = 0; c < 2; ++c) {
            os_[c].reset(); dry_[c].clear();
            x1_[c].reset(); x2_[c].reset(); xa_[c].reset();
            for (int b = 0; b < 4; ++b) { env_[c][b] = 0; slowEnv_[c][b] = 0; post_[c][b].reset(); }
        }
        wR_.reset();
    }
    int latency() const { return os_[0].latency(); }

    void process(float* L, float* R, int n, const ExcParams& p) {
        float a[kSub], b[kSub], w[kSub], h[2][kSub];
        const bool ms = p.proc != kStereo;
        for (int i = 0; i < n; ++i) {
            if (ms) { a[i] = 0.5f * (L[i] + R[i]); b[i] = 0.5f * (L[i] - R[i]); }
            else { a[i] = L[i]; b[i] = R[i]; }
        }
        float* ch[2] = {a, b};
        const bool doCh[2] = {p.proc != kSide, p.proc != kMid};
        // 2x is enough: the shapes are at most cubic, so nothing folds back below the base band.
        const double fo = fs_ * 2;
        const double amt[4] = {p.low * 0.25, p.mid * 0.25, p.high * 0.25, p.air * 0.25};
        const double eo = p.evenOdd;
        const double atk = tcCoef(0.0005, fo), rel = tcCoef(0.03, fo), slow = tcCoef(0.3, fo);
        for (int c = 0; c < 2; ++c) {
            x1_[c].setKeep(p.lowSplit, fo);
            x2_[c].setKeep(p.highSplit, fo);
            Biquad t;
            t.highpass(p.airFrom, 0.7071, fo); LR4::copyCoefs(xa_[c].hp1, t); LR4::copyCoefs(xa_[c].hp2, t);
            const double edges[4] = {30.0, p.lowSplit, p.highSplit, p.airFrom};
            for (int k = 0; k < 4; ++k) { t.highpass(edges[k], 0.7071, fo); LR4::copyCoefs(post_[c][k], t); }
        }
        for (int c = 0; c < 2; ++c) {
            if (!doCh[c]) { for (int i = 0; i < n; ++i) h[c][i] = 0; continue; }
            double* u = os_[c].up(ch[c], n, 2);
            for (int i = 0; i < n * 2; ++i) {
                const double x = u[i];
                double lo, rest, mid, hi, airLo, air;
                x1_[c].split(x, lo, rest);
                x2_[c].split(rest, mid, hi);
                xa_[c].split(x, airLo, air);
                const double band[4] = {lo, mid, hi, air};
                double sum = 0;
                for (int k = 0; k < 4; ++k) {
                    const double v = band[k];
                    const double av = std::abs(v);
                    double& e = env_[c][k];
                    e = av > e ? av + (e - av) * atk : av + (e - av) * rel;
                    double& se = slowEnv_[c][k];
                    se = e + (se - e) * slow;
                    if (amt[k] <= 0) { post_[c][k].process(0.0); continue; }
                    const double un = clamp(v / (e + 1e-9), -1.0, 1.0);
                    const double t2 = 2 * un * un - 1, t3 = (4 * un * un - 3) * un;
                    const double busy = std::max(0.0, gainToDb(se) + 30.0) / 6.0;
                    const double dyn = 1.0 / (1.0 + p.dynamic * busy);
                    sum += post_[c][k].process(e * ((1 - eo) * t2 + eo * t3)) * amt[k] * dyn;
                }
                u[i] = sum;
            }
            os_[c].down(h[c], n);
        }
        wR_.set(p.weight, n, w);
        const size_t lat = size_t(latency());
        for (int c = 0; c < 2; ++c) {
            for (int i = 0; i < n; ++i) {
                dry_[c].push(ch[c][i]);
                ch[c][i] = dry_[c].tap(lat) + w[i] * h[c][i];
            }
        }
        for (int i = 0; i < n; ++i) {
            if (ms) { L[i] = a[i] + b[i]; R[i] = a[i] - b[i]; }
            else { L[i] = a[i]; R[i] = b[i]; }
        }
    }

private:
    double fs_ = 48000;
    Oversampler os_[2];
    Delay dry_[2];
    LR4 x1_[2], x2_[2], xa_[2];
    Biquad post_[2][4];
    double env_[2][4]{}, slowEnv_[2][4]{};
    Ramp wR_;
};

// ---------------------------------------------------------------------------
// Width: mid/side, side level in three bands, bass mono, balance, and a guard
// that pulls the sides back when the correlation falls too low for mono.
struct WidthParams {
    float width = 1, bassMonoHz = 20, guard = 0, low = 1, mid = 1, high = 1, lowSplit = 200, highSplit = 4000, balanceDb = 0, guardAt = 0.2f, guardMs = 100;
    float weight = 0;
};

class Width {
public:
    void prepare(double fs, int) { fs_ = fs; reset(); }
    void reset() {
        lo_.reset(); hi_.reset(); bm1_.reset(); bm2_.reset();
        xl_ = xr_ = xx_ = 1e-9; guardGain_ = 1; wR_.reset(); sR_.reset();
        corr_ = 1;
    }
    float correlation() const { return float(corr_); }

    void process(float* L, float* R, int n, const WidthParams& p) {
        float w[kSub], sg[kSub];
        const double midG = std::max(0.01, double(p.mid));
        Biquad t;
        t.lowShelf(p.lowSplit, clamp(gainToDb(std::max(0.0, double(p.low)) / midG), -40.0, 12.0), fs_, 0.7); LR4::copyCoefs(lo_, t);
        t.highShelf(p.highSplit, clamp(gainToDb(double(p.high) / midG), -40.0, 12.0), fs_, 0.7); LR4::copyCoefs(hi_, t);
        const bool mono = p.bassMonoHz > 22.0f;
        if (mono) {
            t.highpass(p.bassMonoHz, 0.5411961, fs_); LR4::copyCoefs(bm1_, t);
            t.highpass(p.bassMonoHz, 1.3065630, fs_); LR4::copyCoefs(bm2_, t);
        }
        const double bal = std::pow(10.0, p.balanceDb / 40.0);
        const double cAv = tcCoef(p.guardMs * 0.001, fs_);
        const double minGuard = 1.0 - 0.5 * p.guard;
        const double step = 1.0 / (p.guardMs * 0.001 * fs_);
        wR_.set(p.weight, n, w);
        sR_.set(float(p.width * midG), n, sg);
        for (int i = 0; i < n; ++i) {
            const double l = L[i], r = R[i];
            double m = 0.5 * (l + r) * bal, s = 0.5 * (l - r) / bal;
            s = hi_.process(lo_.process(s * sg[i]));
            const double hp = bm2_.process(bm1_.process(s));   // runs either way, so turning it on is smooth
            s = (mono ? hp : s) * guardGain_;
            const double ol = m + s, orr = m - s;
            // Correlation of the result, for the guard.
            xl_ = ol * ol + (xl_ - ol * ol) * cAv;
            xr_ = orr * orr + (xr_ - orr * orr) * cAv;
            xx_ = ol * orr + (xx_ - ol * orr) * cAv;
            corr_ = xx_ / std::sqrt(xl_ * xr_ + 1e-18);
            if (corr_ < p.guardAt && xl_ + xr_ > 1e-8) guardGain_ = std::max(minGuard, guardGain_ - step);
            else guardGain_ = std::min(1.0, guardGain_ + step * 0.25);
            L[i] = float(l + w[i] * (ol - l));
            R[i] = float(r + w[i] * (orr - r));
        }
    }

private:
    double fs_ = 48000;
    Biquad lo_, hi_, bm1_, bm2_;
    double xl_ = 1e-9, xr_ = 1e-9, xx_ = 1e-9, guardGain_ = 1, corr_ = 1;
    Ramp wR_, sR_;
};

// ---------------------------------------------------------------------------
// Limiter: pre-gain, an optional oversampled clip stage, then a lookahead
// limiter that reads true peaks (4x) and holds a ceiling in dBTP, with a
// fast and a slow release blended by character.
struct LimParams {
    float gainDb = 0, trimDb = 0, releaseMs = 300, character = 0.3f, lookMs = 5, fastMs = 8, clip = 0, clipAmt = 0.2f, link = 0.9f;
    float ceilingDb = -1, ceilingMargin = 0;   // margin (linear) left for dither
    int osFactor = 8;
};

class Limiter {
public:
    void prepare(double fs, int maxBlock) {
        fs_ = fs;
        maxLook_ = int(std::lround(0.010 * fs));
        for (int c = 0; c < 2; ++c) {
            os_[c].prepare(maxBlock);
            clipDry_[c].prepare(size_t(os_[c].latency() + 8 * kSub + 8));
            audio_[c].prepare(size_t(maxLook_ + TruePeak::kDelay + 8));
            det_[c].prepare(size_t(maxLook_ + 8));
            hring_[c].assign(size_t(maxLook_ + 8), 1.0);
            dqIdx_[c].assign(size_t(maxLook_ + 16), 0);
            dqVal_[c].assign(size_t(maxLook_ + 16), 1.0);
        }
        reset();
    }
    void reset() {
        for (int c = 0; c < 2; ++c) {
            os_[c].reset(); clipDry_[c].clear(); audio_[c].clear(); det_[c].fill(1.0f); tp_[c].reset();
            std::fill(hring_[c].begin(), hring_[c].end(), 1.0);
            hsum_[c] = 0; dqHead_[c] = dqTail_[c] = 0;
            eFast_[c] = eSlow_[c] = 0;
        }
        n_ = 0; curLook_ = -1; osCold_ = true;
        gR_.reset(); cR_.reset();
        grMax_ = 0;
    }
    int latency() const { return os_[0].latency() + maxLook_ + TruePeak::kDelay - 1; }
    static void clipStage(double* u, int m, double thr) {
        for (int i = 0; i < m; ++i) {
            const double v = u[i] / thr, av = std::abs(v);
            // Soft clip: linear to 0.7, then a smooth knee into 1.
            const double o = av < 0.7 ? av : 0.7 + 0.3 * std::tanh((av - 0.7) / 0.3);
            u[i] = (v < 0 ? -o : o) * thr;
        }
    }
    float takeGr() { const float g = grMax_; grMax_ = 0; return g; }

    void process(float* L, float* R, int n, const LimParams& p) {
        float* ch[2] = {L, R};
        float g[kSub], cw[kSub];
        gR_.set(float(dbToGain(p.gainDb + p.trimDb)), n, g);
        cR_.set(p.clip, n, cw);
        const double ceil = dbToGain(p.ceilingDb) - p.ceilingMargin;
        // Clip stage. While it is out its oversampling rests; coming back, it is primed with the
        // recent input first, so it starts from the real signal (the latency never changes).
        const double thr = ceil * dbToGain(6.0 - 6.0 * p.clipAmt);
        const bool clipLive = p.clip > 0.0f || cR_.cur > 0.0f;
        for (int c = 0; c < 2; ++c) {
            float x[kSub], y[kSub];
            for (int i = 0; i < n; ++i) x[i] = ch[c][i] * g[i];
            const size_t lat = size_t(os_[c].latency());
            if (!clipLive) {
                for (int i = 0; i < n; ++i) { clipDry_[c].push(x[i]); ch[c][i] = clipDry_[c].tap(lat); }
                osCold_ = true;
                continue;
            }
            if (osCold_) {
                os_[c].reset();
                float h[kSub], dump[kSub];
                const int prime = 8;   // chunks of history
                for (int k = prime - 1; k >= 0; --k) {
                    for (int i = 0; i < kSub; ++i) h[i] = clipDry_[c].tap(size_t(k * kSub + (kSub - 1 - i)));
                    clipStage(os_[c].up(h, kSub, p.osFactor), kSub * p.osFactor, thr);
                    os_[c].down(dump, kSub);
                }
            }
            clipStage(os_[c].up(x, n, p.osFactor), n * p.osFactor, thr);
            os_[c].down(y, n);
            for (int i = 0; i < n; ++i) {
                clipDry_[c].push(x[i]);
                const float d = clipDry_[c].tap(lat);
                ch[c][i] = d + cw[i] * (y[i] - d);
            }
        }
        if (clipLive) osCold_ = false;
        // Lookahead window.
        const int look = clamp(int(std::lround(p.lookMs * 0.001 * fs_)), 8, maxLook_);
        if (look != curLook_) {
            curLook_ = look;
            for (int c = 0; c < 2; ++c) {
                hsum_[c] = 0;
                for (int k = 0; k < look; ++k) hsum_[c] += hring_[c][size_t((n_ - 1 - k + int64_t(hring_[c].size()) * 4) % int64_t(hring_[c].size()))];
            }
        }
        const double rf = tcCoef(p.fastMs * 0.001, fs_), rs = tcCoef(p.releaseMs * 0.001, fs_), sa = tcCoef(0.03, fs_);
        const double chr = p.character;
        const size_t detTap = size_t(maxLook_ - look);
        const size_t audTap = size_t(maxLook_ + TruePeak::kDelay - 1);
        const size_t hsz = hring_[0].size();
        for (int i = 0; i < n; ++i) {
            double need[2];
            for (int c = 0; c < 2; ++c) {
                audio_[c].push(ch[c][i]);
                const double tp = tp_[c].process(ch[c][i]);
                need[c] = tp > ceil ? ceil / tp : 1.0;
            }
            const double linked = std::min(need[0], need[1]);
            for (int c = 0; c < 2; ++c) {
                const double r = p.link * linked + (1 - p.link) * need[c];
                det_[c].push(float(r));
                const double rd = double(det_[c].tap(detTap));
                // Sliding minimum over the window (monotonic queue).
                auto& qi = dqIdx_[c]; auto& qv = dqVal_[c];
                const size_t qn = qi.size();
                while (dqTail_[c] != dqHead_[c] && qv[(dqTail_[c] + qn - 1) % qn] >= rd) dqTail_[c] = (dqTail_[c] + qn - 1) % qn;
                qi[dqTail_[c]] = n_; qv[dqTail_[c]] = rd; dqTail_[c] = (dqTail_[c] + 1) % qn;
                while (qi[dqHead_[c]] <= n_ - look) dqHead_[c] = (dqHead_[c] + 1) % qn;
                const double hmin = qv[dqHead_[c]];
                // Box average of the held minimum over the same window.
                const size_t slot = size_t(n_ % int64_t(hsz));
                const double old = hring_[c][size_t((n_ - look + int64_t(hsz) * 4) % int64_t(hsz))];
                hring_[c][slot] = hmin;
                hsum_[c] += hmin - old;
                const double avg = std::min(1.0, hsum_[c] / look);
                // Release in dB: fast and slow, blended by character; never less than needed.
                const double t = -gainToDb(std::max(avg, 1e-9));
                eFast_[c] = t > eFast_[c] ? t : t + (eFast_[c] - t) * rf;
                eSlow_[c] = t > eSlow_[c] ? eSlow_[c] + (t - eSlow_[c]) * (1 - sa) : t + (eSlow_[c] - t) * rs;
                const double gr = std::max(t, (1 - chr) * std::max(eFast_[c], eSlow_[c]) + chr * eFast_[c]);
                grMax_ = std::max(grMax_, float(gr));
                double y = audio_[c].tap(audTap) * dbToGain(-gr);
                y = clamp(y, -ceil, ceil);   // a last guard; the gain above already keeps it under
                ch[c][i] = float(y);
            }
            ++n_;
            if ((n_ & 0xFFFF) == 0) {   // keep the running sums exact
                for (int c = 0; c < 2; ++c) {
                    hsum_[c] = 0;
                    for (int k = 0; k < look; ++k) hsum_[c] += hring_[c][size_t((n_ - 1 - k + int64_t(hsz) * 4) % int64_t(hsz))];
                }
            }
        }
    }

private:
    double fs_ = 48000;
    int maxLook_ = 480, curLook_ = -1;
    Oversampler os_[2];
    Delay clipDry_[2], audio_[2], det_[2];
    TruePeak tp_[2];
    std::vector<double> hring_[2];
    double hsum_[2]{};
    std::vector<int64_t> dqIdx_[2];
    std::vector<double> dqVal_[2];
    size_t dqHead_[2]{}, dqTail_[2]{};
    double eFast_[2]{}, eSlow_[2]{};
    int64_t n_ = 0;
    Ramp gR_, cR_;
    float grMax_ = 0;
    bool osCold_ = true;
};

// ---------------------------------------------------------------------------
// Dither: TPDF, or TPDF with first-order noise shaping, to 16 or 24 bits.
class Dither {
public:
    void reset() { rng_ = Rng(); e_[0] = e_[1] = 0; }
    static double lsb(int bits) { return 1.0 / double(1 << (bits - 1)); }
    void process(float* L, float* R, int n, int type, int bits) {
        if (type == 0) return;
        const double q = lsb(bits), inv = 1.0 / q;
        float* ch[2] = {L, R};
        for (int c = 0; c < 2; ++c)
            for (int i = 0; i < n; ++i) {
                const double d = (rng_.uniform() - rng_.uniform()) * q;
                double x = ch[c][i];
                if (type == 2) x -= e_[c];
                const double y = std::round((x + d) * inv) * q;
                if (type == 2) e_[c] = y - x;
                ch[c][i] = float(y);
            }
    }

private:
    Rng rng_;
    double e_[2]{};
};

}  // namespace lp
