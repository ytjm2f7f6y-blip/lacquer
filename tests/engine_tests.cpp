// Lacquer's engine tests: run with ctest, or on their own.
//
// Each check prints one line; the run fails if any check fails.

#include "lq_assistant.hpp"
#include "lq_engine.hpp"
#include "testsig.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <memory>
#include <string>
#include <thread>

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

// Everything out but what's being tested.
lq::Values bare() {
    using namespace lq;
    Values w;
    for (int p : {cleanIn, toneIn, dynIn, colIn, stIn, spIn, loudIn}) w[p] = 0.0f;
    w[oDither] = 0.0f;
    return w;
}

// The level (dB) of a sine through an engine, on the mid (L = R) or the side (L = -R), once settled.
double sineGain(lq::Engine& e, const lq::Values& v, double f, bool side, double secs = 1.0) {
    const int n = int(secs * kFs);
    std::vector<float> L(static_cast<size_t>(n)), R(static_cast<size_t>(n));
    for (int i = 0; i < n; ++i) { const float x = float(0.1 * std::sin(2 * 3.14159265358979 * f * i / kFs)); L[size_t(i)] = x; R[size_t(i)] = side ? -x : x; }
    lq::Transport t;
    for (int i = 0; i < n; i += 512) e.process(L.data() + i, R.data() + i, std::min(512, n - i), v, t);
    double a = 0;
    const int from = n / 2;
    for (int i = from; i < n; ++i) a += double(L[size_t(i)]) * L[size_t(i)] + double(R[size_t(i)]) * R[size_t(i)];
    return 10 * std::log10(a / (n - from) / (0.01)) ;
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

    // ---- TONE: four bands at their own frequencies, each on the stereo, the mid or the side
    {
        Values w = bare();
        w[toneIn] = 1.0f; w[tMode] = 0.0f;
        w[tLow] = 0; w[tPres] = 0; w[tAir] = 0;
        w[tMud] = -4.0f; w[tMudHz] = 300.0f;
        auto e = engine();
        const double at300 = sineGain(*e, w, 300, false), at3k = sineGain(*e, w, 3000, false);
        check(std::abs(at300 + 4.0) < 0.15 && std::abs(at3k) < 0.4, "MUD cuts at its own frequency and leaves the top alone", num(at300, 2) + " dB at 300 Hz, " + num(at3k, 2) + " dB at 3 kHz");
        w[tMud] = 0.0f; w[tAir] = 6.0f; w[tAirHz] = 8000.0f; w[tAirPl] = 2.0f;   // air on the side only
        auto e2 = engine(), e3 = engine();
        const double mid = sineGain(*e2, w, 14000, false), side = sineGain(*e3, w, 14000, true);
        check(std::abs(mid) < 0.05 && side > 4.0, "AIR on the side lifts the side and leaves the mid alone", "mid " + num(mid, 2) + " dB, side " + num(side, 2) + " dB");
        // Linear phase: the same response, a fixed delay.
        Values lw = w;
        lw[tMud] = -4.0f; lw[tAirPl] = 0.0f; lw[tPhase] = 1.0f;
        Values nw = lw; nw[tPhase] = 0.0f;
        bool same = true;
        std::string worst;
        for (double f : {60.0, 300.0, 1000.0, 3000.0, 12000.0}) {
            auto a = engine(), b = engine();
            const double gl = sineGain(*a, lw, f, false, 1.5), gn = sineGain(*b, nw, f, false, 1.5);
            if (std::abs(gl - gn) > 0.3) { same = false; worst += num(f, 0) + " Hz: " + num(gl, 2) + " vs " + num(gn, 2) + "; "; }
        }
        auto a = engine();
        check(same, "linear phase draws the same curve as natural phase", worst);
        check(a->latencyFor(true) - a->latencyFor(false) > 1000 && a->latencyFor(true) - a->latencyFor(false) < int(0.06 * kFs),
              "and costs a fixed delay", std::to_string(a->latencyFor(true) - a->latencyFor(false)) + " samples more");
        // TONE's impulse in linear phase is symmetric: nothing smeared ahead of or behind anything else.
        {
            lq::LinearTone lt;
            lt.prepare(kFs);
            const ToneParams tp = Engine::toneParamsFrom(lw);
            float wl[32] = {}, wr[32] = {};
            for (int k = 0; k < 400; ++k) { lt.process(wl, wr, 32, tp); std::this_thread::sleep_for(std::chrono::microseconds(500)); }
            const int n = 16384;
            std::vector<float> L(static_cast<size_t>(n), 0.0f), R(static_cast<size_t>(n), 0.0f);
            L[100] = R[100] = 0.5f;
            for (int i = 0; i < n; i += 32) lt.process(L.data() + i, R.data() + i, 32, tp);
            int pk = 0;
            for (int i = 0; i < n; ++i) if (std::abs(L[size_t(i)]) > std::abs(L[size_t(pk)])) pk = i;
            double asym = 0, tot = 0;
            for (int k = 1; k < 1500 && pk - k >= 0 && pk + k < n; ++k) { asym += std::abs(L[size_t(pk - k)] - L[size_t(pk + k)]); tot += std::abs(L[size_t(pk + k)]); }
            check(pk - 100 == lt.latency() && tot > 0 && asym / tot < 0.02, "and its impulse is symmetric round the delay (linear phase)",
                  "peak at +" + std::to_string(pk - 100) + " samples, asymmetry " + num(asym / std::max(tot, 1e-12), 4));
        }
    }

    // ---- GLUE's attack: a fast one takes the front off the hits
    {
        Values w = bare();
        w[dynIn] = 1.0f; w[dGlue] = 8.0f; w[dPunch] = 0.0f; w[dEven] = 0.0f;
        auto crest = [&](int attack) {
            auto e = engine();
            Values x = w; x[dAttack] = float(attack);
            const Run r = run(*e, s, x, 512);
            double pk = 0, ms = 0;
            for (size_t i = size_t(4 * kFs); i < r.L.size(); ++i) { pk = std::max(pk, double(std::abs(r.L[i]))); ms += double(r.L[i]) * r.L[i]; }
            return lq::gainToDb(pk) - 10 * std::log10(ms / double(r.L.size() - size_t(4 * kFs)));
        };
        const double fast = crest(0), slow = crest(4);
        check(fast < slow - 0.5, "GLUE with a 1 ms attack takes more off the hits than with 80 ms", "crest " + num(fast, 1) + " vs " + num(slow, 1) + " dB");
    }

    // ---- the limiter's styles: all under the ceiling; DENSE gets more loudness from the same drive
    {
        double loud[3] = {};
        bool under = true;
        for (int st = 0; st < 3; ++st) {
            auto e = engine();
            Values w = v;
            w[lStyle] = float(st); w[lGuard] = 0.0f; w[lTarget] = -7.0f;
            e->setMeasuredInput(lufs(s.L, s.R), true);
            const Run r = run(*e, s, w, 512);
            loud[st] = lufs(r.L, r.R, size_t(2 * kFs));
            double tp = -200;
            lp::TruePeak tpm[2];
            for (size_t i = 0; i < r.L.size(); ++i) tp = std::max({tp, lq::gainToDb(tpm[0].process(r.L[i])), lq::gainToDb(tpm[1].process(r.R[i]))});
            under = under && tp <= w[lCeiling] + 0.15;
        }
        check(under, "every limiter style keeps the true peak under the ceiling");
        check(loud[2] > loud[0] + 0.2, "DENSE makes more of the same drive than CLEAN", "clean " + num(loud[0], 2) + ", punchy " + num(loud[1], 2) + ", dense " + num(loud[2], 2) + " LUFS");
    }

    // ---- COMPARE's MIX: the song as it came in, lined up and as loud as the master
    {
        auto a = engine(), b = engine();
        Values w = v, bw = v;
        bw[oCompare] = 0.0f;
        a->setMeasuredInput(lufs(s.L, s.R), true);
        b->setMeasuredInput(lufs(s.L, s.R), true);
        const Run ra = run(*a, s, w, 512), rb = run(*b, s, bw, 512);
        const double la = lufs(ra.L, ra.R, size_t(8 * kFs)), lb = lufs(rb.L, rb.R, size_t(8 * kFs));
        // Lined up: the output is the input delayed by the latency, times one gain.
        const size_t lat = size_t(b->latency());
        double xy = 0, xx = 0, yy = 0;
        for (size_t i = size_t(10 * kFs); i < size_t(20 * kFs); ++i) { const double x = s.L[i - lat], y = rb.L[i]; xy += x * y; xx += x * x; yy += y * y; }
        const double corr = xy / std::sqrt(xx * yy + 1e-30);
        check(std::abs(la - lb) < 1.0 && corr > 0.99, "COMPARE MIX plays the mix in time with the master, at the master's loudness",
              "master " + num(la, 1) + ", mix " + num(lb, 1) + " LUFS, correlation " + num(corr, 4));
        auto c = engine();
        c->setMeasuredInput(lufs(s.L, s.R), true);
        const Run rc = run(*c, s, bw, 512, true), rd = ra;
        double d = 0;
        for (size_t i = 0; i < rc.L.size(); ++i) d = std::max(d, double(std::abs(rc.L[i] - rd.L[i])));
        check(d < 1e-5, "and never reaches a bounce");

        // REF: a reference song, 9 dB quieter than the master, played in its place at the master's loudness.
        RefAudio ref;
        ref.L.resize(size_t(30 * kFs)); ref.R.resize(ref.L.size());
        uint32_t seed = 3;
        for (size_t i = 0; i < ref.L.size(); ++i) {
            seed = seed * 1664525u + 1013904223u;
            const float x = float(int32_t(seed)) / 2147483648.0f * 0.05f * float(1 + 0.5 * std::sin(i * 0.0003));
            ref.L[i] = x; ref.R[i] = 0.8f * x;
        }
        ref.lufs = lufs(ref.L, ref.R);
        Values rw = v;
        rw[oCompare] = 2.0f;
        auto e = engine();
        e->setMeasuredInput(lufs(s.L, s.R), true);
        e->setReference(&ref);
        const Run re = run(*e, s, rw, 512);
        const double lr = lufs(re.L, re.R, size_t(12 * kFs));
        // (The engine works in sub-blocks of kSub samples, so the reference comes out a sub-block late here.)
        double rcorr = 0;
        for (size_t lag = 0; lag <= 64; ++lag) {
            double rxy = 0, rxx = 0, ryy = 0;
            for (size_t i = size_t(12 * kFs); i < size_t(20 * kFs); ++i) { const double x = ref.L[i - lag], y = re.L[i]; rxy += x * y; rxx += x * x; ryy += y * y; }
            rcorr = std::max(rcorr, rxy / std::sqrt(rxx * ryy + 1e-30));
        }
        // (Matched to the master's integrated loudness over the play so far, the whole song by the end.)
        const double lm = lufs(ra.L, ra.R);
        check(std::abs(lr - lm) < 1.5 && rcorr > 0.99 && ref.lufs < lr - 6,
              "COMPARE REF plays the reference in the master's place, at the master's loudness",
              "reference " + num(ref.lufs, 1) + " LUFS as it is, " + num(lr, 1) + " as heard; the master " + num(lm, 1));
        auto f = engine();
        f->setMeasuredInput(lufs(s.L, s.R), true);
        f->setReference(&ref);
        const Run rf = run(*f, s, rw, 512, true);
        double dr = 0;
        for (size_t i = 0; i < rf.L.size(); ++i) dr = std::max(dr, double(std::abs(rf.L[i] - ra.L[i])));
        check(dr < 1e-5, "and REF never reaches a bounce either");
    }

    // ---- AS STREAMED: turned to Spotify's -14 or Apple Music's -16, as each service does it
    {
        check(std::abs(Engine::streamGain(-14, -20, -5) - 4.0) < 1e-9 && std::abs(Engine::streamGain(-14, -9, -1) + 5.0) < 1e-9,
              "a quiet master is lifted only as far as its peaks allow (Spotify's own example: -20 LUFS peaking at -5 plays at -16)");
        Values w = v;
        w[lTarget] = -10.0f;
        auto a = engine(), b = engine(), c = engine();
        for (auto* e : {a.get(), b.get(), c.get()}) { e->setMeasuredInput(lufs(s.L, s.R), true); e->setKnee(-6, true); }
        const Run ra = run(*a, s, w, 512);
        Values ws = w;
        ws[oStream] = 1.0f;
        const Run rb = run(*b, s, ws, 512);
        const double sp = b->meters().spotifyDb.load(), ap = b->meters().appleDb.load();
        const double la = lufs(ra.L, ra.R, size_t(14 * kFs)), lb = lufs(rb.L, rb.R, size_t(14 * kFs));
        check(std::abs(sp - (-14 - la)) < 0.8 && std::abs(ap - (-16 - la)) < 0.8 && std::abs((lb - la) - sp) < 0.6,
              "AS STREAMED SPOTIFY turns a loud master down to -14 LUFS, and says what Apple Music would do",
              "master " + num(la, 1) + " LUFS; Spotify " + num(sp, 1) + " dB, Apple " + num(ap, 1) + " dB; heard at " + num(lb, 1));
        const Run rc = run(*c, s, ws, 512, true);
        double d = 0;
        for (size_t i = 0; i < rc.L.size(); ++i) d = std::max(d, double(std::abs(rc.L[i] - ra.L[i])));
        check(d < 1e-5, "and AS STREAMED never reaches a bounce");
    }

    // ---- the automatic ceiling: GUARD gives a master louder than -14 LUFS -2 dBTP, as Spotify asks
    {
        auto peakOf = [&](const Run& r) {
            lp::TruePeak tpm[2];
            double tp = -120;
            for (size_t i = size_t(4 * kFs); i < r.L.size(); ++i) tp = std::max({tp, lq::gainToDb(tpm[0].process(r.L[i])), lq::gainToDb(tpm[1].process(r.R[i]))});
            return tp;
        };
        auto at = [&](float target, bool guard, double& ceil) {
            auto e = engine();
            e->setMeasuredInput(lufs(s.L, s.R), true);
            e->setKnee(-6, true);
            Values w = v;
            w[lTarget] = target; w[lGuard] = guard ? 1.0f : 0.0f; w[oDither] = 0.0f;
            const Run r = run(*e, s, w, 512);
            ceil = e->meters().ceilingDb.load();
            return peakOf(r);
        };
        double c1 = 0, c2 = 0, c3 = 0;
        const double loudGuard = at(-9.0f, true, c1), loudOpen = at(-9.0f, false, c2);
        at(-15.0f, true, c3);
        check(std::abs(c1 + 2) < 0.01 && loudGuard <= -2 + 0.15 && std::abs(c2 + 1) < 0.01 && loudOpen > -1.6 && std::abs(c3 + 1) < 0.01,
              "a loud master gets a -2 dBTP ceiling with GUARD on, -1 without it, and a quiet one keeps -1",
              "-9 LUFS: peaks " + num(loudGuard, 2) + " dBTP with GUARD, " + num(loudOpen, 2) + " without");
    }

    // ---- the bar map: each bar counts once, so a loop of the chorus can't pass for the song
    {
        auto e = engine();
        Values w = v;
        w[lGuard] = 0.0f;
        auto play = [&](size_t from, size_t to) {
            lq::Transport t;
            t.playing = true; t.hasPpq = true; t.bpm = s.bpm;
            std::vector<float> L(s.L.begin() + long(from), s.L.begin() + long(to)), R(s.R.begin() + long(from), s.R.begin() + long(to));
            for (size_t i = 0; i < L.size(); i += 512) {
                const int n = int(std::min<size_t>(512, L.size() - i));
                t.ppq = double(from + i) / kFs * s.bpm / 60.0;
                e->process(L.data() + i, R.data() + i, n, w, t);
            }
            lq::Transport stop;
            float z[64] = {}, z2[64] = {};
            e->process(z, z2, 64, w, stop);
        };
        play(0, s.L.size());
        double whole = 0, cov = 0;
        const bool ok1 = lq::loudnessFromBars(e->bars(), 18, whole, cov);
        const double byPass = e->calibratedInput();
        e->setMeasuredInput(whole, true);
        play(size_t(12 * kFs), size_t(20 * kFs));   // the first chorus, again
        play(size_t(12 * kFs), size_t(20 * kFs));
        double after = 0, cov2 = 0;
        lq::loudnessFromBars(e->bars(), 18, after, cov2);
        e->clearPass();
        play(size_t(12 * kFs), size_t(20 * kFs));
        const double loopOnly = e->calibratedInput();
        check(ok1 && cov > 0.9 && std::abs(whole - byPass) < 1.0, "the bar map learns the song's loudness from a full play",
              num(whole, 2) + " LUFS (a pass says " + num(byPass, 2) + "), " + num(cov * 100, 0) + "% of the song");
        check(std::abs(after - whole) < 0.3 && std::abs(loopOnly - whole) > 0.5, "looping the chorus doesn't move it",
              "after the loop " + num(after, 2) + " LUFS; the loop alone would say " + num(loopOnly, 2));
        auto f = engine();
        std::swap(e, f);
        play(size_t(12 * kFs), size_t(20 * kFs));
        double part = 0, cov3 = 0;
        lq::loudnessFromBars(e->bars(), 18, part, cov3);
        check(cov3 > 0.15 && cov3 < 0.35, "a loop on its own says how little of the song it is", num(cov3 * 100, 0) + "% of the song heard");
    }

    // ---- not crushing a song learnt from its quiet start: the safety hold
    {
        // A full-level song whose intro is 26 dB under the rest.
        auto q = songOf(1.0);
        for (size_t i = 0; i < size_t(4 * kFs); ++i) { q.L[i] *= 0.05f; q.R[i] *= 0.05f; }
        auto runOnce = [&](bool hold) {
        auto e = engine();
        Values w = v;
        w[lTarget] = -10.0f; w[lGuard] = 0.0f;
        lq::Transport t;
        t.playing = true; t.hasPpq = true; t.bpm = s.bpm;
        auto play = [&](size_t from, size_t to, std::vector<float>* outL = nullptr) {
            std::vector<float> L(q.L.begin() + long(from), q.L.begin() + long(to)), R(q.R.begin() + long(from), q.R.begin() + long(to));
            for (size_t i = 0; i < L.size(); i += 512) {
                const int n = int(std::min<size_t>(512, L.size() - i));
                t.ppq = double(from + i) / kFs * s.bpm / 60.0;
                e->process(L.data() + i, R.data() + i, n, w, t);
            }
            if (outL) *outL = L;
            lq::Transport stop;
            float z[64] = {}, z2[64] = {};
            e->process(z, z2, 64, w, stop);
            return L;
        };
        // Only the quiet intro, then calibrated from it alone: the whole song now looks quiet.
        play(0, size_t(4 * kFs));
        double m = 0, cov = 0;
        lq::loudnessFromBars(e->bars(), 2, m, cov);
        e->setMeasuredInput(m, true);
        e->setSafetyHold(hold);
        std::vector<float> outL;
        play(0, q.L.size(), &outL);
        // How hard the limiter works on the choruses.
        double gr = 0;
        float z[512] = {}, z2[512] = {};
        (void)z; (void)z2;
        std::vector<float> l(outL.begin() + long(13 * kFs), outL.begin() + long(19.5 * kFs)), r = l;
        gr = lufs(l, r) - 3.01;   // one channel, as if both
        double pk = 0;
        for (float x : l) pk = std::max(pk, double(std::abs(x)));
        return std::make_pair(gr, lq::gainToDb(pk) - gr);   // loudness, and peak-to-loudness (how much punch is left)
        };
        const auto with = runOnce(true), without = runOnce(false);
        check(with.second > without.second + 3.0 && std::abs(with.first + 10.0) < 1.5, "a song learnt from its quiet intro isn't crushed when the loud part comes",
              "the chorus at " + num(with.first, 1) + " LUFS with " + num(with.second, 1) + " dB of punch left; without the hold " + num(without.first, 1) + " LUFS and " +
              num(without.second, 1) + " dB");
    }
    {   // GUARD keeps the target to -13 until it has heard the song.
        auto e = engine();
        Values w = v;
        w[lTarget] = -8.0f; w[lGuard] = 1.0f;
        e->setMeasuredInput(lufs(s.L, s.R), true);
        const Run r = run(*e, s, w, 512);
        const double got = lufs(r.L, r.R, size_t(2 * kFs));
        check(e->meters().guarding.load() && got < -11.5, "before it knows the song, GUARD holds the target at -13", num(got, 1) + " LUFS for a target of -8");
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

    // ---- calibration against released records: an already-limited mix, a song's natural offset,
    // and the targets' shape
    {
        // 40 s of drum-like hits over a bed: an unlimited mix, then the same pushed into a hard ceiling.
        std::vector<float> L(size_t(40 * kFs)), R(L.size());
        uint32_t seed = 7;
        auto rnd = [&] { seed = seed * 1664525u + 1013904223u; return float(int32_t(seed)) / 2147483648.0f; };
        double env = 0;
        for (size_t i = 0; i < L.size(); ++i) {
            if (i % size_t(kFs / 4) == 0) env = 0.3 + 0.7 * std::abs(rnd());   // a hit every quarter second, uneven
            env *= 0.9997;
            const float x = float(env) * rnd() * 0.5f + 0.05f * rnd();
            L[i] = x; R[i] = 0.9f * x + 0.02f * rnd();
        }
        auto pile = [&](float drive) {
            Analyser a;
            a.prepare(kFs);
            std::vector<float> l = L, r = R;
            for (size_t i = 0; i < l.size(); ++i) {
                l[i] = std::clamp(l[i] * drive, -0.9f, 0.9f);
                r[i] = std::clamp(r[i] * drive, -0.9f, 0.9f);
            }
            for (size_t i = 0; i < l.size(); i += 512) a.push(l.data() + i, r.data() + i, int(std::min<size_t>(512, l.size() - i)), 0);
            return peakPile(a.stats().key[0]);
        };
        const double open = pile(1.0f), limited = pile(6.0f);
        check(open < 0.12 && limited > 0.12, "a mix that comes in limited is told so, and an open one isn't",
              "peaks near the top: " + num(100 * open, 0) + "% open, " + num(100 * limited, 0) + "% limited");

        // The same open mix with the small offset kicks and bass leave (-75 dBFS), then a real one (-40).
        auto dcFlag = [&](float off) {
            auto e = engine();
            testsig::Song t;
            t.L = L; t.R = R; t.bpm = 120;
            for (auto& x : t.L) x += off;
            for (auto& x : t.R) x += off;
            e->setListening(true);
            run(*e, t, v, 512);
            e->setListening(false);
            Values sec[kNumKeys];
            for (int k = 0; k < kNumKeys; ++k) sec[k] = e->valuesFor(k, v);
            const Result res = assist(e->analyser().stats(), genreTarget(0), v, sec, Taste{}, false);
            for (const auto& c : res.checks) if (c.name == "DC OFFSET") return !c.ok;
            return false;
        };
        const bool small = dcFlag(0.0002f), real = dcFlag(0.01f);
        check(!small && real, "DC OFFSET ignores a song's natural offset and flags a real one");

        // The targets fall away above the mids, as released records do, and the corridor is widest at the ends.
        const Target t = genreTarget(0);
        double mid = 0, top = 0; int nm = 0, nt = 0;
        for (int b = 0; b < kBands; ++b) {
            const double f = bandCentre(b);
            if (f >= 200 && f <= 2000) { mid += t.curve[b]; ++nm; }
            if (f >= 8000 && f <= 12500) { top += t.curve[b]; ++nt; }
        }
        top /= nt; mid /= nm;
        int b40 = 0, b1k = 0;
        for (int b = 0; b < kBands; ++b) { if (bandCentre(b) < 40) b40 = b; if (bandCentre(b) < 1000) b1k = b; }
        check(top < mid - 5 && top > mid - 11 && t.width[b40] > t.width[b1k], "the targets sit the top end under the mids, as records do",
              "8 to 12.5 kHz " + num(top - mid, 1) + " dB against 200 Hz to 2 kHz");
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
