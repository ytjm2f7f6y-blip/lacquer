#pragma once
// The assistant: from a listening pass to targets, suggested moves, the loudness knee and the
// checks. Runs on the message thread, after the pass.
//
// The genre curves start from the average balance of released records, measured with this
// analyser (see kRecords below), bent a little for each style. They are starting points, meant to
// be tuned by ear, and REFS or MINE replace them.

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

// How released records sit, on average: twelve commercially released, professionally mixed and
// mastered songs (indie, rock, metal, pop, R&B, electronic, hip-hop, acoustic) measured with this
// analyser, then smoothed. Against pink noise they fall about 1.5 dB an octave from 100 Hz to 4 kHz
// and faster above that, the shape Pestana et al. (AES 2013) found across commercial records from
// 1950 to 2010: a severe roll-off under about 40 Hz, a lift around the kick and bass, and a top end
// well under the mids. (A flat target here would ask every good record for more top.)
struct CurvePoint { double hz, db; };
inline const CurvePoint* recordsCurve(int& n) {
    static const CurvePoint c[] = {{20, -20.0}, {25, -14.0}, {31.5, -8.5}, {40, -3.0}, {50, 1.5}, {63, 3.6}, {80, 4.0},
                                   {100, 3.5}, {125, 2.9}, {160, 2.0}, {200, 1.5}, {250, 1.2}, {315, 0.8}, {400, 0.8},
                                   {500, 0.4}, {630, 0.4}, {800, -0.2}, {1000, -0.7}, {1250, -1.0}, {1600, -1.3},
                                   {2000, -1.6}, {2500, -2.2}, {3150, -3.2}, {4000, -4.4}, {5000, -5.6}, {6300, -6.3},
                                   {8000, -6.9}, {10000, -8.1}, {12500, -9.7}, {16000, -12.5}, {20000, -18.0}};
    n = int(sizeof(c) / sizeof(c[0]));
    return c;
}
// How far good records spread either side of it (about one standard deviation of the same twelve,
// band by band): inside this, a balance is a matter of taste, not a fault.
inline const CurvePoint* recordsSpread(int& n) {
    static const CurvePoint c[] = {{20, 7.0}, {31.5, 5.0}, {45, 3.0}, {63, 2.2}, {160, 2.0}, {1000, 2.0},
                                   {2000, 2.3}, {4000, 2.6}, {8000, 3.2}, {16000, 4.5}, {22000, 6.0}};
    n = int(sizeof(c) / sizeof(c[0]));
    return c;
}
inline double curveAt(const CurvePoint* c, int n, double f) {
    if (f <= c[0].hz) return c[0].db;
    for (int i = 1; i < n; ++i)
        if (f <= c[i].hz) {
            const double t = std::log2(f / c[i - 1].hz) / std::log2(c[i].hz / c[i - 1].hz);
            return c[i - 1].db + t * (c[i].db - c[i - 1].db);
        }
    return c[n - 1].db;
}

// Each style against that average: more or less low end (a bell at lowHz), low-mids, presence, and
// top (a tilt from 2 kHz, all of it by 16 kHz). Records with more percussion carry more low end and
// more top (Elowsson and Friberg, AES 2017); quieter, acoustic styles carry less of both.
struct GenreShape { const char* name; double lowDb, lowHz, lowMidDb, presDb, topDb; };
inline const GenreShape* genres(int& n) {
    static const GenreShape g[] = {
        {"indie rock", 0.0, 70, 0.3, 0.3, 0.0},
        {"pop", 0.5, 60, -0.4, 0.6, 1.0},
        {"hip-hop", 2.0, 55, -0.5, -0.3, 0.5},
        {"electronic", 1.5, 50, -0.8, 0.0, 1.5},
        {"acoustic", -1.5, 70, 1.2, -1.5, -2.0},
        {"jazz", -2.0, 80, 1.0, -1.0, -2.5},
    };
    n = int(sizeof(g) / sizeof(g[0]));
    return g;
}

constexpr double kLowSideMax = -12.0;   // side against mid under 100 Hz, dB

inline Target genreTarget(int i) {
    int n = 0, nc = 0, ns = 0;
    const auto* g = genres(n);
    const auto* rc = recordsCurve(nc);
    const auto* rs = recordsSpread(ns);
    const auto& s = g[std::max(0, std::min(i, n - 1))];
    Target t;
    t.name = s.name;
    for (int b = 0; b < kBands; ++b) {
        const double f = bandCentre(b);
        double c = curveAt(rc, nc, f);
        c += s.lowDb * std::exp(-std::pow(std::log2(f / s.lowHz) / 1.4, 2));
        c += s.lowMidDb * std::exp(-std::pow(std::log2(f / 350.0) / 1.0, 2));
        c += s.presDb * std::exp(-std::pow(std::log2(f / 2800.0) / 1.0, 2));
        c += s.topDb * clamp(std::log2(f / 2000.0) / 3.0, 0.0, 1.0);
        t.curve[b] = c;
        t.width[b] = curveAt(rs, ns, f);
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
    auto bell = [&](double fc, double g, double oct) { return g * std::exp(-std::pow(std::log2(f / fc) / oct, 2)); };
    double d = 0;
    if (v[toneIn] > 0.5f) {
        // TONE's own response, mid and side together (as the balance curve is).
        ToneParams tp;
        const int db[kToneBands] = {tLow, tMud, tPres, tAir}, hz[kToneBands] = {tLowHz, tMudHz, tPresHz, tAirHz}, pl[kToneBands] = {tLowPl, tMudPl, tPresPl, tAirPl};
        for (int b = 0; b < kToneBands; ++b) { tp.b[b].db = v[db[b]]; tp.b[b].hz = v[hz[b]]; tp.b[b].place = v[pl[b]]; }
        tp.weight = 1;
        std::complex<double> hm, hs;
        toneResponseMS(tp, f, 48000.0, hm, hs);
        d += 10 * std::log10(0.75 * std::norm(hm) + 0.25 * std::norm(hs) + 1e-12);   // the mid carries most of a mix
    }
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
enum MoveKind { mkLow, mkPres, mkAir, mkHarsh, mkRumble, mkMono, mkWidth, mkLoud, mkGlue, mkRes, mkLift, mkMud, kNumMoveKinds };

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
// ceilingDb: the ceiling knob. autoCeiling: GUARD's lower ceiling over -14 LUFS (see ceilingFor).
inline Knee kneeFromPeaks(const double* pk, double windows, double inputLufs, double ceilingDb, double clip, bool autoCeiling = false) {
    Knee k;
    if (windows < 50) return k;
    k.inputLufs = inputLufs;
    const double clipHelp = clip / 10.0 * 1.5;   // the clipper takes the sharpest peaks off cleanly
    double kneeAt = -6;
    bool found = false;
    for (int i = 0; i <= 100; ++i) {
        const double L = -16 + i * 0.1, drive = L - k.inputLufs;
        const double ceil = autoCeiling && L > kLoudForStreams ? std::min(ceilingDb, kLoudCeiling) : ceilingDb;
        double ex = 0;
        for (int b = 0; b < kPkBins; ++b) {
            if (pk[b] <= 0) continue;
            const double p = -70 + b * 0.5 + 0.25;
            const double gr = std::max(0.0, p + drive - ceil - clipHelp);
            ex += pk[b] * std::max(0.0, gr - 1.0);
        }
        ex /= windows;
        k.kept[i] = clamp(100.0 - 14.0 * ex, 30.0, 100.0);
        if (!found && ex > 0.45) { kneeAt = L; found = true; }
    }
    k.kneeLufs = found ? kneeAt : -6;
    k.known = true;
    return k;
}
inline Knee kneeFrom(const KeyStats& song, double ceilingDb, double clip, bool autoCeiling = false) {
    return kneeFromPeaks(song.pk, song.windows, song.integrated(), ceilingDb, clip, autoCeiling);
}

// What each destination usually asks for. Streaming services turn anything louder than about -14
// down, so STREAM aims only a little over that; a CD and a club want it loud; a cut wants room.
inline int destinationLufs(int dest) { static const int d[4] = {-12, -9, -13, -8}; return d[std::max(0, std::min(dest, 3))]; }

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
    double peakPile = 0;  // the share of the loud 10 ms windows peaking within 2 dB of the top
};

// How much of the song's loud stretches peak just under its loudest peaks (the top 0.1% of windows
// set the top): near 0 for an unlimited mix, a large share for one that went through a limiter.
inline double peakPile(const KeyStats& k) {
    double total = 0;
    int top = -1;
    for (int b = kPkBins - 1; b >= 0; --b) { total += k.pk[b]; if (top < 0 && k.pk[b] > 0) top = b; }
    if (top < 0 || total < 100) return 0;
    double cum = 0;
    int p999 = top;
    for (int b = top; b >= 0; --b) { cum += k.pk[b]; if (cum >= 0.001 * total) { p999 = b; break; } }
    double loud = 0, near = 0;
    for (int b = 0; b < kPkBins; ++b) {
        if (b < top - 60) continue;                      // within 30 dB of the loudest
        loud += k.pk[b];
        if (b >= p999 - 4) near += k.pk[b];              // within 2 dB of the top
    }
    return loud > 0 ? near / loud : 0;
}

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
            if (bandCentre(b) < 100) { lowM += st.key[k].mid[b]; lowS += st.key[k].side[b]; }   // kick and bass
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

    // TONE: low, mud, presence, air. Moves are capped; more than that belongs in the mix.
    struct ToneBand { int param; int kind; double f0, f1; const char* name; const char* hi; const char* lo; };
    const ToneBand tb[4] = {{tLow, mkLow, 45, 160, "LOW", "The low end sits heavy against the target.", "The low end sits light against the target."},
                            {tMud, mkMud, 200, 500, "MUD", "200 to 500 Hz builds up: the mix sounds thick and boxy.", "200 to 500 Hz sits thin: the body is missing."},
                            {tPres, mkPres, 1600, 4000, "PRESENCE", "2 to 4 kHz is a touch forward.", "The upper mids sit back."},
                            {tAir, mkAir, 8000, 16000, "AIR", "The top end sits above the target.", "The top end sits under the target."}};
    for (const auto& b : tb) {
        // Inside the corridor is taste; a move only once the band's average leaves it.
        const double dev = avgDev(0, b.f0, b.f1), room = avgWidth(b.f0, b.f1), sectRoom = 0.5 * room + 0.4;
        if (std::abs(dev) > room) {
            Move m; m.key = 0; m.param = b.param; m.kind = b.kind; m.module = "TONE";
            m.full = float(clamp(-dev * 0.9, -3.0, 3.0));
            m.extraParam = toneIn; m.extraValue = 1;
            m.what = std::string(b.name) + "  " + fmt1(m.full, true) + " dB";
            m.why = dev > 0 ? b.hi : b.lo;
            m.conf = confFor(dev, room, b.kind);
            add(m);
            if (std::abs(dev) > room + 3.0) r.mixNotes.push_back(std::string(b.name == std::string("LOW") ? "The low end" : b.name == std::string("AIR") ? "The top end" : b.name == std::string("MUD") ? "The low-mids" : "The upper mids") +
                                                          " is " + fmt1(std::abs(dev)) + " dB " + (dev > 0 ? "over" : "under") + " the target. Fix the bulk of it in the mix.");
        }
        // A section that stands apart from the song gets its own move: the two that stand out most
        // (with eight sections, a move for every one that differs a little would bury the useful ones).
        int pick[2] = {-1, -1};
        double score[2] = {0, 0};
        for (int k = 1; k < kNumKeys; ++k) {
            if (!r.keyHeard[k] || r.keySeconds[k] < 8) continue;
            const double dk = avgDev(k, b.f0, b.f1) - avgDev(0, b.f0, b.f1);
            if (std::abs(dk) <= sectRoom) continue;
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
                m.conf = confFor(dk, sectRoom, b.kind) - 8;
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
        // Released records show a bump of up to 1.8 dB here and aren't harsh; past 2 dB, it is.
        if (hSong > 2.0 && sectionMoves < 2) {
            Move m; m.key = 0; m.param = cHarsh; m.kind = mkHarsh; m.module = "CLEAN";
            m.full = float(clamp(hSong, 1.0, 4.0));
            m.extraParam = cleanIn; m.extraValue = 1;
            m.what = "HARSH  " + fmt1(song[cHarsh]) + " \xe2\x86\x92 " + fmt1(std::min(10.0f, song[cHarsh] + m.full));
            m.why = "The song is harsh around 3 to 4 kHz.";
            m.conf = confFor(hSong, 2.0, mkHarsh);
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
    // Released records keep the side under 100 Hz 18 to 33 dB down; a few wide ones sit near -10.
    if (r.sideLowDb[0] > kLowSideMax && !(song[stIn] > 0.5f && song[sMono] > 1)) {
        Move m; m.key = 0; m.param = sMono; m.absolute = true; m.full = 120; m.kind = mkMono; m.module = "STEREO";
        m.extraParam = stIn; m.extraValue = 1;
        m.what = "BASS MONO  BELOW 120 Hz"; m.why = "The low end drifts off centre.";
        m.conf = confFor(r.sideLowDb[0] - kLowSideMax, 3, mkMono);
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
    // The knee from the limiter's own input when the pass has it (so PUNCH, GLUE and COLOUR count).
    r.knee = kneeFrom(st.limIn[0].windows >= 50 ? st.limIn[0] : all, song[lCeiling], song[lClip], song[lGuard] > 0.5f);
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

    // A mix that comes in already limited: its 10 ms peaks pile up just under one level. Released
    // masters put 16 to 60% of their loud windows within 2 dB of the top; gentle masters (and mixes)
    // under 3%. Lacquer can't take a limiter back off, so this is a note for the mix.
    r.peakPile = peakPile(all);
    if (all.seconds >= 30 && r.peakPile > 0.12)
        r.mixNotes.insert(r.mixNotes.begin(), "The mix comes in already limited. Bounce it without the mix-bus limiter and let LOUDNESS do that job.");

    // Checks: what the pass heard.
    const double dc = std::max(std::abs(st.dc[0]), std::abs(st.dc[1])) / std::max(1.0, st.samples);
    // PHASE: a moment of negative correlation (a wide reverb tail, a stereo synth) is normal on good
    // records; the sides cancelling for more than a second or two of the song's loud stretches is not.
    {
        const double negPct = st.corrSeconds > 0 ? 100.0 * st.corrNegSeconds / st.corrSeconds : 0;
        const bool ok = negPct < 1.0 && st.corrMin > -0.5;
        r.checks.push_back({"PHASE", ok ? (st.corrMin > 0 ? "correlation stays above " + fmt1(std::floor(st.corrMin * 10) / 10, true) : "the sides stay together")
                                        : "the sides cancel for " + fmt1(negPct) + "% of the song",
                            ok});
    }
    const bool monoFixed = song[stIn] > 0.5f && song[sMono] > 1;
    r.checks.push_back({"MONO", monoFixed ? "low end centred below " + std::to_string(int(song[sMono])) + " Hz" : (r.sideLowDb[0] > kLowSideMax ? "the low end drifts off centre" : "the low end is centred"), monoFixed || r.sideLowDb[0] <= kLowSideMax});
    const bool rumbleFixed = song[cleanIn] > 0.5f && song[cRumble] > 1;
    r.checks.push_back({"RUMBLE", rumbleFixed ? "cut below " + std::to_string(int(song[cRumble])) + " Hz" : (r.subDb > -10 ? "rumble under the kick" : "nothing under the kick"), rumbleFixed || r.subDb <= -10});
    // A whole song's average sits around -75 dBFS from lopsided kicks and bass alone; a converter's
    // offset is more like -50 dBFS and up.
    r.checks.push_back({"DC OFFSET", dc < 3e-3 ? "none" : "the waveform sits off centre", dc < 3e-3});
    r.checks.push_back({"START AND END", st.firstLevelDb > -30 ? "the song starts abruptly: check for a click" : "clean, no clicks", st.firstLevelDb <= -30});
    return r;
}

}  // namespace lq
