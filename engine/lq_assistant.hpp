#pragma once
// The assistant: from a listening pass to targets, suggested moves, the loudness knee and the
// checks. Runs on the message thread, after the pass.
//
// The genre curves are starting points worked out from how finished masters in each style tend to
// sit against pink noise. They are meant to be tuned by ear, and REFS or MINE replace them.

#include "lq_analysis.hpp"

#include <cstring>
#include <string>
#include <vector>

namespace lq {

// ---------------------------------------------------------------------------
// Targets: a curve per sixth-octave band (dB, pink reads flat, 200 Hz-2 kHz average at 0) and
// how far either side of it a track can sit and still be fine.
struct Target {
    double curve[kBands]{};
    double width[kBands]{};
    std::string name = "indie rock";
};

struct GenreShape { const char* name; double lowDb, lowHz, lowMidDb, presDb, airDb, airHz; };
inline const GenreShape* genres(int& n) {
    static const GenreShape g[] = {
        {"indie rock", 2.0, 70, 0.3, 0.0, -2.6, 9000},
        {"pop", 2.6, 60, -0.4, 0.6, -1.6, 10000},
        {"hip-hop", 4.5, 55, -0.6, 0.0, -2.2, 9500},
        {"electronic", 4.0, 50, -0.8, 0.2, -1.4, 11000},
        {"acoustic", 0.8, 90, 0.4, 0.2, -3.0, 8500},
        {"jazz", 0.6, 100, 0.6, -0.3, -3.4, 8000},
    };
    n = int(sizeof(g) / sizeof(g[0]));
    return g;
}

inline Target genreTarget(int i) {
    int n = 0;
    const auto* g = genres(n);
    const auto& s = g[std::max(0, std::min(i, n - 1))];
    Target t;
    t.name = s.name;
    for (int b = 0; b < kBands; ++b) {
        const double f = bandCentre(b), o = std::log2(f / 1000.0);
        double c = s.lowDb * std::exp(-std::pow(std::log2(f / s.lowHz) / 1.4, 2));
        c += s.lowMidDb * std::exp(-std::pow(std::log2(f / 350.0) / 1.0, 2));
        c += s.presDb * std::exp(-std::pow(std::log2(f / 2800.0) / 1.0, 2));
        c += s.airDb * std::max(0.0, std::log2(f / s.airHz)) / 1.0;
        if (f < 40) c -= 3.0 * std::log2(40.0 / f) * 1.4;                  // masters roll off under the kick
        if (f > 16000) c -= 4.0 * std::log2(f / 16000.0);
        t.curve[b] = c;
        t.width[b] = 1.3 + 0.7 * std::min(1.0, std::pow(std::abs(o + 0.5) / 5.0, 2));
    }
    // The curve's own 200 Hz-2 kHz average at 0, like the measured ones.
    double ref = 0; int k = 0;
    for (int b = 0; b < kBands; ++b) if (bandCentre(b) >= 200 && bandCentre(b) <= 2000) { ref += t.curve[b]; ++k; }
    for (int b = 0; b < kBands; ++b) t.curve[b] -= ref / k;
    return t;
}

// A target made from measured curves (reference tracks, or your own masters).
inline Target averageTarget(const std::vector<std::vector<double>>& curves, const std::string& name) {
    Target t = genreTarget(0);
    t.name = name;
    if (curves.empty()) return t;
    for (int b = 0; b < kBands; ++b) {
        double s = 0, s2 = 0;
        for (const auto& c : curves) { s += c[size_t(b)]; s2 += c[size_t(b)] * c[size_t(b)]; }
        const double m = s / double(curves.size()), var = std::max(0.0, s2 / double(curves.size()) - m * m);
        t.curve[b] = m;
        t.width[b] = std::max(t.width[b], std::min(3.5, 1.0 + std::sqrt(var)));
    }
    return t;
}

// ---------------------------------------------------------------------------
// How the master's controls bend a curve (a static picture, for the balance screen and the moves).
inline double toneResponse(double f, const Values& v) {
    auto shelfLo = [&](double fc, double g) { const double gl = dbToGain(g); const double r = 1.0 / (1.0 + (f / fc) * (f / fc)); return gainToDb(1.0 + (gl - 1.0) * std::sqrt(r)); };
    auto shelfHi = [&](double fc, double g) { const double gl = dbToGain(g); const double r = 1.0 / (1.0 + (fc / f) * (fc / f)); return gainToDb(1.0 + (gl - 1.0) * std::sqrt(r)); };
    auto bell = [&](double fc, double g, double oct) { return g * std::exp(-std::pow(std::log2(f / fc) / oct, 2)); };
    double d = 0;
    if (v[toneIn] > 0.5f) d += shelfLo(110, v[tLow]) + bell(2500, v[tPres], 1.1) + shelfHi(10000, v[tAir]);
    if (v[cleanIn] > 0.5f) {
        d += bell(3400, -0.35 * v[cHarsh], 0.7);
        if (v[cRumble] > 1) { const double r = f / v[cRumble]; d += 10 * std::log10(r * r * r * r / (1 + r * r * r * r)); }
    }
    return d;
}

// ---------------------------------------------------------------------------
// Moves.
struct Move {
    int key = 0;            // 0 the whole song, 1..8 one section
    int param = 0;
    bool absolute = false;  // sets a value (rumble's frequency) rather than adding to it
    float full = 0;         // the whole suggested change (or the value, for absolute moves)
    float extraParam = -1, extraValue = 0;   // a second control it sets (a module's IN)
    int conf = 50;
    std::string module, what, why;
    bool taken = false;
    float before = 0, beforeExtra = 0, applied = 0;   // what taking it changed, for skipping it again
    bool beforeOwn = false, beforeExtraOwn = false;   // the section had its own value before (else it followed the song)
    int kind = 0;           // for LEARN: which kind of move
};
enum MoveKind { mkLow, mkPres, mkAir, mkHarsh, mkRumble, mkMono, mkWidth, mkLoud, mkGlue, mkRes, mkLift, kNumMoveKinds };

struct Taste {   // how often each kind of move was taken and skipped
    float taken[kNumMoveKinds]{}, skipped[kNumMoveKinds]{};
    double lean(int k) const { const double t = taken[k] + 1, s = skipped[k] + 1; return t / (t + s); }
};

struct Check { std::string name, what; bool ok = true; };

struct Knee {
    bool known = false;
    double kneeLufs = -6;
    double inputLufs = -20;
    double kept[101]{};   // transients kept (%) from -16 to -6 LUFS in 0.1 steps
};

inline double knob(float x) { return std::round(x * 10.0) / 10.0; }
inline std::string fmt1(double x, bool sign = false) {
    char b[32];
    std::snprintf(b, sizeof b, sign ? "%+.1f" : "%.1f", x);
    std::string s = b;
    if (s[0] == '-') s = "\xe2\x88\x92" + s.substr(1);   // a real minus
    return s;
}
// The section keys' names until you change them.
inline std::string defaultKeyName(int k) {
    static const char* n[kNumKeys] = {"song", "intro", "verse", "pre-chorus", "chorus", "bridge", "solo", "breakdown", "outro"};
    return n[std::max(0, std::min(k, kNumKeys - 1))];
}
inline std::string plural(const std::string& s) {
    auto ends = [&](const char* e) { const size_t n = std::strlen(e); return s.size() >= n && s.compare(s.size() - n, n, e) == 0; };
    if (s.empty()) return s;
    if (ends("s") || ends("x") || ends("ch") || ends("sh")) return s + "es";
    return s + "s";
}

// What the limiter does to one section, from the pass: its loudness going in and coming out, and
// the drive it was given. loss = in + drive - out.
struct SectionLevel { bool ok = false; double in = -70, out = -70, drive = 0, loss = 0, seconds = 0; };
inline SectionLevel sectionLevel(const SongStats& st, int k) {
    SectionLevel s;
    const auto& a = st.limIn[k];
    const auto& b = st.out[k];
    s.seconds = std::min(a.seconds, b.seconds);
    if (s.seconds < 6) return s;
    s.in = a.integrated(); s.out = b.integrated(); s.drive = b.gatedAuxMean();
    s.ok = s.in > -60 && s.out > -60;
    s.loss = s.in + s.drive - s.out;
    return s;
}
// The loudest heard section, and how far each other one sits under it, in the mix and after the limiter.
struct Contrast { int loud = -1; double mixGap[kNumKeys]{}, masterGap[kNumKeys]{}, drive[kNumKeys]{}, seconds[kNumKeys]{}; bool ok[kNumKeys]{}; };
inline Contrast contrastFrom(const SongStats& st) {
    Contrast c;
    SectionLevel lv[kNumKeys];
    for (int k = 1; k < kNumKeys; ++k) {
        lv[k] = sectionLevel(st, k);
        if (lv[k].ok && lv[k].seconds >= 8 && (c.loud < 0 || lv[k].in > lv[c.loud].in)) c.loud = k;
    }
    if (c.loud < 0) return c;
    c.drive[c.loud] = lv[c.loud].drive; c.seconds[c.loud] = lv[c.loud].seconds;
    for (int k = 1; k < kNumKeys; ++k) {
        if (!lv[k].ok || k == c.loud) continue;
        c.ok[k] = true;
        c.drive[k] = lv[k].drive; c.seconds[k] = lv[k].seconds;
        c.mixGap[k] = lv[c.loud].in - lv[k].in;
        c.masterGap[k] = lv[c.loud].out - lv[k].out;
    }
    return c;
}

// The loudness knee: how much of the drums' snap survives as the drive goes up, from the
// histogram of 10 ms peaks. A window loses what the limiter takes off it past the first dB.
inline Knee kneeFrom(const KeyStats& song, double ceilingDb, double clip) {
    Knee k;
    if (song.windows < 50) return k;
    k.inputLufs = song.integrated();
    const double clipHelp = clip / 10.0 * 1.5;   // the clipper takes the sharpest peaks off cleanly
    double kneeAt = -6;
    bool found = false;
    for (int i = 0; i <= 100; ++i) {
        const double L = -16 + i * 0.1, drive = L - k.inputLufs;
        double ex = 0;
        for (int b = 0; b < kPkBins; ++b) {
            if (song.pk[b] <= 0) continue;
            const double pk = -70 + b * 0.5 + 0.25;
            const double gr = std::max(0.0, pk + drive - ceilingDb - clipHelp);
            ex += song.pk[b] * std::max(0.0, gr - 1.0);
        }
        ex /= song.windows;
        k.kept[i] = clamp(100.0 - 14.0 * ex, 30.0, 100.0);
        if (!found && ex > 0.45) { kneeAt = L; found = true; }
    }
    k.kneeLufs = found ? kneeAt : -6;
    k.known = true;
    return k;
}

inline int destinationLufs(int dest) { static const int d[4] = {-10, -9, -12, -8}; return d[std::max(0, std::min(dest, 3))]; }

struct Result {
    bool any = false;
    double curve[kNumKeys][kBands]{};   // measured, per key
    bool keyHeard[kNumKeys]{};
    double keySeconds[kNumKeys]{};
    double sideLowDb[kNumKeys]{}, widthDb[kNumKeys]{};
    double subDb = -60, crestDb = 12;
    Knee knee;
    std::vector<Move> moves;
    std::vector<Check> checks;
    std::vector<std::string> mixNotes;
    Contrast contrast;   // the loudest section against the others, in the mix and after the limiter
};

// From the pass and the target to moves, checks and notes.
inline Result assist(const SongStats& st, const Target& tgt, const Values& song, const Values* sectionVals /* [kNumKeys] */,
                     const Taste& taste, bool useTaste, const std::string* names = nullptr /* [kNumKeys] */) {
    Result r;
    auto keyName = [&](int k) { return names ? names[k] : defaultKeyName(k); };
    auto many = [&](int k) { return plural(keyName(k)); };
    const auto& all = st.key[0];
    if (all.frames < 8) return r;
    r.any = true;
    for (int k = 0; k < kNumKeys; ++k) {
        r.keyHeard[k] = st.key[k].frames >= 4;
        r.keySeconds[k] = st.key[k].seconds;
        if (r.keyHeard[k]) balanceCurve(st.key[k], true, r.curve[k]);
        double lowM = 0, lowS = 0, m = 0, s = 0;
        for (int b = 0; b < kBands; ++b) {
            m += st.key[k].mid[b]; s += st.key[k].side[b];
            if (bandCentre(b) < 150) { lowM += st.key[k].mid[b]; lowS += st.key[k].side[b]; }
        }
        r.sideLowDb[k] = 10 * std::log10((lowS + 1e-20) / (lowM + 1e-20));
        r.widthDb[k] = 10 * std::log10((s + 1e-20) / (m + 1e-20));
    }
    auto avgDev = [&](int k, double f0, double f1) {
        double s = 0; int n = 0;
        for (int b = 0; b < kBands; ++b) { const double f = bandCentre(b); if (f >= f0 && f <= f1) { s += r.curve[k][b] - tgt.curve[b]; ++n; } }
        return n ? s / n : 0.0;
    };
    auto avgWidth = [&](double f0, double f1) {
        double s = 0; int n = 0;
        for (int b = 0; b < kBands; ++b) { const double f = bandCentre(b); if (f >= f0 && f <= f1) { s += tgt.width[b]; ++n; } }
        return n ? s / n : 1.5;
    };
    auto confFor = [&](double dev, double room, int kind) {
        double c = 45 + 40 * clamp((std::abs(dev) - room) / (room + 1.0), 0.0, 1.0);
        if (useTaste) c *= 0.75 + 0.5 * taste.lean(kind);
        return int(clamp(c, 35.0, 95.0));
    };
    auto add = [&](Move m) { r.moves.push_back(std::move(m)); };

    // TONE: low, presence, air. Moves are capped; more than that belongs in the mix.
    struct ToneBand { int param; int kind; double f0, f1; const char* name; const char* hi; const char* lo; };
    const ToneBand tb[3] = {{tLow, mkLow, 45, 160, "LOW", "The low end sits heavy against the target.", "The low end sits light against the target."},
                            {tPres, mkPres, 1600, 4000, "PRESENCE", "2 to 4 kHz is a touch forward.", "The upper mids sit back."},
                            {tAir, mkAir, 8000, 16000, "AIR", "The top end sits above the target.", "The top end sits under the target."}};
    for (const auto& b : tb) {
        const double dev = avgDev(0, b.f0, b.f1), room = avgWidth(b.f0, b.f1) * 0.7;
        if (std::abs(dev) > room) {
            Move m; m.key = 0; m.param = b.param; m.kind = b.kind; m.module = "TONE";
            m.full = float(clamp(-dev * 0.9, -3.0, 3.0));
            m.extraParam = toneIn; m.extraValue = 1;
            m.what = std::string(b.name) + "  " + fmt1(m.full, true) + " dB";
            m.why = dev > 0 ? b.hi : b.lo;
            m.conf = confFor(dev, room, b.kind);
            add(m);
            if (std::abs(dev) > 4.5) r.mixNotes.push_back(std::string(b.name == std::string("LOW") ? "The low end" : b.name == std::string("AIR") ? "The top end" : "The upper mids") +
                                                          " is " + fmt1(std::abs(dev)) + " dB " + (dev > 0 ? "over" : "under") + " the target. Fix the bulk of it in the mix.");
        }
        // A section that stands apart from the song gets its own move: the two that stand out most
        // (with eight sections, a move for every one that differs a little would bury the useful ones).
        int pick[2] = {-1, -1};
        double score[2] = {0, 0};
        for (int k = 1; k < kNumKeys; ++k) {
            if (!r.keyHeard[k] || r.keySeconds[k] < 8) continue;
            const double dk = avgDev(k, b.f0, b.f1) - avgDev(0, b.f0, b.f1);
            if (std::abs(dk) <= room + 0.4) continue;
            const double sc = std::abs(dk) * std::sqrt(r.keySeconds[k]);
            if (sc > score[0]) { pick[1] = pick[0]; score[1] = score[0]; pick[0] = k; score[0] = sc; }
            else if (sc > score[1]) { pick[1] = k; score[1] = sc; }
        }
        for (int k : pick) {
            if (k < 0) continue;
            const double dk = avgDev(k, b.f0, b.f1) - avgDev(0, b.f0, b.f1);
            {
                Move m; m.key = k; m.param = b.param; m.kind = b.kind; m.module = "TONE";
                m.full = float(clamp(-dk * 0.8, -2.5, 2.5));
                m.what = std::string(b.name) + "  " + fmt1(m.full, true) + " dB";
                m.why = "The " + many(k) + " sit " + (dk > 0 ? "brighter" : "duller") + " than the rest of the song.";
                if (b.param == tLow) m.why = "The " + many(k) + " carry " + (dk > 0 ? "more" : "less") + " low end than the rest of the song.";
                m.conf = confFor(dk, room, b.kind) - 8;
                add(m);
            }
        }
    }

    // HARSH: the 2.5-5 kHz region standing above its neighbours. A section is measured against the
    // other sections (so a harsh chorus stands out from verses that aren't); the song against the target.
    {
        auto harshOf = [&](int k) { return avgDev(k, 2500, 5000) - 0.5 * (avgDev(k, 1000, 2000) + avgDev(k, 6000, 9000)); };
        const double hSong = harshOf(0);
        int sectionMoves = 0;
        for (int k = 1; k < kNumKeys; ++k) {
            if (!r.keyHeard[k]) continue;
            double base = 0, wsum = 0;
            for (int j = 1; j < kNumKeys; ++j)
                if (j != k && r.keyHeard[j]) { base += harshOf(j) * r.keySeconds[j]; wsum += r.keySeconds[j]; }
            if (wsum <= 0) continue;
            const double rel = harshOf(k) - base / wsum;
            if (rel > 1.4) {
                Move m; m.key = k; m.param = cHarsh; m.kind = mkHarsh; m.module = "CLEAN";
                m.full = float(clamp(rel * 1.0, 1.0, 4.0));
                m.extraParam = cleanIn; m.extraValue = 1;
                const float base0 = sectionVals ? sectionVals[k][cHarsh] : song[cHarsh];
                m.what = "HARSH  " + fmt1(base0) + " \xe2\x86\x92 " + fmt1(std::min(10.0f, base0 + m.full));
                m.why = "The " + many(k) + " get harsh around 3 to 4 kHz.";
                m.conf = confFor(rel, 1.4, mkHarsh);
                add(m);
                ++sectionMoves;
            }
        }
        if (hSong > 1.4 && sectionMoves < 2) {
            Move m; m.key = 0; m.param = cHarsh; m.kind = mkHarsh; m.module = "CLEAN";
            m.full = float(clamp(hSong, 1.0, 4.0));
            m.extraParam = cleanIn; m.extraValue = 1;
            m.what = "HARSH  " + fmt1(song[cHarsh]) + " \xe2\x86\x92 " + fmt1(std::min(10.0f, song[cHarsh] + m.full));
            m.why = "The song is harsh around 3 to 4 kHz.";
            m.conf = confFor(hSong, 1.4, mkHarsh);
            add(m);
        }
    }

    // RUMBLE: energy under 30 Hz against the kick's region.
    {
        double sub = 0, kick = 0;
        for (int b = 0; b < kBands; ++b) { const double f = bandCentre(b); if (f < 30) sub += all.mid[b] + all.side[b]; else if (f >= 40 && f < 90) kick += all.mid[b] + all.side[b]; }
        r.subDb = 10 * std::log10((sub + 1e-20) / (kick + 1e-20));
        if (r.subDb > -10 && song[cRumble] < 1) {
            Move m; m.key = 0; m.param = cRumble; m.absolute = true; m.full = 25; m.kind = mkRumble; m.module = "CLEAN";
            m.extraParam = cleanIn; m.extraValue = 1;
            m.what = "RUMBLE  CUT BELOW 25 Hz"; m.why = "Sub rumble under the kick.";
            m.conf = confFor(r.subDb + 10, 2, mkRumble);
            add(m);
        }
    }

    // BASS MONO: side energy in the low end.
    if (r.sideLowDb[0] > -14 && !(song[stIn] > 0.5f && song[sMono] > 1)) {
        Move m; m.key = 0; m.param = sMono; m.absolute = true; m.full = 120; m.kind = mkMono; m.module = "STEREO";
        m.extraParam = stIn; m.extraValue = 1;
        m.what = "BASS MONO  BELOW 120 Hz"; m.why = "The low end drifts off centre.";
        m.conf = confFor(r.sideLowDb[0] + 14, 3, mkMono);
        add(m);
    }

    // WIDTH: a section noticeably narrower than the song gets opened up a little.
    for (int k = 1; k < kNumKeys; ++k) {
        if (!r.keyHeard[k]) continue;
        const double d = r.widthDb[k] - r.widthDb[0];
        if (d < -1.5 && (keyName(k).find("chorus") != std::string::npos || r.keySeconds[k] > 10)) {
            Move m; m.key = k; m.param = sWidth; m.kind = mkWidth; m.module = "STEREO";
            m.full = float(clamp(-d * 5.0, 5.0, 15.0));
            m.extraParam = stIn; m.extraValue = 1;
            const float base = sectionVals ? sectionVals[k][sWidth] : song[sWidth];
            m.what = "IN, WIDTH  " + std::to_string(int(std::lround(base + m.full))) + "%";
            m.why = "The " + many(k) + " sit narrower than the rest. They can open up a little.";
            m.conf = confFor(d, 1.5, mkWidth) - 10;
            add(m);
        }
    }

    // GLUE: a very spiky mix.
    {
        double pkSum = 0, n = 0, top = -120;
        for (int b = kPkBins - 1; b >= 0; --b) if (all.pk[b] > 0) { top = -70 + b * 0.5; break; }
        for (int b = 0; b < kPkBins; ++b) { pkSum += all.pk[b] * (-70 + b * 0.5); n += all.pk[b]; }
        (void)pkSum; (void)n;
        r.crestDb = top - all.integrated();
        if (r.crestDb > 17 && song[dGlue] < 5) {
            Move m; m.key = 0; m.param = dGlue; m.kind = mkGlue; m.module = "DYNAMICS"; m.full = 2;
            m.extraParam = dynIn; m.extraValue = 1;
            m.what = "GLUE  " + fmt1(song[dGlue]) + " \xe2\x86\x92 " + fmt1(song[dGlue] + 2);
            m.why = "The peaks stand well clear of the body of the mix.";
            m.conf = confFor(r.crestDb - 17, 1, mkGlue) - 10;
            add(m);
        }
    }

    // LOUDNESS: as loud as the destination asks, but not past the knee.
    r.knee = kneeFrom(all, song[lCeiling], song[lClip]);
    if (r.knee.known) {
        const double want = std::min(double(destinationLufs(int(song[aDest]))), r.knee.kneeLufs - 0.2);
        const double t = std::round(clamp(want, -16.0, -6.0) * 10) / 10;
        if (std::abs(t - song[lTarget]) > 0.25) {
            Move m; m.key = 0; m.param = lTarget; m.absolute = true; m.full = float(t); m.kind = mkLoud; m.module = "LOUDNESS";
            m.what = "TARGET  " + fmt1(t) + " LUFS";
            m.why = t < destinationLufs(int(song[aDest])) ? "Any louder and the drums start to smear." : "As loud as the destination wants, short of the knee.";
            m.conf = 88;
            add(m);
        }
    }

    // SECTION LIFT: the limiter works hardest on the loudest section, so it evens the song out:
    // a chorus that sat well over the verses in the mix comes out only a little over them. A quieter
    // section's own TARGET, a little under the song's, gives some of that lift back.
    {
        r.contrast = contrastFrom(st);
        const Contrast& c = r.contrast;
        for (int k = 1; k < kNumKeys && c.loud > 0; ++k) {
            if (!c.ok[k] || c.mixGap[k] < 0.5) continue;
            const double lost = c.mixGap[k] - c.masterGap[k];
            if (lost < 0.8) continue;
            const double base = sectionVals ? sectionVals[k][lTarget] : song[lTarget];
            const double give = std::round(clamp(0.7 * lost, 0.5, 3.0) * 10) / 10;
            const double t = std::max(-16.0, std::round((base - give) * 10) / 10);
            if (base - t < 0.3) continue;
            Move m; m.key = k; m.param = lTarget; m.absolute = true; m.full = float(t); m.kind = mkLift; m.module = "LOUDNESS";
            m.what = "TARGET  " + fmt1(t) + " LUFS";
            m.why = "The " + many(c.loud) + " stand " + fmt1(c.mixGap[k]) + " dB over the " + many(k) + " in the mix, " + fmt1(std::max(0.0, c.masterGap[k])) +
                    " after the limiter. This gives back " + fmt1(base - t) + ".";
            m.conf = int(clamp(50 + 12 * (lost - 0.8), 45.0, 85.0) * (useTaste ? 0.75 + 0.5 * taste.lean(mkLift) : 1.0));
            add(m);
        }
    }

    // ...and the loudest section, which the limiter works hardest, needs less glue of its own to keep its punch.
    if (r.contrast.loud > 0 && song[dynIn] > 0.5f) {
        const int L = r.contrast.loud;
        const SectionLevel lv = sectionLevel(st, L);
        const float g0 = sectionVals ? sectionVals[L][dGlue] : song[dGlue];
        if (lv.ok && lv.loss > 2.0 && g0 >= 2.5f) {
            Move m; m.key = L; m.param = dGlue; m.kind = mkGlue; m.module = "DYNAMICS"; m.full = -1.5f;
            m.what = "GLUE  " + fmt1(g0) + " \xe2\x86\x92 " + fmt1(g0 - 1.5f);
            m.why = "The limiter already takes " + fmt1(lv.loss) + " dB off the " + many(L) + ". Less glue there keeps their punch.";
            m.conf = int(clamp(45 + 8 * (lv.loss - 2.0), 45.0, 75.0));
            add(m);
        }
    }

    // Checks: what the pass heard.
    const double dc = std::max(std::abs(st.dc[0]), std::abs(st.dc[1])) / std::max(1.0, st.samples);
    r.checks.push_back({"PHASE", st.corrMin > 0 ? "correlation stays above " + fmt1(std::floor(st.corrMin * 10) / 10, true) : "the sides cancel in places (correlation " + fmt1(st.corrMin, true) + ")", st.corrMin > 0});
    const bool monoFixed = song[stIn] > 0.5f && song[sMono] > 1;
    r.checks.push_back({"MONO", monoFixed ? "low end centred below " + std::to_string(int(song[sMono])) + " Hz" : (r.sideLowDb[0] > -14 ? "the low end drifts off centre" : "the low end is centred"), monoFixed || r.sideLowDb[0] <= -14});
    const bool rumbleFixed = song[cleanIn] > 0.5f && song[cRumble] > 1;
    r.checks.push_back({"RUMBLE", rumbleFixed ? "cut below " + std::to_string(int(song[cRumble])) + " Hz" : (r.subDb > -10 ? "rumble under the kick" : "nothing under the kick"), rumbleFixed || r.subDb <= -10});
    r.checks.push_back({"DC OFFSET", dc < 1e-4 ? "none" : "the waveform sits off centre", dc < 1e-4});
    r.checks.push_back({"START AND END", st.firstLevelDb > -30 ? "the song starts abruptly: check for a click" : "clean, no clicks", st.firstLevelDb <= -30});
    return r;
}

}  // namespace lq
