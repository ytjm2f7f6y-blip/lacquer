#include "PluginProcessor.h"

#include "PluginEditor.h"

using namespace lq;

namespace {

const juce::String kMinus = juce::String::fromUTF8("\xe2\x88\x92");

juce::String signedNum(float v, int dp) {
    juce::String s = juce::String(std::abs(v), dp);
    if (v < -0.00001f && s.getDoubleValue() != 0.0) return kMinus + s;
    return s;
}

// A value as the faceplate and the host show it.
juce::String valueText(int p, float v) {
    const auto& d = def(p);
    const juce::String unit = d.unit;
    switch (d.kind) {
        case kChoice: return juce::StringArray::fromTokens(d.choices, "|", "")[juce::jlimit(0, d.nChoices - 1, juce::roundToInt(v))];
        case kBool:
            if (d.choices) return juce::StringArray::fromTokens(d.choices, "|", "")[v >= 0.5f ? 1 : 0];
            return v >= 0.5f ? "on" : "off";
        case kOffLog: if (v <= 0.5f) return "off"; return juce::String(juce::roundToInt(v)) + " " + unit;
        default: break;
    }
    if (p == spDecay) return juce::String(v, 1) + " s";
    if (p == spHigh) return juce::String(v, 1) + " kHz";
    if (p == spLow) return juce::String(juce::roundToInt(v)) + " Hz";
    if (unit == "%" || p == spPre) return juce::String(juce::roundToInt(v)) + (unit.isEmpty() ? "" : " " + unit);
    if (unit == "dB" || unit == "LUFS" || unit == "dBTP") {
        const bool plus = unit == "dB" && p != oMonitor && p != spAmount;
        return (plus && v > 0.049f ? "+" : "") + signedNum(v, 1) + " " + unit;
    }
    return signedNum(v, 1) + (unit.isEmpty() ? "" : " " + unit);
}

juce::AudioProcessorValueTreeState::ParameterLayout createLayout() {
    juce::AudioProcessorValueTreeState::ParameterLayout l;
    for (int p = 0; p < kNumParams; ++p) {
        const auto& d = def(p);
        const juce::ParameterID id{d.id, 1};
        if (d.kind == kChoice) {
            l.add(std::make_unique<juce::AudioParameterChoice>(id, d.name, juce::StringArray::fromTokens(d.choices, "|", ""), int(d.def)));
        } else if (d.kind == kBool) {
            l.add(std::make_unique<juce::AudioParameterBool>(id, d.name, d.def >= 0.5f,
                juce::AudioParameterBoolAttributes().withStringFromValueFunction([p](bool b, int) { return valueText(p, b ? 1.0f : 0.0f); })));
        } else {
            const float lo = d.kind == kOffLog ? 0.0f : d.lo;
            juce::NormalisableRange<float> range(lo, d.hi,
                [p](float, float, float n) { return fromNorm(p, n); },
                [p](float, float, float x) { return toNorm(p, x); });
            l.add(std::make_unique<juce::AudioParameterFloat>(id, d.name, range, d.def,
                juce::AudioParameterFloatAttributes()
                    .withStringFromValueFunction([p](float v, int) { return valueText(p, v); })
                    .withValueFromStringFunction([p](const juce::String& s) {
                        const auto t = s.trim().toLowerCase();
                        if (t == "off") return 0.0f;
                        return t.replace(kMinus, "-").getFloatValue();
                    })));
        }
    }
    return l;
}

juce::PropertiesFile::Options settingsOptions() {
    juce::PropertiesFile::Options o;
    o.applicationName = "Lacquer";
    o.folderName = "Rumpus Room Records";
    o.filenameSuffix = ".settings";
    o.osxLibrarySubFolder = "Application Support";
    return o;
}

juce::String curveToText(const double* c, int n) {
    juce::StringArray a;
    for (int i = 0; i < n; ++i) a.add(juce::String(c[i], 2));
    return a.joinIntoString(",");
}
std::vector<double> curveFromText(const juce::String& s) {
    std::vector<double> v;
    for (const auto& t : juce::StringArray::fromTokens(s, ",", "")) v.push_back(t.getDoubleValue());
    if (v.size() != size_t(kBands)) v.clear();
    return v;
}

}  // namespace

// ---------------------------------------------------------------------------
LacquerProcessor::LacquerProcessor()
    : AudioProcessor(BusesProperties()
                         .withInput("Input", juce::AudioChannelSet::stereo(), true)
                         .withOutput("Output", juce::AudioChannelSet::stereo(), true)),
      apvts(*this, nullptr, "params", createLayout()) {
    for (int p = 0; p < kNumParams; ++p) {
        params_[size_t(p)] = apvts.getParameter(def(p).id);
        raw_[size_t(p)] = apvts.getRawParameterValue(def(p).id);
        jassert(params_[size_t(p)] != nullptr);
    }
    engine_.prepare(48000, 512);
    loadTaste();
    rebuildTarget();
    startTimerHz(15);
}

LacquerProcessor::~LacquerProcessor() {
    alive_->store(false);
    stopTimer();
    pool_.removeAllJobs(true, 8000);
}

bool LacquerProcessor::isBusesLayoutSupported(const BusesLayout& layouts) const {
    const auto in = layouts.getMainInputChannelSet(), out = layouts.getMainOutputChannelSet();
    return in == out && (out == juce::AudioChannelSet::stereo() || out == juce::AudioChannelSet::mono());
}

void LacquerProcessor::prepareToPlay(double sampleRate, int samplesPerBlock) {
    sampleRate_ = sampleRate;
    engine_.prepare(sampleRate, std::max(16, samplesPerBlock));
    setLatencySamples(engine_.latency());
}

Values LacquerProcessor::songValues() const {
    Values v;
    for (int p = 0; p < kNumParams; ++p) v[p] = raw_[size_t(p)]->load(std::memory_order_relaxed);
    return v;
}

void LacquerProcessor::setSong(int p, float value) {
    auto* prm = params_[size_t(p)];
    prm->setValueNotifyingHost(prm->convertTo0to1(value));
    bump();
}

// ---------------------------------------------------------------------------
// Audio
void LacquerProcessor::processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer&) {
    juce::ScopedNoDenormals noDenormals;
    const int n = buffer.getNumSamples();
    if (n <= 0) return;
    Transport t;
    if (auto* ph = getPlayHead())
        if (auto pos = ph->getPosition()) {
            t.playing = pos->getIsPlaying();
            if (auto q = pos->getPpqPosition()) { t.ppq = *q; t.hasPpq = true; }
            if (auto b = pos->getBpm()) t.bpm = *b > 1 ? *b : 120.0;
            if (auto ts = pos->getTimeSignature()) if (ts->denominator > 0) t.quartersPerBar = ts->numerator * 4.0 / ts->denominator;
            if (auto lb = pos->getPpqPositionOfLastBarStart()) lastBarPpq_.store(*lb);
        }
    t.nonRealtime = isNonRealtime();
    playing_.store(t.playing);
    ppq_.store(t.ppq);
    quartersPerBar_.store(t.quartersPerBar);
    if (t.playing && t.ppq > maxPpq_.load()) maxPpq_.store(t.ppq);
    const double secs = double(n) / sampleRate_;

    // Each play starts a fresh count for the loudness (and the integrated readout).
    if ((t.playing && !wasPlaying_) || resetPass_.exchange(false)) {
        if (!engineListening_) engine_.clearPass();
        playedSeconds_ = 0;
    }
    // The listening pass.
    const bool want = listenWanted_.load();
    if (want && !engineListening_) {
        engine_.analyser().reset();
        engine_.analyser().stats().clear();
        engine_.clearPass();
        engineListening_ = true;
        listenSeconds_.store(0);
    }
    auto endPass = [&] {
        engineListening_ = false;
        engine_.setListening(false);
        engine_.setMeasuredInput(engine_.calibratedInput(), true);
        passReady_.store(true);
    };
    if (!want && engineListening_) endPass();
    if (wasPlaying_ && !t.playing) {
        if (engineListening_ && listenSeconds_.load() > 8.0) { listenWanted_.store(false); endPass(); }
        else if (!engineListening_ && playedSeconds_ >= 20.0 && (!engine_.measuredKnown() || playedSeconds_ >= 0.5 * longestPlay_)) {
            // A long enough play keeps the loudness stage calibrated as the settings change.
            engine_.setMeasuredInput(engine_.calibratedInput(), true);
            longestPlay_ = std::max(longestPlay_, playedSeconds_);
            calibrated_.store(true);
        }
    }
    engine_.setListening(engineListening_);
    if (engineListening_ && t.playing) listenSeconds_.store(listenSeconds_.load() + secs);
    if (t.playing) playedSeconds_ += secs;
    wasPlaying_ = t.playing;

    const Values v = songValues();
    float* L = buffer.getWritePointer(0);
    if (buffer.getNumChannels() >= 2) {
        engine_.process(L, buffer.getWritePointer(1), n, v, t);
    } else {
        if (int(mono_.size()) < n) mono_.resize(size_t(n));
        std::copy(L, L + n, mono_.begin());
        engine_.process(L, mono_.data(), n, v, t);
        for (int i = 0; i < n; ++i) L[i] = 0.5f * (L[i] + mono_[size_t(i)]);
    }
    for (int c = 2; c < buffer.getNumChannels(); ++c) buffer.clear(c, 0, n);
}

// ---------------------------------------------------------------------------
// Keys and values
int LacquerProcessor::litKey() const {
    if (playing_.load() && songMap().count > 0) return meters().key.load();
    return engine_.editKey_.load();
}

void LacquerProcessor::setEditKey(int key) {
    engine_.editKey_.store(juce::jlimit(0, kNumKeys - 1, key));
    bump();
}

float LacquerProcessor::shownValue(int p) const {
    const int k = litKey();
    if (k > 0 && def(p).perSection) {
        const auto& m = songMap();
        if (m.has[k][p]) return m.val[k][p];
    }
    return songValue(p);
}

bool LacquerProcessor::shownIsOwn(int p) const {
    const int k = litKey();
    return k > 0 && def(p).perSection && songMap().has[k][p];
}

void LacquerProcessor::setAt(int key, int p, float v) {
    const auto& d = def(p);
    v = juce::jlimit(d.kind == kOffLog ? 0.0f : d.lo, d.hi, v);
    if (key <= 0 || !d.perSection) { setSong(p, v); return; }
    editSongMap([&](SongMap& m) { m.has[key][p] = true; m.val[key][p] = v; });
    bump();
}

void LacquerProcessor::clearAt(int key, int p) {
    if (key <= 0) return;
    editSongMap([&](SongMap& m) { m.has[key][p] = false; });
    bump();
}

void LacquerProcessor::setShown(int p, float value) { setAt(litKey(), p, value); }

void LacquerProcessor::resetShown(int p) {
    const int k = litKey();
    if (k > 0 && def(p).perSection) { clearAt(k, p); return; }
    setSong(p, def(p).def);
}

// ---------------------------------------------------------------------------
// Sections
void LacquerProcessor::setMarking(bool on) {
    marking_ = on;
    lastTapPpq_ = ppq_.load() - 1e-6;
    bump();
}

void LacquerProcessor::tapKey(int key) {
    key = juce::jlimit(0, kNumKeys - 1, key);
    if (marking_ && playing_.load()) {
        const double q = quartersPerBar_.load(), at = ppq_.load(), last = lastBarPpq_.load();
        double mark = (at - last >= 0 && at - last < 4 * q) ? (at - last < q * 0.5 ? last : last + q) : std::round(at / q) * q;
        mark = std::max(0.0, mark);
        const double from = std::min(lastTapPpq_, mark - 1e-6);
        editSongMap([&](SongMap& m) {
            for (int i = m.count - 1; i >= 0; --i)
                if (m.marks[i].ppq > from + 1e-6 && m.marks[i].ppq <= mark + 1e-6) m.removeMark(i);
            m.addMark(mark, key);
        });
        lastTapPpq_ = mark;
    }
    engine_.editKey_.store(key);
    bump();
}

void LacquerProcessor::moveMark(int index, double ppq) {
    const double q = quartersPerBar_.load();
    const double at = std::max(0.0, std::round(ppq / q) * q);
    editSongMap([&](SongMap& m) {
        if (index < 0 || index >= m.count) return;
        const int key = m.marks[index].key;
        m.removeMark(index);
        m.addMark(at, key);
    });
    bump();
}

void LacquerProcessor::removeMark(int index) {
    editSongMap([&](SongMap& m) { m.removeMark(index); });
    bump();
}

void LacquerProcessor::setMarkKey(int index, int key) {
    editSongMap([&](SongMap& m) { if (index >= 0 && index < m.count) m.marks[index].key = key; });
    bump();
}

double LacquerProcessor::songEndPpq() const {
    const auto& m = songMap();
    const double q = quartersPerBar_.load();
    double end = std::max(maxPpq_.load(), 16 * q);
    if (m.count > 0) end = std::max(end, m.marks[m.count - 1].ppq + 8 * q);
    return end;
}

void LacquerProcessor::setSectionName(int key, const juce::String& n) {
    if (key < 1 || key > 5) return;
    sectionNames_[size_t(key - 1)] = n.trim().substring(0, 14).toLowerCase();
    bump();
}

// ---------------------------------------------------------------------------
// The listening pass and the assistant
void LacquerProcessor::setListening(bool on) {
    listenWanted_.store(on);
    bump();
}

void LacquerProcessor::idle() {
    if (passReady_.exchange(false)) finishPass();
    if (calibrated_.exchange(false)) bump();
    const float amt = songValue(aAmount);
    if (std::abs(amt - lastAmount_) > 1e-4f) { lastAmount_ = amt; applyAmount(); }
    const int tgt = juce::roundToInt(songValue(aTarget));
    if (tgt != lastTarget_) { lastTarget_ = tgt; rebuildTarget(); if (result_.any) runAssistant(); }
}

void LacquerProcessor::finishPass() {
    lastStats_ = engine_.analyser().stats();   // the audio thread has stopped feeding it
    haveStats_ = lastStats_.key[0].frames >= 8;
    if (haveStats_) runAssistant();
    bump();
}

void LacquerProcessor::rebuildTarget() {
    const int t = juce::roundToInt(songValue(aTarget));
    if (t == 1 && !refCurves_.empty()) target_ = averageTarget(refCurves_, juce::String(int(refCurves_.size())).toStdString() + (refCurves_.size() == 1 ? " reference" : " references"));
    else if (t == 2 && !mineCurves_.empty()) target_ = averageTarget(mineCurves_, "your masters");
    else target_ = genreTarget(genre_);
    if (t == 1 && refCurves_.empty()) target_.name = "no references yet: " + target_.name;
    if (t == 2 && mineCurves_.empty()) target_.name = "no masters yet: " + target_.name;
}

void LacquerProcessor::setGenre(int g) {
    int n = 0;
    genres(n);
    genre_ = (g % n + n) % n;
    rebuildTarget();
    if (haveStats_) runAssistant();
    bump();
}

void LacquerProcessor::runAssistant() {
    if (!haveStats_) return;
    rebuildTarget();
    const bool learn = songValue(aLearn) >= 0.5f;
    // Moves the last pass suggested and you left alone count as skipped.
    if (learn) for (const auto& m : result_.moves) if (!m.taken) taste_.skipped[m.kind] += 1;
    // Taken moves stay in force: the new pass is measured with them, and won't suggest them again.
    Values sec[kNumKeys];
    const Values song = songValues();
    for (int k = 0; k < kNumKeys; ++k) sec[k] = valuesFor(k);
    result_ = assist(lastStats_, target_, song, sec, taste_, learn);
    engine_.setKnee(result_.knee.kneeLufs, result_.knee.known);
    if (learn) saveTaste();
    bump();
}

float LacquerProcessor::moveValue(const Move& m) const {
    if (m.absolute) return m.full;
    return m.before + m.full * songValue(aAmount) / 100.0f;
}

void LacquerProcessor::takeMove(int i, bool take) {
    if (i < 0 || i >= int(result_.moves.size())) return;
    auto& m = result_.moves[size_t(i)];
    if (take == m.taken) return;
    const auto& map = songMap();
    auto valueAt = [&](int key, int p) { return (key > 0 && def(p).perSection && map.has[key][p]) ? map.val[key][p] : songValue(p); };
    auto ownAt = [&](int key, int p) { return key > 0 && def(p).perSection && map.has[key][p]; };
    if (take) {
        m.before = valueAt(m.key, m.param);
        m.beforeOwn = ownAt(m.key, m.param);
        if (m.extraParam >= 0) { m.beforeExtra = valueAt(m.key, int(m.extraParam)); m.beforeExtraOwn = ownAt(m.key, int(m.extraParam)); }
        m.taken = true;
        setAt(m.key, m.param, moveValue(m));
        if (m.extraParam >= 0) setAt(m.key, int(m.extraParam), m.extraValue);
        if (songValue(aLearn) >= 0.5f) taste_.taken[m.kind] += 1;
    } else {
        m.taken = false;
        if (m.key > 0 && !m.beforeOwn && def(m.param).perSection) clearAt(m.key, m.param); else setAt(m.key, m.param, m.before);
        if (m.extraParam >= 0) {
            if (m.key > 0 && !m.beforeExtraOwn && def(int(m.extraParam)).perSection) clearAt(m.key, int(m.extraParam));
            else setAt(m.key, int(m.extraParam), m.beforeExtra);
        }
        if (songValue(aLearn) >= 0.5f) taste_.skipped[m.kind] += 1;
    }
    saveTaste();
    bump();
}

void LacquerProcessor::takeAll() {
    for (int i = 0; i < int(result_.moves.size()); ++i) takeMove(i, true);
}

void LacquerProcessor::applyAmount() {
    for (const auto& m : result_.moves)
        if (m.taken && !m.absolute) setAt(m.key, m.param, moveValue(m));
    bump();
}

int LacquerProcessor::ideaCount(int module) const {
    static const char* names[6] = {"CLEAN", "TONE", "DYNAMICS", "COLOUR", "STEREO", "LOUDNESS"};
    int n = 0;
    for (const auto& m : result_.moves) if (m.taken && m.module == names[module]) ++n;
    return n;
}

void LacquerProcessor::loadTaste() {
    juce::PropertiesFile f(settingsOptions());
    for (int k = 0; k < kNumMoveKinds; ++k) {
        taste_.taken[k] = float(f.getDoubleValue("taken" + juce::String(k), 0));
        taste_.skipped[k] = float(f.getDoubleValue("skipped" + juce::String(k), 0));
    }
    mineCurves_.clear();
    for (const auto& line : juce::StringArray::fromLines(f.getValue("mine")))
        if (auto c = curveFromText(line); !c.empty()) mineCurves_.push_back(std::move(c));
}

void LacquerProcessor::saveTaste() const {
    juce::PropertiesFile f(settingsOptions());
    for (int k = 0; k < kNumMoveKinds; ++k) {
        f.setValue("taken" + juce::String(k), double(taste_.taken[k]));
        f.setValue("skipped" + juce::String(k), double(taste_.skipped[k]));
    }
    juce::StringArray lines;
    for (const auto& c : mineCurves_) lines.add(curveToText(c.data(), kBands));
    f.setValue("mine", lines.joinIntoString("\n"));
    f.saveIfNeeded();
}

void LacquerProcessor::addReferences(const juce::StringArray& files) {
    auto alive = alive_;
    for (const auto& path : files) {
        refJobs_.fetch_add(1);
        pool_.addJob([this, alive, path] {
            std::vector<double> curve;
            {
                juce::AudioFormatManager fm;
                fm.registerBasicFormats();
                std::unique_ptr<juce::AudioFormatReader> r(fm.createReaderFor(juce::File(path)));
                if (r && r->lengthInSamples > 0) {
                    auto an = std::make_unique<Analyser>();
                    an->prepare(r->sampleRate);
                    const int chunk = 8192;
                    juce::AudioBuffer<float> buf(2, chunk);
                    const juce::int64 total = std::min<juce::int64>(r->lengthInSamples, juce::int64(r->sampleRate * 600));
                    for (juce::int64 pos = 0; pos < total && alive->load(); pos += chunk) {
                        const int m = int(std::min<juce::int64>(chunk, total - pos));
                        r->read(&buf, 0, m, pos, true, true);
                        an->push(buf.getReadPointer(0), buf.getReadPointer(r->numChannels > 1 ? 1 : 0), m, 0);
                    }
                    if (an->stats().key[0].frames >= 8) {
                        curve.resize(size_t(kBands));
                        balanceCurve(an->stats().key[0], true, curve.data());
                    }
                }
            }
            juce::MessageManager::callAsync([this, alive, curve] {
                if (!alive->load()) return;
                refJobs_.fetch_sub(1);
                if (!curve.empty()) refCurves_.push_back(curve);
                else lastNote = "COULD NOT READ THAT FILE";
                rebuildTarget();
                if (haveStats_) runAssistant();
                bump();
            });
        });
    }
    bump();
}

void LacquerProcessor::clearReferences() { refCurves_.clear(); rebuildTarget(); if (haveStats_) runAssistant(); bump(); }

void LacquerProcessor::rememberAsMine() {
    if (!result_.any) return;
    std::vector<double> c(static_cast<size_t>(kBands));
    const Values v = songValues();
    for (int b = 0; b < kBands; ++b) c[size_t(b)] = result_.curve[0][b] + toneResponse(bandCentre(b), v);
    mineCurves_.push_back(c);
    if (mineCurves_.size() > 40) mineCurves_.erase(mineCurves_.begin());
    saveTaste();
    rebuildTarget();
    lastNote = "REMEMBERED: " + juce::String(int(mineCurves_.size())) + " OF YOUR MASTERS";
    bump();
}

void LacquerProcessor::clearMine() { mineCurves_.clear(); saveTaste(); rebuildTarget(); bump(); }

// ---------------------------------------------------------------------------
// Snapshots: A/B, presets
juce::ValueTree LacquerProcessor::snapshot() const {
    juce::ValueTree t("snapshot");
    for (int p = 0; p < kNumParams; ++p) t.setProperty(def(p).id, songValue(p), nullptr);
    juce::ValueTree own("own");
    const auto& m = songMap();
    for (int k = 1; k < kNumKeys; ++k)
        for (int p = 0; p < kNumParams; ++p)
            if (m.has[k][p]) {
                juce::ValueTree o("v");
                o.setProperty("key", k, nullptr); o.setProperty("p", def(p).id, nullptr); o.setProperty("value", m.val[k][p], nullptr);
                own.appendChild(o, nullptr);
            }
    t.appendChild(own, nullptr);
    return t;
}

void LacquerProcessor::restore(const juce::ValueTree& t, bool withMarks) {
    if (!t.isValid()) return;
    for (int p = 0; p < kNumParams; ++p)
        if (t.hasProperty(def(p).id)) setSong(p, float(t.getProperty(def(p).id)));
    const auto own = t.getChildWithName("own");
    editSongMap([&](SongMap& m) {
        for (auto& row : m.has) for (auto& h : row) h = false;
        for (const auto& o : own) {
            const int k = o.getProperty("key"), p = findParam(o.getProperty("p").toString().toRawUTF8());
            if (k >= 1 && k < kNumKeys && p >= 0) { m.has[k][p] = true; m.val[k][p] = float(o.getProperty("value")); }
        }
        if (withMarks) {
            m.count = 0;
            for (const auto& mk : t.getChildWithName("marks")) m.addMark(double(mk.getProperty("ppq")), int(mk.getProperty("key")));
        }
    });
    bump();
}

void LacquerProcessor::selectAb(int slot) {
    slot = juce::jlimit(0, 1, slot);
    if (blind_) { slot ^= blindSwap_ ? 1 : 0; ++blindPicks_; }
    if (slot == ab_) return;
    abState_[ab_] = snapshot();
    if (!abState_[slot].isValid()) abState_[slot] = abState_[ab_].createCopy();
    ab_ = slot;
    restore(abState_[slot], false);
}

void LacquerProcessor::setBlind(bool on) {
    if (on == blind_) return;
    blind_ = on;
    if (on) {
        blindSwap_ = juce::Random::getSystemRandom().nextBool();
        blindPicks_ = 0;
        lastNote = "BLIND: A AND B ARE SHUFFLED. PICK BY EAR, THEN PRESS BLIND AGAIN";
    } else {
        lastNote = "YOU PICKED " + juce::String(ab_ == 0 ? "A" : "B") + juce::String(blindPicks_ > 0 ? "" : " (YOU NEVER SWITCHED)");
    }
    bump();
}

juce::StringArray LacquerProcessor::factoryPresets() {
    return {"room to breathe", "loud and proud", "quiet storm", "cut for vinyl", "club weight", "flat start"};
}

void LacquerProcessor::loadFactory(int i) {
    const auto names = factoryPresets();
    if (i < 0 || i >= names.size()) return;
    Values v;   // the factory settings, then each preset's own
    switch (i) {
        case 1: v[lTarget] = -8.0f; v[dGlue] = 5.0f; v[lClip] = 5.0f; v[dPunch] = 1.5f; v[cDrive] = 4.0f; v[aDest] = 3; break;
        case 2: v[lTarget] = -14.0f; v[dGlue] = 2.0f; v[dPunch] = 0.5f; v[cDrive] = 1.5f; v[lClip] = 1.0f; break;
        case 3: v[aDest] = 2; v[lTarget] = -12.0f; v[stIn] = 1; v[sMono] = 150.0f; v[sWidth] = 100.0f; v[cHarsh] = 4.0f; v[oDither] = 2; break;
        case 4: v[aDest] = 3; v[lTarget] = -8.0f; v[tLow] = 1.0f; v[stIn] = 1; v[sMono] = 120.0f; v[dPunch] = 2.0f; break;
        case 5: for (int p : {cleanIn, toneIn, dynIn, colIn, stIn, spIn}) v[p] = 0; v[lTarget] = -14.0f; break;
        default: break;
    }
    for (int p = 0; p < kNumParams; ++p) setSong(p, v[p]);
    editSongMap([](SongMap& m) { for (auto& row : m.has) for (auto& h : row) h = false; });
    presetName = names[i];
    bump();
}

juce::File LacquerProcessor::presetFolder() const {
    return juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory)
#if JUCE_MAC
        .getChildFile("Audio/Presets")
#endif
        .getChildFile("Rumpus Room Records").getChildFile("Lacquer");
}

void LacquerProcessor::savePreset(const juce::String& name) {
    const auto n = name.trim().removeCharacters("/\\:*?\"<>|");
    if (n.isEmpty()) return;
    presetFolder().createDirectory();
    auto t = snapshot();
    t.setProperty("name", n, nullptr);
    if (auto xml = t.createXml()) xml->writeTo(presetFolder().getChildFile(n + ".lacquer"));
    presetName = n.toLowerCase();
    bump();
}

void LacquerProcessor::loadPreset(const juce::File& f) {
    if (auto xml = juce::XmlDocument::parse(f)) {
        restore(juce::ValueTree::fromXml(*xml), false);
        presetName = f.getFileNameWithoutExtension().toLowerCase();
        bump();
    }
}

// ---------------------------------------------------------------------------
// State
void LacquerProcessor::getStateInformation(juce::MemoryBlock& destData) {
    juce::ValueTree root("lacquer");
    root.setProperty("version", JucePlugin_VersionString, nullptr);
    root.appendChild(apvts.copyState(), nullptr);
    // Sections.
    juce::ValueTree sec("sections");
    const auto& m = songMap();
    juce::ValueTree marks("marks");
    for (int i = 0; i < m.count; ++i) {
        juce::ValueTree mk("mark");
        mk.setProperty("ppq", m.marks[i].ppq, nullptr); mk.setProperty("key", m.marks[i].key, nullptr);
        marks.appendChild(mk, nullptr);
    }
    sec.appendChild(marks, nullptr);
    sec.appendChild(snapshot().getChildWithName("own").createCopy(), nullptr);
    for (int k = 1; k <= 5; ++k) sec.setProperty("name" + juce::String(k), sectionNames_[size_t(k - 1)], nullptr);
    sec.setProperty("editKey", editKey(), nullptr);
    root.appendChild(sec, nullptr);
    // The assistant.
    juce::ValueTree as("assistant");
    as.setProperty("measured", engine_.measuredInput(), nullptr);
    as.setProperty("measuredKnown", engine_.measuredKnown(), nullptr);
    as.setProperty("genre", genre_, nullptr);
    juce::StringArray refs;
    for (const auto& c : refCurves_) refs.add(curveToText(c.data(), kBands));
    as.setProperty("refs", refs.joinIntoString("\n"), nullptr);
    if (result_.any) {
        juce::ValueTree r("result");
        for (int k = 0; k < kNumKeys; ++k) {
            r.setProperty("heard" + juce::String(k), result_.keyHeard[k], nullptr);
            r.setProperty("seconds" + juce::String(k), result_.keySeconds[k], nullptr);
            if (result_.keyHeard[k]) r.setProperty("curve" + juce::String(k), curveToText(result_.curve[k], kBands), nullptr);
        }
        r.setProperty("kneeKnown", result_.knee.known, nullptr);
        r.setProperty("knee", result_.knee.kneeLufs, nullptr);
        r.setProperty("kneeIn", result_.knee.inputLufs, nullptr);
        r.setProperty("kept", curveToText(result_.knee.kept, 101), nullptr);
        for (const auto& mv : result_.moves) {
            juce::ValueTree t("move");
            t.setProperty("key", mv.key, nullptr); t.setProperty("p", def(mv.param).id, nullptr); t.setProperty("abs", mv.absolute, nullptr);
            t.setProperty("full", mv.full, nullptr); t.setProperty("extra", mv.extraParam, nullptr); t.setProperty("extraValue", mv.extraValue, nullptr);
            t.setProperty("conf", mv.conf, nullptr); t.setProperty("module", juce::String(mv.module), nullptr);
            t.setProperty("what", juce::String::fromUTF8(mv.what.c_str()), nullptr); t.setProperty("why", juce::String::fromUTF8(mv.why.c_str()), nullptr);
            t.setProperty("taken", mv.taken, nullptr); t.setProperty("before", mv.before, nullptr); t.setProperty("beforeExtra", mv.beforeExtra, nullptr);
            t.setProperty("beforeOwn", mv.beforeOwn, nullptr); t.setProperty("beforeExtraOwn", mv.beforeExtraOwn, nullptr); t.setProperty("kind", mv.kind, nullptr);
            r.appendChild(t, nullptr);
        }
        for (const auto& c : result_.checks) {
            juce::ValueTree t("check");
            t.setProperty("name", juce::String::fromUTF8(c.name.c_str()), nullptr); t.setProperty("what", juce::String::fromUTF8(c.what.c_str()), nullptr); t.setProperty("ok", c.ok, nullptr);
            r.appendChild(t, nullptr);
        }
        for (const auto& n : result_.mixNotes) {
            juce::ValueTree t("note");
            t.setProperty("text", juce::String::fromUTF8(n.c_str()), nullptr);
            r.appendChild(t, nullptr);
        }
        as.appendChild(r, nullptr);
    }
    root.appendChild(as, nullptr);
    // The window.
    juce::ValueTree ui("ui");
    ui.setProperty("scale", uiScale, nullptr); ui.setProperty("page", page, nullptr); ui.setProperty("tray", trayOpen, nullptr);
    ui.setProperty("preset", presetName, nullptr); ui.setProperty("ab", ab_, nullptr);
    for (int s = 0; s < 2; ++s) if (abState_[s].isValid()) { auto c = abState_[s].createCopy(); c.setProperty("slot", s, nullptr); ui.appendChild(c, nullptr); }
    root.appendChild(ui, nullptr);
    if (auto xml = root.createXml()) copyXmlToBinary(*xml, destData);
}

void LacquerProcessor::setStateInformation(const void* data, int sizeInBytes) {
    auto xml = getXmlFromBinary(data, sizeInBytes);
    if (!xml) return;
    const auto root = juce::ValueTree::fromXml(*xml);
    if (!root.hasType("lacquer")) return;
    if (auto p = root.getChildWithName(apvts.state.getType()); p.isValid()) apvts.replaceState(p);
    const auto sec = root.getChildWithName("sections");
    juce::ValueTree snap("snapshot");
    snap.appendChild(sec.getChildWithName("own").createCopy(), nullptr);
    snap.appendChild(sec.getChildWithName("marks").createCopy(), nullptr);
    restore(snap, true);
    for (int k = 1; k <= 5; ++k) if (sec.hasProperty("name" + juce::String(k))) sectionNames_[size_t(k - 1)] = sec.getProperty("name" + juce::String(k)).toString();
    engine_.editKey_.store(juce::jlimit(0, kNumKeys - 1, int(sec.getProperty("editKey", 0))));
    const auto as = root.getChildWithName("assistant");
    if (bool(as.getProperty("measuredKnown", false))) engine_.setMeasuredInput(double(as.getProperty("measured", -14.0)), true);
    genre_ = int(as.getProperty("genre", 0));
    refCurves_.clear();
    for (const auto& line : juce::StringArray::fromLines(as.getProperty("refs").toString()))
        if (auto c = curveFromText(line); !c.empty()) refCurves_.push_back(std::move(c));
    rebuildTarget();
    result_ = Result();
    if (const auto r = as.getChildWithName("result"); r.isValid()) {
        result_.any = true;
        for (int k = 0; k < kNumKeys; ++k) {
            result_.keyHeard[k] = bool(r.getProperty("heard" + juce::String(k), false));
            result_.keySeconds[k] = double(r.getProperty("seconds" + juce::String(k), 0.0));
            const auto c = curveFromText(r.getProperty("curve" + juce::String(k)).toString());
            if (!c.empty()) std::copy(c.begin(), c.end(), result_.curve[k]);
            else result_.keyHeard[k] = false;
        }
        result_.knee.known = bool(r.getProperty("kneeKnown", false));
        result_.knee.kneeLufs = double(r.getProperty("knee", -6.0));
        result_.knee.inputLufs = double(r.getProperty("kneeIn", -20.0));
        const auto kept = juce::StringArray::fromTokens(r.getProperty("kept").toString(), ",", "");
        for (int i = 0; i < std::min(101, kept.size()); ++i) result_.knee.kept[i] = kept[i].getDoubleValue();
        for (const auto& t : r) {
            if (t.hasType("move")) {
                Move mv;
                mv.key = t.getProperty("key"); mv.param = std::max(0, findParam(t.getProperty("p").toString().toRawUTF8())); mv.absolute = t.getProperty("abs");
                mv.full = t.getProperty("full"); mv.extraParam = t.getProperty("extra"); mv.extraValue = t.getProperty("extraValue");
                mv.conf = t.getProperty("conf"); mv.module = t.getProperty("module").toString().toStdString();
                mv.what = t.getProperty("what").toString().toStdString(); mv.why = t.getProperty("why").toString().toStdString();
                mv.taken = t.getProperty("taken"); mv.before = t.getProperty("before"); mv.beforeExtra = t.getProperty("beforeExtra");
                mv.beforeOwn = t.getProperty("beforeOwn"); mv.beforeExtraOwn = t.getProperty("beforeExtraOwn"); mv.kind = t.getProperty("kind");
                result_.moves.push_back(mv);
            } else if (t.hasType("check")) {
                result_.checks.push_back({t.getProperty("name").toString().toStdString(), t.getProperty("what").toString().toStdString(), bool(t.getProperty("ok"))});
            } else if (t.hasType("note")) {
                result_.mixNotes.push_back(t.getProperty("text").toString().toStdString());
            }
        }
        engine_.setKnee(result_.knee.kneeLufs, result_.knee.known);
    }
    const auto ui = root.getChildWithName("ui");
    uiScale = float(ui.getProperty("scale", 1.0f));
    page = int(ui.getProperty("page", 0));
    trayOpen = bool(ui.getProperty("tray", false));
    presetName = ui.getProperty("preset", presetName).toString();
    ab_ = int(ui.getProperty("ab", 0));
    for (const auto& c : ui) if (c.hasType("snapshot")) abState_[juce::jlimit(0, 1, int(c.getProperty("slot", 0)))] = c.createCopy();
    lastAmount_ = songValue(aAmount);
    lastTarget_ = juce::roundToInt(songValue(aTarget));
    bump();
}

juce::AudioProcessorEditor* LacquerProcessor::createEditor() { return new LacquerEditor(*this); }

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter() { return new LacquerProcessor(); }
