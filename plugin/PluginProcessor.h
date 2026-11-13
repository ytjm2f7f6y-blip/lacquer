#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_audio_formats/juce_audio_formats.h>

#include "lq_assistant.hpp"
#include "lq_engine.hpp"

#include <array>
#include <atomic>
#include <memory>
#include <vector>

#ifndef JucePlugin_Name
 #define JucePlugin_Name "Lacquer"   // set by the plugin build; this covers test builds
#endif
#ifndef JucePlugin_VersionString
 #define JucePlugin_VersionString "0.0.0"
#endif

// Lacquer: a mastering desk with an assistant, for the master bus.
//
// The host's parameters are the song's settings. A section's own settings (what it changes from
// the song) live in the engine's song map and are saved with the session. The knobs show the
// settings of the lit key: SONG, or a section (while the song plays, the section it's in).
class LacquerProcessor final : public juce::AudioProcessor, private juce::Timer {
public:
    LacquerProcessor();
    ~LacquerProcessor() override;

    void prepareToPlay(double sampleRate, int samplesPerBlock) override;
    void releaseResources() override {}
    bool isBusesLayoutSupported(const BusesLayout& layouts) const override;
    void processBlock(juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    using AudioProcessor::processBlock;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    const juce::String getName() const override { return JucePlugin_Name; }
    bool acceptsMidi() const override { return false; }
    bool producesMidi() const override { return false; }
    bool isMidiEffect() const override { return false; }
    double getTailLengthSeconds() const override { return 3.0; }
    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram(int) override {}
    const juce::String getProgramName(int) override { return presetName; }
    void changeProgramName(int, const juce::String&) override {}

    void getStateInformation(juce::MemoryBlock& destData) override;
    void setStateInformation(const void* data, int sizeInBytes) override;

    juce::AudioProcessorValueTreeState apvts;

    // ---- the song's settings (the host's parameters)
    juce::RangedAudioParameter* param(int p) const { return params_[size_t(p)]; }
    float songValue(int p) const { return raw_[size_t(p)]->load(std::memory_order_relaxed); }
    lq::Values songValues() const;
    void setSong(int p, float value);                 // in the parameter's own units, host told

    // ---- the lit key and what the knobs show
    int litKey() const;                               // 0 the song, 1..8 a section
    void setEditKey(int key);
    int editKey() const { return engine_.editKey_.load(); }
    float shownValue(int p) const;                    // the lit key's value
    bool shownIsOwn(int p) const;                     // the lit section has its own value for p
    void setShown(int p, float value);                // edits the song, or the lit section's own value
    void resetShown(int p);                           // a section: follow the song again; the song: default
    lq::Values valuesFor(int key) const { return engine_.valuesFor(key, songValues()); }

    // ---- sections
    const lq::SongMap& songMap() const { return engine_.song().read(); }
    void editSongMap(const std::function<void(lq::SongMap&)>& f) { engine_.song().edit(f); }
    bool marking() const { return marking_; }
    void setMarking(bool on);
    void tapKey(int key);                             // a section key pressed: mark (while marking) or edit
    bool follow() const { return follow_.load(); }    // while playing, the lit key follows the song
    void setFollow(bool on);
    void moveMark(int index, double ppq);
    void removeMark(int index);
    void setMarkKey(int index, int key);
    int addMarkAtPlayhead(int key);                   // the bar the playhead is in; returns the new mark's index
    int splitMark(int index);                         // halves a block (on a bar line); returns the second half's index
    void joinMark(int index);                         // folds a block into the one before it
    double blockEnd(int index) const;                 // where a marked block ends
    double barLength() const { return quartersPerBar_.load(); }   // in quarter notes
    double songEndPpq() const;                        // how far the SECTIONS screen reaches
    juce::String sectionName(int key) const { return key <= 0 ? "song" : sectionNames_[size_t(juce::jlimit(1, kSections, key) - 1)]; }
    void setSectionName(int key, const juce::String& n);
    static constexpr int kSections = lq::kNumKeys - 1;
    static juce::StringArray sectionVocabulary();     // names offered for a section key
    static juce::String plural(const juce::String& name) { return juce::String(lq::plural(name.toStdString())); }

    // ---- the transport, as the audio thread last saw it
    bool playing() const { return playing_.load(); }
    double playheadPpq() const { return ppq_.load(); }

    // ---- the listening pass and the assistant (message thread)
    bool listening() const { return listenWanted_.load(); }
    void setListening(bool on);
    double listenedSeconds() const { return listenSeconds_.load(); }
    const lq::Result& result() const { return result_; }
    const lq::Target& target() const { return target_; }
    int genre() const { return genre_; }
    void setGenre(int g);
    int numRefs() const { return int(refCurves_.size()); }
    int numMine() const { return int(mineCurves_.size()); }
    void addReferences(const juce::StringArray& files);   // analysed off the audio thread
    bool analysingRefs() const { return refJobs_.load() > 0; }
    void clearReferences();
    void rememberAsMine();                            // the current expected balance joins MINE
    void clearMine();
    void takeMove(int i, bool take);
    void takeAll();
    void applyAmount();                               // AMOUNT moved: rescale the taken moves
    int ideaCount(int module) const;                  // taken moves working in a module (0 clean .. 5 loudness)
    // How far the loudest section stands over section k: in the mix, and (about) after the limiter
    // with the targets as they are now. False if the pass didn't hear both.
    bool sectionGap(int k, double& mix, double& master) const;
    double driveFor(int key) const;                   // the drive a key plays with now (dB)
    juce::String lastNote;                            // a one-line note for the screen (blind result etc.)

    // ---- A / B and blind
    int abSlot() const { return ab_; }
    void selectAb(int slot);                          // 0 A, 1 B (blind: the hidden one)
    bool blind() const { return blind_; }
    void setBlind(bool on);

    // ---- presets
    juce::String presetName = "room to breathe";
    static juce::StringArray factoryPresets();
    void loadFactory(int i);
    juce::File presetFolder() const;
    void savePreset(const juce::String& name);
    void loadPreset(const juce::File& f);

    // ---- meters
    lq::Meters& meters() { return engine_.meters(); }
    const lq::Meters& meters() const { return const_cast<lq::Engine&>(engine_).meters(); }
    double sampleRate() const { return sampleRate_; }
    void resetIntegrated() { resetPass_.store(true); }
    bool loudnessKnown() const { return engine_.measuredKnown(); }

    // ---- the window
    float uiScale = 1.0f;
    int page = 0;                                     // 0 balance, 1 loudness, 2 moves, 3 checks, 4 sections
    bool trayOpen = false;
    std::atomic<int> revision{0};                     // bumps when anything the window shows changes

    // The message thread's housekeeping (finishing a listening pass, following AMOUNT and TARGET).
    // The timer calls it; the tests call it directly.
    void idle();

private:
    void timerCallback() override { idle(); }
    void finishPass();
    void runAssistant();
    void rebuildTarget();
    void loadTaste();
    void saveTaste() const;
    juce::ValueTree snapshot() const;                 // song settings and sections' own values (A/B, presets)
    void restore(const juce::ValueTree& t, bool withMarks);
    float moveValue(const lq::Move& m) const;
    void setAt(int key, int p, float v);              // a key's value (the song's: host told)
    void clearAt(int key, int p);
    void bump() { revision.fetch_add(1); }

    lq::Engine engine_;
    std::array<juce::RangedAudioParameter*, lq::kNumParams> params_{};
    std::array<std::atomic<float>*, lq::kNumParams> raw_{};
    double sampleRate_ = 48000;

    std::vector<float> mono_;
    std::shared_ptr<std::atomic<bool>> alive_ = std::make_shared<std::atomic<bool>>(true);
    float lastAmount_ = -1;
    int lastTarget_ = -1;
    bool haveStats_ = false;

    // transport
    std::atomic<bool> playing_{false};
    std::atomic<double> ppq_{0}, lastBarPpq_{0}, quartersPerBar_{4}, maxPpq_{0};
    bool wasPlaying_ = false;
    double playedSeconds_ = 0, longestPlay_ = 0;
    std::atomic<bool> calibrated_{false};

    // listening
    std::atomic<bool> listenWanted_{false}, passReady_{false}, resetPass_{false};
    bool engineListening_ = false;
    std::atomic<double> listenSeconds_{0};
    lq::SongStats lastStats_;
    lq::Result result_;
    lq::Target target_;
    lq::Taste taste_;
    int genre_ = 0;
    std::vector<std::vector<double>> refCurves_, mineCurves_;
    std::atomic<int> refJobs_{0};
    juce::ThreadPool pool_{1};

    // sections
    bool marking_ = false;
    std::atomic<bool> follow_{true};
    double lastTapPpq_ = -1e9;
    std::array<juce::String, kSections> sectionNames_;

    // A / B
    int ab_ = 0;
    juce::ValueTree abState_[2];
    bool blind_ = false, blindSwap_ = false;
    int blindPicks_ = 0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(LacquerProcessor)
};
