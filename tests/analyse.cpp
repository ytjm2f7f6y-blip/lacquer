// Runs finished songs through Lacquer the way a session would: one play to learn the loudness,
// a listening pass, then the assistant against a style. Prints what it measured and suggested,
// and writes a JSON line per song, for checking the assistant against records that are already
// well mixed and mastered (where it should find little to do).
//
//   lacquer_analyse [--style N] [--json out.jsonl] file...
#include <juce_audio_formats/juce_audio_formats.h>

#include "lq_assistant.hpp"
#include "lq_engine.hpp"

#include <cstdio>

using namespace lq;

namespace {

struct Song { std::vector<float> L, R; double fs = 48000; };

bool load(const juce::File& f, Song& s) {
    juce::AudioFormatManager fm;
    fm.registerBasicFormats();
    std::unique_ptr<juce::AudioFormatReader> r(fm.createReaderFor(f));
    if (!r || r->lengthInSamples <= 0) return false;
    const int n = int(std::min<juce::int64>(r->lengthInSamples, juce::int64(r->sampleRate * 900)));
    juce::AudioBuffer<float> b(2, n);
    r->read(&b, 0, n, 0, true, r->numChannels > 1);
    if (r->numChannels == 1) b.copyFrom(1, 0, b, 0, 0, n);
    s.fs = r->sampleRate;
    s.L.assign(b.getReadPointer(0), b.getReadPointer(0) + n);
    s.R.assign(b.getReadPointer(1), b.getReadPointer(1) + n);
    return true;
}

// Integrated loudness, max short-term loudness and true peak of a signal.
struct Level { double integrated = -70, shortMax = -70, truePeak = -120, lra = 0; };
Level measure(const std::vector<float>& L, const std::vector<float>& R, double fs) {
    KLevel k;
    k.prepare(fs);
    KeyStats ks;
    const int len = int(0.1 * fs);
    double sub = 0, q[30]{};
    int n = 0, qi = 0;
    std::vector<double> st;
    lp::TruePeak tp[2];
    double pk = 0;
    for (size_t i = 0; i < L.size(); ++i) {
        const double l = k.rlb[0].process(k.pre[0].process(L[i])), r = k.rlb[1].process(k.pre[1].process(R[i]));
        pk = std::max({pk, tp[0].process(L[i]), tp[1].process(R[i])});
        sub += l * l + r * r;
        if (++n == len) {
            q[qi % 30] = sub / len;
            ++qi;
            if (qi >= 4) {
                const double m4 = 0.25 * (q[(qi - 1) % 30] + q[(qi - 2) % 30] + q[(qi - 3) % 30] + q[(qi - 4) % 30]);
                ks.blocks.push_back(-0.691 + 10 * std::log10(m4 + 1e-20));
            }
            if (qi >= 30) {
                double m = 0;
                for (double x : q) m += x;
                st.push_back(-0.691 + 10 * std::log10(m / 30 + 1e-20));
            }
            sub = 0; n = 0;
        }
    }
    Level lv;
    lv.integrated = ks.integrated();
    for (double x : st) lv.shortMax = std::max(lv.shortMax, x);
    lv.truePeak = gainToDb(pk + 1e-12);
    // Loudness range (EBU Tech 3342, roughly): 10th to 95th percentile of gated 3 s values.
    std::vector<double> g;
    for (double x : st) if (x > -70 && x > lv.integrated - 20) g.push_back(x);
    if (g.size() > 10) {
        std::sort(g.begin(), g.end());
        lv.lra = g[size_t(0.95 * double(g.size() - 1))] - g[size_t(0.10 * double(g.size() - 1))];
    }
    return lv;
}

// Gain reduction over a play: the meters hold the most since they were last read, so read them each block.
struct Gr { double glueSum = 0, glueMax = 0, limSum = 0, limMax = 0, limOver1 = 0; int n = 0; };

// One play through the engine, the transport running at 120 bpm from the top.
void play(Engine& e, const Song& s, const Values& v, std::vector<float>* outL = nullptr, std::vector<float>* outR = nullptr, Gr* gr = nullptr) {
    std::vector<float> L = s.L, R = s.R;
    Transport t;
    t.playing = true; t.hasPpq = true; t.bpm = 120;
    for (size_t i = 0; i < L.size(); i += 512) {
        const int n = int(std::min<size_t>(512, L.size() - i));
        t.ppq = double(i) / s.fs * 2.0;
        e.process(L.data() + i, R.data() + i, n, v, t);
        const double g = e.meters().glueGrMax.exchange(0), l = e.meters().limGrMax.exchange(0);
        if (gr) {
            gr->glueSum += g; gr->glueMax = std::max(gr->glueMax, g);
            gr->limSum += l; gr->limMax = std::max(gr->limMax, l);
            gr->limOver1 += l > 1.0 ? 1 : 0; gr->n += 1;
        }
    }
    Transport stop;
    float z[64] = {}, z2[64] = {};
    e.process(z, z2, 64, v, stop);
    if (outL) *outL = std::move(L);
    if (outR) *outR = std::move(R);
}

std::string esc(const std::string& s) {
    std::string o;
    for (char c : s) { if (c == '"' || c == '\\') o += '\\'; o += c; }
    return o;
}

}  // namespace

int main(int argc, char** argv) {
    juce::ignoreUnused(argc);
    int style = 0;
    std::string jsonPath;
    bool quick = false;
    std::vector<std::string> files;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--style" && i + 1 < argc) style = std::atoi(argv[++i]);
        else if (a == "--json" && i + 1 < argc) jsonPath = argv[++i];
        else if (a == "--quick") quick = true;
        else files.push_back(a);
    }
    FILE* js = jsonPath.empty() ? nullptr : std::fopen(jsonPath.c_str(), "a");
    int ng = 0;
    const auto* gs = genres(ng);
    for (const auto& path : files) {
        Song s;
        if (!load(juce::File(path), s)) { std::printf("%s: can't read\n", path.c_str()); continue; }
        const Level in = measure(s.L, s.R, s.fs);
        // As a session would: defaults, a first play to learn the song, then a listening pass.
        auto e = std::make_unique<Engine>();
        e->prepare(s.fs, 512);
        Values v;
        play(*e, s, v);
        double m = 0, cov = 0;
        const int bars = int(std::ceil(double(s.L.size()) / s.fs * 2.0 / 4.0));
        if (loudnessFromBars(e->bars(), bars, m, cov)) e->setMeasuredInput(m, true);
        e->analyser().reset();
        e->analyser().stats().clear();
        e->clearPass();
        e->setListening(true);
        std::vector<float> dL, dR;
        Gr gr;
        play(*e, s, v, &dL, &dR, &gr);
        e->setListening(false);
        const Level def = measure(dL, dR, s.fs);
        const double glueMax = gr.glueMax, limMax = gr.limMax, glueAvg = gr.glueSum / std::max(1, gr.n),
                     limAvg = gr.limSum / std::max(1, gr.n), limPct = 100.0 * gr.limOver1 / std::max(1, gr.n);
        Values sec[kNumKeys];
        for (int k = 0; k < kNumKeys; ++k) sec[k] = e->valuesFor(k, v);
        const Target tgt = genreTarget(style);
        const Result r = assist(e->analyser().stats(), tgt, v, sec, Taste{}, false);
        // How the balance sits in the corridor.
        int inside = 0, total = 0;
        double worst = 0, worstF = 0, worstEx = -1e9;
        for (int b = 0; b < kBands; ++b) {
            const double f = bandCentre(b);
            if (f < 30 || f > 16000) continue;
            const double d = r.curve[0][b] - tgt.curve[b];
            ++total;
            if (std::abs(d) <= tgt.width[b]) ++inside;
            if (std::abs(d) - tgt.width[b] > worstEx) { worstEx = std::abs(d) - tgt.width[b]; worst = d; worstF = f; }
        }
        // And the same song mastered by Lacquer with every move taken.
        Values taken = v;
        for (const auto& mv : r.moves) {
            if (mv.key != 0) continue;
            taken[mv.param] = mv.absolute ? mv.full : taken[mv.param] + mv.full * v[aAmount] / 100.0f;
            if (mv.extraParam >= 0) taken[int(mv.extraParam)] = mv.extraValue;
        }
        Level out = def;
        if (!quick) {
            auto e2 = std::make_unique<Engine>();
            e2->prepare(s.fs, 512);
            play(*e2, s, taken);
            if (loudnessFromBars(e2->bars(), bars, m, cov)) e2->setMeasuredInput(m, true);
            std::vector<float> oL, oR;
            play(*e2, s, taken, &oL, &oR);
            out = measure(oL, oR, s.fs);
        }

        const std::string name = juce::File(path).getFileNameWithoutExtension().toStdString();
        std::printf("\n%s  (%.0f s, %.0f Hz) against %s\n", name.c_str(), s.L.size() / s.fs, s.fs, gs[style].name);
        std::printf("  as it came:  %6.1f LUFS integrated, %6.1f short-term max, %5.1f dBTP, PLR %4.1f dB, LRA %4.1f LU\n",
                    in.integrated, in.shortMax, in.truePeak, in.truePeak - in.integrated, in.lra);
        std::printf("  balance:     %d%% inside the corridor; furthest out %+.1f dB at %.0f Hz\n", total ? 100 * inside / total : 0, worst, worstF);
        std::printf("  knee:        %s %.1f LUFS\n", r.knee.known ? "at" : "not found,", r.knee.kneeLufs);
        std::printf("  stereo:      side under 100 Hz %.1f dB; correlation down to %.2f, below zero %.2f%% of the loud stretches; peak pile %.0f%%\n",
                    r.sideLowDb[0], e->analyser().stats().corrMin,
                    100.0 * e->analyser().stats().corrNegSeconds / std::max(1e-9, e->analyser().stats().corrSeconds), 100 * r.peakPile);
        std::printf("  moves (%d):\n", int(r.moves.size()));
        for (const auto& mv : r.moves) std::printf("    %-9s %-26s %3d%%  %s\n", mv.module.c_str(), mv.what.c_str(), mv.conf, mv.why.c_str());
        int looks = 0;
        for (const auto& c : r.checks) if (!c.ok) { ++looks; std::printf("  check:       %s: %s\n", c.name.c_str(), c.what.c_str()); }
        for (const auto& n : r.mixNotes) std::printf("  mix note:    %s\n", n.c_str());
        std::printf("  opening set: %6.1f LUFS integrated, %6.1f short-term max, %5.1f dBTP, PLR %4.1f dB, LRA %4.1f LU\n"
                    "               glue %.1f dB on average (most %.1f), limiter %.1f dB on average (most %.1f, over 1 dB %.0f%% of the time)\n",
                    def.integrated, def.shortMax, def.truePeak, def.truePeak - def.integrated, def.lra, glueAvg, glueMax, limAvg, limMax, limPct);
        std::printf("  mastered:    %6.1f LUFS integrated, %6.1f short-term max, %5.1f dBTP, PLR %4.1f dB, LRA %4.1f LU\n",
                    out.integrated, out.shortMax, out.truePeak, out.truePeak - out.integrated, out.lra);
        if (js) {
            std::fprintf(js, "{\"file\":\"%s\",\"style\":\"%s\",\"in_lufs\":%.2f,\"in_short_max\":%.2f,\"in_tp\":%.2f,\"in_lra\":%.2f,\"inside_pct\":%d,"
                             "\"worst_db\":%.2f,\"worst_hz\":%.0f,\"knee\":%.2f,\"knee_known\":%s,\"n_moves\":%d,\"n_looks\":%d,\"n_notes\":%d,\"side_low\":%.2f,\"corr_min\":%.2f,\"corr_neg_pct\":%.2f,\"peak_pile\":%.3f,"
                             "\"def_lufs\":%.2f,\"def_tp\":%.2f,\"def_lra\":%.2f,\"glue_max\":%.2f,\"lim_max\":%.2f,\"glue_avg\":%.2f,\"lim_avg\":%.2f,\"lim_pct\":%.1f,"
                             "\"out_lufs\":%.2f,\"out_tp\":%.2f,\"out_lra\":%.2f,\"curve\":[",
                         esc(name).c_str(), gs[style].name, in.integrated, in.shortMax, in.truePeak, in.lra, total ? 100 * inside / total : 0,
                         worst, worstF, r.knee.kneeLufs, r.knee.known ? "true" : "false", int(r.moves.size()), looks, int(r.mixNotes.size()),
                         r.sideLowDb[0], e->analyser().stats().corrMin,
                         100.0 * e->analyser().stats().corrNegSeconds / std::max(1e-9, e->analyser().stats().corrSeconds), r.peakPile,
                         def.integrated, def.truePeak, def.lra, glueMax, limMax, glueAvg, limAvg, limPct, out.integrated, out.truePeak, out.lra);
            for (int b = 0; b < kBands; ++b) std::fprintf(js, "%s%.2f", b ? "," : "", r.curve[0][b]);
            std::fprintf(js, "],\"moves\":[");
            for (size_t i = 0; i < r.moves.size(); ++i)
                std::fprintf(js, "%s{\"module\":\"%s\",\"what\":\"%s\",\"conf\":%d,\"key\":%d}", i ? "," : "", esc(r.moves[i].module).c_str(), esc(r.moves[i].what).c_str(), r.moves[i].conf, r.moves[i].key);
            std::fprintf(js, "]}\n");
        }
    }
    if (js) std::fclose(js);
    return 0;
}
