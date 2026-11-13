#pragma once
// long player: the space. Six types built on one high-density core: an
// eight-line feedback delay network (Hadamard mixing, modulated lines,
// in-loop diffusion, frequency-dependent decay set from the decay time), with
// input diffusion and early reflections shaped per type, and a dispersive
// spring tank for the spring. Generic designs, not models of named hardware.
// Every type is level-matched, and tuned to stay clean at low levels.

#include "lp_modules.hpp"
#include "lp_spacetypes.hpp"

namespace lp {

struct SpaceParams {
    int type = sClean;
    float amountDb = -60, decay = 1.4f, tone = 0.5f, preMs = 20, lowCut = 200, highCut = 8000, gap = 0.6f;
    float midW = 0, sideW = 1;
    float t1 = 0.5f, t2 = 0.5f;   // the type's own two controls, 0..1 (for the type above)
    float typeCtl[kNumSpaceTypes][2] = {{0.7f, 0.3f}, {0.5f, 0.4f}, {0.4f, 0.6f}, {0.5f, 0.25f}, {0.45f, 0.6f}, {0.35f, 0.5f}};   // every type's own two
    float weight = 0;
};

namespace detail {

struct Allpass {
    Delay d;
    int len = 1;
    double g = 0.5;
    void prepare(int maxLen) { d.prepare(size_t(maxLen + 2)); }
    void set(int l, double gain) { len = std::max(1, l); g = gain; }
    inline double process(double x) {
        const double o = d.tap(size_t(len - 1));
        const double v = x + g * o;
        d.push(float(v));
        return o - g * v;
    }
    void clear() { d.clear(); }
};

// Jot's absorbent filter: decay time at DC and at Nyquist for a loop of `len` samples.
struct Absorb {
    double k = 0.9, b = 0, z = 0;
    void set(double lenSamples, double rtDc, double ratioNyq, double fs) {
        k = std::pow(10.0, -3.0 * lenSamples / (std::max(0.05, rtDc) * fs));
        const double a = clamp(ratioNyq, 0.1, 1.0);
        b = std::log(10.0) / 4.0 * std::log10(k) * (1.0 - 1.0 / (a * a));
        b = clamp(b, 0.0, 0.98);
    }
    inline double process(double x) { z = k * (1 - b) * x + b * z; return z; }
    void reset() { z = 0; }
};

}  // namespace detail

class SpaceEngine {
public:
    static constexpr int N = 16;

    void prepare(double fs) {
        fs_ = fs;
        const int maxLine = int(0.25 * fs) + 64;
        for (int i = 0; i < N; ++i) {
            line_[i].prepare(size_t(maxLine));
            loopAp_[i].prepare(int(0.016 * fs));
        }
        for (int c = 0; c < 2; ++c) {
            for (auto& a : inAp_[c]) a.prepare(int(0.02 * fs));
            er_[c].prepare(size_t(0.12 * fs));
        }
        for (int s = 0; s < 2; ++s) {
            spring_[s].line.prepare(size_t(0.08 * fs) + 64);
            for (auto& a : spring_[s].disp) a.prepare(16);
        }
        configure(sClean, 0.5f, 0.5f);
        reset();
    }

    void reset() {
        for (int i = 0; i < N; ++i) { line_[i].clear(); loopAp_[i].clear(); absorb_[i].reset(); }
        for (int c = 0; c < 2; ++c) { for (auto& a : inAp_[c]) a.clear(); er_[c].clear(); erLp_[c].reset(); bright_[c].reset(); }
        for (auto& s : spring_) { s.line.clear(); for (auto& a : s.disp) a.clear(); s.lp.reset(); s.hp.reset(); s.fb = 0; }
        for (int i = 0; i < N; ++i) { phase_[i] = 0.137 * i; phase2_[i] = 0.291 * i - std::floor(0.291 * i); }
        peak_ = 0;
        silentFor_ = 0;
    }

    int type() const { return type_; }

    // Set the type's structure. Called when the type changes (state is reset by the caller).
    void configure(int type, float t1, float t2) {
        type_ = type;
        static const double clean[N] = {23.1, 27.7, 31.3, 34.9, 37.1, 41.3, 43.9, 47.3, 51.1, 53.9, 57.7, 61.3, 64.1, 67.9, 71.3, 74.9};
        static const double studio[N] = {31.7, 37.3, 41.9, 47.1, 52.3, 57.1, 61.7, 66.1, 71.9, 77.3, 81.7, 86.9, 91.3, 97.1, 103.7, 109.3};
        static const double plate[N] = {9.7, 11.3, 12.9, 14.3, 15.9, 17.3, 18.9, 20.7, 22.3, 24.1, 25.9, 27.7, 29.3, 31.1, 33.3, 35.3};
        static const double chamber[N] = {17.3, 19.9, 22.7, 25.1, 27.3, 29.9, 32.3, 34.7, 37.1, 39.7, 42.3, 45.1, 47.9, 50.3, 53.9, 57.1};
        static const double room[N] = {6.1, 7.3, 8.3, 9.1, 10.3, 11.1, 12.3, 13.3, 14.1, 15.3, 16.1, 17.3, 18.7, 19.9, 21.1, 22.7};
        // In-loop diffusion: allpass lengths (ms) across the lines, per type.
        static const double apRange[kNumSpaceTypes][2] = {{2.1, 9.3}, {1.7, 6.9}, {4.3, 14.1}, {2.1, 9.3}, {3.1, 11.3}, {3.3, 10.1}};
        apMinMs_ = apRange[type][0];
        apMaxMs_ = apRange[type][1];
        const double* base = clean;
        switch (type) {
            case sStudio: base = studio; break;
            case sPlate: base = plate; break;
            case sChamber: base = chamber; break;
            case sRoom: base = room; break;
            case sSpring: base = clean; break;
            default: break;
        }
        for (int i = 0; i < N; ++i) { baseMs_[i] = base[i]; lineLen_[i] = 0; }
        (void)t1; (void)t2;
        // Spring tank: two springs of different length; tension sets the stretch of the dispersion.
        for (int s = 0; s < 2; ++s) {
            spring_[s].stretch = 0;   // set on first update
        }
    }

    // Per sub-block settings (continuous values).
    void update(const SpaceParams& p) {
        const double fs = fs_;
        double size = 1.0, diff = 0.65, er = 0.0, mod = 0.12, ratio = 0.6, inDiffN = 4, loopApG = 0.5, erSpan = 0.03;
        double brightDb = 0.0;
        switch (type_) {
            case sClean:
                diff = 0.55 + 0.25 * p.t1; er = 0.6 * p.t2; mod = 0.35; ratio = 0.7; erSpan = 0.035; break;
            case sStudio:
                diff = 0.6; inDiffN = 2; er = 0.45; mod = 0.08 + 0.6 * p.t2; ratio = 0.85 - 0.6 * p.t1; erSpan = 0.05; brightDb = 1.5; break;
            case sPlate:
                diff = 0.75; er = 0.0; mod = 0.25; ratio = 0.9 - 0.6 * p.t1 + 0.1 * p.t2; loopApG = 0.6; brightDb = 4.0 * p.t2; break;
            case sSpring:
                diff = 0.6; er = 0.0; mod = 0.25; ratio = 0.6; break;
            case sChamber:
                size = 0.7 + 0.6 * p.t2; diff = 0.65; er = 0.55; mod = 0.35; ratio = 0.8 - 0.55 * p.t1; erSpan = 0.03 * size; break;
            case sRoom:
                size = 0.6 + 1.0 * p.t1; diff = 0.6; inDiffN = 3; er = 0.25 + 0.75 * p.t2; mod = 0.35; ratio = 0.7; erSpan = 0.018 * size; break;
            default: break;
        }
        inDiffN_ = int(inDiffN);
        { Biquad t; t.highShelf(5000, brightDb, fs, 0.7); LR4::copyCoefs(bright_[0], t); LR4::copyCoefs(bright_[1], t); }
        // Lines and their decay.
        const double rt = std::max(0.1, double(p.decay)) * kRtScale[type_];
        for (int i = 0; i < N; ++i) {
            targetLen_[i] = baseMs_[i] * size * 0.001 * fs;
            if (lineLen_[i] <= 0) lineLen_[i] = targetLen_[i];
            const double apMs = apMinMs_ + (apMaxMs_ - apMinMs_) * double((i * 7) % N) / double(N - 1);
            const int apLen = int(apMs * 0.001 * fs * std::min(1.0, size));
            loopAp_[i].set(apLen, loopApG);
            absorb_[i].set(lineLen_[i] + apLen, rt, ratio, fs);
        }
        modDepth_ = mod * 0.001 * fs;
        // Input diffusion.
        static const double apMs[2][4] = {{4.77, 3.59, 12.73, 9.31}, {4.31, 3.17, 13.27, 8.71}};
        for (int c = 0; c < 2; ++c)
            for (int k = 0; k < 4; ++k) inAp_[c][size_t(k)].set(int(apMs[c][k] * 0.001 * fs), diff);
        // Early reflections: 12 taps per side, spread over the span, falling 0.6 dB a tap.
        erGain_ = er;
        for (int c = 0; c < 2; ++c) {
            double sumSq = 0;
            for (int k = 0; k < kTaps; ++k) {
                const double t = 0.002 + erSpan * std::pow((k + 0.5 + 0.21 * c) / kTaps, 1.5);
                erTap_[c][k] = size_t(t * fs);
                erG_[c][k] = ((k + c) % 2 ? -1.0 : 1.0) * std::pow(10.0, -0.6 * k / 20.0);
                sumSq += erG_[c][k] * erG_[c][k];
            }
            for (int k = 0; k < kTaps; ++k) erG_[c][k] /= std::sqrt(sumSq);
            erLp_[c].setCutoff(7000, fs);
        }
        // Spring tank.
        if (type_ == sSpring) {
            const int stretch = int(std::lround(5 + 7 * (1.0 - p.t1)));   // looser springs chirp more
            const double a = 0.3 + 0.45 * p.t2;                           // drip: deeper dispersion (lows arrive later)
            for (int s = 0; s < 2; ++s) {
                auto& sp = spring_[s];
                if (sp.stretch != stretch) {
                    sp.stretch = stretch;
                    for (auto& ap : sp.disp) { ap.clear(); }
                }
                for (auto& ap : sp.disp) ap.set(stretch, a);
                sp.len = (s == 0 ? 0.0337 : 0.0419) * (0.8 + 0.4 * (1.0 - p.t1)) * fs;
                // Dispersion adds about stages * stretch * (1-a)/(1+a) samples at low frequencies; use a mid value.
                const double dispDelay = kStages * stretch * 1.0;
                // Capped, so the springs give their chirp without ringing; the network carries the tail.
                sp.fb = std::min(0.55, std::pow(10.0, -3.0 * (sp.len + dispDelay) / (rt * fs)));
                sp.lp.setCutoff(4500.0 + 1500.0 * p.t1, fs);
                sp.hp.setCutoff(110.0, fs);
            }
        }
        // Per-type gain, so switching type keeps the level (measured in the engine tests).
        outGain_ = kTypeTrim[type_];
    }

    // in: pre-delayed, filtered input. Adds into out (stereo). feed scales the input.
    void process(const float* inL, const float* inR, float* outL, float* outR, int n, const float* feed) {
        if (idle_ && feed[0] == 0.0f && feed[n - 1] == 0.0f) return;
        idle_ = false;
        double blockPeak = 0;
        static const double rate[N] = {0.31, 0.43, 0.57, 0.67, 0.79, 0.89, 0.97, 0.37, 0.53, 0.61, 0.71, 0.83, 0.93, 0.41, 0.47, 0.73};
        for (int j = 0; j < n; ++j) {
            const double xl = inL[j] * feed[j], xr = inR[j] * feed[j];
            double ol = 0, orr = 0;
            if (type_ == sSpring) {
                springSample(xl, xr, ol, orr);
            } else {
                fdnSample(xl, xr, ol, orr, rate);
            }
            ol = bright_[0].process(ol) * outGain_; orr = bright_[1].process(orr) * outGain_;
            outL[j] += float(ol);
            outR[j] += float(orr);
            blockPeak = std::max(blockPeak, std::max(std::abs(ol), std::abs(orr)));
        }
        // Glide line lengths toward their targets (size changes) slowly, so there is no pitch jump.
        for (int i = 0; i < N; ++i) lineLen_[i] += clamp(targetLen_[i] - lineLen_[i], -0.02 * n, 0.02 * n);
        if (feed[n - 1] == 0.0f && blockPeak < 1e-7) {
            silentFor_ += n;
            if (silentFor_ > int(fs_)) { reset(); idle_ = true; }
        } else silentFor_ = 0;
    }

    bool idle() const { return idle_; }

private:
    static constexpr int kTaps = 12, kStages = 18;
    // Measured: wet level of each type for pink noise at 1.4 s, so that amount 0 db is about the dry level.
    static constexpr double kTypeTrim[kNumSpaceTypes] = {1.6032, 1.5668, 1.3490, 1.7378, 1.3490, 1.0471};
    // Measured T30 against the set decay, per type.
    static constexpr double kRtScale[kNumSpaceTypes] = {1.085, 1.176, 1.06, 1.06, 1.13, 1.07};

    inline void fdnSample(double xl, double xr, double& ol, double& orr, const double* rate) {
        // Input diffusion.
        double dl = xl, dr = xr;
        for (int k = 0; k < inDiffN_; ++k) { dl = inAp_[0][size_t(k)].process(dl); dr = inAp_[1][size_t(k)].process(dr); }
        // Early reflections from the undiffused input.
        er_[0].push(float(xl));
        er_[1].push(float(xr));
        double el = 0, eR = 0;
        if (erGain_ > 0) {
            for (int k = 0; k < kTaps; ++k) { el += erG_[0][k] * er_[0].tap(erTap_[0][k]); eR += erG_[1][k] * er_[1].tap(erTap_[1][k]); }
            el = erLp_[0].lp(el) * erGain_;
            eR = erLp_[1].lp(eR) * erGain_;
        }
        // The network.
        double y[N];
        for (int i = 0; i < N; ++i) {
            phase_[i] += rate[i] / fs_;
            if (phase_[i] >= 1.0) phase_[i] -= 1.0;
            phase2_[i] += rate[i] * 2.71 / fs_;
            if (phase2_[i] >= 1.0) phase2_[i] -= 1.0;
            // Two slow sines per line, so the movement never repeats exactly.
            const double m = modDepth_ * (1.0 + 0.6 * std::sin(2.0 * kPi * phase_[i]) + 0.4 * std::sin(2.0 * kPi * phase2_[i])) * 0.5;
            const double o = line_[i].tapFrac(std::max(1.0, lineLen_[i] - 1.0 + m));
            y[i] = absorb_[i].process(loopAp_[i].process(o));
            // Outputs and inputs on orthogonal Hadamard rows: the two sides are uncorrelated.
            ol += hrow(1, i) * o;
            orr += hrow(2, i) * o;
        }
        hadamard(y);
        for (int i = 0; i < N; ++i) line_[i].push(float(y[i] + (dl * hrow(3, i) + dr * hrow(5, i)) * 0.35));
        ol = ol * 0.25 + el;
        orr = orr * 0.25 + eR;
    }
    static inline double hrow(int r, int i) { return (__builtin_popcount(unsigned(r & i)) & 1) ? -1.0 : 1.0; }

    inline void springSample(double xl, double xr, double& ol, double& orr) {
        const double in[2] = {xl, xr};
        double out[2];
        for (int s = 0; s < 2; ++s) {
            auto& sp = spring_[s];
            double v = sp.line.tapFrac(std::max(1.0, sp.len - 1.0));
            for (auto& ap : sp.disp) v = ap.process(v);
            v = sp.hp.hp(sp.lp.lp(v));
            out[s] = v;
            sp.line.push(float(in[s] + in[1 - s] * 0.3 + v * sp.fb));
        }
        // A little of the smooth network under the springs keeps the tail from ringing.
        double dl = 0, dr = 0;
        static const double rate[N] = {0.31, 0.43, 0.57, 0.67, 0.79, 0.89, 0.97, 0.37, 0.53, 0.61, 0.71, 0.83, 0.93, 0.41, 0.47, 0.73};
        fdnSample(out[0] * 0.6 + xl * 0.25, out[1] * 0.6 + xr * 0.25, dl, dr, rate);
        ol = out[0] * 0.45 + dl * 0.8;
        orr = out[1] * 0.45 + dr * 0.8;
    }

    static inline void hadamard(double* x) {
        for (int h = 1; h < N; h <<= 1)
            for (int i = 0; i < N; i += h * 2)
                for (int j = i; j < i + h; ++j) { const double a = x[j], b = x[j + h]; x[j] = a + b; x[j + h] = a - b; }
        const double s = 1.0 / std::sqrt(double(N));
        for (int i = 0; i < N; ++i) x[i] *= s;
    }

    double fs_ = 48000;
    int type_ = sClean;
    Delay line_[N];
    detail::Allpass loopAp_[N];
    detail::Absorb absorb_[N];
    std::array<detail::Allpass, 4> inAp_[2];
    int inDiffN_ = 4;
    double baseMs_[N]{}, lineLen_[N]{}, targetLen_[N]{}, phase_[N]{}, phase2_[N]{};
    double modDepth_ = 5, erGain_ = 0, outGain_ = 1, apMinMs_ = 2, apMaxMs_ = 9;
    Delay er_[2];
    size_t erTap_[2][kTaps]{};
    double erG_[2][kTaps]{};
    OnePole erLp_[2];
    Biquad bright_[2];
    struct Spring {
        Delay line;
        std::array<detail::Allpass, kStages> disp;
        OnePole lp, hp;
        double len = 1000, fb = 0;
        int stretch = 0;
    } spring_[2];
    double peak_ = 0;
    int silentFor_ = 0;
    bool idle_ = false;
};

// The space module: pre-delay, input filters, two engines (a type change
// feeds the new one while the old tail rings out), gap-fill ducking, tone,
// mid/side placement, and the amount.
class Space {
public:
    void prepare(double fs, int) {
        fs_ = fs;
        pre_[0].prepare(size_t(0.1 * fs) + 8);
        pre_[1].prepare(size_t(0.1 * fs) + 8);
        for (auto& e : eng_) e.prepare(fs);
        reset();
    }
    void reset() {
        for (int c = 0; c < 2; ++c) { pre_[c].clear(); lc_[c].reset(); hc_[c].reset(); tl_[c].reset(); th_[c].reset(); }
        for (auto& e : eng_) e.reset();
        feedGain_[0] = 1; feedGain_[1] = 0; active_ = 0;
        engType_[0] = -1; engType_[1] = -1;
        fast_ = slow_ = 0;
        wR_.reset();
    }

    void process(float* L, float* R, int n, const SpaceParams& p) {
        // Which engine takes the input.
        if (engType_[0] < 0 && engType_[1] < 0) {
            eng_[0].reset();
            eng_[0].configure(p.type, p.t1, p.t2);
            engType_[0] = p.type;
            active_ = 0;
            feedGain_[0] = 1; feedGain_[1] = 0;
        } else if (engType_[active_] != p.type) {
            const int other = 1 - active_;
            if (engType_[other] != p.type) {
                eng_[other].reset();
                eng_[other].configure(p.type, p.t1, p.t2);
                engType_[other] = p.type;
            }
            active_ = other;
        }
        for (int e = 0; e < 2; ++e) {
            if (engType_[e] < 0) continue;
            SpaceParams q = p;
            q.type = engType_[e];
            q.t1 = p.typeCtl[q.type][0];   // each engine keeps its own type's controls
            q.t2 = p.typeCtl[q.type][1];
            eng_[e].update(q);
        }
        // Input filters and pre-delay.
        Biquad t;
        t.highpass(p.lowCut, 0.7071, fs_); for (auto& f : lc_) LR4::copyCoefs(f, t);
        t.lowpass(p.highCut, 0.7071, fs_); for (auto& f : hc_) LR4::copyCoefs(f, t);
        const double tilt = (p.tone - 0.5) * 8.0;   // +/- 4 db
        t.lowShelf(500, -tilt * 0.5, fs_, 0.6); for (auto& f : tl_) LR4::copyCoefs(f, t);
        t.highShelf(3500, tilt * 0.5, fs_, 0.6); for (auto& f : th_) LR4::copyCoefs(f, t);
        const size_t preS = size_t(std::lround(clamp(double(p.preMs), 0.0, 100.0) * 0.001 * fs_));
        float il[kSub], ir[kSub], wl[kSub], wr[kSub], f0[kSub], f1[kSub], w[kSub];
        for (int i = 0; i < n; ++i) {
            pre_[0].push(L[i]); pre_[1].push(R[i]);
            il[i] = float(hc_[0].process(lc_[0].process(pre_[0].tap(preS))));
            ir[i] = float(hc_[1].process(lc_[1].process(pre_[1].tap(preS))));
            wl[i] = wr[i] = 0;
        }
        // Feed crossfade, 60 ms.
        const float step = float(n) / float(0.06 * fs_);
        for (int i = 0; i < n; ++i) {
            for (int e = 0; e < 2; ++e) {
                const float target = e == active_ ? 1.0f : 0.0f;
                feedGain_[e] += clamp(target - feedGain_[e], -step / float(n), step / float(n));
            }
            f0[i] = feedGain_[0]; f1[i] = feedGain_[1];
        }
        if (engType_[0] >= 0) eng_[0].process(il, ir, wl, wr, n, f0);
        if (engType_[1] >= 0) eng_[1].process(il, ir, wl, wr, n, f1);
        // Gap-fill: compare a fast and a slow envelope of the dry signal.
        const double fa = tcCoef(0.005, fs_), frl = tcCoef(0.12, fs_), sl = tcCoef(1.5, fs_);
        const double amount = dbToGain(p.amountDb);
        wR_.set(p.weight, n, w);
        for (int i = 0; i < n; ++i) {
            const double lev = 0.5 * (std::abs(L[i]) + std::abs(R[i]));
            fast_ = lev > fast_ ? lev + (fast_ - lev) * fa : lev + (fast_ - lev) * frl;
            slow_ = fast_ + (slow_ - fast_) * sl;
            const double diff = gainToDb(fast_ + 1e-9) - gainToDb(slow_ + 1e-9);
            const double duck = 1.0 - p.gap * clamp((diff + 9.0) / 12.0, 0.0, 1.0);
            double a = th_[0].process(tl_[0].process(wl[i]));
            double b = th_[1].process(tl_[1].process(wr[i]));
            const double m = 0.5 * (a + b) * p.midW, s = 0.5 * (a - b) * p.sideW;
            const double g = amount * duck * w[i];
            L[i] += float((m + s) * g);
            R[i] += float((m - s) * g);
        }
    }

private:
    double fs_ = 48000;
    Delay pre_[2];
    Biquad lc_[2], hc_[2], tl_[2], th_[2];
    SpaceEngine eng_[2];
    int engType_[2]{-1, -1};
    float feedGain_[2]{1, 0};
    int active_ = 0;
    double fast_ = 0, slow_ = 0;
    Ramp wR_;
};

}  // namespace lp
