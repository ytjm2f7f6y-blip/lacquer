#pragma once
// Lacquer's engine: the chain, the sections, the loudness stage and the meters.
//
//   in -> CLEAN -> TONE -> DYNAMICS (glue, punch, even) -> COLOUR -> STEREO -> SPACE
//      -> LOUDNESS (drive, clipper, true-peak limiter) -> dither -> meters -> listening aids -> out
//
// Audio runs in fixed sub-blocks of lp::kSub samples, so the result is the same whatever block
// size the host uses. Settings are worked out at the start of each sub-block from the song
// position: the song's values, with the current section's own values gliding in over a beat.

#include "lp_space.hpp"
#include "lq_analysis.hpp"
#include "lq_linear.hpp"
#include "lq_params.hpp"

#include <atomic>
#include <cstring>

namespace lq {

constexpr int kMaxMarks = 64;

struct Mark { double ppq = 0; int key = 0; };

// Everything the sections need: where they start and what each one changes.
struct SongMap {
    Mark marks[kMaxMarks];
    int count = 0;
    bool has[kNumKeys][kNumParams]{};
    float val[kNumKeys][kNumParams]{};

    int indexAt(double ppq) const {   // the last mark at or before ppq, or -1
        int idx = -1;
        for (int i = 0; i < count; ++i) if (marks[i].ppq <= ppq + 1e-9) idx = i;
        return idx;
    }
    int keyAt(double ppq) const { const int i = indexAt(ppq); return i < 0 ? 0 : marks[i].key; }
    float value(int key, int p, const Values& song) const { return (key > 0 && has[key][p]) ? val[key][p] : song[p]; }
    int changes(int key) const { int c = 0; if (key > 0) for (int p = 0; p < kNumParams; ++p) c += has[key][p] ? 1 : 0; return c; }
    void addMark(double ppq, int key) {
        for (int i = 0; i < count; ++i) if (std::abs(marks[i].ppq - ppq) < 1e-6) { marks[i].key = key; return; }
        if (count >= kMaxMarks) return;
        int at = count;
        while (at > 0 && marks[at - 1].ppq > ppq) { marks[at] = marks[at - 1]; --at; }
        marks[at] = {ppq, key};
        ++count;
    }
    void removeMark(int i) { if (i < 0 || i >= count) return; for (int j = i; j + 1 < count; ++j) marks[j] = marks[j + 1]; --count; }
};

// Written by the message thread, read by the audio thread: two copies and an index.
class SharedSong {
public:
    const SongMap& read() const { return buf_[idx_.load(std::memory_order_acquire)]; }
    template <typename F> void edit(F&& f) {
        const int cur = idx_.load(std::memory_order_relaxed), next = 1 - cur;
        buf_[next] = buf_[cur];
        f(buf_[next]);
        idx_.store(next, std::memory_order_release);
    }
    const SongMap& current() const { return read(); }
private:
    SongMap buf_[2];
    std::atomic<int> idx_{0};
};

struct Transport {
    bool playing = false, hasPpq = false, nonRealtime = false;
    double ppq = 0, bpm = 120, quartersPerBar = 4;
};

// Readings for the faceplate, written by the audio thread.
struct Meters {
    std::atomic<float> stLufs{-70}, plr{0}, integrated{-70}, truePeak{-120}, peakHold{-120};
    std::atomic<float> driveDb{0}, gr{0}, harshCut{0};
    // The most gain reduction since the window last looked (it swaps them back to 0), the input, BEFORE.
    std::atomic<float> glueGrMax{0}, limGrMax{0}, inLufs{-70}, inPeak{-120}, beforeDb{0}, codecPeak{-120};
    std::atomic<bool> guarding{false}, over{false}, learning{true}, spaceActive{false};
    std::atomic<int> key{0};
    std::atomic<float> corr{1};
};

// Something the monitoring runs through after the master is made (the codec preview). Never in a bounce.
struct MonitorTap {
    virtual ~MonitorTap() = default;
    virtual void process(float* L, float* R, int n) = 0;
};

// The song's loudness bar by bar, as last played: what came in, what went out and the drive it had.
// Each bar counts once however often it's played, so looping the chorus can't pass for the song.
constexpr int kMaxBars = 1024;
struct BarLevels {
    std::atomic<float> out[kMaxBars], mix[kMaxBars], drive[kMaxBars];
    std::atomic<bool> heard[kMaxBars];
    BarLevels() { clear(); }
    void clear() { for (int b = 0; b < kMaxBars; ++b) { out[b] = -100; mix[b] = -100; drive[b] = 0; heard[b] = false; } }
};

// From the bars heard (of the first `bars`): what the limiter's input would measure for the whole
// song (the output, less the drive each bar had), and how much of the song that rests on.
inline bool loudnessFromBars(const BarLevels& bl, int bars, double& measured, double& coverage) {
    bars = std::max(1, std::min(bars, kMaxBars));
    int heard = 0;
    double pw = 0; int n = 0;
    for (int b = 0; b < bars; ++b) {
        if (!bl.heard[b].load()) continue;
        ++heard;
        const double o = bl.out[b].load();
        if (o > -70) { pw += std::pow(10.0, o / 10.0); ++n; }
    }
    coverage = double(heard) / bars;
    if (n == 0) return false;
    const double rel = 10 * std::log10(pw / n) - 10;   // the relative gate, bar by bar
    pw = 0; n = 0;
    double dr = 0;
    for (int b = 0; b < bars; ++b) {
        if (!bl.heard[b].load()) continue;
        const double o = bl.out[b].load();
        if (o <= -70 || o <= rel) continue;
        pw += std::pow(10.0, o / 10.0); dr += bl.drive[b].load(); ++n;
    }
    if (n == 0) return false;
    measured = 10 * std::log10(pw / n) - dr / n;
    return true;
}

class Engine {
public:
    ~Engine() = default;
    void prepare(double fs, int maxBlock) {
        fs_ = fs;
        clean_.prepare(fs); tone_.prepare(fs); punch_.prepare(fs); even_.prepare(fs); iron_.prepare(fs);
        for (auto& s : sat_) s.prepare(fs, kSub);
        comp_.prepare(fs, kSub); width_.prepare(fs, kSub); space_.prepare(fs, kSub); lim_.prepare(fs, kSub);
        listen_.prepare(fs); outK_.prepare(fs); limInK_.prepare(fs); inK_.prepare(fs);
        lin_.prepare(fs);
        dry_[0].prepare(size_t(0.4 * fs) + 64); dry_[1].prepare(size_t(0.4 * fs) + 64);
        for (int c = 0; c < 2; ++c) { tp_[c] = lp::TruePeak(); }
        an_.prepare(fs);
        colDry_[0].prepare(size_t(sat_[0].latency() + 4)); colDry_[1].prepare(size_t(sat_[0].latency() + 4));
        for (int m = 0; m < kProbes; ++m) { probeIn_[m].prepare(fs); probeOut_[m].prepare(fs); }
        (void)maxBlock;
        reset();
    }
    void reset() {
        clean_.reset(); tone_.reset(); punch_.reset(); even_.reset(); iron_.reset();
        for (auto& s : sat_) s.reset();
        comp_.reset(); width_.reset(); space_.reset(); lim_.reset(); dither_.reset(); listen_.reset();
        outK_.reset(); limInK_.reset(); inK_.reset(); lin_.reset();
        dry_[0].clear(); dry_[1].clear(); beforeR_.reset();
        barIdx_ = -1; barOutPw_ = barMixPw_ = barDrive_ = 0; barOutN_ = barMixN_ = 0;
        for (auto& t : tp_) t.reset();
        colDry_[0].clear(); colDry_[1].clear();
        for (int m = 0; m < kProbes; ++m) { probeIn_[m].reset(); probeOut_[m].reset(); contrib_[m] = 0; }
        fill_ = 0; prevKey_ = 0; curKey_ = 0; glideFrom_ = 0; glideStartPpq_ = -1;
        monR_.reset(); compR_.reset();
        pkWin_ = 0; pkN_ = 0; stPk_ = 0;
        for (int c = 0; c < 2; ++c) {
            std::fill(std::begin(inBuf_[c]), std::end(inBuf_[c]), 0.0f);
            std::fill(std::begin(outBuf_[c]), std::end(outBuf_[c]), 0.0f);
        }
    }
    // The delay the host is told about, with TONE in natural or linear phase.
    int latencyFor(bool linear) const { return kSub + sat_[0].latency() + comp_.latency() + lim_.latency() + (linear ? lin_.latency() : 0); }
    int latency() const { return latencyFor(linearOn_); }
    void setMonitorTap(MonitorTap* t) { tap_ = t; }
    BarLevels& bars() { return bars_; }
    const BarLevels& bars() const { return bars_; }

    SharedSong& song() { return song_; }
    const SharedSong& song() const { return song_; }
    Meters& meters() { return meters_; }
    const Meters& meters() const { return meters_; }
    Analyser& analyser() { return an_; }

    // Loudness: what the limiter's input measured (the drive is the target minus this).
    void setMeasuredInput(double lufs, bool known) { measured_ = lufs; measuredKnown_ = known; }
    bool measuredKnown() const { return measuredKnown_; }
    double measuredInput() const { return measured_; }
    // After a pass: what the limiter's input would measure, with the limiter's own loss taken in, so
    // the next pass lands on the target. (The output, less the drive the pass used on average.)
    double calibratedInput() const { return passOut_.integrated() - passAvgDrive(); }
    double passAvgDrive() const { return passOut_.gatedAuxMean(); }
    double runningInputLufs() const { return passIn_.integrated(); }
    double passSeconds() const { return passIn_.seconds; }
    void clearPass() { passIn_.clear(); passOut_.clear(); peakTp_ = -120; passDriveSum_ = 0; passDriveN_ = 0; }
    double passOutputLufs() const { return passOut_.integrated(); }
    void setKnee(double kneeLufs, bool known) { knee_ = kneeLufs; kneeKnown_ = known; }
    void setListening(bool on) { listening_ = on; }
    bool listening() const { return listening_; }
    double peakTpDb() const { return peakTp_; }

    // The plugin calls this once per host block. Audio goes through a FIFO of one sub-block, so
    // every sub-block is a whole kSub samples whatever block size the host uses: the sound never
    // depends on it. That costs kSub samples of delay, counted in latency().
    void process(float* L, float* R, int n, const Values& song, const Transport& t) {
        lp::NoDenormals nd;
        for (int i = 0; i < n; ++i) {
            const float xl = L[i], xr = R[i];
            L[i] = outBuf_[0][fill_]; R[i] = outBuf_[1][fill_];
            inBuf_[0][fill_] = xl; inBuf_[1][fill_] = xr;
            if (++fill_ == kSub) {
                Transport tt = t;
                if (t.hasPpq) tt.ppq = t.ppq + double(i + 1 - kSub) / fs_ * t.bpm / 60.0;
                std::memcpy(outBuf_[0], inBuf_[0], sizeof(inBuf_[0]));
                std::memcpy(outBuf_[1], inBuf_[1], sizeof(inBuf_[1]));
                processSub(outBuf_[0], outBuf_[1], kSub, song, tt);
                fill_ = 0;
            }
        }
    }

    // The values one key would play with (song values, plus that section's own).
    Values valuesFor(int key, const Values& song) const {
        Values v = song;
        const auto& s = song_.read();
        if (key > 0) for (int p = 0; p < kNumParams; ++p) if (s.has[key][p]) v[p] = s.val[key][p];
        return v;
    }

private:
    static constexpr int kProbes = 5;   // clean, tone, dynamics, colour, stereo: for matched IN switches

    void processSub(float* L, float* R, int n, const Values& song, const Transport& t) {
        const auto& map = song_.read();
        // Which section, and how far into its glide.
        int key = 0;
        double frac = 1;
        if (t.hasPpq && t.playing && map.count > 0) {
            const int idx = map.indexAt(t.ppq);
            key = idx < 0 ? 0 : map.marks[idx].key;
            if (key != curKey_) {
                glideFrom_ = curKey_;
                curKey_ = key;
                glideStartPpq_ = idx < 0 ? t.ppq : std::max(map.marks[idx].ppq, t.ppq - 1.0);
            }
            if (glideStartPpq_ >= 0) frac = clamp((t.ppq - glideStartPpq_) / 1.0, 0.0, 1.0);   // over one beat
        } else {
            key = editKey_.load();
            if (key != curKey_) { glideFrom_ = curKey_; curKey_ = key; glideStartPpq_ = -1; glideT_ = 0; }
            glideT_ = std::min(1.0, glideT_ + n / (0.5 * fs_));
            frac = glideT_;
        }
        meters_.key.store(curKey_);
        const double e = frac * frac * (3 - 2 * frac);
        Values a = valuesFor(glideFrom_, song), b = valuesFor(curKey_, song), v = b;
        float typeW[3]{0, 0, 0};
        for (int p = 0; p < kNumParams; ++p) {
            const auto& d = def(p);
            if (!d.perSection || frac >= 1) continue;
            switch (d.glide) {
                case gLin: case gBlend: v[p] = float(a[p] + (b[p] - a[p]) * e); break;
                case gLog: {
                    if (a[p] > 0 && b[p] > 0) v[p] = float(std::exp(std::log(a[p]) + (std::log(b[p]) - std::log(a[p])) * e));
                    else v[p] = e < 0.5 ? a[p] : b[p];
                    break;
                }
                case gStep: v[p] = e < 0.5 ? a[p] : b[p]; break;
            }
        }
        {
            const int ta = int(a[cType]), tb = int(b[cType]);
            typeW[clamp(ta, 0, 2)] += float(1 - (frac >= 1 ? 1 : e));
            typeW[clamp(tb, 0, 2)] += float(frac >= 1 ? 1 : e);
        }
        const bool aids = !t.nonRealtime;   // listening aids never reach a bounce made offline

        // The input, for the listening pass, the input meter, BEFORE and the bar map.
        if (listening_ && t.playing) an_.push(L, R, n, curKey_);
        inK_.push(L, R, n);
        {
            float pk = 0;
            for (int i = 0; i < n; ++i) { dry_[0].push(L[i]); dry_[1].push(R[i]); pk = std::max(pk, std::max(std::abs(L[i]), std::abs(R[i]))); }
            inPk_ = std::max(double(pk), inPk_ * std::pow(tcCoef(1.5, fs_), n));
            meters_.inLufs.store(float(inK_.db));
            meters_.inPeak.store(float(gainToDb(inPk_ + 1e-12)));
        }
        if (t.playing && t.hasPpq) feedPass(passMix_, L, R, n, mixBlock_, nullptr, 1);
        trackBar(t);

        // CLEAN
        probeIn_[0].push(L, R, n);
        CleanParams cp;
        cp.res = v[cRes]; cp.harsh = v[cHarsh]; cp.rumbleHz = v[cRumble]; cp.weight = v[cleanIn];
        cp.removed = aids && song[cHear] > 0.5f;
        clean_.process(L, R, n, cp);
        probeOut_[0].push(L, R, n);
        meters_.harshCut.store(clean_.harshCut());

        // TONE
        probeIn_[1].push(L, R, n);
        ToneParams tp = toneParamsFrom(v);
        const bool linear = song[tPhase] >= 0.5f;
        if (linear != linearOn_) { linearOn_ = linear; lin_.reset(); tone_.reset(); }
        if (linear) lin_.process(L, R, n, tp);   // (DYNAMIC is a natural-phase thing)
        else tone_.process(L, R, n, tp);
        probeOut_[1].push(L, R, n);

        // DYNAMICS: glue (a slow bus compressor whose threshold follows the music), punch, even.
        probeIn_[2].push(L, R, n);
        {
            double ms = 0;
            for (int i = 0; i < n; ++i) ms += 0.5 * (double(L[i]) * L[i] + double(R[i]) * R[i]);
            ms /= std::max(1, n);
            progMs_ = ms + (progMs_ - ms) * std::pow(tcCoef(3.0, fs_), n);
            lp::CompParams c;
            c.proc = song[dChan] > 0.5f ? 1 : 0;
            c.threshDb = float(lp::powToDb(progMs_) + 6.0 - v[dGlue] * 0.9);
            static const float attack[5] = {1, 3, 10, 30, 80}, release[4] = {250, 100, 300, 800}, ratio[4] = {1.5f, 2, 3, 4};
            const int rl = clamp(int(v[dRelease] + 0.5f), 0, 3);
            c.ratio = ratio[clamp(int(v[dRatio] + 0.5f), 0, 3)]; c.mix = 1.0f;
            c.attackMs = attack[clamp(int(v[dAttack] + 0.5f), 0, 4)]; c.releaseMs = release[rl]; c.autoRel = rl == 0 ? 1.0f : 0.0f;
            c.kneeDb = 6; c.scHz = 90;
            c.lookMs = 0; c.link = 1; c.autoMakeup = 0;
            c.weight = v[dynIn] * clamp(v[dGlue] / 1.5f, 0.0f, 1.0f);
            comp_.process(L, R, n, c);
            const float gr = comp_.takeGr();
            meters_.gr.store(gr);
            if (gr > meters_.glueGrMax.load(std::memory_order_relaxed)) meters_.glueGrMax.store(gr, std::memory_order_relaxed);
        }
        punch_.process(L, R, n, v[dPunch], v[dynIn]);
        even_.process(L, R, n, v[dEven], v[dynIn]);
        probeOut_[2].push(L, R, n);

        // COLOUR: tape and valve run oversampled; iron is lined up with them.
        probeIn_[3].push(L, R, n);
        {
            float iL[kSub], iR[kSub];
            const float wIron = typeW[1] * v[colIn];
            for (int i = 0; i < n; ++i) { iL[i] = L[i]; iR[i] = R[i]; }
            for (int s = 0; s < 2; ++s) {
                lp::SatParams sp;
                sp.mode = s == 0 ? 1 : 0;   // [0] tape, [1] valve
                sp.drive = v[cDrive]; sp.toneDb = (v[cShade] - 5.0f) * 0.8f; sp.mix = v[cMix] / 100.0f;
                sp.bias = s == 0 ? 0.3f : 0.5f; sp.bumpDb = 1; sp.bumpHz = 60; sp.rolloffKhz = 14; sp.osFactor = 4;
                sp.weight = (s == 0 ? typeW[0] : typeW[2]) * v[colIn];
                sat_[s].process(L, R, n, sp);
            }
            // Iron on the lined-up copy, then mixed in by its own weight against what the others left.
            const size_t lat = size_t(sat_[0].latency());
            for (int i = 0; i < n; ++i) { colDry_[0].push(iL[i]); colDry_[1].push(iR[i]); iL[i] = colDry_[0].tap(lat); iR[i] = colDry_[1].tap(lat); }
            if (wIron > 0.0005f) {
                float wl[kSub], wr[kSub];
                for (int i = 0; i < n; ++i) { wl[i] = iL[i]; wr[i] = iR[i]; }
                iron_.process(wl, wr, n, v[cDrive], v[cShade]);
                const float mix = wIron * v[cMix] / 100.0f;
                for (int i = 0; i < n; ++i) { L[i] += mix * (wl[i] - iL[i]); R[i] += mix * (wr[i] - iR[i]); }
            }
        }
        probeOut_[3].push(L, R, n);

        // STEREO
        probeIn_[4].push(L, R, n);
        {
            lp::WidthParams w;
            w.width = v[sWidth] / 100.0f; w.bassMonoHz = v[sMono] > 1 ? v[sMono] : 20.0f; w.balanceDb = v[sCentre];
            w.guard = 0; w.weight = v[stIn];
            width_.process(L, R, n, w);
            meters_.corr.store(width_.correlation());
        }
        probeOut_[4].push(L, R, n);

        // SPACE
        {
            lp::SpaceParams sp;
            sp.type = clamp(int(v[spType]), 0, 5);
            sp.amountDb = v[spAmount]; sp.decay = v[spDecay]; sp.tone = v[spTone] / 100.0f; sp.preMs = v[spPre];
            sp.lowCut = v[spLow]; sp.highCut = v[spHigh] * 1000.0f; sp.gap = v[spGap] / 100.0f;
            const int ap = int(v[spApply]);
            sp.midW = ap == 1 ? 0.0f : 1.0f; sp.sideW = ap == 0 ? 0.0f : 1.0f;
            sp.t1 = v[spA] / 10.0f; sp.t2 = v[spB] / 10.0f;
            for (int ty = 0; ty < lp::kNumSpaceTypes; ++ty) { sp.typeCtl[ty][0] = sp.t1; sp.typeCtl[ty][1] = sp.t2; }
            sp.weight = v[spIn];
            space_.process(L, R, n, sp);
            meters_.spaceActive.store(v[spIn] > 0.5f && v[spAmount] > -59.0f);
        }

        // Matched IN switches: a module switched out leaves its level contribution behind.
        {
            double compDb = 0;
            const float ins[kProbes] = {v[cleanIn], v[toneIn], v[dynIn], v[colIn], v[stIn]};
            for (int m = 0; m < kProbes; ++m) {
                if (ins[m] > 0.99f && probeIn_[m].db > -60) contrib_[m] = contrib_[m] + (clamp(probeOut_[m].db - probeIn_[m].db, -12.0, 12.0) - contrib_[m]) * 0.002;
                compDb += (1.0 - ins[m]) * contrib_[m];
            }
            if (song[oMatch] < 0.5f) compDb = 0;
            float cg[kSub];
            compR_.set(float(dbToGain(compDb)), n, cg);
            for (int i = 0; i < n; ++i) { L[i] *= cg[i]; R[i] *= cg[i]; }
        }

        // LOUDNESS: the drive is the target less what the limiter's input measured. It is fixed while
        // the song plays (so the song's own ups and downs are kept) and moves only when it stops.
        limInK_.push(L, R, n);
        if (t.playing) feedPass(passIn_, L, R, n, limInBlock_, listening_ ? an_.stats().limIn : nullptr);
        // The limiter's input peaks, for the knee: after CLEAN, TONE, PUNCH and the rest have had their say.
        if (listening_ && t.playing) {
            const int win = std::max(1, int(0.01 * fs_));
            auto& st = an_.stats();
            for (int i = 0; i < n; ++i) {
                limPk_ = std::max(limPk_, double(std::max(std::abs(L[i]), std::abs(R[i]))));
                if (++limPkN_ >= win) {
                    const int bin = int(clamp((gainToDb(limPk_ + 1e-12) + 70.0) * 2.0, 0.0, double(kPkBins - 1)));
                    st.limIn[0].pk[bin] += 1; st.limIn[0].windows += 1;
                    if (curKey_ > 0) { st.limIn[curKey_].pk[bin] += 1; st.limIn[curKey_].windows += 1; }
                    limPk_ = 0; limPkN_ = 0;
                }
            }
        }
        // The section's own target, if it has one (gliding in with the rest of its settings): a
        // section driven less than the song plays quieter and keeps more of its peaks.
        double target = v[lTarget];
        bool guarding = false;
        if (song[lGuard] > 0.5f && kneeKnown_ && target > knee_) { target = knee_; guarding = true; }
        double drive = 0;
        if (measuredKnown_) drive = target - measured_;
        else if (passIn_.seconds > 3) drive = target - passIn_.integrated();
        drive = clamp(drive, -12.0, 24.0);
        // Known input: the drive is a plain gain and follows the target at once. Not known yet (the
        // first pass): it learns gently while the song plays.
        if (measuredKnown_ || !t.playing || !driveInit_) { driveNow_ = drive; driveInit_ = true; }
        else driveNow_ += (drive - driveNow_) * 0.001;
        if (t.playing) { passDriveSum_ += driveNow_; passDriveN_ += 1; }
        meters_.driveDb.store(float(driveNow_));
        meters_.guarding.store(guarding);
        meters_.learning.store(!measuredKnown_);
        {
            lp::LimParams lpm;
            const bool on = song[loudIn] > 0.5f;
            lpm.gainDb = on ? float(driveNow_) : 0.0f;
            lpm.ceilingDb = on ? song[lCeiling] : 0.0f;
            lpm.clip = on && song[lClip] > 0.05f ? 1.0f : 0.0f;
            lpm.clipAmt = song[lClip] / 10.0f;
            // Styles: CLEAN holds on and lets go slowly; PUNCHY reacts late and lets go sooner, so the hits
            // keep their front; DENSE lets go fast, for the most loudness from the least peak.
            static const float look[3] = {7, 2.5f, 5}, chr[3] = {0.25f, 0.45f, 0.75f}, fast[3] = {12, 6, 3}, rel[3] = {400, 180, 80};
            static const float relSet[4] = {0, 60, 200, 600};
            const int st = clamp(int(song[lStyle] + 0.5f), 0, 2), rs = clamp(int(song[lRelease] + 0.5f), 0, 3);
            lpm.releaseMs = rs == 0 ? rel[st] : relSet[rs]; lpm.character = chr[st]; lpm.lookMs = look[st]; lpm.fastMs = fast[st];
            lpm.link = 0.9f; lpm.osFactor = 4;
            const int dith = int(song[oDither]);
            lpm.ceilingMargin = dith == 2 ? float(2.0 / 32768.0) : 0.0f;
            lim_.process(L, R, n, lpm);
            const float lg = lim_.takeGr();
            if (lg > meters_.limGrMax.load(std::memory_order_relaxed)) meters_.limGrMax.store(lg, std::memory_order_relaxed);
            if (dith > 0) dither_.process(L, R, n, 1, dith == 2 ? 16 : 24);
        }

        // Meters, on what leaves for the bounce.
        outK_.push(L, R, n);
        if (t.playing) feedPass(passOut_, L, R, n, outBlock_, listening_ ? an_.stats().out : nullptr, t.hasPpq ? 0 : -1);
        double tpk = 0;
        for (int i = 0; i < n; ++i) {
            const double a = std::max(tp_[0].process(L[i]), tp_[1].process(R[i]));
            tpk = std::max(tpk, a);
        }
        const double tpDb = gainToDb(tpk);
        stPk_ = std::max(stPk_ * std::pow(tcCoef(3.0, fs_), n), tpk);
        if (t.playing) peakTp_ = std::max(peakTp_, tpDb);
        meters_.stLufs.store(float(outK_.db));
        meters_.plr.store(float(gainToDb(stPk_) - outK_.db));
        meters_.truePeak.store(float(tpDb));
        meters_.peakHold.store(float(peakTp_));
        meters_.integrated.store(float(passOut_.seconds > 1 ? passOut_.integrated() : -70.0));
        meters_.over.store(tpDb > song[lCeiling] + 0.05);

        // BEFORE: the song as it came in, lined up and turned to the master's loudness.
        {
            if (inK_.db > -50 && outK_.db > -50) {
                const double want = clamp(outK_.db - inK_.db, -12.0, 30.0);
                beforeDb_ += (want - beforeDb_) * (1 - std::pow(tcCoef(3.0, fs_), n));
            }
            meters_.beforeDb.store(float(beforeDb_));
            float bw[kSub];
            beforeR_.set(aids && song[oBefore] > 0.5f ? 1.0f : 0.0f, n, bw);
            if (beforeR_.cur > 0.0f || bw[0] > 0.0f) {
                const size_t lat = size_t(latency() - kSub + n - 1);
                const double g = dbToGain(beforeDb_);
                for (int i = 0; i < n; ++i) {
                    const double dl = dry_[0].tap(lat - size_t(i)) * g, dr = dry_[1].tap(lat - size_t(i)) * g;
                    L[i] = float(L[i] + bw[i] * (dl - L[i]));
                    R[i] = float(R[i] + bw[i] * (dr - R[i]));
                }
            }
        }
        // The codec preview: what the listener's encoder makes of it.
        if (aids && tap_ && song[oCodec] > 0.5f) tap_->process(L, R, n);

        // Listening aids: never in an offline bounce.
        if (aids) {
            if (song[sHear] > 0.5f) for (int i = 0; i < n; ++i) { const float m = 0.5f * (L[i] + R[i]); L[i] = R[i] = m; }
            listen_.process(L, R, n, int(song[oListen]));
            float mg[kSub];
            monR_.set(float(dbToGain(song[oMonitor])), n, mg);
            for (int i = 0; i < n; ++i) { L[i] *= mg[i]; R[i] *= mg[i]; }
        }
    }

    // 400 ms K-weighted blocks with 75 % overlap, for the integrated loudness of a pass.
    struct BlockAcc { lp::Biquad pre[2], rlb[2]; double sub = 0, q[4]{}; int n = 0, qi = 0; bool init = false; };
    // stream: 0 the output (the bar map's level and drive), 1 the mix as it came in, -1 neither.
    void feedPass(KeyStats& ks, const float* L, const float* R, int n, BlockAcc& b, KeyStats* perKey = nullptr, int stream = -1) {
        if (!b.init) {
            KLevel k; k.prepare(fs_);
            for (int c = 0; c < 2; ++c) { b.pre[c] = k.pre[c]; b.rlb[c] = k.rlb[c]; }
            b.init = true;
        }
        const int len = int(0.1 * fs_);
        for (int i = 0; i < n; ++i) {
            const double l = b.rlb[0].process(b.pre[0].process(L[i])), r = b.rlb[1].process(b.pre[1].process(R[i]));
            b.sub += l * l + r * r;
            if (++b.n == len) {
                b.q[b.qi & 3] = b.sub / len;
                if (++b.qi >= 4) {
                    const double ms = 0.25 * (b.q[0] + b.q[1] + b.q[2] + b.q[3]);
                    const double lu = -0.691 + 10 * std::log10(ms + 1e-20);
                    if (stream == 0 && barIdx_ >= 0) { barOutPw_ += ms; barDrive_ += driveNow_; ++barOutN_; }
                    if (stream == 1 && barIdx_ >= 0) { barMixPw_ += ms; ++barMixN_; }
                    if (stream == 1) { b.sub = 0; b.n = 0; continue; }   // the mix only feeds the bar map
                    ks.blocks.push_back(lu);
                    ks.aux.push_back(driveNow_);
                    ks.seconds += 0.1;
                    if (perKey) {   // the listening pass: the song, and the section it's in
                        for (KeyStats* k : {&perKey[0], curKey_ > 0 ? &perKey[curKey_] : nullptr}) {
                            if (!k) continue;
                            k->blocks.push_back(lu); k->aux.push_back(driveNow_); k->seconds += 0.1;
                        }
                    }
                }
                b.sub = 0; b.n = 0;
            }
        }
    }

    // The bar map: the bar the playhead is in collects the blocks that end in it; leaving it (or
    // stopping) files them, replacing whatever that bar had from an earlier play.
    void trackBar(const Transport& t) {
        const int bar = (t.playing && t.hasPpq && t.ppq >= 0) ? int(std::floor(t.ppq / std::max(1.0, t.quartersPerBar) + 1e-9)) : -1;
        if (bar == barIdx_) return;
        if (barIdx_ >= 0 && barIdx_ < kMaxBars && barOutN_ >= 2) {
            bars_.out[barIdx_].store(float(-0.691 + 10 * std::log10(barOutPw_ / barOutN_ + 1e-20)));
            bars_.drive[barIdx_].store(float(barDrive_ / barOutN_));
            if (barMixN_ > 0) bars_.mix[barIdx_].store(float(-0.691 + 10 * std::log10(barMixPw_ / barMixN_ + 1e-20)));
            bars_.heard[barIdx_].store(true);
        }
        barIdx_ = bar;
        barOutPw_ = barMixPw_ = barDrive_ = 0; barOutN_ = barMixN_ = 0;
    }
public:
    static ToneParams toneParamsFrom(const Values& v) {
        ToneParams tp;
        const int db[kToneBands] = {tLow, tMud, tPres, tAir}, hz[kToneBands] = {tLowHz, tMudHz, tPresHz, tAirHz}, pl[kToneBands] = {tLowPl, tMudPl, tPresPl, tAirPl};
        for (int b = 0; b < kToneBands; ++b) { tp.b[b].db = v[db[b]]; tp.b[b].hz = v[hz[b]]; tp.b[b].place = v[pl[b]]; }
        tp.dynamic = v[tMode]; tp.weight = v[toneIn];
        return tp;
    }

public:
    std::atomic<int> editKey_{0};   // the key lit while stopped

private:
    double fs_ = 48000;
    SharedSong song_;
    Meters meters_;
    Clean clean_; Tone tone_; Punch punch_; Even even_; Iron iron_;
    lp::Saturation sat_[2];
    lp::Compressor comp_;
    lp::Width width_;
    lp::Space space_;
    lp::Limiter lim_;
    lp::Dither dither_;
    ListenOn listen_;
    KLevel outK_, limInK_, inK_, probeIn_[kProbes], probeOut_[kProbes];
    LinearTone lin_;
    bool linearOn_ = false;
    lp::Delay dry_[2];
    Ramp beforeR_;
    double beforeDb_ = 0, inPk_ = 0;
    MonitorTap* tap_ = nullptr;
    BarLevels bars_;
    KeyStats passMix_;
    BlockAcc mixBlock_;
    int barIdx_ = -1, barOutN_ = 0, barMixN_ = 0, limPkN_ = 0;
    double limPk_ = 0;
    double barOutPw_ = 0, barMixPw_ = 0, barDrive_ = 0;
    double contrib_[kProbes]{};
    lp::TruePeak tp_[2];
    lp::Delay colDry_[2];
    Analyser an_;
    KeyStats passIn_, passOut_;
    BlockAcc limInBlock_, outBlock_;
    Ramp monR_, compR_;
    double progMs_ = 1e-4;
    double measured_ = -14, knee_ = -6, driveNow_ = 0, peakTp_ = -120, stPk_ = 0, glideStartPpq_ = -1, glideT_ = 1;
    bool measuredKnown_ = false, kneeKnown_ = false, listening_ = false, driveInit_ = false;
    int fill_ = 0, prevKey_ = 0, curKey_ = 0, glideFrom_ = 0, pkN_ = 0;
    double pkWin_ = 0, passDriveSum_ = 0, passDriveN_ = 0;
    float inBuf_[2][kSub]{}, outBuf_[2][kSub]{};
};

}  // namespace lq
