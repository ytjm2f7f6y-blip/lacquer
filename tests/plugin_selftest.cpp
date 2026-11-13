// End-to-end: the real processor, driven the way a host drives it (a playhead, blocks, stop and start).
#include "PluginEditor.h"
#include "PluginProcessor.h"
#include "testsig.hpp"

#include <cstdio>

static int fails = 0;
#define CHECK(cond, msg) do { if (!(cond)) { ++fails; std::printf("FAIL: %s\n", msg); } else std::printf("ok:   %s\n", msg); } while (0)

using namespace lq;

namespace {

struct Head final : juce::AudioPlayHead {
    bool playing = false;
    double ppq = 0, bpm = 120;
    juce::Optional<PositionInfo> getPosition() const override {
        PositionInfo p;
        p.setIsPlaying(playing);
        p.setPpqPosition(ppq);
        p.setBpm(bpm);
        p.setTimeSignature(TimeSignature{4, 4});
        p.setPpqPositionOfLastBarStart(std::floor(ppq / 4.0) * 4.0);
        return p;
    }
};

// Plays the song from the top, then stops (one more block, as a host keeps calling while stopped).
// `atBar` is called after the block that crosses into each bar.
void play(LacquerProcessor& p, Head& head, const testsig::Song& s, int block, const std::function<void(int)>& atBar = {}) {
    juce::AudioBuffer<float> io(2, block);
    juce::MidiBuffer midi;
    head.playing = true;
    const size_t total = s.L.size();
    const double spb = s.secondsPerBar() * s.fs;
    int lastBar = -1;
    for (size_t at = 0; at + size_t(block) <= total; at += size_t(block)) {
        head.ppq = double(at) / s.fs * s.bpm / 60.0;
        for (int i = 0; i < block; ++i) { io.setSample(0, i, s.L[at + size_t(i)]); io.setSample(1, i, s.R[at + size_t(i)]); }
        p.processBlock(io, midi);
        const int bar = int(double(at) / spb);
        if (bar != lastBar) { lastBar = bar; if (atBar) atBar(bar); }
    }
    head.playing = false;
    io.clear();
    p.processBlock(io, midi);
}

bool finite(const juce::AudioBuffer<float>& b) {
    for (int c = 0; c < b.getNumChannels(); ++c)
        for (int i = 0; i < b.getNumSamples(); ++i) if (!std::isfinite(b.getSample(c, i))) return false;
    return true;
}

// The same audio through two processors in different block sizes.
double blockSizeDiff(const testsig::Song& s, bool offline, int listenOn) {
    LacquerProcessor a, b;
    for (auto* p : {&a, &b}) { p->setRateAndBufferSizeDetails(48000, 1024); p->prepareToPlay(48000, 1024); p->setNonRealtime(offline); }
    a.setSong(oListen, float(listenOn));
    const int n = 48000 * 6;
    juce::AudioBuffer<float> x(2, n), y(2, n);
    for (int i = 0; i < n; ++i) { x.setSample(0, i, s.L[size_t(i)]); x.setSample(1, i, s.R[size_t(i)]); }
    y.makeCopyOf(x);
    juce::MidiBuffer midi;
    for (int at = 0; at < n; at += 64) { juce::AudioBuffer<float> v(x.getArrayOfWritePointers(), 2, at, std::min(64, n - at)); a.processBlock(v, midi); }
    for (int at = 0, k = 0; at < n; ++k) {
        const int m = std::min(n - at, k % 2 ? 1000 : 37);
        juce::AudioBuffer<float> v(y.getArrayOfWritePointers(), 2, at, m);
        b.processBlock(v, midi);
        at += m;
    }
    double worst = x.getMagnitude(0, n / 2, n / 2) > 0.01f ? 0 : 1;   // (and it isn't silent)
    for (int c = 0; c < 2; ++c) for (int i = 0; i < n; ++i) worst = std::max(worst, double(std::abs(x.getSample(c, i) - y.getSample(c, i))));
    return worst;
}

}  // namespace

int main() {
    juce::ScopedJuceInitialiser_GUI gui;
    const double sr = 48000;
    const int block = 512;
    // 32 bars at 120: quiet verse, loud chorus, twice.
    const auto song = testsig::song(sr, 120, {{8, false}, {8, true}, {8, false}, {8, true}});

    {
        LacquerProcessor p;
        Head head;
        p.setPlayHead(&head);
        p.setRateAndBufferSizeDetails(sr, block);
        p.prepareToPlay(sr, block);
        CHECK(int(p.getParameters().size()) == kNumParams, "every control is a host parameter");
        CHECK(p.getLatencySamples() >= 32, "the host is told about the delay");

        // ---- loudness: the first play learns the song, the second lands on the target
        play(p, head, song, block);
        CHECK(p.loudnessKnown() && p.coverage() > 0.9, "a full play teaches it the song's loudness, bar by bar");
        play(p, head, song, block);
        const float integ = p.meters().integrated.load(), tp = p.meters().peakHold.load();
        std::printf("      second play: %.2f LUFS integrated (target %.1f), true peak %.2f dBTP (ceiling %.1f)\n", integ, p.songValue(lTarget), tp, p.songValue(lCeiling));
        CHECK(std::abs(integ - p.songValue(lTarget)) < 1.0f, "the second play lands within a dB of the target");
        CHECK(tp <= p.songValue(lCeiling) + 0.15f, "and stays under the ceiling");

        // ---- marking sections by tapping the keys while it plays, and listening at the same time
        p.setListening(true);
        p.setMarking(true);
        play(p, head, song, block, [&](int bar) {
            if (bar == 0 || bar == 16) p.tapKey(2);       // verse
            if (bar == 8 || bar == 24) p.tapKey(3);       // chorus
        });
        p.setMarking(false);
        const auto& m = p.songMap();
        std::printf("      marks:");
        for (int i = 0; i < m.count; ++i) std::printf(" bar %d=%d", int(m.marks[i].ppq / 4) + 1, m.marks[i].key);
        std::printf("\n");
        CHECK(m.count == 4 && m.marks[1].key == 3 && std::abs(m.marks[1].ppq - 32.0) < 1e-6 && std::abs(m.marks[3].ppq - 96.0) < 1e-6, "tapping 2, 3, 2, 3 at the bar lines marks the four sections");
        CHECK(!p.listening(), "stopping after a long enough listen ends the pass");
        p.idle();
        const auto& r = p.result();
        std::printf("      listened %.0f s: %d moves, %d checks, %d mix notes; knee %s at %.1f LUFS\n", p.listenedSeconds(), int(r.moves.size()), int(r.checks.size()),
                    int(r.mixNotes.size()), r.knee.known ? "found" : "not found", r.knee.kneeLufs);
        for (const auto& mv : r.moves) std::printf("        %-9s %-24s %s\n", mv.module.c_str(), mv.what.c_str(), mv.key ? p.sectionName(mv.key).toRawUTF8() : "song");
        CHECK(r.any && r.keyHeard[0] && r.keyHeard[2] && r.keyHeard[3] && !r.keyHeard[1], "the assistant heard the song, the verses and the choruses (and no intro)");
        CHECK(r.knee.known, "and found where loudness starts to cost the drums");
        double mixGap = 0, masterGap = 0;
        const bool gap = p.sectionGap(2, mixGap, masterGap);
        std::printf("      choruses over verses: %.1f dB in the mix, %.1f dB mastered\n", mixGap, masterGap);
        CHECK(r.contrast.loud == 3 && gap && mixGap > masterGap, "it measured how much the limiter evens the choruses out against the verses");
        const float verseT = p.valuesFor(2)[lTarget];
        p.editSongMap([&](SongMap& sm) { sm.has[2][lTarget] = true; sm.val[2][lTarget] = verseT - 1.5f; });
        double mix2 = 0, master2 = 0;
        p.sectionGap(2, mix2, master2);
        p.editSongMap([&](SongMap& sm) { sm.has[2][lTarget] = false; });
        CHECK(std::abs(mix2 - mixGap) < 1e-9 && master2 > masterGap + 1.0, "a verse target 1.5 dB under the song widens the gap again");

        // ---- the lit key follows the song while it plays (FOLLOW), until you press a key
        int seen[6] = {};
        bool followOff = false;
        play(p, head, song, block, [&](int bar) {
            if (bar == 4 || bar == 12) seen[bar == 4 ? 0 : 1] = p.litKey();
            if (bar == 18) { p.tapKey(2); followOff = !p.follow(); }
            if (bar == 26) seen[2] = p.litKey();
            if (bar == 27) p.setFollow(true);
            if (bar == 28) seen[3] = p.litKey();
        });
        CHECK(seen[0] == 2 && seen[1] == 3, "while it plays, the lit key is the section it's in");
        CHECK(followOff && seen[2] == 2 && seen[3] == 3, "pressing a key while it plays turns FOLLOW off and holds that section; FOLLOW picks the song back up");

        // ---- marks made with the transport stopped, split and joined
        {
            juce::AudioBuffer<float> io(2, block);
            juce::MidiBuffer midi;
            io.clear();
            head.playing = false;
            head.ppq = 41.5;   // in bar 11
            p.processBlock(io, midi);
            const int before = p.songMap().count;
            p.setMarking(true);
            p.tapKey(5);
            p.setMarking(false);
            const auto& mm = p.songMap();
            int at = -1;
            for (int i = 0; i < mm.count; ++i) if (std::abs(mm.marks[i].ppq - 40.0) < 1e-6) at = i;
            CHECK(mm.count == before + 1 && at >= 0 && mm.marks[at].key == 5, "stopped, with MARK lit, a key marks the bar the playhead is in");
            const int half = p.splitMark(at);
            CHECK(half == at + 1 && p.songMap().count == before + 2 && std::abs(p.songMap().marks[half].ppq - 52.0) < 1e-6 && p.songMap().marks[half].key == 5,
                  "SPLIT halves a block on a bar line");
            p.joinMark(half);
            p.removeMark(at);
            CHECK(p.songMap().count == before, "JOIN folds it back");
        }

        // ---- a section's own setting
        const float glue = p.songValue(dGlue);
        p.setEditKey(3);
        p.setShown(dGlue, glue + 3);
        CHECK(p.songMap().has[3][dGlue] && std::abs(p.songValue(dGlue) - glue) < 1e-6f, "turning a knob with the chorus lit changes the chorus, not the song");
        CHECK(p.shownIsOwn(dGlue) && std::abs(p.shownValue(dGlue) - (glue + 3)) < 1e-5f, "and the knob shows the chorus's own value");
        const float songTarget = p.songValue(lTarget);
        p.setShown(lTarget, songTarget - 1.5f);
        p.setShown(lCeiling, -0.5f);
        CHECK(p.songMap().has[3][lTarget] && std::abs(p.songValue(lTarget) - songTarget) < 1e-4f, "with a section lit, TARGET is that section's own");
        CHECK(!p.songMap().has[3][lCeiling] && std::abs(p.songValue(lCeiling) + 0.5f) < 0.01f, "while the ceiling stays song-wide");
        p.resetShown(lTarget);
        p.setSong(lCeiling, def(lCeiling).def);
        p.setEditKey(0);
        CHECK(std::abs(p.shownValue(dGlue) - glue) < 1e-6f, "with SONG lit the knob shows the song's value");

        // ---- taking the moves, and taking them back
        Values before = p.songValues();
        p.takeAll();
        int changed = 0, taken = 0, ideas = 0;
        const Values after = p.songValues();
        for (int q = 0; q < kNumParams; ++q) changed += std::abs(after[q] - before[q]) > 1e-6f ? 1 : 0;
        for (const auto& mv : r.moves) taken += mv.taken ? 1 : 0;
        for (int mod = 0; mod < 6; ++mod) ideas += p.ideaCount(mod);
        std::printf("      took %d moves, %d song settings changed\n", taken, changed);
        CHECK(taken == int(r.moves.size()) && (r.moves.empty() || ideas > 0), "TAKE ALL takes every move, and the modules' idea lamps light");
        for (int i = 0; i < int(r.moves.size()); ++i) p.takeMove(i, false);
        const Values back = p.songValues();
        bool same = true;
        for (int q = 0; q < kNumParams; ++q) same = same && std::abs(back[q] - before[q]) < 1e-4f;
        CHECK(same && p.songMap().has[3][dGlue], "skipping them again puts every setting back");
        p.takeAll();

        // ---- A / B
        const float t0 = p.songValue(lTarget);
        p.selectAb(1);
        p.setSong(lTarget, -12.0f);
        p.selectAb(0);
        const float inA = p.songValue(lTarget);
        p.selectAb(1);
        CHECK(std::abs(inA - t0) < 0.01f && std::abs(p.songValue(lTarget) + 12) < 0.01f, "A and B keep their own settings");
        p.selectAb(0);

        // ---- the session comes back as it was
        p.setSectionName(3, "Hook");
        juce::MemoryBlock state;
        p.getStateInformation(state);
        LacquerProcessor q;
        q.setRateAndBufferSizeDetails(sr, block);
        q.prepareToPlay(sr, block);
        q.setStateInformation(state.getData(), int(state.getSize()));
        const auto& qm = q.songMap();
        CHECK(qm.count == 4 && q.result().contrast.loud == 3 && qm.has[3][dGlue] && std::abs(qm.val[3][dGlue] - p.songMap().val[3][dGlue]) < 1e-4f && q.sectionName(3) == "hook", "marks, sections' own settings and names come back");
        CHECK(q.result().any && q.result().moves.size() == r.moves.size() && q.loudnessKnown() && q.result().knee.known, "and so do the assistant's findings and the learnt loudness");

        // ---- TONE in linear phase tells the host about its delay
        {
            const int before = p.getLatencySamples();
            p.setSong(tPhase, 1.0f);
            p.idle();
            const int lin = p.getLatencySamples();
            p.setSong(tPhase, 0.0f);
            p.idle();
            CHECK(lin > before + 1000 && p.getLatencySamples() == before, "linear phase adds its delay, and the host is told");
        }

        // ---- the codec preview
        {
#if JUCE_MAC
            juce::AudioBuffer<float> io(2, block);
            juce::MidiBuffer midi;
            auto runFor = [&](double secs, bool codec) {
                p.setSong(oCodec, codec ? 1.0f : 0.0f);
                double ms = 0; int cnt = 0;
                const int blocks = int(secs * sr / block);
                for (int k = 0; k < blocks; ++k) {
                    for (int i = 0; i < block; ++i) { const float x = 0.2f * std::sin(0.0261799f * float(k * block + i)); io.setSample(0, i, x); io.setSample(1, i, x); }
                    p.processBlock(io, midi);
                    p.codec().drain(50);
                    if (k > blocks * 3 / 4) for (int i = 0; i < block; ++i) { ms += io.getSample(0, i) * io.getSample(0, i); ++cnt; }
                }
                return 10 * std::log10(ms / std::max(1, cnt) + 1e-20);
            };
            const double plain = runFor(1.0, false), coded = runFor(2.0, true);
            std::printf("      codec %s: %.2f dB through AAC against %.2f dB without, peak after encoding %.2f dBTP\n", p.codecAvailable() ? "on" : "unavailable", coded, plain, p.codecPeak());
            CHECK(p.codecAvailable() && std::abs(coded - plain) < 1.5 && p.codecPeak() > -100, "CODEC plays the master back through Apple's AAC encoder");
            p.setSong(oCodec, 0.0f);
#else
            CHECK(!p.codecAvailable() && p.codecWhy().isNotEmpty(), "CODEC says why it can't run here (it needs a Mac)");
#endif
        }

        // ---- presets
        bool presetsOk = true;
        for (int i = 0; i < LacquerProcessor::factoryPresets().size(); ++i) {
            q.loadFactory(i);
            presetsOk = presetsOk && q.presetName == LacquerProcessor::factoryPresets()[i] && !q.songMap().has[3][dGlue];
        }
        CHECK(presetsOk && std::abs(q.songValue(lTarget) + 14) < 0.01f, "factory presets load (and clear the sections' own settings)");

        // ---- the window opens, at any size, with the tray in or out
        bool windowOk = true;
        for (float s : {0.75f, 1.0f, 1.5f})
            for (bool tray : {false, true}) {
                p.uiScale = s; p.trayOpen = tray;
                std::unique_ptr<juce::AudioProcessorEditor> ed(p.createEditor());
                const int wantH = juce::roundToInt((layout::H + (tray ? layout::kTrayH : 0) + (p.showHelpStrip() ? LacquerEditor::kHelpH : 0.0f)) * s);
                windowOk = windowOk && ed->getWidth() == juce::roundToInt(layout::W * s) && ed->getHeight() == wantH;
                for (int pg = 0; pg < 6; ++pg) { p.page = pg; for (int tab = 0; tab < 4; ++tab) { p.detailTab = tab; if (pg != 5 && tab > 0) break; auto img = ed->createComponentSnapshot(ed->getLocalBounds(), true, 1.0f); windowOk = windowOk && img.isValid(); } }
            }
        CHECK(windowOk, "the window draws every screen page (and every DETAIL tab) at 75, 100 and 150%, with the tray in and out");
    }

    // ---- block sizes, mono, offline
    {
        const double d = blockSizeDiff(song, false, 0);
        std::printf("      64-sample blocks against 37/1000: worst difference %.2e\n", d);
        CHECK(d < 1e-5, "the sound doesn't depend on the host's block size");
        const double off = blockSizeDiff(song, true, 1);
        CHECK(off < 1e-5, "a bounce leaves out LISTEN ON (the phone speaker isn't printed)");

        LacquerProcessor p;
        p.setRateAndBufferSizeDetails(44100, 256);
        p.prepareToPlay(44100, 256);
        bool ok = true;
        juce::MidiBuffer midi;
        for (int bs : {1, 17, 256, 4096}) {
            juce::AudioBuffer<float> io(2, bs);
            for (int k = 0; k < 30; ++k) {
                for (int i = 0; i < bs; ++i) { io.setSample(0, i, 0.4f * float(std::sin(0.03 * (k * bs + i)))); io.setSample(1, i, 0.0f); }
                p.processBlock(io, midi);
                ok = ok && finite(io);
            }
        }
        CHECK(ok, "blocks of 1 to 4096 samples (bigger than it was told) all come out finite");
        LacquerProcessor mono;
        juce::AudioProcessor::BusesLayout l;
        l.inputBuses.add(juce::AudioChannelSet::mono());
        l.outputBuses.add(juce::AudioChannelSet::mono());
        CHECK(mono.setBusesLayout(l), "it runs on a mono track");
        mono.setRateAndBufferSizeDetails(48000, 512);
        mono.prepareToPlay(48000, 512);
        juce::AudioBuffer<float> io(1, 512);
        bool monoOk = true;
        for (int k = 0; k < 200; ++k) {
            for (int i = 0; i < 512; ++i) io.setSample(0, i, song.L[size_t(k * 512 + i)]);
            mono.processBlock(io, midi);
            monoOk = monoOk && finite(io);
        }
        CHECK(monoOk && io.getMagnitude(0, 0, 512) > 0.01f, "and makes sound there");
    }
    std::printf("%s\n", fails ? "SOME TESTS FAILED" : "all tests passed");
    return fails ? 1 : 0;
}
