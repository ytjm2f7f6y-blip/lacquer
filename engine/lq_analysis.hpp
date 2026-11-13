#pragma once
// What the assistant learns from a listening pass, for each section key and for the whole song:
// the long-term spectrum of the mid and the side (sixth-octave bands, 20 Hz to 20 kHz), the
// loudness, a histogram of short peaks against the loudness (for where limiting starts to hurt),
// the stereo picture, DC and how the song starts and ends.
//
// The audio thread writes; the message thread reads once the pass is over.

#include "kt_phase.hpp"
#include "lq_modules.hpp"
#include "lq_params.hpp"

#include <atomic>
#include <vector>

namespace lq {

constexpr int kBands = 60;               // sixth-octave bands from 20 Hz
constexpr int kFft = 4096, kHop = 2048;
constexpr int kPkBins = 160;             // short-peak histogram: -70..+10 dBFS in 0.5 dB steps

inline double bandCentre(int b) { return 20.0 * std::pow(2.0, (b + 0.5) / 6.0); }
inline double bandEdge(int b) { return 20.0 * std::pow(2.0, b / 6.0); }

struct KeyStats {
    double mid[kBands]{}, side[kBands]{};   // summed power per band
    double frames = 0;
    double kSum = 0, kBlocks = 0;           // gated 400 ms K-weighted blocks (sum of mean squares above -70 LUFS)
    std::vector<double> blocks;             // the blocks' loudness, for gating
    std::vector<double> aux;                // a value kept with each block (the drive in use), optional
    double pk[kPkBins]{};                   // 10 ms windows: peak, counted
    double windows = 0;
    double seconds = 0;
    void clear() { *this = KeyStats(); }
    double integrated() const {   // BS.1770 gated
        if (blocks.empty()) return -70;
        double s = 0; int n = 0;
        for (double b : blocks) if (b > -70) { s += std::pow(10.0, (b + 0.691) / 10.0); ++n; }
        if (!n) return -70;
        const double rel = -0.691 + 10 * std::log10(s / n) - 10;
        s = 0; n = 0;
        for (double b : blocks) if (b > rel && b > -70) { s += std::pow(10.0, (b + 0.691) / 10.0); ++n; }
        return n ? -0.691 + 10 * std::log10(s / n) : -70;
    }
    // The mean of `aux` over the blocks the integrated loudness counts (both gates).
    double gatedAuxMean() const {
        if (blocks.empty() || aux.size() != blocks.size()) return 0;
        double s = 0; int n = 0;
        for (double b : blocks) if (b > -70) { s += std::pow(10.0, (b + 0.691) / 10.0); ++n; }
        if (!n) return 0;
        const double rel = -0.691 + 10 * std::log10(s / n) - 10;
        double a = 0; int m = 0;
        for (size_t i = 0; i < blocks.size(); ++i) if (blocks[i] > rel && blocks[i] > -70) { a += aux[i]; ++m; }
        return m ? a / m : 0;
    }
};

struct SongStats {
    KeyStats key[kNumKeys];   // [0] the whole song (every second counts here too)
    double dc[2]{}, samples = 0;
    double corrMin = 1;
    double firstLevelDb = -120, lastLevelDb = -120;   // the first 5 ms and the last 50 ms heard
    bool started = false;
    double subRatioDb = -60;    // filled in by summarise()
    void clear() { for (auto& k : key) k.clear(); dc[0] = dc[1] = 0; samples = 0; corrMin = 1; firstLevelDb = lastLevelDb = -120; started = false; }
};

class Analyser {
public:
    void prepare(double fs) {
        fs_ = fs;
        win_.resize(kFft);
        for (int i = 0; i < kFft; ++i) win_[i] = 0.5 - 0.5 * std::cos(2 * kPi * i / kFft);
        buf_[0].assign(kFft, 0); buf_[1].assign(kFft, 0);
        fft_.resize(kFft);
        // Each bin covers df around its centre; a band takes the part of each bin that falls inside it.
        // (Below about 100 Hz the bands are narrower than a bin: they share its power by width, rather
        // than reading as empty.)
        share_.clear();
        const double df = fs / kFft;
        for (int k = 1; k <= kFft / 2; ++k) {
            const double lo = (k - 0.5) * df, hi = (k + 0.5) * df;
            for (int b = 0; b < kBands; ++b) {
                const double a = std::max(lo, bandEdge(b)), z = std::min(hi, bandEdge(b + 1));
                if (z > a) share_.push_back({k, b, (z - a) / df});
            }
        }
        kl_.prepare(fs);
        blockLen_ = int(0.1 * fs);
        winLen_ = int(0.01 * fs);
        corrA_ = tcCoef(0.4, fs);
        reset();
    }
    void reset() {
        fill_ = 0; std::fill(buf_[0].begin(), buf_[0].end(), 0); std::fill(buf_[1].begin(), buf_[1].end(), 0);
        kl_.reset(); subMs_ = 0; subN_ = 0; for (auto& q : quarter_) q = 0; qi_ = 0; winPk_ = 0; winN_ = 0;
        xl_ = xr_ = xx_ = 1e-9; tail_ = 0;
    }
    SongStats& stats() { return st_; }
    const SongStats& stats() const { return st_; }

    // Feed the input (before any processing) while the listening pass runs.
    void push(const float* L, const float* R, int n, int key) {
        auto& song = st_.key[0];
        auto* sec = key > 0 ? &st_.key[key] : nullptr;
        for (int i = 0; i < n; ++i) {
            const double l = L[i], r = R[i];
            const double m = 0.5 * (l + r), s = 0.5 * (l - r);
            buf_[0][fill_] = m; buf_[1][fill_] = s;
            if (++fill_ == kFft) { frame(song, sec); }
            st_.dc[0] += l; st_.dc[1] += r; st_.samples += 1;
            const double a = std::max(std::abs(l), std::abs(r));
            if (!st_.started && a > 1e-4) { st_.started = true; st_.firstLevelDb = gainToDb(a); }
            winPk_ = std::max(winPk_, a);
            if (++winN_ == winLen_) {
                const int bin = int(clamp((gainToDb(winPk_) + 70.0) * 2.0, 0.0, double(kPkBins - 1)));
                song.pk[bin] += 1; song.windows += 1;
                if (sec) { sec->pk[bin] += 1; sec->windows += 1; }
                tail_ = winPk_;
                winPk_ = 0; winN_ = 0;
            }
            xl_ = l * l + (xl_ - l * l) * corrA_;
            xr_ = r * r + (xr_ - r * r) * corrA_;
            xx_ = l * r + (xx_ - l * r) * corrA_;
            if (xl_ + xr_ > 1e-6) st_.corrMin = std::min(st_.corrMin, xx_ / std::sqrt(xl_ * xr_ + 1e-18));
        }
        // K-weighted 100 ms slots, four to a 400 ms block (75 % overlap).
        for (int i = 0; i < n; ++i) {
            const double l = kl_.rlb[0].process(kl_.pre[0].process(L[i]));
            const double r = kl_.rlb[1].process(kl_.pre[1].process(R[i]));
            subMs_ += l * l + r * r;
            if (++subN_ == blockLen_) {
                quarter_[qi_ & 3] = subMs_ / blockLen_;
                if (++qi_ >= 4) {
                    const double ms = 0.25 * (quarter_[0] + quarter_[1] + quarter_[2] + quarter_[3]);
                    const double lu = -0.691 + 10 * std::log10(ms + 1e-20);
                    song.blocks.push_back(lu); song.seconds += 0.1;
                    if (sec) { sec->blocks.push_back(lu); sec->seconds += 0.1; }
                }
                subMs_ = 0; subN_ = 0;
            }
        }
        st_.lastLevelDb = gainToDb(std::max(tail_, 1e-9));
    }

private:
    void frame(KeyStats& song, KeyStats* sec) {
        for (int ch = 0; ch < 2; ++ch) {
            for (int i = 0; i < kFft; ++i) fft_[size_t(i)] = kt::cplx(buf_[ch][size_t(i)] * win_[size_t(i)], 0);
            kt::fft(fft_.data(), kFft, false);
            double acc[kBands]{};
            for (const auto& sh : share_) acc[sh.b] += std::norm(fft_[size_t(sh.k)]) * sh.w;
            for (int b = 0; b < kBands; ++b) {
                (ch == 0 ? song.mid : song.side)[b] += acc[b];
                if (sec) (ch == 0 ? sec->mid : sec->side)[b] += acc[b];
            }
        }
        song.frames += 1;
        if (sec) sec->frames += 1;
        for (int ch = 0; ch < 2; ++ch) std::copy(buf_[ch].begin() + kHop, buf_[ch].end(), buf_[ch].begin());
        fill_ = kFft - kHop;
    }

    double fs_ = 48000;
    SongStats st_;
    std::vector<double> win_, buf_[2];
    std::vector<kt::cplx> fft_;
    struct Share { int k, b; double w; };
    std::vector<Share> share_;
    int fill_ = 0;
    KLevel kl_;
    double subMs_ = 0, quarter_[4]{};
    int subN_ = 0, qi_ = 0, blockLen_ = 4800, winLen_ = 480, winN_ = 0;
    double winPk_ = 0, tail_ = 0;
    double xl_ = 1e-9, xr_ = 1e-9, xx_ = 1e-9, corrA_ = 0;
};

// The spectrum of one key as dB per band, its overall level taken out (the 200 Hz to 2 kHz
// average set to 0) and tilted by +3 dB an octave, so pink noise reads flat.
inline void balanceCurve(const KeyStats& k, bool withSide, double* out) {
    double raw[kBands];
    for (int b = 0; b < kBands; ++b) {
        const double p = (k.mid[b] + (withSide ? k.side[b] : 0.0)) / std::max(1.0, k.frames);
        const double width = bandEdge(b + 1) - bandEdge(b);
        raw[b] = 10 * std::log10(p / width + 1e-30) + 10 * std::log10(bandCentre(b) / 1000.0);
    }
    double ref = 0; int n = 0;
    for (int b = 0; b < kBands; ++b) if (bandCentre(b) >= 200 && bandCentre(b) <= 2000) { ref += raw[b]; ++n; }
    ref /= std::max(1, n);
    for (int b = 0; b < kBands; ++b) out[b] = raw[b] - ref;
    // A third-octave smoothing, so single bins don't read as moves.
    double sm[kBands];
    for (int b = 0; b < kBands; ++b) {
        double s = 0, w = 0;
        for (int d = -1; d <= 1; ++d) { const int j = b + d; if (j < 0 || j >= kBands) continue; const double ww = d == 0 ? 2 : 1; s += out[j] * ww; w += ww; }
        sm[b] = s / w;
    }
    for (int b = 0; b < kBands; ++b) out[b] = sm[b];
}

}  // namespace lq
