// Renders the window to PNG files after playing the test song through the plugin,
// for checking it without a host:  ui_snapshot [folder]
#include "PluginEditor.h"
#include "PluginProcessor.h"
#include "testsig.hpp"

#include <cstring>

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

const testsig::Song& song() {
    static const auto s = testsig::song(48000, 120, {{4, false}, {8, false}, {8, true}, {8, false}, {8, true}, {4, false}}, 0.8);
    return s;
}

// Plays from `fromSec` for `seconds`, 30 frames a second, ticking the window's meters as it goes.
void play(LacquerProcessor& p, Head& head, LacquerEditor* ed, double fromSec, double seconds, bool transport, const std::function<void(int)>& atBar = {}) {
    const auto& s = song();
    const int block = 1600;
    juce::AudioBuffer<float> io(2, block);
    juce::MidiBuffer midi;
    head.playing = transport;
    int lastBar = -1;
    for (size_t at = size_t(fromSec * s.fs); at + block <= s.L.size() && at < size_t((fromSec + seconds) * s.fs); at += block) {
        head.ppq = double(at) / s.fs * s.bpm / 60.0;
        for (int i = 0; i < block; ++i) { io.setSample(0, i, s.L[at + size_t(i)]); io.setSample(1, i, s.R[at + size_t(i)]); }
        p.processBlock(io, midi);
        const int bar = int(double(at) / (s.secondsPerBar() * s.fs));
        if (bar != lastBar) { lastBar = bar; if (atBar) atBar(bar); }
        if (ed) ed->tick(1);
    }
    if (transport) { head.playing = false; io.clear(); p.processBlock(io, midi); }
}

// A session that has been listened to: sections marked, the assistant's moves waiting.
juce::MemoryBlock listenedSession() {
    LacquerProcessor p;
    Head head;
    p.setPlayHead(&head);
    p.setRateAndBufferSizeDetails(48000, 1600);
    p.prepareToPlay(48000, 1600);
    play(p, head, nullptr, 0, 1000, true);
    p.setListening(true);
    p.setMarking(true);
    play(p, head, nullptr, 0, 1000, true, [&](int bar) {
        if (bar == 0) p.tapKey(1);
        if (bar == 4 || bar == 20) p.tapKey(2);
        if (bar == 10 || bar == 26) p.tapKey(3);
        if (bar == 12 || bar == 28) p.tapKey(4);
        if (bar == 36) p.tapKey(8);
    });
    p.setMarking(false);
    p.idle();
    p.setSong(cDrive, 3.0f);
    juce::MemoryBlock m;
    p.getStateInformation(m);
    return m;
}

void shot(const juce::String& file, const juce::MemoryBlock* session, const std::function<void(LacquerProcessor&)>& setup, float scale = 1.0f,
          const char* help = nullptr, bool tray = false, int pick = -1) {
    LacquerProcessor proc;
    Head head;
    proc.setPlayHead(&head);
    proc.setRateAndBufferSizeDetails(48000, 1600);
    proc.prepareToPlay(48000, 1600);
    if (session) proc.setStateInformation(session->getData(), int(session->getSize()));
    proc.uiScale = scale;
    proc.trayOpen = tray;
    if (setup) setup(proc);
    std::unique_ptr<juce::AudioProcessorEditor> ed(proc.createEditor());
    auto* le = dynamic_cast<LacquerEditor*>(ed.get());
    play(proc, head, le, 26, 5.0, false);   // a chorus, transport stopped: the meters move, the keys stay put
    if (help) le->showHelp(help);
    if (pick >= 0) le->pickBlock(pick);
    if (help) for (int i = 0; i < layout::kNumControls; ++i) if (std::strcmp(layout::kControls[i].id, help) == 0 && layout::kControls[i].type == layout::Knob) le->showBubble(i);
    auto img = ed->createComponentSnapshot(ed->getLocalBounds(), true, 2.0f);
    juce::File f(file);
    f.deleteFile();
    juce::FileOutputStream os(f);
    juce::PNGImageFormat().writeImageToStream(img, os);
    std::printf("wrote %s (%dx%d)\n", f.getFullPathName().toRawUTF8(), img.getWidth(), img.getHeight());
}

}  // namespace

int main(int argc, char** argv) {
    juce::ScopedJuceInitialiser_GUI gui;
    const juce::String dir = argc > 1 ? argv[1] : ".";
    const auto session = listenedSession();
    shot(dir + "/ui_balance.png", &session, [](LacquerProcessor& p) { p.page = 0; }, 1.0f, "crt_bal");
    shot(dir + "/ui_loudness.png", &session, [](LacquerProcessor& p) {
        p.page = 1;
        for (int i = 0; i < int(p.result().moves.size()); ++i)
            if (p.result().moves[size_t(i)].kind == mkLift) p.takeMove(i, true);
        p.setEditKey(0);
    }, 1.0f, "crt_loud");
    shot(dir + "/ui_moves.png", &session, [](LacquerProcessor& p) {
        p.page = 2;
        for (int i = 0; i < int(p.result().moves.size()); i += 2) p.takeMove(i, true);
    }, 1.0f, "a_amount");
    shot(dir + "/ui_checks.png", &session, [](LacquerProcessor& p) { p.page = 3; }, 1.0f, "key_checks");
    shot(dir + "/ui_sections.png", &session, [](LacquerProcessor& p) {
        p.page = 4;
        p.setEditKey(4);
        p.setShown(dGlue, 6.0f);
        p.setShown(sWidth, 125.0f);
        p.setShown(spIn, 1.0f);
    }, 1.0f, "sec_4", false, 3);
    shot(dir + "/ui_tray.png", &session, [](LacquerProcessor& p) { p.page = 4; p.setShown(spIn, 1.0f); p.setShown(spAmount, -14.0f); }, 1.0f, "sp_type", true);
    shot(dir + "/ui_detail_tone.png", &session, [](LacquerProcessor& p) {
        p.page = 5; p.detailTab = 0; p.setEditKey(0);
        p.setSong(tMud, -2.5f); p.setSong(tMudHz, 280.0f); p.setSong(tAirPl, 2.0f); p.setSong(tAir, 3.0f);
    }, 1.0f, "crt_detail");
    shot(dir + "/ui_detail_dyn.png", &session, [](LacquerProcessor& p) { p.page = 5; p.detailTab = 1; p.setEditKey(0); p.setSong(dGlue, 7.0f); }, 1.0f, "d_glue");
    shot(dir + "/ui_detail_loud.png", &session, [](LacquerProcessor& p) { p.page = 5; p.detailTab = 2; p.setEditKey(0); p.setSong(lTarget, -8.0f); }, 1.0f, "l_target");
    shot(dir + "/ui_detail_out.png", &session, [](LacquerProcessor& p) { p.page = 5; p.detailTab = 3; p.setEditKey(0); }, 1.0f, "o_before");
    shot(dir + "/ui_empty.png", nullptr, nullptr, 1.0f);
    shot(dir + "/ui_75.png", &session, [](LacquerProcessor& p) { p.page = 2; p.takeAll(); }, 0.75f);
    return 0;
}
