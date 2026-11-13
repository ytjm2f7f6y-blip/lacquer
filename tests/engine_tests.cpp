// Lacquer's engine tests: run with ctest, or on their own.
//
// Each check prints one line; the run fails if any check fails.

#include "lq_assistant.hpp"
#include "lq_engine.hpp"
#include "testsig.hpp"

#include <chrono>
#include <cstdio>
#include <memory>
#include <string>

namespace {

int failures = 0, checks = 0;

void check(bool ok, const std::string& what, const std::string& detail = "") {
    ++checks;
    if (!ok) ++failures;
    std::printf("%s  %s%s%s\n", ok ? "pass" : "FAIL", what.c_str(), detail.empty() ? "" : "  -- ", detail.c_str());
}

std::string num(double v, int dp = 2) { char b[64]; std::snprintf(b, sizeof b, "%.*f", dp, v); return b; }

constexpr double kFs = 48000;

struct Run {
    std::vector<float> L, R;
};

// Runs a signal through an engine in blocks of `block`, as a host would, with the transport playing.
Run run(lq::Engine& e, const testsig::Song& s, const lq::Values& v, int block, bool nonRealtime = false, bool playing = true) {
    Run out{s.L, s.R};
    lq::Transport t;
    t.playing = playing; t.hasPpq = true; t.bpm = s.bpm; t.nonRealtime = nonRealtime;
    for (size_t i = 0; i < out.L.size(); i += size_t(block)) {
        const int n = int(std::min<size_t>(size_t(block), out.L.size() - i));
        t.ppq = double(i) / kFs * s.bpm / 60.0;
        e.process(out.L.data() + i, out.R.data() + i, n, v, t);
    }
    return out;
}


// Integrated loudness of a stereo signal (BS.1770, gated), from `from` on.
double lufs(const std::vector<float>& L, const std::vector<float>& R, size_t from = 0) {
    lq::KeyStats ks;
    lq::KLevel k;
    k.prepare(kFs);
    const int len = int(0.1 * kFs);
    double sub = 0, q[4]{};
    int n = 0, qi = 0;
    for (size_t i = from; i < L.size(); ++i) {
        const double l = k.rlb[0].process(k.pre[0].process(L[i])), r = k.rlb[1].process(k.pre[1].process(R[i]));
        sub += l * l + r * r;
        if (++n == len) {
            q[qi & 3] = sub / len;
            if (++qi >= 4) ks.blocks.push_back(-0.691 + 10 * std::log10(0.25 * (q[0] + q[1] + q[2] + q[3]) + 1e-20));
            sub = 0; n = 0;
        }
    }
    return ks.integrated();
}

bool finite(const Run& r) {
    for (size_t i = 0; i < r.L.size(); ++i) if (!std::isfinite(r.L[i]) || !std::isfinite(r.R[i])) return false;
    return true;
}

std::unique_ptr<lq::Engine> engine() {
    auto e = std::make_unique<lq::Engine>();
    e->prepare(kFs, 512);
    return e;
}

// The test song: a quiet intro, a verse, a loud chorus, a verse, a loud chorus.
testsig::Song songOf(double gain = 0.35) {
    return testsig::song(kFs, 120, {{2, false}, {4, false}, {4, true}, {4, false}, {4, true}}, gain);
}

}  // namespace

int main() {
    using namespace lq;
    const auto t0 = std::chrono::steady_clock::now();
    const auto s = songOf();
    Values v;   // the factory settings

    // ---- the basics
    {
        auto e = engine();
        const Run r = run(*e, s, v, 512);
        check(finite(r), "default settings: no NaN or infinity");
        check(e->latency() > 0 && e->latency() < int(0.05 * kFs), "latency is set and under 50 ms", std::to_string(e->latency()) + " samples");
    }
    {   // The same output whatever block size the host uses.
        auto a = engine(), b = engine();
        const Run ra = run(*a, s, v, 64), rb = run(*b, s, v, 997);
        double d = 0;
        for (size_t i = 0; i < ra.L.size(); ++i) d = std::max(d, double(std::abs(ra.L[i] - rb.L[i])));
        check(d < 1e-5, "block size doesn't change the sound (64 vs 997 samples)", "max difference " + num(lq::gainToDb(d + 1e-30), 1) + " dB");
    }

    // ---- loudness: the drive learns on the first pass, the second lands on the target
    {
        auto e = engine();
        Values w = v;
        w[lTarget] = -10.0f; w[lGuard] = 0.0f;
        e->clearPass();
        run(*e, s, w, 512);
        e->setMeasuredInput(e->calibratedInput(), true);
        e->clearPass();
        const Run r2 = run(*e, s, w, 512);
        const double got = lufs(r2.L, r2.R, size_t(2 * kFs));
        check(std::abs(got - (-10.0)) < 1.0, "loudness lands on the target after one pass", "target -10.0, got " + num(got, 1) + " LUFS");
        double tp = -200;
        lp::TruePeak tpm[2];
        for (size_t i = 0; i < r2.L.size(); ++i) tp = std::max({tp, lq::gainToDb(tpm[0].process(r2.L[i])), lq::gainToDb(tpm[1].process(r2.R[i]))});
        check(tp <= w[lCeiling] + 0.15, "true peak stays under the ceiling", "ceiling " + num(w[lCeiling], 1) + ", true peak " + num(tp, 2) + " dBTP");
    }
    {   // GUARD holds the target at the knee.
        auto e = engine();
        Values w = v;
        w[lTarget] = -6.0f; w[lGuard] = 1.0f;
        e->setMeasuredInput(-20.0, true);
        e->setKnee(-9.0, true);
        run(*e, s, w, 512);
        check(e->meters().guarding.load() && std::abs(e->meters().driveDb.load() - 11.0f) < 0.01f, "GUARD holds the drive at the knee", "drive " + num(e->meters().driveDb.load(), 2) + " dB");
    }

    // ---- listening aids never reach an offline bounce
    {
        auto a = engine(), b = engine();
        Values w = v;
        w[oMonitor] = -30.0f; w[oListen] = 1.0f;
        const Run live = run(*a, s, w, 512, false), off = run(*b, s, w, 512, true);
        const double dl = lufs(live.L, live.R), dof = lufs(off.L, off.R);
        auto c = engine();
        Values plain = v;
        plain[oMonitor] = 0.0f;
        const Run ref = run(*c, s, plain, 512, true);
        check(dl < dof - 20, "MONITOR and LISTEN ON change what you hear", num(dl, 1) + " vs " + num(dof, 1) + " LUFS");
        double d = 0;
        for (size_t i = 0; i < ref.L.size(); ++i) d = std::max(d, double(std::abs(ref.L[i] - off.L[i])));
        check(d < 1e-6, "an offline bounce leaves them out", "max difference " + num(d, 8));
    }

    // ---- sections: a mark switches the settings, and the change glides in over a beat
    {
        auto e = engine();
        e->song().edit([](SongMap& m) {
            m.addMark(0.0, 1);
            m.addMark(16.0, 3);   // bar 5: chorus
            m.has[3][tLow] = true;
            m.val[3][tLow] = 6.0f;
        });
        const auto& m = e->song().read();
        check(m.keyAt(4.0) == 1 && m.keyAt(16.0) == 3 && m.keyAt(40.0) == 3, "marks: the section in force at a position");
        const Values cv = e->valuesFor(3, v);
        check(cv[tLow] == 6.0f && cv[tAir] == v[tAir], "a section keeps the song's value for anything it hasn't changed");
        run(*e, s, v, 512);
        check(e->meters().key.load() == 3, "while playing, the lit key follows the song", "key " + std::to_string(e->meters().key.load()));
        check(e->song().read().changes(3) == 1 && e->song().read().changes(1) == 0, "changes are counted per section");
    }
    {   // A section's change of air shows up in that section only.
        auto a = engine(), b = engine();
        b->song().edit([](SongMap& m) { m.addMark(0.0, 1); m.addMark(16.0, 3); m.has[3][tLow] = true; m.val[3][tLow] = 6.0f; });
        a->song().edit([](SongMap& m) { m.addMark(0.0, 1); m.addMark(16.0, 3); });
        Values w = v;
        w[loudIn] = 0.0f;   // compare the tone, not the limiter
        const Run ra = run(*a, s, w, 512), rb = run(*b, s, w, 512);
        auto diffDb = [&](double t0s, double t1s) {
            double ea = 0, eb = 0;
            for (size_t i = size_t(t0s * kFs); i < size_t(t1s * kFs) && i < ra.L.size(); ++i) {
                const double d = double(rb.L[i]) - ra.L[i];
                ea += double(ra.L[i]) * ra.L[i]; eb += d * d;
            }
            return 10 * std::log10((eb + 1e-20) / (ea + 1e-20));
        };
        const double before = diffDb(1.0, 7.5), after = diffDb(9.0, 15.0);
        check(before < -80 && after > -40, "a section's own setting changes that section only", "difference before " + num(before, 0) + " dB, after " + num(after, 0) + " dB");
    }

    // ---- a section's own TARGET: the verses driven less than the song, the choruses as they were
    {
        auto a = engine(), b = engine();
        const float own = v[lTarget] - 2.5f;
        for (auto* e : {a.get(), b.get()}) {
            e->setMeasuredInput(-26.0, true);
            e->song().edit([&](SongMap& m) {
                m.addMark(0, 1); m.addMark(8, 2); m.addMark(24, 3); m.addMark(40, 2); m.addMark(56, 3);
                if (e == b.get()) { m.has[2][lTarget] = true; m.val[2][lTarget] = own; }
            });
        }
        Values w = v;
        w[lGuard] = 0.0f;
        const Run ra = run(*a, s, w, 512), rb = run(*b, s, w, 512);
        auto at = [&](const Run& r, double t0s, double t1s) {
            const size_t i0 = size_t(t0s * kFs), i1 = size_t(t1s * kFs);
            std::vector<float> l(r.L.begin() + long(i0), r.L.begin() + long(i1)), rr(r.R.begin() + long(i0), r.R.begin() + long(i1));
            return lufs(l, rr);
        };
        const double verse = at(rb, 5.0, 11.5) - at(ra, 5.0, 11.5), chorus = at(rb, 13.0, 19.5) - at(ra, 13.0, 19.5);
        check(verse < -1.0 && verse > -2.8 && std::abs(chorus) < 0.3, "a verse's own TARGET 2.5 dB under the song plays it quieter, and leaves the chorus alone",
              "verse " + num(verse, 2) + " dB, chorus " + num(chorus, 2) + " dB");
        // The change glides in over a beat (bar 5 starts at 8 s; a beat is 0.5 s): no step at the mark.
        double worst = 0;
        for (size_t i = size_t(7.9 * kFs); i < size_t(8.6 * kFs); i += 480) {
            std::vector<float> l(rb.L.begin() + long(i), rb.L.begin() + long(i + 480)), r(rb.R.begin() + long(i), rb.R.begin() + long(i + 480));
            double m = 0; for (size_t j = 0; j < l.size(); ++j) m = std::max(m, double(std::abs(l[j])));
            worst = std::max(worst, m);
        }
        check(finite(rb) && worst <= lq::dbToGain(w[lCeiling] + 0.2), "and the change between sections stays under the ceiling");
    }

    // ---- the hidden space
    {
        auto a = engine(), b = engine();
        Values off = v, on = v;
        off[spIn] = 0.0f; on[spIn] = 1.0f; on[spAmount] = -20.0f; on[spApply] = 2.0f; on[spGap] = 0.0f;
        off[loudIn] = on[loudIn] = 0.0f;
        const Run ra = run(*a, s, off, 512), rb = run(*b, s, on, 512);
        double d = 0, e0 = 0;
        for (size_t i = 0; i < ra.L.size(); ++i) { const double x = double(rb.L[i]) - ra.L[i]; d += x * x; e0 += double(ra.L[i]) * ra.L[i]; }
        const double rel = 10 * std::log10(d / e0);
        check(rel > -32 && rel < -8, "SPACE adds a tail under the mix", "the space sits " + num(rel, 1) + " dB under it");
    }

    // ---- CLEAN's 'removed' setting plays only what it takes out
    {
        auto a = engine();
        Values w = v;
        w[cHear] = 1.0f; w[cRumble] = 0.0f; w[loudIn] = 0.0f;
        for (int p : {toneIn, dynIn, colIn, stIn, spIn}) w[p] = 0.0f;
        const Run r = run(*a, s, w, 512);
        check(lufs(r.L, r.R) < lufs(s.L, s.R) - 15, "HEAR REMOVED plays only what CLEAN takes out", num(lufs(r.L, r.R), 1) + " LUFS");
    }

    // ---- the assistant
    {
        // A harsh chorus: the loud sections get a lift at 3.5 kHz; the whole song gets sub rumble and side bass.
        auto song = songOf();
        {
            lp::Biquad bump[2], rum;
            for (auto& b : bump) b.peak(3500, 1.4, 8.0, kFs);
            const size_t c0 = size_t(6 * 2.0 * kFs), c1 = size_t(10 * 2.0 * kFs), c2 = size_t(14 * 2.0 * kFs);
            testsig::Noise nz;
            rum.lowpass(25, 0.7, kFs);
            for (size_t i = 0; i < song.L.size(); ++i) {
                const bool chorus = (i >= c0 && i < c1) || i >= c2;
                if (chorus) { song.L[i] = float(bump[0].process(song.L[i])); song.R[i] = float(bump[1].process(song.R[i])); }
                const double r = rum.process(nz.next()) * 3.0;
                song.L[i] += float(r); song.R[i] += float(r * 0.2);
            }
        }
        auto e = engine();
        e->song().edit([](SongMap& m) { m.addMark(0, 1); m.addMark(8, 2); m.addMark(24, 3); m.addMark(40, 2); m.addMark(56, 3); });
        e->analyser().reset();
        e->analyser().stats().clear();
        e->setListening(true);
        Values w = v;
        w[cRumble] = 0.0f;
        run(*e, song, w, 512);
        e->setListening(false);
        Values sec[kNumKeys];
        for (int k = 0; k < kNumKeys; ++k) sec[k] = e->valuesFor(k, w);
        const Result res = assist(e->analyser().stats(), genreTarget(0), w, sec, Taste{}, false);
        bool harshChorus = false, rumble = false, mono = false, loud = false;
        std::string list;
        for (const auto& m : res.moves) {
            list += m.module + "/" + defaultKeyName(m.key) + ":" + m.what + "; ";
            if (m.param == cHarsh && m.key == 3) harshChorus = true;
            if (m.param == cRumble) rumble = true;
            if (m.param == sMono) mono = true;
            if (m.param == lTarget && m.key == 0) loud = true;
        }
        check(res.any && res.keyHeard[3] && res.keyHeard[2], "the listening pass hears each section");
        check(harshChorus, "a harsh chorus gets a chorus-only HARSH move", list);
        check(rumble, "sub rumble gets a RUMBLE move");
        check(mono, "side bass gets a BASS MONO move");
        check(loud && res.knee.known, "the pass finds the loudness knee and suggests a target", "knee " + num(res.knee.kneeLufs, 1) + " LUFS");
        bool harshVerse = false;
        for (const auto& m : res.moves) if (m.param == cHarsh && m.key == 2) harshVerse = true;
        check(!harshVerse, "the verses, which aren't harsh, get no HARSH move");
        int looks = 0;
        for (const auto& c : res.checks) looks += c.ok ? 0 : 1;
        check(looks >= 2, "the checks flag rumble and the off-centre low end", std::to_string(looks) + " to look at");
    }

    // ---- section lift: pushed hard, the limiter flattens the loud choruses against the verses;
    // the assistant gives the verses their own, lower target to win some of that back.
    {
        auto e = engine();
        e->song().edit([](SongMap& m) { m.addMark(0, 1); m.addMark(8, 2); m.addMark(24, 4); m.addMark(40, 2); m.addMark(56, 4); });
        e->setMeasuredInput(lufs(s.L, s.R), true);
        e->analyser().reset();
        e->analyser().stats().clear();
        e->setListening(true);
        Values w = v;
        w[lTarget] = -7.0f; w[lGuard] = 0.0f; w[lClip] = 0.0f;
        run(*e, s, w, 512);
        e->setListening(false);
        Values sec[kNumKeys];
        for (int k = 0; k < kNumKeys; ++k) sec[k] = e->valuesFor(k, w);
        const auto c = contrastFrom(e->analyser().stats());
        const Result res = assist(e->analyser().stats(), genreTarget(0), w, sec, Taste{}, false);
        const Move* lift = nullptr;
        for (const auto& m : res.moves) if (m.kind == mkLift && m.key == 2) lift = &m;
        check(c.loud == 4 && c.ok[2] && c.mixGap[2] - c.masterGap[2] > 0.8, "the limiter evens out the choruses against the verses",
              "mix " + num(c.mixGap[2], 1) + " dB apart, master " + num(c.masterGap[2], 1) + " dB");
        check(lift && lift->full < w[lTarget] - 0.3f && lift->full >= w[lTarget] - 3.0f, "the verses get their own, lower TARGET",
              lift ? lift->what + ": " + lift->why : std::string("no move"));
    }

    // ---- cost
    {
        auto e = engine();
        Values w = v;
        w[spIn] = 1.0f;
        const auto a = std::chrono::steady_clock::now();
        const Run r = run(*e, s, w, 512);
        const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - a).count();
        const double audio = double(r.L.size()) / kFs;
        std::printf("info  real time: %.1f%% of one core (everything on, 48 kHz)\n", 100.0 * secs / audio);
    }

    const double total = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    std::printf("\n%d of %d checks passed (%.1f s)\n", checks - failures, checks, total);
    return failures == 0 ? 0 : 1;
}
