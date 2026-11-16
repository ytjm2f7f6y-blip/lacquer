#include "PluginEditor.h"

#include "BinaryData.h"
#include "HelpText.h"

#include <cmath>
#include <cstring>

using namespace layout;

namespace {

constexpr double kFrameRate = 30.0;
const juce::String kDot = juce::String::fromUTF8(" \xc2\xb7 ");
const juce::String kMinus = juce::String::fromUTF8("\xe2\x88\x92");
const juce::String kArrow = juce::String::fromUTF8(" \xe2\x86\x92 ");

juce::Image load(const void* data, int size) { return juce::ImageFileFormat::loadFrom(data, size_t(size)); }

juce::Image named(const juce::String& file) {
    int size = 0;
    const auto res = file.replaceCharacter('.', '_').replaceCharacter('-', '_');
    if (const char* d = BinaryData::getNamedResource(res.toRawUTF8(), size)) return load(d, size);
    return {};
}

// A round knob's strip: colour from a JPEG (premultiplied on black), outline from one mask that every frame shares.
juce::Image roundStrip(const juce::String& jpg, const juce::String& mask) {
    const juce::Image rgb = named(jpg), m = named(mask);
    if (!rgb.isValid() || !m.isValid()) return {};
    const int w = rgb.getWidth(), h = rgb.getHeight(), mw = m.getWidth();
    juce::Image out(juce::Image::ARGB, w, h, true);
    const juce::Image::BitmapData src(rgb, juce::Image::BitmapData::readOnly);
    const juce::Image::BitmapData msk(m, juce::Image::BitmapData::readOnly);
    juce::Image::BitmapData dst(out, juce::Image::BitmapData::writeOnly);
    std::vector<uint8_t> alpha(size_t(mw * mw));
    for (int y = 0; y < mw; ++y)
        for (int x = 0; x < mw; ++x) alpha[size_t(y * mw + x)] = msk.getPixelColour(x, y).getRed();
    for (int y = 0; y < h; ++y) {
        const int my = y % mw;
        for (int x = 0; x < w && x < mw; ++x) {
            const auto a = alpha[size_t(my * mw + x)];
            if (a == 0) continue;
            const auto c = src.getPixelColour(x, y);
            auto* px = reinterpret_cast<juce::PixelARGB*>(dst.getPixelPointer(x, y));
            px->setARGB(a, std::min(c.getRed(), a), std::min(c.getGreen(), a), std::min(c.getBlue(), a));
        }
    }
    return out;
}

float knobBox(int kind) { return 2 * kKnobRadius[kind] + (kind == kSel ? 20.0f : 12.0f); }

int indexOf(const char* id) {
    for (int i = 0; i < kNumControls; ++i)
        if (std::strcmp(kControls[i].id, id) == 0) return i;
    return -1;
}
bool is(const Control& c, const char* id) { return std::strcmp(c.id, id) == 0; }
// sec_1 .. sec_8: the section key's number, else 0.
int sectionKeyOf(const char* id) {
    if (std::strncmp(id, "sec_", 4) != 0 || id[4] < '1' || id[4] > '9' || id[5] != 0) return 0;
    const int k = id[4] - '0';
    return k <= kNumSections ? k : 0;
}
// What fits on the strip of tape over a key.
juce::String tapeLabel(const juce::String& name) {
    static const char* const shortNames[][2] = {{"pre-chorus", "pre"}, {"post-chorus", "post"}, {"breakdown", "break"}, {"instrumental", "inst"},
                                                {"interlude", "inter"}, {"middle 8", "mid 8"}, {"refrain", "refr"}};
    for (const auto& sn : shortNames) if (name == sn[0]) return sn[1];
    return name;
}
bool startsWith(const char* s, const char* p) { return std::strncmp(s, p, std::strlen(p)) == 0; }
bool endsWith(const char* s, const char* e) { const size_t a = std::strlen(s), b = std::strlen(e); return a >= b && std::strcmp(s + a - b, e) == 0; }

const help::Entry* helpFor(const juce::String& id) {
    for (int i = 0; i < help::kNumEntries; ++i)
        if (id == help::kEntries[i].id) return &help::kEntries[i];
    return nullptr;
}

// The tube's phosphor.
const juce::Colour kText(0xffa6ffc9), kCurve(0xffd4ffe4), kGlow(0x8c5dff9a), kFill(0x1f58e08f), kInk(0xff0b2216);

constexpr int kNumPages = 6;
const char* const kScreenKeys[kNumPages] = {"key_bal", "key_loud", "key_moves", "key_checks", "key_sect", "key_detail"};
const char* const kModules[6] = {"clean", "tone", "dyn", "col", "st", "loud"};
const char* const kModuleNames[6] = {"CLEAN", "TONE", "DYNAMICS", "COLOUR", "STEREO", "LOUDNESS"};

int moduleOf(int p) {
    using namespace lq;
    if (p >= cleanIn && p <= cHear) return 0;
    if (p >= toneIn && p <= tMode) return 1;
    if (p >= dynIn && p <= dChan) return 2;
    if (p >= colIn && p <= cType) return 3;
    if (p >= stIn && p <= sHear) return 4;
    if (p >= spIn && p <= spB) return 6;
    return 5;
}

juce::String up(const juce::String& s) { return s.toUpperCase(); }
juce::String num1(double v, bool sign = false) {
    juce::String s = juce::String(std::abs(v), 1);
    if (v < -0.04) return kMinus + s;
    return (sign && v > 0.04 ? "+" : "") + s;
}

}  // namespace

LacquerArt::LacquerArt() {
    panel = named("panel.jpg");
    tray = named("tray.jpg");
    strip[kLarge] = roundStrip("knob_large.jpg", "knob_large_mask.png");
    strip[kMed] = roundStrip("knob_med.jpg", "knob_med_mask.png");
    strip[kSel] = named("knob_sel.png");
    for (int k = 0; k < 4; ++k) stripFrames[k] = strip[k].isValid() ? strip[k].getHeight() / strip[k].getWidth() : 0;
    toggleUp = named("toggle_up.png");
    toggleDown = named("toggle_down.png");
    glass = named("glass_meter.png");
    for (int g = 0; g < kNumLitGroups; ++g) lit[g] = named(juce::String("lit_") + kLitGroupNames[g] + ".png");
    int n = 0;
    if (const char* d = BinaryData::getNamedResource("CaveatBrushRegular_ttf", n)) marker = juce::Typeface::createSystemTypefaceFor(d, size_t(n));
    if (const char* d = BinaryData::getNamedResource("BarlowSemiCondensedSemiBold_ttf", n)) legend = juce::Typeface::createSystemTypefaceFor(d, size_t(n));
}

// ---------------------------------------------------------------------------
LacquerEditor::LacquerEditor(LacquerProcessor& p) : AudioProcessorEditor(p), proc_(p) {
    painted_.resize(size_t(kNumControls));
    drawn_.assign(size_t(kNumControls), -1e9f);
    params_.assign(size_t(kNumControls), -1);
    for (int i = 0; i < kNumControls; ++i) params_[size_t(i)] = lq::findParam(kControls[i].id);
    meterL_ = indexOf("m_loud"); meterP_ = indexOf("m_punch"); overLamp_ = indexOf("over");
    setOpaque(true);
    setWantsKeyboardFocus(false);
    setScale(proc_.uiScale);
    startTimerHz(int(kFrameRate));
}

void LacquerEditor::setScale(float s) {
    s_ = juce::jlimit(0.75f, 1.5f, s);
    proc_.uiScale = s_;
    helpOn_ = proc_.showHelpStrip();
    setSize(int(std::round(W * s_)), int(std::round((H + trayH() + helpH()) * s_)));
    repaint();
}

void LacquerEditor::setTray(bool open) {
    proc_.trayOpen = open;
    setScale(s_);
}

juce::Font LacquerEditor::font(float h) const { return juce::Font(juce::FontOptions(art_->legend).withHeight(h * s_)); }

// ---------------------------------------------------------------------------
// Controls
float LacquerEditor::value(int ci) const {
    const int p = paramOf(ci);
    return p >= 0 ? proc_.shownValue(p) : 0.0f;
}

float LacquerEditor::norm(int ci) const {
    const int p = paramOf(ci);
    return p >= 0 ? lq::toNorm(p, value(ci)) : 0.0f;
}

int LacquerEditor::selIndex(int ci) const { return juce::jlimit(0, std::max(0, kControls[ci].nSteps - 1), juce::roundToInt(value(ci))); }

bool LacquerEditor::lit(int ci) const {
    const auto& c = kControls[ci];
    for (int k = 0; k < kNumPages; ++k) if (is(c, kScreenKeys[k])) return proc_.page == k;
    if (is(c, "sec_song")) return proc_.litKey() == 0;
    if (const int k = sectionKeyOf(c.id)) return proc_.litKey() == k;
    if (is(c, "sec_mark")) return proc_.marking();
    if (is(c, "sec_follow")) return proc_.follow();
    if (is(c, "a_listen")) return proc_.listening();
    if (endsWith(c.id, "_idea"))
        for (int m = 0; m < 6; ++m) if (startsWith(c.id, kModules[m]) && c.id[std::strlen(kModules[m])] == '_') return proc_.ideaCount(m) > 0;
    if (is(c, "guarding")) return proc_.meters().guarding.load();
    if (is(c, "over")) return clock_ < overUntil_;
    if (is(c, "o_blind")) return proc_.blind();
    if (is(c, "power")) return true;
    if (is(c, "space_on")) return proc_.meters().spaceActive.load();
    if (is(c, "ab_a")) return !proc_.blind() && proc_.abSlot() == 0;
    if (is(c, "ab_b")) return !proc_.blind() && proc_.abSlot() == 1;
    if (paramOf(ci) >= 0 && (c.type == Button || c.type == Lamp)) return value(ci) >= 0.5f;
    return false;
}

juce::Rectangle<float> LacquerEditor::boundsOf(int ci) const {
    const auto& c = kControls[ci];
    const float y = cy(c);
    switch (c.type) {
        case Knob: { const float b = knobBox(c.kind); return {c.x - b / 2, y - b / 2, b, b}; }
        case Toggle: return {c.x - 22, y - 22, 44, 44};
        case Button:
        case Lamp: return c.litGroup >= 0 ? juce::Rectangle<float>(c.lx, c.ly + (c.tray ? H : 0), c.lw, c.lh) : juce::Rectangle<float>(c.x - c.w / 2 - 4, y - c.h / 2 - 4, c.w + 8, c.h + 8);
        case Meter: return {c.x - c.w / 2 - 2, y - c.h / 2 - 2, c.w + 4, c.h + 4};
        case layout::Screen: return {kFaceX - 2, kFaceY - 2, kFaceW + 4, kFaceH + 4};
    }
    return {};
}

void LacquerEditor::repaintPts(juce::Rectangle<float> r) {
    if (!r.isEmpty()) repaint((r * s_).getSmallestIntegerContainer().expanded(2));
}

int LacquerEditor::hit(juce::Point<float> pt) const {
    for (int i = 0; i < kNumControls; ++i) {
        const auto& c = kControls[i];
        if (c.tray && !proc_.trayOpen) continue;
        const float y = cy(c);
        switch (c.type) {
            case Knob: if (pt.getDistanceFrom({c.x, y}) < kKnobRadius[c.kind] + 4) return i; break;
            case Toggle: if (std::abs(pt.x - c.x) < 13 && std::abs(pt.y - y) < 17) return i; break;
            case Button: if (juce::Rectangle<float>(c.x - c.w / 2 - 2, y - c.h / 2 - 3, c.w + 4, c.h + 6).contains(pt)) return i; break;
            case Meter: if (juce::Rectangle<float>(c.x - c.w / 2, y - c.h / 2, c.w, c.h).contains(pt)) return i; break;
            case Lamp: if (pt.getDistanceFrom({c.x, y}) < c.w / 2 + 5) return i; break;
            case layout::Screen: break;
        }
    }
    if (juce::Rectangle<float>(kFaceX, kFaceY, kFaceW, kFaceH).contains(pt)) return kScreen;
    if (pt.getDistanceFrom({kLatchX, kLatchY}) < kLatchR + 5) return kLatch;
    if (juce::Rectangle<float>(kPresetTapeX - kPresetTapeW / 2, kPresetTapeY - kPresetTapeH / 2, kPresetTapeW, kPresetTapeH).contains(pt)) return kPresetTape;
    if (juce::Rectangle<float>(kSectTapeX - kSectTapeW / 2, kSectTapeY - kSectTapeH / 2 - 2, kSectTapeW, kSectTapeH + 4).contains(pt)) return kSectTape;
    if (juce::Rectangle<float>(kReadoutLufs[0] - kReadoutLufs[2] / 2, kReadoutLufs[1] - kReadoutLufs[3] / 2, kReadoutLufs[2], kReadoutLufs[3] + 22).contains(pt)) return kReadLufs;
    if (juce::Rectangle<float>(kReadoutTp[0] - kReadoutTp[2] / 2, kReadoutTp[1] - kReadoutTp[3] / 2, kReadoutTp[2], kReadoutTp[3] + 22).contains(pt)) return kReadTp;
    return kNone;
}

void LacquerEditor::beginEdit(int p) {
    if (p < 0) return;
    if (proc_.litKey() == 0 || !lq::def(p).perSection) proc_.param(p)->beginChangeGesture();
}

void LacquerEditor::endEdit(int p) {
    if (p < 0) return;
    if (proc_.litKey() == 0 || !lq::def(p).perSection) proc_.param(p)->endChangeGesture();
}

void LacquerEditor::click(int ci, const juce::MouseEvent& e) {
    const auto& c = kControls[ci];
    for (int k = 0; k < kNumPages; ++k) if (is(c, kScreenKeys[k])) { proc_.page = k; moveScroll_ = 0; repaint(); return; }
    if (is(c, "sec_song")) { proc_.tapKey(0); repaint(); return; }
    if (const int k = sectionKeyOf(c.id)) {
        if (e.mods.isPopupMenu()) { showNameMenu(k); return; }   // right-click: what this key is called
        proc_.tapKey(k);
        repaint();
        return;
    }
    if (is(c, "sec_mark")) { proc_.setMarking(!proc_.marking()); proc_.page = 4; repaint(); return; }
    if (is(c, "sec_follow")) { proc_.setFollow(!proc_.follow()); repaint(); return; }
    if (is(c, "a_listen")) { proc_.setListening(!proc_.listening()); repaint(); return; }
    if (is(c, "a_take")) { proc_.takeAll(); proc_.page = 2; repaint(); return; }
    if (is(c, "o_blind")) { proc_.setBlind(!proc_.blind()); repaint(); return; }
    if (is(c, "ab_a") || is(c, "ab_b")) { proc_.selectAb(is(c, "ab_a") ? 0 : 1); repaint(); return; }
    if (is(c, "menu")) { showSetMenu(); return; }
    if (is(c, "preset_prev") || is(c, "preset_next")) { stepPreset(is(c, "preset_next") ? 1 : -1); return; }
    const int p = paramOf(ci);
    if (p < 0) return;
    followDetail(p);
    if (c.type == Toggle || c.type == Button) {
        beginEdit(p);
        proc_.setShown(p, value(ci) >= 0.5f ? 0.0f : 1.0f);
        endEdit(p);
    } else if (c.type == Knob && c.kind == kSel) {
        // A click steps a selector round; right-click or Alt steps it back.
        const int n = c.nSteps, back = e.mods.isPopupMenu() || e.mods.isAltDown();
        const int idx = (selIndex(ci) + (back ? n - 1 : 1)) % n;
        beginEdit(p);
        proc_.setShown(p, float(idx));
        endEdit(p);
    }
    repaintPts(boundsOf(ci));
}

void LacquerEditor::showSetMenu() {
    juce::PopupMenu m, size, load;
    for (float s : {0.75f, 1.0f, 1.25f, 1.5f})
        size.addItem(juce::String(juce::roundToInt(s * 100)) + " %", true, std::abs(s_ - s) < 0.01f, [this, s] { setScale(s); });
    m.addSubMenu("Window size", size);
    m.addItem("Show the help strip", true, helpOn_, [this] { proc_.setShowHelpStrip(!helpOn_); setScale(s_); });
    m.addSeparator();
    m.addItem("Save preset...", [this] {
        auto* w = new juce::AlertWindow("Save preset", "A name for these settings:", juce::MessageBoxIconType::NoIcon);
        w->addTextEditor("name", proc_.presetName);
        w->addButton("Save", 1, juce::KeyPress(juce::KeyPress::returnKey));
        w->addButton("Cancel", 0, juce::KeyPress(juce::KeyPress::escapeKey));
        w->enterModalState(true, juce::ModalCallbackFunction::create([this, w](int r) {
            if (r == 1) proc_.savePreset(w->getTextEditorContents("name"));
            repaint();
        }), true);
    });
    m.addSeparator();
    m.addItem("Remember this master's balance for MINE", proc_.result().any, false, [this] { proc_.rememberAsMine(); repaint(); });
    m.addItem("Forget my masters (" + juce::String(proc_.numMine()) + ")", proc_.numMine() > 0, false, [this] { proc_.clearMine(); repaint(); });
    m.addItem("Clear the references (" + juce::String(proc_.numRefs()) + ")", proc_.numRefs() > 0, false, [this] { proc_.clearReferences(); repaint(); });
    m.addItem("Name the sections...", [this] { renameSections(); });
    m.addItem("Reset the integrated loudness", [this] { proc_.resetIntegrated(); });
    m.addItem("Forget the song's loudness (a new mix)", [this] { proc_.forgetLoudness(); repaint(); });
    m.addSeparator();
    m.addItem("Lacquer " JucePlugin_VersionString, false, false, [] {});
    m.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(this).withMousePosition());
}

void LacquerEditor::showPresetMenu() {
    juce::PopupMenu m;
    m.addSectionHeader("Factory");
    const auto f = LacquerProcessor::factoryPresets();
    for (int i = 0; i < f.size(); ++i) m.addItem(f[i], true, proc_.presetName == f[i], [this, i] { proc_.loadFactory(i); repaint(); });
    const auto files = proc_.presetFolder().findChildFiles(juce::File::findFiles, false, "*.lacquer");
    if (!files.isEmpty()) {
        m.addSectionHeader("Yours");
        for (const auto& file : files) m.addItem(file.getFileNameWithoutExtension(), [this, file] { proc_.loadPreset(file); repaint(); });
    }
    m.addSeparator();
    m.addItem("Save preset...", [this] { showSetMenu(); });
    m.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(this).withMousePosition());
}

void LacquerEditor::stepPreset(int dir) {
    const auto f = LacquerProcessor::factoryPresets();
    const int cur = f.indexOf(proc_.presetName);
    proc_.loadFactory(((cur < 0 ? 0 : cur + dir) % f.size() + f.size()) % f.size());
    repaint();
}

void LacquerEditor::renameSections() {
    auto* w = new juce::AlertWindow("Name the sections", "What each section key is called (on the tape and the SECTIONS screen):", juce::MessageBoxIconType::NoIcon);
    for (int k = 1; k <= LacquerProcessor::kSections; ++k) w->addTextEditor("s" + juce::String(k), proc_.sectionName(k), juce::String(k));
    w->addButton("OK", 1, juce::KeyPress(juce::KeyPress::returnKey));
    w->addButton("Cancel", 0, juce::KeyPress(juce::KeyPress::escapeKey));
    w->enterModalState(true, juce::ModalCallbackFunction::create([this, w](int r) {
        if (r == 1)
            for (int k = 1; k <= LacquerProcessor::kSections; ++k) proc_.setSectionName(k, w->getTextEditorContents("s" + juce::String(k)));
        repaint();
    }), true);
}

// ---------------------------------------------------------------------------
// The screen. Its pages are drawn in the mockup's coordinates: 380 x 216 from just outside the face.
juce::Point<float> LacquerEditor::sc(float x, float y) const { return {kFaceX - 2 + x, kFaceY - 2 + y}; }

namespace {
struct Ink {
    juce::Graphics& g;
    juce::Typeface::Ptr tf;
    float ox, oy;
    juce::Font f(float h, float kern = 0.05f) const { return juce::Font(juce::FontOptions(tf).withHeight(h * 1.17f)).withExtraKerningFactor(kern); }
    float width(const juce::String& s, float h, float kern = 0.05f) const { return juce::GlyphArrangement::getStringWidth(f(h, kern), s); }
    // align: 0 left, 1 centre, 2 right; x and the baseline in screen coordinates
    void text(const juce::String& s, float x, float base, float h, int align = 0, float alpha = 1.0f, juce::Colour c = kText, float kern = 0.05f) const {
        const auto fo = f(h, kern);
        const float w = juce::GlyphArrangement::getStringWidth(fo, s);
        const float x0 = align == 0 ? x : (align == 1 ? x - w / 2 : x - w);
        juce::GlyphArrangement ga;
        ga.addLineOfText(fo, s, ox + x0, oy + base);
        g.setColour(c.withMultipliedAlpha(alpha));
        ga.draw(g);
    }
    juce::Point<float> p(float x, float y) const { return {ox + x, oy + y}; }
    void glowLine(const juce::Path& path) const {
        g.setColour(kGlow); g.strokePath(path, juce::PathStrokeType(4.5f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
        g.setColour(kGlow.withMultipliedAlpha(0.45f)); g.strokePath(path, juce::PathStrokeType(8.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
        g.setColour(kCurve); g.strokePath(path, juce::PathStrokeType(1.7f, juce::PathStrokeType::curved));
    }
    void dashed(const juce::Path& path, float a, float b, float width, juce::Colour c) const {
        juce::Path d;
        const float dash[2] = {a, b};
        juce::PathStrokeType(width).createDashedStroke(d, path, dash, 2);
        g.setColour(c); g.fillPath(d);
    }
};
}  // namespace

void LacquerEditor::header(juce::Graphics& g, const juce::String& l1, const juce::String& l2) {
    const Ink ink{g, art_->legend, kFaceX - 2, kFaceY - 2};
    ink.text(l1, 14, 22, 13.5f, 0, 1.0f, kText, 0.06f);
    juce::String second = l2;
    if (proc_.listening()) second = "LISTENING: " + juce::String(juce::roundToInt(proc_.listenedSeconds())) + " S. PLAY THE SONG THROUGH, THEN STOP";
    else if (proc_.analysingRefs()) second = "READING THE REFERENCES...";
    ink.text(second, 14, 38, 11.5f, 0, 0.74f);
}

juce::String LacquerEditor::keyLabel(int key) const { return key <= 0 ? "WHOLE SONG" : up(proc_.sectionName(key)); }

void LacquerEditor::drawScreen(juce::Graphics& g) {
    juce::Graphics::ScopedSaveState ss(g);
    juce::Path clip;
    clip.addRoundedRectangle(juce::Rectangle<float>(kFaceX - 1, kFaceY - 1, kFaceW + 2, kFaceH + 2) * s_, 10.0f * s_);
    g.reduceClipRegion(clip);
    g.addTransform(juce::AffineTransform::scale(s_));
    switch (proc_.page) {
        case 1: drawLoudness(g); break;
        case 2: drawMoves(g); break;
        case 3: drawChecks(g); break;
        case 4: drawSections(g); break;
        case 5: drawDetail(g); break;
        default: drawBalance(g); break;
    }
    if (clock_ < noteUntil_ && note_.isNotEmpty()) {
        const Ink ink{g, art_->legend, kFaceX - 2, kFaceY - 2};
        g.setColour(juce::Colour(0xe0071610));
        g.fillRect(juce::Rectangle<float>(ink.p(6, 192), ink.p(374, 212)));
        ink.text(note_, 14, 206, 11.0f, 0, 1.0f);
    }
    if (dropHover_) {
        g.setColour(kText.withAlpha(0.12f));
        g.fillRoundedRectangle(juce::Rectangle<float>(kFaceX, kFaceY, kFaceW, kFaceH), 9.0f);
        const Ink ink{g, art_->legend, kFaceX - 2, kFaceY - 2};
        ink.text("DROP REFERENCE TRACKS HERE", 190, 112, 15.0f, 1);
    }
}

// ---- BALANCE: the measured curve against the target corridor
void LacquerEditor::drawBalance(juce::Graphics& g) {
    const Ink ink{g, art_->legend, kFaceX - 2, kFaceY - 2};
    const auto& r = proc_.result();
    const auto& t = proc_.target();
    int key = proc_.litKey();
    const bool heard = r.any && r.keyHeard[key];
    if (r.any && !heard) key = 0;
    auto X = [](double f) { return float(8 + 364 * std::log(f / 20.0) / std::log(1000.0)); };
    auto Y = [](double db) { return float(118 - db * 6); };
    const int N = lq::kBands;
    auto pathOf = [&](auto fn) {
        juce::Path p;
        for (int b = 0; b < N; ++b) {
            const double f = lq::bandCentre(b);
            if (f > 20000) break;
            const auto pt = ink.p(X(f), juce::jlimit(44.0f, 196.0f, Y(fn(b))));
            if (b == 0) p.startNewSubPath(pt); else p.lineTo(pt);
        }
        return p;
    };
    // The corridor.
    {
        juce::Path c = pathOf([&](int b) { return t.curve[b] + t.width[b]; });
        juce::Path lo = pathOf([&](int b) { return t.curve[b] - t.width[b]; });
        juce::Path area = c;
        for (int b = N - 1; b >= 0; --b) {
            const double f = lq::bandCentre(b);
            if (f > 20000) continue;
            area.lineTo(ink.p(X(f), juce::jlimit(44.0f, 196.0f, Y(t.curve[b] - t.width[b]))));
        }
        area.closeSubPath();
        g.setColour(kFill); g.fillPath(area);
        ink.dashed(c, 3, 3, 0.9f, kText.withAlpha(0.45f));
        ink.dashed(lo, 3, 3, 0.9f, kText.withAlpha(0.45f));
    }
    int inside = 0, outside = 0, total = 0;
    if (r.any) {
        const lq::Values v = proc_.valuesFor(key);
        auto now = [&](int b) { return r.curve[key][b] + lq::toneResponse(lq::bandCentre(b), v); };
        // Out of the corridor: shaded, and named.
        int runStart = -1;
        struct Flag { float x; float y; juce::String t; };
        std::vector<Flag> flags;
        for (int b = 0; b <= N; ++b) {
            const double f = b < N ? lq::bandCentre(b) : 1e9;
            const bool out = b < N && f >= 25 && f <= 18000 && std::abs(now(b) - t.curve[b]) > t.width[b];
            if (b < N && f >= 25 && f <= 18000) { ++total; inside += out ? 0 : 1; }
            if (out && runStart < 0) runStart = b;
            if (!out && runStart >= 0) {
                int k = runStart;
                for (int j = runStart; j < b; ++j) if (std::abs(now(j) - t.curve[j]) > std::abs(now(k) - t.curve[k])) k = j;
                const double fk = lq::bandCentre(k), d = now(k) - t.curve[k];
                const bool over = d > 0;
                const char* word = fk < 50 ? (over ? "RUMBLE" : "NO SUB") : fk < 250 ? (over ? "BOOMY" : "THIN") : fk < 1200 ? (over ? "MUDDY" : "HOLLOW") : fk < 6500 ? (over ? "HARSH" : "DISTANT") : (over ? "SHARP" : "DULL");
                const float xa = X(lq::bandEdge(runStart)), xb = X(lq::bandEdge(b));
                g.setColour(juce::Colour(0x1cc9ffe0));
                g.fillRect(juce::Rectangle<float>(ink.p(xa, 48), ink.p(xb, 190)));
                const float yv = juce::jlimit(44.0f, 196.0f, Y(now(k)));
                const juce::String label = juce::String(word) + " " + num1(d, true) + " dB";
                const float hw = ink.width(label, 11.0f) / 2 + 4;
                flags.push_back({juce::jlimit(8 + hw, 372 - hw, X(fk)), juce::jlimit(58.0f, 188.0f, over ? (yv - 12 < 60 ? yv + 22 : yv - 12) : yv + 20), label});
                ++outside;
                runStart = -1;
            }
        }
        // How it came in (dotted), and as it leaves with the tone moves (bright).
        ink.dashed(pathOf([&](int b) { return r.curve[key][b]; }), 1.2f, 2.6f, 1.1f, kText.withAlpha(0.5f));
        ink.glowLine(pathOf(now));
        for (const auto& fl : flags) ink.text(fl.t, fl.x, fl.y, 11.0f, 1);
    }
    for (const auto& fr : {std::pair<double, const char*>{50, "50"}, {100, "100"}, {200, "200"}, {500, "500"}, {1000, "1k"}, {2000, "2k"}, {5000, "5k"}, {10000, "10k"}})
        ink.text(fr.second, X(fr.first), 205, 10.0f, 1, 0.62f);
    ink.text("Hz", 372, 205, 10.0f, 2, 0.62f);
    const int tsel = juce::roundToInt(proc_.songValue(lq::aTarget));
    const juce::String l1 = "BALANCE   " + keyLabel(key);
    // The target, top right: a style to step through, references to add and keep, or your masters.
    {
        auto chip = [&](float x0, float x1, const juce::String& text, bool enabled, int arrow = 0) {
            const float a = enabled ? 1.0f : 0.3f;
            const auto r = juce::Rectangle<float>(ink.p(x0, 11), ink.p(x1, 26));
            g.setColour(kText.withAlpha(0.07f * a)); g.fillRoundedRectangle(r, 2.0f);
            g.setColour(kText.withAlpha(0.6f * a)); g.drawRoundedRectangle(r, 2.0f, 0.8f);
            if (arrow) {
                const float c0 = (x0 + x1) / 2, d = float(arrow);
                juce::Path tri; tri.addTriangle(ink.p(c0 + 3.0f * d, 18.5f), ink.p(c0 - 2.0f * d, 15), ink.p(c0 - 2.0f * d, 22));
                g.setColour(kText.withAlpha(a)); g.fillPath(tri);
                return;
            }
            juce::String tx = text;
            while (tx.length() > 3 && ink.width(tx, 10.5f) > x1 - x0 - 6) tx = tx.dropLastCharacters(1);
            ink.text(tx, (x0 + x1) / 2, 22.5f, 10.5f, 1, a);
        };
        if (tsel == 0) {
            chip(220, 236, {}, true, -1);
            chip(236, 352, up(proc_.styleName(proc_.genre())) + (proc_.styleIsYours(proc_.genre()) ? " *" : ""), true);
            chip(352, 368, {}, true, 1);
        } else if (tsel == 1) {
            chip(220, 290, "+ ADD", true);
            chip(296, 368, "SAVE AS STYLE", proc_.numRefs() > 0);
        } else {
            ink.text("YOUR MASTERS: " + juce::String(proc_.numMine()), 368, 22.5f, 10.5f, 2, 0.8f);
        }
    }
    juce::String l2;
    if (!r.any) l2 = "PRESS LISTEN AND PLAY THE SONG THROUGH ONCE";
    else if (proc_.litKey() != key) l2 = "THIS SECTION WASN'T HEARD: SHOWING THE SONG";
    else l2 = juce::String(total ? juce::roundToInt(100.0 * inside / total) : 100) + "% INSIDE THE CORRIDOR   " + kDot.trim() + "   " +
              (outside ? juce::String(outside) + (outside > 1 ? " AREAS" : " AREA") + " OUTSIDE" : juce::String("NOTHING OUTSIDE"));
    if (tsel == 1 && proc_.numRefs() == 0 && r.any) l2 = "ADD REFERENCE TRACKS, OR DROP THEM ON THE SCREEN";
    header(g, l1, l2);
}

// ---- LOUDNESS: how much snap survives as the song gets louder
void LacquerEditor::drawLoudness(juce::Graphics& g) {
    const Ink ink{g, art_->legend, kFaceX - 2, kFaceY - 2};
    const auto& r = proc_.result();
    auto XL = [](double v) { return float(8 + 364 * (v + 16) / 10); };
    auto YK = [](double k) { return float(52 + 140 * (1 - (k - 40) / 60)); };
    // The lit key's target: the song's, or a section's own.
    const int lk = proc_.litKey();
    const bool own = proc_.shownIsOwn(lq::lTarget);
    double target = proc_.shownValue(lq::lTarget);
    const bool guard = proc_.songValue(lq::lGuard) >= 0.5f && r.knee.known && target > r.knee.kneeLufs;
    if (guard) target = r.knee.kneeLufs;
    const auto& map = proc_.songMap();
    g.setColour(kText.withAlpha(0.08f));
    if (r.knee.known) g.fillRect(juce::Rectangle<float>(ink.p(XL(r.knee.kneeLufs), 46), ink.p(372, 196)));
    auto vline = [&](float x) { juce::Path p; p.startNewSubPath(ink.p(x, 46)); p.lineTo(ink.p(x, 196)); ink.dashed(p, 3, 3, 1.0f, kText.withAlpha(0.6f)); };
    vline(XL(-14));
    ink.text("STREAM " + kMinus + "14 LUFS", XL(-14) + 5, 178, 10.5f, 0, 1.0f);
    ink.text("turns it down " + juce::String(std::max(0.0, target + 14), 1) + " dB", XL(-14) + 5, 190, 10.5f, 0, 0.74f);
    juce::String l1, l2;
    if (r.knee.known) {
        vline(XL(r.knee.kneeLufs));
        juce::Path c;
        for (int i = 0; i <= 100; ++i) {
            const auto pt = ink.p(XL(-16 + i * 0.1), YK(r.knee.kept[i]));
            if (i == 0) c.startNewSubPath(pt); else c.lineTo(pt);
        }
        ink.glowLine(c);
        const int ti = juce::jlimit(0, 100, int(std::round((target + 16) * 10)));
        const auto np = ink.p(XL(target), YK(r.knee.kept[ti]));
        // Sections with a target of their own: a numbered ring where each one sits on the curve
        // (sections at the same target share a ring).
        {
            std::vector<std::pair<double, juce::String>> rings;
            for (int k = 1; k < lq::kNumKeys; ++k) {
                if (!map.has[k][lq::lTarget] || k == lk) continue;
                const double tk = map.val[k][lq::lTarget];
                bool merged = false;
                for (auto& rg : rings) if (std::abs(rg.first - tk) < 0.25) { rg.second << " " << k; merged = true; }
                if (!merged) rings.push_back({tk, juce::String(k)});
            }
            for (const auto& rg : rings) {
                const int i = juce::jlimit(0, 100, int(std::round((rg.first + 16) * 10)));
                const auto sp = ink.p(XL(rg.first), YK(r.knee.kept[i]));
                g.setColour(kText.withAlpha(0.8f)); g.drawEllipse(sp.x - 5, sp.y - 5, 10, 10, 1.0f);
                ink.text(rg.second, XL(rg.first), YK(r.knee.kept[i]) + 16, 10.0f, 1, 0.8f);
            }
        }
        g.setColour(kText.withAlpha(0.6f)); g.drawEllipse(np.x - 9, np.y - 9, 18, 18, 1.0f);
        g.setColour(kCurve); g.fillEllipse(np.x - 4.5f, np.y - 4.5f, 9, 9);
        const float ny = YK(r.knee.kept[ti]);
        ink.text("NOW", juce::jlimit(24.0f, 356.0f, XL(target)), ny - 14 < 54 ? ny + 21 : ny - 14, 10.5f, 1);
        // The knee's note sits to its right, or to its left when the knee is near the right edge.
        const bool right = XL(r.knee.kneeLufs) < 290;
        const float kx = right ? XL(r.knee.kneeLufs) + 6 : XL(r.knee.kneeLufs) - 6;
        ink.text("DAMAGE STARTS", kx, 178, 10.5f, right ? 0 : 2);
        ink.text("at " + num1(r.knee.kneeLufs) + " LUFS", kx, 190, 10.5f, right ? 0 : 2, 0.74f);
        l1 = (lk > 0 && own ? up(proc_.sectionName(lk)) + " " : juce::String()) + "TARGET " + num1(target) + " LUFS   " + kDot.trim() + "   " + juce::String(juce::roundToInt(r.knee.kept[ti])) + "% OF TRANSIENTS KEPT";
        if (lk != 0 && own) {
            const double d = target - proc_.songValue(lq::lTarget);
            l2 = "THE " + up(proc_.sectionName(lk)) + "'S OWN TARGET: " + (std::abs(d) < 0.05 ? juce::String("THE SAME AS THE SONG")
                                                                                         : juce::String(std::abs(d), 1) + " dB " + (d < 0 ? "UNDER" : "OVER") + " THE SONG");
        } else if (lk != 0) l2 = "THE " + up(proc_.sectionName(lk)) + " FOLLOWS THE SONG'S TARGET: TURN IT TO GIVE IT ITS OWN";
        else if (guard) l2 = "GUARD STOPS THE LIMITER JUST BEFORE THE KNEE";
        else if (target > r.knee.kneeLufs) l2 = "PAST THE KNEE: THE DRUMS ARE STARTING TO SMEAR";
        else l2 = "SHORT OF THE KNEE: THE DRUMS KEEP THEIR SNAP";
    } else {
        l1 = "TARGET " + num1(target) + " LUFS";
        l2 = proc_.loudnessKnown() ? "PRESS LISTEN AND PLAY THE SONG ONCE TO FIND THE KNEE" : "LEARNING THE SONG'S LOUDNESS AS IT PLAYS";
    }
    // How far the loudest section stands over the others: the mix's own lift, and what's left of it.
    {
        const auto& c = r.contrast;
        int other = -1;
        for (int k = 1; k < lq::kNumKeys; ++k)
            if (c.ok[k] && (other < 0 || c.seconds[k] > c.seconds[other])) other = k;
        if (lk > 0 && lk != c.loud && c.ok[lk]) other = lk;
        double mix = 0, master = 0;
        if (other > 0 && proc_.sectionGap(other, mix, master)) {
            ink.text(up(LacquerProcessor::plural(proc_.sectionName(c.loud))) + " OVER " + up(LacquerProcessor::plural(proc_.sectionName(other))), 14, 116, 10.5f, 0, 0.9f);
            ink.text("in the mix " + num1(mix, true) + " dB, mastered about " + num1(master, true) + " dB", 14, 129, 10.5f, 0, 0.74f);
        }
    }
    for (int v : {-16, -14, -12, -10, -8, -6}) {
        const juce::String s = (v < 0 ? kMinus : "") + juce::String(std::abs(v)) + (v == -6 ? " LUFS" : "");
        ink.text(s, v == -16 ? 9.0f : (v == -6 ? 371.0f : XL(v)), 205, 10.0f, v == -16 ? 0 : (v == -6 ? 2 : 1), 0.62f);
    }
    header(g, l1, l2);
}

// ---- MOVES: the suggestions, each one taken or skipped with a click
int LacquerEditor::moveRowAt(juce::Point<float> pt) const {
    const auto o = sc(0, 0);
    const float x = pt.x - o.x, y = pt.y - o.y;
    if (x < 8 || x > 372 || y < 48) return -1;
    const int row = int((y - 48) / 25.0f);
    const int i = row + moveScroll_;
    if (row < 0 || row >= movesShown() || i >= int(proc_.result().moves.size())) return -1;
    return i;
}

void LacquerEditor::drawMoves(juce::Graphics& g) {
    const Ink ink{g, art_->legend, kFaceX - 2, kFaceY - 2};
    const auto& r = proc_.result();
    int taken = 0;
    for (const auto& m : r.moves) taken += m.taken ? 1 : 0;
    const int n = int(r.moves.size());
    for (int row = 0; row < movesShown() && row + moveScroll_ < n; ++row) {
        const auto& m = r.moves[size_t(row + moveScroll_)];
        const float top = 50.0f + row * 25.0f;
        const float a = m.taken ? 1.0f : 0.5f;
        const auto b0 = ink.p(12, top + 1);
        if (m.taken) {
            g.setColour(kText); g.fillRoundedRectangle(b0.x, b0.y, 11, 11, 1.5f);
            juce::Path tk; tk.startNewSubPath(ink.p(14.5f, top + 6.5f)); tk.lineTo(ink.p(17, top + 9)); tk.lineTo(ink.p(21, top + 3.5f));
            g.setColour(kInk); g.strokePath(tk, juce::PathStrokeType(1.8f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
        } else {
            g.setColour(kText.withAlpha(0.7f)); g.drawRoundedRectangle(b0.x + 0.5f, b0.y + 0.5f, 10, 10, 1.5f, 1.1f);
        }
        ink.text(juce::String(row + moveScroll_ + 1), 30, top + 10, 12.0f, 0, a);
        ink.text(juce::String(m.module), 42, top + 10, 12.0f, 0, a);
        ink.text(juce::String::fromUTF8(m.what.c_str()), 108, top + 10, 12.0f, 0, a);
        ink.text(juce::String::fromUTF8(m.why.c_str()), 42, top + 22, 11.0f, 0, 0.74f * a, kText, 0.02f);
        ink.text(m.key == 0 ? juce::String("SONG") : keyLabel(m.key), 310, top + 10, 10.5f, 2, 0.8f * a, kText, 0.06f);
        for (int j = 0; j < 5; ++j) {
            g.setColour(kText.withAlpha((m.conf >= 20 * (j + 1) - 10 ? 0.85f : 0.18f) * a));
            g.fillRect(juce::Rectangle<float>(ink.p(318 + j * 6.5f, top + 3.5f), ink.p(323 + j * 6.5f, top + 10.5f)));
        }
        ink.text(juce::String(m.conf) + "%", 368, top + 10, 11.0f, 2, 0.8f * a);
    }
    if (n > 6) ink.text(juce::String(moveScroll_ + 1) + kMinus + juce::String(std::min(n, moveScroll_ + 5)) + " OF " + juce::String(n) + ": SCROLL FOR MORE", 368, 205, 10.0f, 2, 0.62f);
    juce::String l1 = "MOVES   " + juce::String(n) + " SUGGESTED   " + kDot.trim() + "   " + juce::String(taken) + " TAKEN";
    juce::String l2 = "AMOUNT " + juce::String(juce::roundToInt(proc_.songValue(lq::aAmount))) + "%   " + kDot.trim() + "   CLICK A MOVE TO TAKE OR SKIP IT";
    if (!r.any) { l1 = "MOVES"; l2 = "PRESS LISTEN AND PLAY THE SONG THROUGH ONCE"; }
    else if (n == 0) l2 = "NOTHING TO SUGGEST: IT SITS WELL AGAINST THE TARGET";
    header(g, l1, l2);
}

// ---- CHECKS: the preflight
void LacquerEditor::drawChecks(juce::Graphics& g) {
    const Ink ink{g, art_->legend, kFaceX - 2, kFaceY - 2};
    const auto& r = proc_.result();
    struct Row { juce::String name, what; bool ok; };
    std::vector<Row> rows;
    const float tp = proc_.meters().peakHold.load(), ceil = proc_.songValue(lq::lCeiling);
    if (tp > -100) rows.push_back({"TRUE PEAK", num1(tp) + " dBTP against a ceiling of " + num1(ceil), tp <= ceil + 0.05f});
    else rows.push_back({"TRUE PEAK", "play the song to measure it", true});
    for (const auto& c : r.checks) rows.push_back({juce::String::fromUTF8(c.name.c_str()), juce::String::fromUTF8(c.what.c_str()), c.ok});
    int looks = 0;
    for (size_t i = 0; i < rows.size() && i < 6; ++i) {
        const float y = 60.0f + float(i) * 17.0f;
        ink.text(rows[i].name, 14, y, 11.5f, 0, 1.0f, kText, 0.05f);
        ink.text(rows[i].what, 104, y, 11.0f, 0, 0.78f, kText, 0.02f);
        if (rows[i].ok) ink.text("OK", 366, y, 11.5f, 2, 1.0f, kText, 0.06f);
        else {
            ++looks;
            g.setColour(kText); g.fillRoundedRectangle(juce::Rectangle<float>(ink.p(330, y - 10.5f), ink.p(368, y + 3.5f)), 2.0f);
            ink.text("LOOK", 349, y, 11.0f, 1, 1.0f, kInk, 0.06f);
        }
    }
    const juce::String note = r.mixNotes.empty() ? juce::String() : juce::String::fromUTF8(r.mixNotes.front().c_str());
    if (note.isNotEmpty()) {
        const auto b = juce::Rectangle<float>(ink.p(10, 163), ink.p(370, 203));
        g.setColour(kText.withAlpha(0.06f)); g.fillRoundedRectangle(b, 3.0f);
        juce::Path p; p.addRoundedRectangle(b, 3.0f);
        ink.dashed(p, 3, 2, 1.0f, kText.withAlpha(0.55f));
        ink.text("MIX NOTE", 18, 179, 11.5f, 0, 1.0f);
        // Two lines at most.
        const auto words = juce::StringArray::fromTokens(note, " ", "");
        juce::String lines[2];
        int li = 0;
        for (const auto& w : words) {
            const juce::String trial = lines[li].isEmpty() ? w : lines[li] + " " + w;
            if (ink.width(trial, 11.0f, 0.02f) > 280 && li == 0) { li = 1; lines[1] = w; }
            else lines[li] = trial;
        }
        ink.text(lines[0], 78, 179, 11.0f, 0, 0.78f, kText, 0.02f);
        ink.text(lines[1], 78, 195, 11.0f, 0, 0.78f, kText, 0.02f);
    }
    juce::String l2 = juce::String(int(rows.size()) - looks) + " OK   " + kDot.trim() + "   " + (looks ? juce::String(looks) + " TO LOOK AT" : juce::String("NOTHING TO LOOK AT"));
    if (note.isNotEmpty()) l2 << "   " << kDot.trim() << "   " << juce::String(int(r.mixNotes.size())) << (r.mixNotes.size() > 1 ? " MIX NOTES" : " MIX NOTE");
    if (!r.any) l2 = "PRESS LISTEN AND PLAY THE SONG THROUGH ONCE";
    header(g, "CHECKS   BEFORE YOU BOUNCE", l2);
}

// ---- SECTIONS: the song's map, and what the lit section changes
float LacquerEditor::xOfPpq(double ppq) const { return kFaceX - 2 + 8 + float(364.0 * ppq / std::max(1.0, proc_.songEndPpq())); }
double LacquerEditor::ppqAtX(float x) const { return double(x - (kFaceX - 2 + 8)) / 364.0 * proc_.songEndPpq(); }

int LacquerEditor::blockAt(juce::Point<float> pt, bool& nearEdge) const {
    nearEdge = false;
    const auto o = sc(0, 0);
    if (pt.y < o.y + 68 || pt.y > o.y + 92) return -1;
    const auto& m = proc_.songMap();
    for (int i = 0; i < m.count; ++i) {
        const float x0 = xOfPpq(m.marks[i].ppq), x1 = i + 1 < m.count ? xOfPpq(m.marks[i + 1].ppq) : o.x + 372;
        if (pt.x >= x0 - 3 && pt.x < x1) { nearEdge = pt.x < x0 + 4; return i; }
    }
    return -1;
}

// The soft keys under the timeline, in screen coordinates: what the picked block is, and what to do with it.
namespace {
struct SoftKey { int id; float x0, x1; };
enum { skPrev, skType, skNext, skSplit, skJoin, skAdd, kNumSoftKeys };
constexpr SoftKey kSoftKeys[kNumSoftKeys] = {{skPrev, 12, 26}, {skType, 28, 138}, {skNext, 140, 154},
                                             {skSplit, 164, 206}, {skJoin, 212, 250}, {skAdd, 260, 368}};
constexpr float kSoftY0 = 103, kSoftY1 = 117;
}  // namespace

int LacquerEditor::softKeyAt(juce::Point<float> pt) const {
    const auto o = sc(0, 0);
    const float x = pt.x - o.x, y = pt.y - o.y;
    if (y < kSoftY0 - 2 || y > kSoftY1 + 2) return -1;
    for (const auto& k : kSoftKeys) if (x >= k.x0 - 1 && x <= k.x1 + 1) return k.id;
    return -1;
}

void LacquerEditor::drawSections(juce::Graphics& g) {
    const Ink ink{g, art_->legend, kFaceX - 2, kFaceY - 2};
    const auto& m = proc_.songMap();
    const int lk = proc_.litKey();
    const double q = proc_.barLength();
    if (selMark_ >= m.count) selMark_ = m.count - 1;
    drawLane(g);
    constexpr float b0 = 71, b1 = 89;   // the blocks
    for (int i = 0; i < m.count; ++i) {
        const float x0 = xOfPpq(m.marks[i].ppq) - (kFaceX - 2), x1 = (i + 1 < m.count ? xOfPpq(m.marks[i + 1].ppq) - (kFaceX - 2) : 372.0f);
        const bool on = lk > 0 && m.marks[i].key == lk;
        const auto r = juce::Rectangle<float>(ink.p(x0 + 1, b0), ink.p(std::max(x0 + 3, x1 - 1), b1));
        g.setColour(kText.withAlpha(on ? 0.88f : 0.10f)); g.fillRoundedRectangle(r, 2.0f);
        g.setColour(kText.withAlpha(on ? 1.0f : 0.55f)); g.drawRoundedRectangle(r, 2.0f, 0.8f);
        if (i == selMark_) {   // the picked block: a bright frame, and a handle on the edge that moves it
            g.setColour(kCurve); g.drawRoundedRectangle(r.expanded(1.5f), 3.0f, 1.6f);
            g.fillRect(juce::Rectangle<float>(r.getX() - 1.5f, r.getY() + 5, 3.0f, r.getHeight() - 10));
        }
        if (x1 - x0 > 9) ink.text(m.marks[i].key == 0 ? juce::String("S") : juce::String(m.marks[i].key), (x0 + x1) / 2, b1 - 5.0f, 12.0f, 1, 1.0f, on ? kInk : kText);
    }
    if (m.count > 0 && m.marks[0].ppq > 0.01) {   // before the first mark: the song settings
        const float x1 = xOfPpq(m.marks[0].ppq) - (kFaceX - 2);
        g.setColour(kText.withAlpha(0.05f)); g.fillRect(juce::Rectangle<float>(ink.p(8, b0), ink.p(x1 - 1, b1)));
    }
    {   // the playhead, playing or not
        const float x = xOfPpq(proc_.playheadPpq());
        g.setColour(kCurve.withAlpha(proc_.playing() ? 1.0f : 0.55f)); g.fillRect(x - 0.6f, ink.p(0, 44).y, 1.2f, b1 - 44 + 2);
    }
    // Bar numbers under the blocks.
    const double end = proc_.songEndPpq(), bars = end / q;
    const int step = bars > 120 ? 32 : (bars > 60 ? 16 : 8);
    for (int b = 0; b * q < end; b += step) ink.text(juce::String(b + 1), xOfPpq(b * q) - (kFaceX - 2), 99, 9.5f, b == 0 ? 0 : 1, 0.5f);

    // The soft keys.
    const bool picked = selMark_ >= 0 && selMark_ < m.count;
    const int pk = picked ? m.marks[selMark_].key : -1;
    bool canSplit = false;
    if (picked) canSplit = proc_.blockEnd(selMark_) - m.marks[selMark_].ppq > q * 1.5;
    for (const auto& k : kSoftKeys) {
        const bool enabled = k.id == skAdd || (picked && (k.id != skSplit || canSplit));
        const float a = enabled ? 1.0f : 0.3f;
        const auto r = juce::Rectangle<float>(ink.p(k.x0, kSoftY0), ink.p(k.x1, kSoftY1));
        g.setColour(kText.withAlpha((softDown_ == k.id ? 0.35f : 0.07f) * a)); g.fillRoundedRectangle(r, 2.0f);
        g.setColour(kText.withAlpha(0.6f * a)); g.drawRoundedRectangle(r, 2.0f, 0.8f);
        const float cxk = (k.x0 + k.x1) / 2, mid = (kSoftY0 + kSoftY1) / 2;
        if (k.id == skPrev || k.id == skNext) {
            juce::Path t;
            const float d = k.id == skPrev ? -1.0f : 1.0f;
            t.addTriangle(ink.p(cxk + 3.5f * d, mid), ink.p(cxk - 2.5f * d, mid - 4), ink.p(cxk - 2.5f * d, mid + 4));
            g.setColour(kText.withAlpha(a)); g.fillPath(t);
            continue;
        }
        juce::String label;
        switch (k.id) {
            case skType: label = !picked ? juce::String("PICK A BLOCK") : (pk == 0 ? juce::String("SONG SETTINGS") : juce::String(pk) + " " + up(proc_.sectionName(pk))); break;
            case skSplit: label = "SPLIT"; break;
            case skJoin: label = "JOIN"; break;
            case skAdd: label = proc_.marking() ? "+ AT PLAYHEAD" : "+ MARK AT PLAYHEAD"; break;
            default: break;
        }
        while (label.length() > 3 && ink.width(label, 10.5f) > k.x1 - k.x0 - 6) label = label.dropLastCharacters(1);
        ink.text(label, cxk, mid + 4, 10.5f, 1, a);
    }

    // What the lit key changes.
    const lq::Values song = proc_.songValues();
    juce::String title, foot;
    std::vector<std::pair<juce::String, juce::String>> rows;
    if (lk == 0) {
        title = "EVERY SECTION STARTS FROM THE SONG SETTINGS";
        for (int k = 1; k <= LacquerProcessor::kSections; ++k) {
            bool used = false;
            for (int i = 0; i < m.count; ++i) used = used || m.marks[i].key == k;
            const int c = m.changes(k);
            if (!used && !c) continue;
            rows.push_back({juce::String(k) + "  " + up(proc_.sectionName(k)), c ? "changes " + juce::String(c) + (c > 1 ? " things" : " thing") : juce::String("follows the song")});
        }
        foot = "Ceiling, clipper and the output are set once, for the whole song.";
    } else {
        title = "WHAT THE " + up(proc_.sectionName(lk)) + " CHANGES";
        // One row per module, its changes joined.
        juce::String perModule[7];
        static const char* const modName[7] = {"CLEAN", "TONE", "DYNAMICS", "COLOUR", "STEREO", "LOUDNESS", "SPACE"};
        for (int p = 0; p < lq::kNumParams; ++p) {
            if (!m.has[lk][p]) continue;
            auto* prm = proc_.param(p);
            auto txt = [&](float v) { return prm->getText(prm->convertTo0to1(v), 32); };
            const juce::String nm = up(juce::String(lq::def(p).name).replace("space ", "").replace(" in", "").replace("stereo hear", "hear").replace("clean hear", "hear"));
            const juce::String item = (lq::def(p).kind == lq::kBool && endsWith(lq::def(p).id, "_in")) ? (m.val[lk][p] >= 0.5f ? "IN" : "OUT")
                                                                                                    : nm + " " + txt(song[p]) + kArrow + txt(m.val[lk][p]);
            auto& s = perModule[moduleOf(p)];
            s = s.isEmpty() ? item : s + ", " + item;
        }
        for (int i = 0; i < 7; ++i) if (perModule[i].isNotEmpty()) rows.push_back({modName[i], perModule[i]});
        if (rows.empty()) rows.push_back({"", "Nothing. It follows the song."});
        foot = "Everything else follows the song. Changes glide in over a beat.";
    }
    if (proc_.marking()) foot = proc_.playing() ? "Tap a number as each section starts. Press MARK again to finish."
                                                : "Put the playhead on a bar and tap a number to mark it there.";
    ink.text(title, 14, 132, 10.5f, 0, 1.0f, kText, 0.04f);
    for (size_t i = 0; i < rows.size() && i < 4; ++i) {
        const float y = 146.0f + float(i) * 13.5f;
        ink.text(rows[i].first, 14, y, 11.5f, 0, 1.0f, kText, 0.05f);
        juce::String what = rows[i].second;
        while (what.length() > 4 && ink.width(what, 11.0f, 0.02f) > 262) what = what.dropLastCharacters(4) + "...";
        ink.text(what, 104, y, 11.0f, 0, 0.78f, kText, 0.02f);
    }
    if (rows.size() > 4) ink.text("and " + juce::String(int(rows.size()) - 4) + " more", 366, 132, 10.0f, 2, 0.6f);
    ink.text(foot, 14, 201, 10.5f, 0, 0.74f);
    juce::String l1 = "SECTIONS   EDITING " + (lk == 0 ? juce::String("THE WHOLE SONG") : juce::String(lk) + " " + up(proc_.sectionName(lk)));
    if (proc_.playing() && !proc_.follow() && m.count > 0) l1 << "   " << kDot.trim() << "   HELD";
    juce::String l2 = juce::String(m.count) + (m.count == 1 ? " SECTION" : " SECTIONS") + " MARKED   " + kDot.trim() + "   CLICK A BLOCK TO PICK IT";
    if (m.count == 0) l2 = "PRESS MARK, PLAY THE SONG, TAP A NUMBER AS EACH PART STARTS";
    if (proc_.marking()) { l1 = "SECTIONS   MARKING"; l2 = proc_.playing() ? "TAP A NUMBER AS EACH SECTION STARTS" : "PLAY THE SONG AND TAP A NUMBER AS EACH SECTION STARTS"; }
    header(g, l1, l2);
}

void LacquerEditor::showKeyMenu(int markIndex) {
    juce::PopupMenu menu;
    const auto& m = proc_.songMap();
    const int cur = markIndex >= 0 && markIndex < m.count ? m.marks[markIndex].key : -1;
    for (int k = 0; k < lq::kNumKeys; ++k)
        menu.addItem((k == 0 ? juce::String("song settings") : juce::String(k) + "  " + proc_.sectionName(k)), true, k == cur, [this, markIndex, k] {
            proc_.setMarkKey(markIndex, k);
            proc_.setEditKey(k);
            repaint();
        });
    if (cur > 0) {
        menu.addSeparator();
        juce::PopupMenu names;
        addNameItems(names, cur);
        menu.addSubMenu("Key " + juce::String(cur) + " is called...", names);
    }
    menu.addSeparator();
    menu.addItem("Split this block", proc_.blockEnd(markIndex) - (cur >= 0 ? m.marks[markIndex].ppq : 0) > proc_.barLength() * 1.5, false,
                 [this, markIndex] { selMark_ = proc_.splitMark(markIndex); repaint(); });
    menu.addItem("Join it to the one before", [this, markIndex] { proc_.joinMark(markIndex); selMark_ = markIndex - 1; repaint(); });
    menu.addItem("Remove every mark", [this] { proc_.editSongMap([](lq::SongMap& mm) { mm.count = 0; }); selMark_ = -1; proc_.setEditKey(0); repaint(); });
    menu.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(this).withMousePosition());
}

// The names a section key can have: the usual ones, and your own.
void LacquerEditor::addNameItems(juce::PopupMenu& menu, int key) {
    const auto names = LacquerProcessor::sectionVocabulary();
    for (const auto& n : names) menu.addItem(n, true, proc_.sectionName(key) == n, [this, key, n] { proc_.setSectionName(key, n); repaint(); });
    menu.addSeparator();
    menu.addItem("Write your own...", [this, key] {
        auto* w = new juce::AlertWindow("Section key " + juce::String(key), "What to call it:", juce::MessageBoxIconType::NoIcon);
        w->addTextEditor("name", proc_.sectionName(key));
        w->addButton("OK", 1, juce::KeyPress(juce::KeyPress::returnKey));
        w->addButton("Cancel", 0, juce::KeyPress(juce::KeyPress::escapeKey));
        w->enterModalState(true, juce::ModalCallbackFunction::create([this, w, key](int r) {
            if (r == 1) proc_.setSectionName(key, w->getTextEditorContents("name"));
            repaint();
        }), true);
    });
}

void LacquerEditor::showNameMenu(int key) {
    if (key < 1 || key > LacquerProcessor::kSections) return;
    juce::PopupMenu menu;
    menu.addSectionHeader("Section key " + juce::String(key));
    addNameItems(menu, key);
    menu.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(this).withMousePosition());
}

void LacquerEditor::screenClick(juce::Point<float> pt, const juce::MouseEvent& e) {
    const auto o = sc(0, 0);
    if (proc_.page == 2) {
        const int i = moveRowAt(pt);
        if (i >= 0) proc_.takeMove(i, !proc_.result().moves[size_t(i)].taken);
    } else if (proc_.page == 0) {
        switch (balanceChipAt(pt)) {
            case 0: proc_.setGenre(proc_.genre() - 1); break;
            case 1: proc_.setGenre(proc_.genre() + 1); break;
            case 2: {
                juce::PopupMenu m;
                for (int i = 0; i < proc_.numStyles(); ++i) m.addItem(proc_.styleName(i), true, i == proc_.genre(), [this, i] { proc_.setGenre(i); repaint(); });
                if (proc_.styleIsYours(proc_.genre())) {
                    m.addSeparator();
                    m.addItem("Remove \"" + proc_.styleName(proc_.genre()) + "\"", [this] { proc_.removeStyle(proc_.genre()); repaint(); });
                }
                m.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(this).withMousePosition());
                break;
            }
            case 3: chooseReferences(); break;
            case 4: askStyleName(); break;
            default: break;
        }
        (void)o;
    } else if (proc_.page == 5) {
        detailClick(pt, e);
    } else if (proc_.page == 4) {
        const auto& m = proc_.songMap();
        const int sk = softKeyAt(pt);
        if (sk >= 0) {
            const bool picked = selMark_ >= 0 && selMark_ < m.count;
            softDown_ = sk;
            switch (sk) {
                case skPrev:
                case skNext:
                    if (picked) {
                        const int k = (m.marks[selMark_].key + (sk == skNext ? 1 : lq::kNumKeys - 1)) % lq::kNumKeys;
                        proc_.setMarkKey(selMark_, k);
                        proc_.setEditKey(k);
                    }
                    break;
                case skType: if (picked) showKeyMenu(selMark_); break;
                case skSplit: if (picked) { const int n = proc_.splitMark(selMark_); if (n >= 0) selMark_ = n; } break;
                case skJoin: if (picked) { proc_.joinMark(selMark_); selMark_ = std::min(selMark_ - 1, proc_.songMap().count - 1); } break;
                case skAdd: {
                    const int lkey = proc_.litKey();
                    selMark_ = proc_.addMarkAtPlayhead(lkey > 0 ? lkey : 1);
                    if (lkey <= 0) proc_.setEditKey(1);
                    break;
                }
                default: break;
            }
            repaint();
            return;
        }
        bool edge = false;
        const int b = blockAt(pt, edge);
        if (b < 0) return;
        selMark_ = b;
        if (e.mods.isPopupMenu()) { repaint(); showKeyMenu(b); return; }
        if (edge) { drag_ = kScreen; dragMark_ = b; repaint(); return; }
        // Picking a block lights its key; while the song plays, that also stops FOLLOW.
        proc_.setEditKey(m.marks[b].key);
        if (proc_.playing()) proc_.setFollow(false);
    }
    repaint();
}


// ---------------------------------------------------------------------------
// DETAIL: what the faceplate's knobs don't reach, for the module you last touched.
namespace {
const char* const kTabs[4] = {"TONE", "DYNAMICS", "LOUDNESS", "OUTPUT"};
constexpr float kTabY0 = 44, kTabY1 = 57;
float tabX0(int t) { return 12.0f + t * 90.0f; }
float tabX1(int t) { return tabX0(t) + 84.0f; }
// The EQ graph.
constexpr float kEqY0 = 64, kEqY1 = 156, kEqMid = 110, kEqPerDb = 3.6f;   // +-12 dB
float eqX(double f) { return float(8 + 364 * std::log(f / 20.0) / std::log(1000.0)); }
double eqF(float x) { return 20.0 * std::pow(1000.0, (x - 8) / 364.0); }
float eqY(double db) { return float(kEqMid - db * kEqPerDb); }
const int kBandDb[4] = {lq::tLow, lq::tMud, lq::tPres, lq::tAir}, kBandHz[4] = {lq::tLowHz, lq::tMudHz, lq::tPresHz, lq::tAirHz},
          kBandPl[4] = {lq::tLowPl, lq::tMudPl, lq::tPresPl, lq::tAirPl};
const char* const kBandName[4] = {"LOW", "MUD", "PRESENCE", "AIR"};
const char* const kBandTag[4] = {"L", "M", "P", "A"};
// A row of the DYNAMICS / LOUDNESS / OUTPUT tabs: a name, and a value stepped with arrows.
struct Row { int p; const char* name; };
const Row kDynRows[4] = {{lq::dAttack, "ATTACK"}, {lq::dRelease, "RELEASE"}, {lq::dRatio, "RATIO"}, {lq::dChan, "CHANNELS"}};
const Row kLoudRows[2] = {{lq::lStyle, "STYLE"}, {lq::lRelease, "RELEASE"}};
const Row kOutRows[1] = {{lq::oCodecRate, "CODEC"}};
constexpr float kRowY0 = 140, kRowPitch = 16;
// The arrows and the value: x 214..368.
constexpr float kArrL0 = 214, kArrL1 = 228, kArrR0 = 354, kArrR1 = 368;
}  // namespace

void LacquerEditor::followDetail(int p) {
    const int m = moduleOf(p);
    int tab = -1;
    if (m == 1) tab = 0;
    else if (m == 2) tab = 1;
    else if (p >= lq::loudIn && p <= lq::lGuard) tab = 2;
    else if (p == lq::lStyle || p == lq::lRelease) tab = 2;
    else if (p == lq::oListen || p == lq::oCodec || p == lq::oDither || p == lq::oMonitor || p == lq::oMatch || p == lq::oBefore || p == lq::oCodecRate) tab = 3;
    else if (p >= lq::tMud && p <= lq::tPhase) tab = 0;
    else if (p >= lq::dAttack && p <= lq::dRatio) tab = 1;
    if (tab >= 0 && tab != proc_.detailTab) { proc_.detailTab = tab; if (proc_.page == 5) repaintPts({kFaceX, kFaceY, kFaceW, kFaceH}); }
}

void LacquerEditor::stepChoice(int p, int dir) {
    const auto& d = lq::def(p);
    const int n = d.kind == lq::kBool ? 2 : d.nChoices;
    const int cur = juce::roundToInt(proc_.shownValue(p));
    beginEdit(p);
    proc_.setShown(p, float(((cur + dir) % n + n) % n));
    endEdit(p);
}

void LacquerEditor::drawDetail(juce::Graphics& g) {
    const Ink ink{g, art_->legend, kFaceX - 2, kFaceY - 2};
    const int tab = juce::jlimit(0, 3, proc_.detailTab);
    for (int t = 0; t < 4; ++t) {
        const auto r = juce::Rectangle<float>(ink.p(tabX0(t), kTabY0), ink.p(tabX1(t), kTabY1));
        g.setColour(kText.withAlpha(t == tab ? 0.85f : 0.07f)); g.fillRoundedRectangle(r, 2.0f);
        g.setColour(kText.withAlpha(0.6f)); g.drawRoundedRectangle(r, 2.0f, 0.8f);
        ink.text(kTabs[t], (tabX0(t) + tabX1(t)) / 2, kTabY1 - 3.5f, 10.5f, 1, 1.0f, t == tab ? kInk : kText);
    }
    auto arrows = [&](float y, const juce::String& value, bool enabled = true) {
        const float a = enabled ? 1.0f : 0.3f;
        for (int side = 0; side < 2; ++side) {
            const float x0 = side ? kArrR0 : kArrL0, x1 = side ? kArrR1 : kArrL1, cx0 = (x0 + x1) / 2, d = side ? 1.0f : -1.0f;
            const auto r = juce::Rectangle<float>(ink.p(x0, y - 10.5f), ink.p(x1, y + 3));
            g.setColour(kText.withAlpha(0.07f * a)); g.fillRoundedRectangle(r, 2.0f);
            g.setColour(kText.withAlpha(0.6f * a)); g.drawRoundedRectangle(r, 2.0f, 0.8f);
            juce::Path tri;
            tri.addTriangle(ink.p(cx0 + 3.0f * d, y - 3.8f), ink.p(cx0 - 2.0f * d, y - 7.3f), ink.p(cx0 - 2.0f * d, y - 0.3f));
            g.setColour(kText.withAlpha(a)); g.fillPath(tri);
        }
        ink.text(value, (kArrL1 + kArrR0) / 2, y, 11.0f, 1, a);
    };
    auto valueOf = [&](int p) { auto* prm = proc_.param(p); return up(prm->getText(prm->convertTo0to1(proc_.shownValue(p)), 32)); };
    auto trace = [&](const float* hist, float y0, float y1, float maxDb, const juce::String& label) {
        g.setColour(kText.withAlpha(0.06f)); g.fillRect(juce::Rectangle<float>(ink.p(8, y0), ink.p(372, y1)));
        juce::Path p;
        for (int i = 0; i < 240; ++i) {
            const float v = hist[(histPos_ + i) % 240];
            const auto pt = ink.p(8 + 364 * i / 239.0f, y0 + (y1 - y0) * juce::jlimit(0.0f, 1.0f, v / maxDb));
            if (i == 0) p.startNewSubPath(pt); else p.lineTo(pt);
        }
        ink.glowLine(p);
        float now = 0;
        for (int i = 0; i < 8; ++i) now = std::max(now, hist[(histPos_ + 239 - i) % 240]);
        ink.text(label + "   " + kMinus + juce::String(now, 1) + " dB", 14, y1 + 11, 10.5f, 0, 0.9f);
        ink.text(kMinus + juce::String(juce::roundToInt(maxDb)) + " dB", 366, y1 - 3, 9.5f, 2, 0.5f);
    };
    juce::String l1 = "DETAIL   " + juce::String(kTabs[tab]), l2;
    const int lk = proc_.litKey();
    if (lk > 0) l1 << "   " << kDot.trim() << "   " << keyLabel(lk);
    if (tab == 0) {
        // The EQ: the mid (bright) and the side (dashed), and the four bands' handles.
        lq::Values v = proc_.valuesFor(lk);
        for (int p = 0; p < lq::kNumParams; ++p) v[p] = proc_.shownValue(p);
        lq::ToneParams tp = lq::Engine::toneParamsFrom(v);
        tp.weight = 1;
        for (int db : {-12, -6, 0, 6, 12}) {
            juce::Path gl; gl.startNewSubPath(ink.p(8, eqY(db))); gl.lineTo(ink.p(372, eqY(db)));
            ink.dashed(gl, 2, 3, 0.7f, kText.withAlpha(db == 0 ? 0.45f : 0.22f));
            ink.text((db > 0 ? "+" : db < 0 ? kMinus : "") + juce::String(std::abs(db)), 14, eqY(db) - 2, 9.5f, 0, 0.5f);
        }
        juce::Path pm, ps;
        bool sideMoves = false;
        for (int i = 0; i <= 180; ++i) {
            const double f = 20.0 * std::pow(1000.0, i / 180.0);
            std::complex<double> hm, hs;
            lq::toneResponseMS(tp, f, 48000.0, hm, hs);
            const float ym = juce::jlimit(kEqY0, kEqY1, eqY(20 * std::log10(std::abs(hm) + 1e-9)));
            const float ys = juce::jlimit(kEqY0, kEqY1, eqY(20 * std::log10(std::abs(hs) + 1e-9)));
            sideMoves = sideMoves || std::abs(ym - ys) > 0.3f;
            if (i == 0) { pm.startNewSubPath(ink.p(eqX(f), ym)); ps.startNewSubPath(ink.p(eqX(f), ys)); }
            else { pm.lineTo(ink.p(eqX(f), ym)); ps.lineTo(ink.p(eqX(f), ys)); }
        }
        if (sideMoves) ink.dashed(ps, 4, 3, 1.4f, kCurve.withAlpha(0.75f));
        ink.glowLine(pm);
        for (int b = 0; b < 4; ++b) {
            const float x = eqX(v[kBandHz[b]]), y = juce::jlimit(kEqY0 + 4, kEqY1 - 4, eqY(v[kBandDb[b]]));
            const bool sel = b == toneBand_;
            const auto c = ink.p(x, y);
            g.setColour(sel ? kCurve : juce::Colour(0xff0b2216)); g.fillEllipse(c.x - 6.5f, c.y - 6.5f, 13, 13);
            g.setColour(kCurve); g.drawEllipse(c.x - 6.5f, c.y - 6.5f, 13, 13, 1.2f);
            ink.text(kBandTag[b], x, y + 3.6f, 9.5f, 1, 1.0f, sel ? kInk : kText);
            const int pl = juce::roundToInt(v[kBandPl[b]]);
            if (pl > 0) ink.text(pl == 1 ? "MID" : "SIDE", x, y + 17, 9.0f, 1, 0.8f);
        }
        for (const auto& fr : {std::pair<double, const char*>{50, "50"}, {100, "100"}, {200, "200"}, {500, "500"}, {1000, "1k"}, {2000, "2k"}, {5000, "5k"}, {10000, "10k"}})
            ink.text(fr.second, eqX(fr.first), 167, 9.5f, 1, 0.55f);
        // The picked band: where it works, and the phase of the whole.
        const int b = toneBand_;
        ink.text(juce::String(kBandName[b]) + "  " + num1(v[kBandDb[b]], true) + " dB  AT  " +
                 (v[kBandHz[b]] >= 1000 ? juce::String(v[kBandHz[b]] / 1000.0, 1) + " kHz" : juce::String(juce::roundToInt(v[kBandHz[b]])) + " Hz"), 14, 184, 11.0f, 0, 1.0f);
        arrows(184, juce::String(juce::StringArray{"STEREO", "MID", "SIDE"}[juce::jlimit(0, 2, juce::roundToInt(v[kBandPl[b]]))]));
        const bool linear = proc_.songValue(lq::tPhase) >= 0.5f;
        ink.text("PHASE", 14, 201, 11.0f, 0, 1.0f);
        arrows(201, linear ? "LINEAR" : "NATURAL");
        l2 = "DRAG A HANDLE: ACROSS IS FREQUENCY, UP AND DOWN IS GAIN";
        if (linear) l2 = "LINEAR PHASE: " + juce::String(juce::roundToInt(1000.0 * proc_.latencyNow() / proc_.sampleRate())) + " MS OF DELAY IN ALL; DYNAMIC MODE RESTS";
    } else if (tab == 1) {
        trace(glueHist_, 62, 120, 8, "GLUE: GAIN REDUCTION");
        for (int i = 0; i < 4; ++i) {
            const float y = kRowY0 + 14 + i * kRowPitch;
            ink.text(kDynRows[i].name, 14, y, 11.0f, 0, 1.0f);
            arrows(y, valueOf(kDynRows[i].p));
        }
        l2 = "GLUE'S TIMING. A FAST ATTACK TAKES THE FRONT OFF THE HITS";
    } else if (tab == 2) {
        trace(limHist_, 62, 120, 8, "LIMITER: GAIN REDUCTION");
        const auto& mt = proc_.meters();
        ink.text("IN " + num1(mt.inLufs.load()) + "   OUT " + num1(mt.stLufs.load()) + " LUFS   DRIVE " + num1(mt.driveDb.load(), true) + " dB", 366, 131, 10.5f, 2, 0.8f);
        for (int i = 0; i < 2; ++i) {
            const float y = kRowY0 + 14 + i * kRowPitch;
            ink.text(kLoudRows[i].name, 14, y, 11.0f, 0, 1.0f);
            arrows(y, valueOf(kLoudRows[i].p));
        }
        const double cov = proc_.coverage();
        ink.text(!proc_.loudnessKnown() ? juce::String("LEARNING THE SONG'S LOUDNESS AS IT PLAYS")
                 : cov < 0 ? juce::String("LOUDNESS LEARNT FROM THE LAST PLAY")
                           : "LOUDNESS LEARNT FROM " + juce::String(juce::roundToInt(cov * 100)) + "% OF THE SONG", 14, 190, 10.5f, 0, 0.8f);
        {
            const auto r = juce::Rectangle<float>(ink.p(300, 179), ink.p(368, 194));
            g.setColour(kText.withAlpha(0.07f)); g.fillRoundedRectangle(r, 2.0f);
            g.setColour(kText.withAlpha(0.6f)); g.drawRoundedRectangle(r, 2.0f, 0.8f);
            ink.text("FORGET", 334, 190, 10.5f, 1, 1.0f);
        }
        static const char* const styles[3] = {"CLEAN HOLDS ON AND LETS GO SLOWLY", "PUNCHY LETS THE FRONT OF EACH HIT THROUGH", "DENSE LETS GO FAST: MORE LOUDNESS, LESS AIR"};
        l2 = styles[juce::jlimit(0, 2, juce::roundToInt(proc_.songValue(lq::lStyle)))];
    } else {
        const auto& mt = proc_.meters();
        ink.text("IN", 14, 76, 11.0f, 0, 1.0f);
        ink.text(num1(mt.inLufs.load()) + " LUFS SHORT-TERM   " + num1(mt.inPeak.load()) + " dBFS PEAK", 70, 76, 11.0f, 0, 0.85f);
        ink.text("OUT", 14, 92, 11.0f, 0, 1.0f);
        ink.text(num1(mt.stLufs.load()) + " LUFS SHORT-TERM   " + num1(outPkHold_) + " dBTP PEAK", 70, 92, 11.0f, 0, 0.85f);
        ink.text("BEFORE", 14, 108, 11.0f, 0, 1.0f);
        ink.text("the mix turned " + num1(mt.beforeDb.load(), true) + " dB to match the master", 70, 108, 11.0f, 0, 0.85f);
        const bool avail = proc_.codecAvailable();
        ink.text("CODEC", 14, 124, 11.0f, 0, 1.0f);
        if (!avail) ink.text(proc_.codecWhy(), 70, 124, 10.5f, 0, 0.7f);
        else if (proc_.songValue(lq::oCodec) < 0.5f) ink.text("off: switch CODEC on to hear the master after encoding", 70, 124, 10.5f, 0, 0.7f);
        else {
            const float cp = proc_.codecPeak();
            ink.text("after encoding the true peak reaches " + (cp > -100 ? num1(cp) + " dBTP" : juce::String("--")), 70, 124, 10.5f, 0, 0.85f);
            if (cp > -0.05f) ink.text("OVER: LOWER THE CEILING", 366, 140, 10.5f, 2, 1.0f);
        }
        for (int i = 0; i < 1; ++i) {
            const float y = 156;
            ink.text(kOutRows[i].name, 14, y, 11.0f, 0, 1.0f);
            arrows(y, valueOf(kOutRows[i].p), avail);
        }
        ink.text("The codec preview is for listening only: a little later than the song, never in a bounce.", 14, 186, 10.0f, 0, 0.6f, kText, 0.0f);
        ink.text("BEFORE and LISTEN ON never reach a bounce either.", 14, 199, 10.0f, 0, 0.6f, kText, 0.0f);
        l2 = "WHAT COMES IN, WHAT GOES OUT, AND HOW IT WILL BE HEARD";
    }
    header(g, l1, l2);
}

bool LacquerEditor::detailClick(juce::Point<float> pt, const juce::MouseEvent& e) {
    const auto o = sc(0, 0);
    const float x = pt.x - o.x, y = pt.y - o.y;
    if (y >= kTabY0 && y <= kTabY1)
        for (int t = 0; t < 4; ++t) if (x >= tabX0(t) && x <= tabX1(t)) { proc_.detailTab = t; repaint(); return true; }
    const int tab = proc_.detailTab;
    auto arrowAt = [&](float rowY) -> int {
        if (y < rowY - 11 || y > rowY + 4) return 0;
        if (x >= kArrL0 - 2 && x <= kArrL1 + 2) return -1;
        if (x >= kArrR0 - 2 && x <= kArrR1 + 2) return 1;
        if (x > kArrL1 && x < kArrR0) return e.mods.isPopupMenu() ? -1 : 1;
        return 0;
    };
    if (tab == 0) {
        // A handle?
        lq::Values v = proc_.valuesFor(proc_.litKey());
        for (int p = 0; p < lq::kNumParams; ++p) v[p] = proc_.shownValue(p);
        int best = -1;
        float bd = 11;
        for (int b = 0; b < 4; ++b) {
            const float d = juce::Point<float>(x, y).getDistanceFrom({eqX(v[kBandHz[b]]), juce::jlimit(kEqY0 + 4, kEqY1 - 4, eqY(v[kBandDb[b]]))});
            if (d < bd) { bd = d; best = b; }
        }
        if (best >= 0) {
            toneBand_ = best;
            if (e.mods.isPopupMenu()) { stepChoice(kBandPl[best], 1); repaint(); return true; }
            dragBand_ = best; drag_ = kScreen;
            beginEdit(kBandDb[best]); beginEdit(kBandHz[best]);
            repaint();
            return true;
        }
        if (const int a = arrowAt(184)) { stepChoice(kBandPl[toneBand_], a); repaint(); return true; }
        if (const int a = arrowAt(201)) { stepChoice(lq::tPhase, a); repaint(); return true; }
        return false;
    }
    if (tab == 1) {
        for (int i = 0; i < 4; ++i) if (const int a = arrowAt(kRowY0 + 14 + i * kRowPitch)) { stepChoice(kDynRows[i].p, a); repaint(); return true; }
        return false;
    }
    if (tab == 2) {
        for (int i = 0; i < 2; ++i) if (const int a = arrowAt(kRowY0 + 14 + i * kRowPitch)) { stepChoice(kLoudRows[i].p, a); repaint(); return true; }
        if (x >= 300 && x <= 368 && y >= 179 && y <= 194) {
            juce::NativeMessageBox::showOkCancelBox(juce::MessageBoxIconType::QuestionIcon, "Forget the song's loudness?",
                "Lacquer learns how loud the song is from every bar you play. Forget it when the mix has changed, then play the song through once.", this,
                juce::ModalCallbackFunction::create([this](int r) { if (r == 1) proc_.forgetLoudness(); repaint(); }));
            return true;
        }
        return false;
    }
    if (const int a = arrowAt(156)) { if (proc_.codecAvailable()) stepChoice(lq::oCodecRate, a); repaint(); return true; }
    return false;
}

void LacquerEditor::detailDrag(juce::Point<float> pt, const juce::MouseEvent& e) {
    if (dragBand_ < 0) return;
    const auto o = sc(0, 0);
    const float x = pt.x - o.x, y = pt.y - o.y;
    const int b = dragBand_;
    const auto& dh = lq::def(kBandHz[b]);
    const float hz = juce::jlimit(dh.lo, dh.hi, float(eqF(juce::jlimit(8.0f, 372.0f, x))));
    float db = float((kEqMid - y) / kEqPerDb);
    if (e.mods.isShiftDown()) db = std::round(db * 2) / 2;
    proc_.setShown(kBandHz[b], hz);
    proc_.setShown(kBandDb[b], juce::jlimit(-12.0f, 12.0f, db));
    repaint();
}

// ---- SECTIONS: the song's loudness bar by bar, the master over the mix
void LacquerEditor::drawLane(juce::Graphics& g) {
    const Ink ink{g, art_->legend, kFaceX - 2, kFaceY - 2};
    const auto& bl = proc_.bars();
    const double q = proc_.barLength(), end = proc_.songEndPpq();
    const int nb = std::min(lq::kMaxBars, int(std::ceil(end / q)));
    double lo = 1e9, hi = -1e9, sOut = 0, sMix = 0;
    int n = 0;
    for (int b = 0; b < nb; ++b) {
        if (!bl.heard[b].load()) continue;
        const double o = bl.out[b].load(), m = bl.mix[b].load();
        if (o < -60) continue;
        lo = std::min(lo, o); hi = std::max(hi, o);
        sOut += o; sMix += m; ++n;
    }
    constexpr float y0 = 45, y1 = 67;
    g.setColour(kText.withAlpha(0.05f)); g.fillRect(juce::Rectangle<float>(ink.p(8, y0), ink.p(372, y1)));
    if (n < 2) {
        ink.text("PLAY THE SONG: ITS LOUDNESS, BAR BY BAR, SHOWS HERE", 190, 60, 10.0f, 1, 0.5f);
        return;
    }
    const double shift = (sOut - sMix) / n;   // the mix, lifted to sit with the master
    for (int b = 0; b < nb; ++b) if (bl.heard[b].load() && bl.mix[b].load() > -60) { lo = std::min(lo, double(bl.mix[b].load()) + shift); hi = std::max(hi, double(bl.mix[b].load()) + shift); }
    const double mid = (lo + hi) / 2, span = std::max(8.0, hi - lo + 2);
    auto Y = [&](double lu) { return float(y1 - 2 - (y1 - y0 - 4) * ((lu - (mid - span / 2)) / span)); };
    auto X = [&](int b) { return xOfPpq((b + 0.5) * q) - (kFaceX - 2); };
    juce::Path pm, pmx;
    bool startedM = false, startedX = false;
    for (int b = 0; b < nb; ++b) {
        if (!bl.heard[b].load() || bl.out[b].load() < -60) { startedM = startedX = false; continue; }
        const auto a = ink.p(X(b), Y(bl.out[b].load()));
        if (!startedM) { pm.startNewSubPath(a); startedM = true; } else pm.lineTo(a);
        const auto c = ink.p(X(b), Y(bl.mix[b].load() + shift));
        if (!startedX) { pmx.startNewSubPath(c); startedX = true; } else pmx.lineTo(c);
    }
    ink.dashed(pmx, 1.2f, 2.4f, 1.1f, kText.withAlpha(0.6f));
    g.setColour(kGlow); g.strokePath(pm, juce::PathStrokeType(3.5f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
    g.setColour(kCurve); g.strokePath(pm, juce::PathStrokeType(1.4f, juce::PathStrokeType::curved));

}

// ---- BALANCE: the target's chips, top right
namespace {
constexpr float kChipY0 = 11, kChipY1 = 26;
}
int LacquerEditor::balanceChipAt(juce::Point<float> pt) const {
    const auto o = sc(0, 0);
    const float x = pt.x - o.x, y = pt.y - o.y;
    if (y < kChipY0 - 2 || y > kChipY1 + 2) return -1;
    const int t = juce::roundToInt(proc_.songValue(lq::aTarget));
    if (t == 0) {
        if (x >= 220 && x <= 236) return 0;     // previous style
        if (x >= 352 && x <= 368) return 1;     // next style
        if (x > 236 && x < 352) return 2;       // the style's menu
    } else if (t == 1) {
        if (x >= 220 && x <= 290) return 3;     // add references
        if (x >= 296 && x <= 368 && proc_.numRefs() > 0) return 4;   // save as a style
    }
    return -1;
}

void LacquerEditor::chooseReferences() {
    chooser_ = std::make_unique<juce::FileChooser>("Reference tracks", juce::File(), "*.wav;*.aif;*.aiff;*.flac;*.mp3;*.m4a;*.caf");
    chooser_->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles | juce::FileBrowserComponent::canSelectMultipleItems,
                          [this](const juce::FileChooser& fc) {
                              juce::StringArray files;
                              for (const auto& f : fc.getResults()) files.add(f.getFullPathName());
                              if (!files.isEmpty()) { proc_.addReferences(files); proc_.setSong(lq::aTarget, 1.0f); }
                              repaint();
                          });
}

void LacquerEditor::askStyleName() {
    auto* w = new juce::AlertWindow("Save the references as a style", "A name for it (it joins the genres under TARGET):", juce::MessageBoxIconType::NoIcon);
    w->addTextEditor("name", "my references");
    w->addButton("Save", 1, juce::KeyPress(juce::KeyPress::returnKey));
    w->addButton("Cancel", 0, juce::KeyPress(juce::KeyPress::escapeKey));
    w->enterModalState(true, juce::ModalCallbackFunction::create([this, w](int r) {
        if (r == 1) proc_.saveReferencesAsStyle(w->getTextEditorContents("name"));
        repaint();
    }), true);
}

// ---- a knob's value, written beside it while it turns
void LacquerEditor::drawBubble(juce::Graphics& g) {
    if (bubble_ < 0 || bubble_ >= kNumControls || clock_ > bubbleUntil_) return;
    const auto& c = kControls[bubble_];
    const int p = paramOf(bubble_);
    if (p < 0 || (c.tray && !proc_.trayOpen)) return;
    auto* prm = proc_.param(p);
    const juce::String t = prm->getText(prm->convertTo0to1(value(bubble_)), 32);
    const juce::Font f = font(13.0f);
    const float w = juce::GlyphArrangement::getStringWidth(f, t) / s_ + 14, h = 19;
    const float r0 = c.type == Knob ? kKnobRadius[c.kind] : 14.0f;
    auto r = juce::Rectangle<float>(c.x - w / 2, cy(c) - r0 - h - 8, w, h);
    if (r.getY() < 2) r.setY(cy(c) + r0 + 8);
    g.setColour(juce::Colour(0xf01b1a17));
    g.fillRoundedRectangle(r * s_, 4 * s_);
    g.setColour(juce::Colour(0xfff1ead2));
    g.setFont(f);
    g.drawText(t, r * s_, juce::Justification::centred, false);
}

// ---------------------------------------------------------------------------
// Drawing
void LacquerEditor::drawSprite(juce::Graphics& g, const juce::Image& im, float cx, float cyy, float wPt, float hPt, int frame, int frames) {
    if (!im.isValid()) return;
    g.setOpacity(1.0f);   // images take the context's opacity: never let an earlier shade fade them
    const int fh = im.getHeight() / std::max(1, frames);
    g.drawImage(im, int(std::round((cx - wPt / 2) * s_)), int(std::round((cyy - hPt / 2) * s_)), int(std::round(wPt * s_)), int(std::round(hPt * s_)),
                0, frame * fh, im.getWidth(), fh);
}

void LacquerEditor::drawLit(juce::Graphics& g, int ci) {
    const auto& c = kControls[ci];
    if (c.litGroup < 0) return;
    const auto& from = art_->lit[c.litGroup];
    if (!from.isValid()) return;
    const float* gr = kLitGroupRect[c.litGroup];
    const int S = kArtScale;
    const float oy = c.tray ? H : 0.0f;
    g.setOpacity(1.0f);
    g.drawImage(from, int(std::round(c.lx * s_)), int(std::round((c.ly + oy) * s_)), int(std::round(c.lw * s_)), int(std::round(c.lh * s_)),
                int((c.lx - gr[0]) * S), int((c.ly - gr[1]) * S), int(c.lw * S), int(c.lh * S));
}

void LacquerEditor::paint(juce::Graphics& g) {
    g.setImageResamplingQuality(juce::Graphics::highResamplingQuality);
    const auto clip = g.getClipBounds().toFloat() / s_;
    g.setOpacity(1.0f);
    if (art_->panel.isValid()) g.drawImage(art_->panel, juce::Rectangle<float>(0, 0, W, H) * s_);
    else g.fillAll(juce::Colour(0xff8a8a86));
    if (proc_.trayOpen) {
        if (art_->tray.isValid()) g.drawImage(art_->tray, juce::Rectangle<float>(0, H, W, kTrayH) * s_);
        else { g.setColour(juce::Colour(0xff404346)); g.fillRect(juce::Rectangle<float>(0, H, W, kTrayH) * s_); }
    }
    for (int i = 0; i < kNumControls; ++i) {
        const auto& c = kControls[i];
        if (c.tray && !proc_.trayOpen) continue;
        if (!boundsOf(i).expanded(2).intersects(clip)) continue;
        switch (c.type) {
            case Knob: drawKnob(g, i); break;
            case Toggle: drawSprite(g, value(i) >= 0.5f ? art_->toggleUp : art_->toggleDown, c.x, cy(c), 44, 44); break;
            case Button:
            case Lamp:
                if (lit(i)) drawLit(g, i);
                if (pressed_ == i) {
                    juce::Graphics::ScopedSaveState keep(g);
                    g.setColour(juce::Colours::black.withAlpha(0.2f));
                    g.fillRoundedRectangle(juce::Rectangle<float>(c.x - c.w / 2 + 2, cy(c) - c.h / 2 + 2, c.w - 4, c.h - 4) * s_, 2 * s_);
                }
                break;
            case Meter: drawMeter(g, i); break;
            case layout::Screen: drawScreen(g); break;
        }
        drawn_[size_t(i)] = paramOf(i) >= 0 ? value(i) : (lit(i) ? 1.0f : 0.0f);
    }
    // A knob a section has its own setting for wears a small mark, so you can see what the section changes.
    {
        const int lk = proc_.litKey();
        if (lk > 0) {
            g.setColour(juce::Colour(0xffe2a04a));
            for (int i = 0; i < kNumControls; ++i) {
                const auto& c = kControls[i];
                if ((c.tray && !proc_.trayOpen) || paramOf(i) < 0 || c.type == Meter) continue;
                if (!proc_.shownIsOwn(paramOf(i))) continue;
                const float rr = c.type == Knob ? kKnobRadius[c.kind] + 3.0f : 12.0f;
                const auto dot = juce::Rectangle<float>(c.x + rr * 0.72f - 3.8f, cy(c) - rr * 0.72f - 3.8f, 7.6f, 7.6f) * s_;
                g.setColour(juce::Colour(0xff2a2219)); g.fillEllipse(dot.expanded(1.2f * s_));
                g.setColour(juce::Colour(0xffe2a04a)); g.fillEllipse(dot);
            }
        }
    }
    const auto& mt = proc_.meters();
    const float integ = mt.integrated.load(), tp = mt.peakHold.load();
    drawReadout(g, kReadoutLufs, integ > -69 ? num1(integ) : juce::String("--.-"));
    drawReadout(g, kReadoutTp, tp > -100 ? num1(tp) : juce::String("--.-"));
    drawTapes(g);
    drawBubble(g);
    if (clip.getBottom() > H + trayH()) drawHelp(g);
    seenRevision_ = proc_.revision.load();
}

void LacquerEditor::drawKnob(juce::Graphics& g, int ci) {
    const auto& c = kControls[ci];
    const float box = knobBox(c.kind);
    if (c.kind == kSel) {
        const float ang = c.steps[selIndex(ci)];
        int best = 0;
        for (int k = 1; k < kNumSelAngles; ++k)
            if (std::abs(kSelAngles[k] - ang) < std::abs(kSelAngles[best] - ang)) best = k;
        drawSprite(g, art_->strip[kSel], c.x, cy(c), box, box, best, art_->stripFrames[kSel]);
        return;
    }
    const int n = art_->stripFrames[c.kind];
    const int frame = juce::jlimit(0, std::max(0, n - 1), int(std::lround(norm(ci) * float(n - 1))));
    if (c.pointer >= 0) drawSprite(g, paintedFrame(ci, frame), c.x, cy(c), box, box);
    else drawSprite(g, art_->strip[c.kind], c.x, cy(c), box, box, frame, n);
}

// One frame of a knob with its white pointer lines repainted in the knob's colour.
const juce::Image& LacquerEditor::paintedFrame(int ci, int frame) {
    auto& p = painted_[size_t(ci)];
    if (p.frame == frame && p.image.isValid()) return p.image;
    const auto& c = kControls[ci];
    juce::Image strip = art_->strip[c.kind];
    const int fw = strip.getWidth(), n = art_->stripFrames[c.kind];
    p.image = juce::Image(juce::Image::ARGB, std::max(1, fw), std::max(1, fw), true);
    p.frame = frame;
    if (!strip.isValid() || n < 2) return p.image;
    const float* col = kPointerColours[c.pointer];
    const float a = juce::degreesToRadians(-kSweep + 2 * kSweep * float(frame) / float(n - 1));
    const float ux = std::sin(a), uy = -std::cos(a);
    const float S = float(kArtScale), r = kKnobRadius[c.kind];
    const float inner = (kCapR * r + 0.3f) * S, outer = (r + 0.5f) * S, half = 1.15f * S;
    const float c0 = float(fw) / 2;
    const juce::Image::BitmapData src(strip, 0, frame * fw, fw, fw, juce::Image::BitmapData::readOnly);
    juce::Image::BitmapData dst(p.image, juce::Image::BitmapData::writeOnly);
    for (int y = 0; y < fw; ++y)
        for (int x = 0; x < fw; ++x) {
            const auto* sp = reinterpret_cast<const juce::PixelARGB*>(src.getPixelPointer(x, y));
            auto* dp = reinterpret_cast<juce::PixelARGB*>(dst.getPixelPointer(x, y));
            *dp = *sp;
            const float vx = float(x) + 0.5f - c0, vy = float(y) + 0.5f - c0;
            const float along = vx * ux + vy * uy, perp = std::abs(vx * uy - vy * ux);
            if (along < inner - 1 || along > outer + 1 || perp > half + 1) continue;
            const float geo = juce::jlimit(0.0f, 1.0f, half + 1 - perp) * juce::jlimit(0.0f, 1.0f, along - inner + 1) * juce::jlimit(0.0f, 1.0f, outer + 1 - along);
            const float al = float(sp->getAlpha());
            if (al < 1) continue;
            const float lo = float(std::min({sp->getRed(), sp->getGreen(), sp->getBlue()})) / al;
            const float t = geo * juce::jlimit(0.0f, 1.0f, (lo - 0.32f) / 0.3f);
            if (t <= 0) continue;
            auto mixc = [&](uint8_t v, float k) { return uint8_t(juce::jlimit(0.0f, al, float(v) * (1 - t + t * k * 1.1f)) + 0.5f); };
            dp->setARGB(sp->getAlpha(), mixc(sp->getRed(), col[0]), mixc(sp->getGreen(), col[1]), mixc(sp->getBlue(), col[2]));
        }
    return p.image;
}

void LacquerEditor::drawMeter(juce::Graphics& g, int ci) {
    const auto& c = kControls[ci];
    const float x0 = c.x - c.w / 2 + kMeterInset, y0 = cy(c) - c.h / 2 + kMeterInset;
    const double pos = needle_[ci == meterL_ ? 0 : 1].pos;
    const double a = juce::degreesToRadians(-double(kMeterSpan) + 2.0 * double(kMeterSpan) * pos);
    const float sx = std::sin(float(a)), cyy = -std::cos(float(a));
    const juce::Point<float> pivot(x0 + kMeterPivotX, y0 + kMeterPivotY);
    const juce::Point<float> tip = pivot + juce::Point<float>(sx, cyy) * (kMeterRadius + 6.5f), tail = pivot + juce::Point<float>(sx, cyy) * (kMeterRadius * 0.18f);
    {
        juce::Graphics::ScopedSaveState ss(g);
        g.reduceClipRegion((juce::Rectangle<float>(x0, y0, kMeterFaceW, kMeterFaceH) * s_).getSmallestIntegerContainer());
        g.setColour(juce::Colours::black.withAlpha(0.16f));
        const juce::Point<float> off(1.3f, 1.8f);
        g.drawLine(juce::Line<float>((tail + off) * s_, (tip + off) * s_), 1.8f * s_);
        g.setColour(juce::Colour(0xff17140f));
        g.drawLine(juce::Line<float>(tail * s_, tip * s_), 0.85f * s_);
        g.setColour(juce::Colours::black.withAlpha(0.10f));
        g.fillRect(juce::Rectangle<float>(x0, y0 + kMeterFaceH - 3, kMeterFaceW, 3) * s_);
    }
    drawSprite(g, art_->glass, c.x, cy(c), c.w + 4, c.h + 4);
}

// A readout's drum wheels: one character a wheel.
void LacquerEditor::drawReadout(juce::Graphics& g, const float* r, const juce::String& text) {
    const float w = r[2] - 6, h = r[3] - 6, x0 = r[0] - w / 2, y0 = r[1] - h / 2;
    const int n = 4;
    const float ww = (w - 1.0f) / n - 1.0f;
    juce::Graphics::ScopedSaveState ss(g);
    g.reduceClipRegion((juce::Rectangle<float>(x0, y0, w, h) * s_).getSmallestIntegerContainer());
    g.setFont(font(19.0f));
    juce::String t = text.replace(kMinus, "-");
    while (t.length() < n) t = " " + t;
    t = t.substring(t.length() - n);
    for (int k = 0; k < n; ++k) {
        const auto rr = juce::Rectangle<float>(x0 + 1.0f + float(k) * (ww + 1.0f), y0, ww, h) * s_;
        g.setColour(juce::Colour(0xff161513));
        g.fillRect(rr);
        g.setColour(juce::Colour(0xffece6d4));
        g.drawText(juce::String::charToString(t[k]), rr, juce::Justification::centred, false);
        g.setGradientFill(juce::ColourGradient(juce::Colours::black.withAlpha(0.75f), rr.getX(), rr.getY(), juce::Colours::transparentBlack, rr.getX(), rr.getY() + rr.getHeight() * 0.36f, false));
        g.fillRect(rr);
        g.setGradientFill(juce::ColourGradient(juce::Colours::black.withAlpha(0.75f), rr.getX(), rr.getBottom(), juce::Colours::transparentBlack, rr.getX(), rr.getBottom() - rr.getHeight() * 0.36f, false));
        g.fillRect(rr);
    }
}

void LacquerEditor::drawTapes(juce::Graphics& g) {
    if (!art_->marker) return;
    auto write = [&](float cx, float cyy, float angle, float maxW, float hPt, const juce::String& text) {
        juce::Graphics::ScopedSaveState ss(g);
        g.addTransform(juce::AffineTransform::rotation(juce::degreesToRadians(angle), cx * s_, cyy * s_));
        const auto r = juce::Rectangle<float>((cx - maxW / 2) * s_, (cyy - hPt * 0.5f) * s_, maxW * s_, hPt * s_);
        g.setFont(juce::Font(juce::FontOptions(art_->marker).withHeight(hPt * 0.74f * s_)));
        g.setColour(juce::Colour(0x1a141a2a));
        g.drawText(text, r.translated(0.5f * s_, 0.6f * s_), juce::Justification::centred, false);
        g.setColour(juce::Colour(0xe0181a26));
        g.drawFittedText(text, r.toNearestInt(), juce::Justification::centred, 1, 0.62f);
    };
    write(kPresetTapeX, kPresetTapeY, kPresetTapeAngle, kPresetTapeW - 30, kPresetTapeH, proc_.presetName);
    for (int k = 0; k < kNumSections; ++k) write(kSectKeyX[k], kSectTapeY + 0.5f, kSectTapeAngle, 34, kSectTapeH + 2, tapeLabel(proc_.sectionName(k + 1)));
}

// The strip under the faceplate: what the control under the pointer does, in plain words.
void LacquerEditor::drawHelp(juce::Graphics& g) {
    if (!helpOn_) return;
    const auto strip = juce::Rectangle<float>(0, H + trayH(), W, kHelpH) * s_;
    g.setColour(juce::Colour(0xff1c1d1f));
    g.fillRect(strip);
    juce::String name = "point at a control", range,
                 text = "Hover over anything and this strip says what it does. Press LISTEN and play the song through once; the assistant's moves appear on MOVES. "
                        "The SECTIONS keys edit one part of the song. The quarter-turn latch at the bottom left lets out the hidden SPACE tray.";
    juce::String id = hoverId_;
    const int ci = indexOf(id.toRawUTF8());
    if (const int k = sectionKeyOf(id.toRawUTF8())) {
        const auto nm = proc_.sectionName(k);
        name = "section " + juce::String(k) + kDot + nm;
        range = "section key. click it; right-click to rename.";
        text = "Edit the " + nm + " on its own: knobs you turn while this key is lit change only the " + nm +
               ", and the rest follow the song. With FOLLOW lit the key for the section you're in lights by itself as the song plays; pressing it while the song plays holds it. "
               "With MARK lit, tapping it marks where a " + nm + " starts.";
    } else {
        if (id.endsWith("_idea")) id = "idea";
        else if (id.endsWith("_in") && id != "sp_in") id = "in_";
        else if (id == "ab_a" || id == "ab_b") id = "compare";
        else if (id == "preset_prev" || id == "preset_next") id = "preset";
        if (const auto* e = helpFor(id)) {
            name = juce::String::fromUTF8(e->name);
            range = juce::String::fromUTF8(e->range);
            text = juce::String::fromUTF8(e->text);
            if (ci >= 0 && paramOf(ci) >= 0) {
                const int p = paramOf(ci);
                auto* prm = proc_.param(p);
                range << ". now " << prm->getText(prm->convertTo0to1(value(ci)), 32).toLowerCase();
                if (proc_.litKey() > 0 && lq::def(p).perSection) range << (proc_.shownIsOwn(p) ? " (the " + proc_.sectionName(proc_.litKey()) + "'s own; double-click to follow the song)" : " (as the song)");
            } else if (range.isNotEmpty()) range << ".";
        }
        if (id == "crt_bal") text << " With TARGET on GENRE, click the target's name to step through the styles; drop audio files on the screen to use them as references.";
    }
    const float pad = 24 * s_;
    g.setColour(juce::Colour(0xff6f6a5c));
    g.setFont(font(11.0f));
    g.drawText("HIDE", juce::Rectangle<float>(strip.getRight() - 54 * s_, strip.getY() + 4 * s_, 46 * s_, 14 * s_), juce::Justification::centredRight, false);
    g.setColour(juce::Colour(0xfff1ead2));
    g.setFont(font(17.0f));
    g.drawText(name.toUpperCase(), juce::Rectangle<float>(pad, strip.getY() + 12 * s_, 190 * s_, 20 * s_), juce::Justification::centredLeft, false);
    g.setColour(juce::Colour(0xffa39e8b));
    g.setFont(font(12.5f));
    g.drawFittedText(range, juce::Rectangle<float>(pad, strip.getY() + 34 * s_, 190 * s_, 40 * s_).toNearestInt(), juce::Justification::topLeft, 3);
    g.setColour(juce::Colour(0xffd8d2bd));
    g.setFont(font(15.0f));
    g.drawFittedText(text, juce::Rectangle<float>(pad + 206 * s_, strip.getY() + 8 * s_, W * s_ - 2 * pad - 206 * s_, 66 * s_).toNearestInt(), juce::Justification::centredLeft, 3);
}

// ---------------------------------------------------------------------------
// Time
void LacquerEditor::tick(int frames) {
    const double dt = 1.0 / kFrameRate;
    for (int fr = 0; fr < frames; ++fr) {
        clock_ += dt;
        const auto& m = proc_.meters();
        double target[2];
        target[0] = juce::jlimit(-0.02, 1.02, (double(m.stLufs.load()) + 24.0) / 18.0);
        target[1] = juce::jlimit(-0.02, 1.02, (double(m.plr.load()) - 3.0) / 15.0);
        if (m.stLufs.load() < -60) target[1] = 0;
        if (m.over.load()) overUntil_ = clock_ + 1.5;
        for (int k = 0; k < 2; ++k) {
            const double w = 18.0, z = 0.8;   // a damped needle: quick enough to follow, with a touch of overshoot
            for (int sub = 0; sub < 4; ++sub) {
                const double h = dt / 4;
                needle_[k].vel += h * (w * w * (target[k] - needle_[k].pos) - 2 * z * w * needle_[k].vel);
                needle_[k].pos += h * needle_[k].vel;
            }
        }
    }
    if (proc_.lastNote.isNotEmpty()) { note_ = proc_.lastNote; proc_.lastNote.clear(); noteUntil_ = clock_ + 6.0; }
    {   // the DETAIL page's gain-reduction traces
        auto& m = proc_.meters();
        glueHist_[histPos_] = m.glueGrMax.exchange(0.0f);
        outPkHold_ = std::max(double(m.truePeak.load()), outPkHold_ - 0.35);
        limHist_[histPos_] = m.limGrMax.exchange(0.0f);
        histPos_ = (histPos_ + 1) % 240;
    }
}

void LacquerEditor::timerCallback() {
    double before[2] = {needle_[0].pos, needle_[1].pos};
    tick(1);
    if (meterL_ >= 0 && std::abs(needle_[0].pos - before[0]) > 2e-4) repaintPts(boundsOf(meterL_));
    if (meterP_ >= 0 && std::abs(needle_[1].pos - before[1]) > 2e-4) repaintPts(boundsOf(meterP_));
    if (proc_.revision.load() != seenRevision_) { repaint(); return; }
    // The screen moves while the song plays (sections, listening), and the readouts with it.
    if (proc_.playing() || proc_.listening() || proc_.page == 5 || clock_ < noteUntil_ + 0.1) repaintPts({kFaceX - 2, kFaceY - 2, kFaceW + 4, kFaceH + 4});
    if (bubble_ >= 0 && clock_ > bubbleUntil_) { repaintPts(boundsOf(bubble_).expanded(40, 46)); bubble_ = -1; }
    repaintPts({kReadoutLufs[0] - kReadoutLufs[2] / 2, kReadoutLufs[1] - kReadoutLufs[3] / 2, kReadoutLufs[2], kReadoutLufs[3]});
    repaintPts({kReadoutTp[0] - kReadoutTp[2] / 2, kReadoutTp[1] - kReadoutTp[3] / 2, kReadoutTp[2], kReadoutTp[3]});
    bool keysChanged = false;
    for (int i = 0; i < kNumControls; ++i) {
        const auto& c = kControls[i];
        if (c.type == layout::Screen || c.type == Meter || (c.tray && !proc_.trayOpen)) continue;
        const float v = paramOf(i) >= 0 ? value(i) : (lit(i) ? 1.0f : 0.0f);
        if (std::abs(v - drawn_[size_t(i)]) > 1e-6f) {
            repaintPts(boundsOf(i));
            if (startsWith(c.id, "sec_")) keysChanged = true;
        }
    }
    if (keysChanged) repaint();   // a new lit key: every knob may show another value
}

// ---------------------------------------------------------------------------
// Mouse and files
void LacquerEditor::mouseMove(const juce::MouseEvent& e) {
    const auto pt = e.position / s_;
    const int h = hit(pt);
    juce::String id;
    if (h == kScreen) id = "crt_" + juce::String(juce::StringArray{"bal", "loud", "moves", "checks", "sect"}[juce::jlimit(0, 4, proc_.page)]);
    else if (h == kPresetTape) id = "preset";
    else if (h == kSectTape) id = "sect_tape";
    else if (h == kLatch) id = "latch";
    else if (h == kReadLufs) id = "r_lufs";
    else if (h == kReadTp) id = "r_tp";
    else if (h >= 0) id = kControls[h].id;
    else if (proc_.trayOpen && pt.y > H && pt.x > 1000) id = "tray_table";
    else if (proc_.trayOpen && pt.y > H && pt.x < 150 && pt.y < H + 90) id = "tray_plate";
    if (id != hoverId_) { hoverId_ = id; repaintPts({0, H + trayH(), W, helpH()}); }
    bool pointing = h == kLatch || h == kPresetTape || h == kSectTape || (h >= 0 && h < kNumControls && kControls[h].type == Button);
    if (h == kScreen) {
        bool edge = false;
        const bool onBlock = proc_.page == 4 && blockAt(pt, edge) >= 0;
        if (onBlock && edge) { setMouseCursor(juce::MouseCursor::LeftRightResizeCursor); return; }
        pointing = (proc_.page == 2 && moveRowAt(pt) >= 0) || onBlock || (proc_.page == 4 && softKeyAt(pt) >= 0) || (proc_.page == 0 && pt.y < sc(0, 28).y);
    }
    setMouseCursor(pointing ? juce::MouseCursor::PointingHandCursor : juce::MouseCursor::NormalCursor);
}

void LacquerEditor::mouseExit(const juce::MouseEvent&) {
    if (hoverId_.isNotEmpty()) { hoverId_.clear(); repaintPts({0, H + trayH(), W, helpH()}); }
}

void LacquerEditor::mouseDown(const juce::MouseEvent& e) {
    const auto pt = e.position / s_;
    const int h = hit(pt);
    drag_ = kNone; dragMark_ = -1; dragMoved_ = false; dragParam_ = -1;
    if (h == kScreen) { screenClick(pt, e); return; }
    if (helpOn_ && pt.y > H + trayH() && pt.y < H + trayH() + 20 && pt.x > W - 60) { proc_.setShowHelpStrip(false); setScale(s_); return; }
    if (h == kLatch) { setTray(!proc_.trayOpen); return; }
    if (h == kPresetTape) { showPresetMenu(); return; }
    if (h == kSectTape) {
        int best = 0;
        for (int k = 1; k < kNumSections; ++k) if (std::abs(pt.x - kSectKeyX[k]) < std::abs(pt.x - kSectKeyX[best])) best = k;
        showNameMenu(best + 1);
        return;
    }
    if (h == kReadLufs) { proc_.resetIntegrated(); return; }
    if (h < 0 || h >= kNumControls) return;
    const auto& c = kControls[h];
    if (c.type == Knob && c.kind != kSel && !e.mods.isPopupMenu()) {
        const int p = paramOf(h);
        if (p >= 0) {
            drag_ = h; dragParam_ = p; dragStartNorm_ = norm(h); dragAccum_ = 0; dragLast_ = e.position;
            followDetail(p);
            bubble_ = h; bubbleUntil_ = clock_ + 1.2;
            repaintPts(boundsOf(h).expanded(40, 46));
            beginEdit(p);
        }
        return;
    }
    if (c.type == Button) { pressed_ = h; repaintPts(boundsOf(h)); }
    click(h, e);
}

void LacquerEditor::mouseDrag(const juce::MouseEvent& e) {
    const auto pt = e.position / s_;
    if (drag_ == kScreen && dragBand_ >= 0) { detailDrag(pt, e); return; }
    if (drag_ == kScreen && dragMark_ >= 0) {
        proc_.moveMark(dragMark_, std::max(0.0, ppqAtX(pt.x)));
        // The mark keeps its place in the list unless it passes a neighbour; find it again by position.
        const auto& m = proc_.songMap();
        const double q = proc_.barLength(), at = std::round(std::max(0.0, ppqAtX(pt.x)) / q) * q;
        for (int i = 0; i < m.count; ++i) if (std::abs(m.marks[i].ppq - at) < 1e-6) dragMark_ = i;
        dragMoved_ = true;
        repaint();
        return;
    }
    if (drag_ >= 0 && drag_ < kNumControls && dragParam_ >= 0) {
        // Up or right turns it up; Shift for fine.
        const float d = ((e.position.x - dragLast_.x) - (e.position.y - dragLast_.y)) / (220.0f * s_);
        dragLast_ = e.position;
        dragAccum_ += d * (e.mods.isShiftDown() ? 0.15f : 1.0f);
        proc_.setShown(dragParam_, lq::fromNorm(dragParam_, juce::jlimit(0.0f, 1.0f, dragStartNorm_ + dragAccum_)));
        dragMoved_ = true;
        bubble_ = drag_; bubbleUntil_ = clock_ + 1.2;
        repaintPts(boundsOf(drag_).expanded(40, 46));
        repaintPts({0, H + trayH(), W, helpH()});
    }
}

void LacquerEditor::mouseUp(const juce::MouseEvent&) {
    if (drag_ >= 0 && drag_ < kNumControls) endEdit(dragParam_);
    if (dragBand_ >= 0) { endEdit(kBandDb[dragBand_]); endEdit(kBandHz[dragBand_]); dragBand_ = -1; }
    drag_ = kNone; dragMark_ = -1; dragParam_ = -1;
    if (pressed_ >= 0) { const int was = pressed_; pressed_ = kNone; repaintPts(boundsOf(was)); }
    if (softDown_ >= 0) { softDown_ = -1; repaintPts({kFaceX, kFaceY, kFaceW, kFaceH}); }
}

void LacquerEditor::mouseDoubleClick(const juce::MouseEvent& e) {
    const auto pt = e.position / s_;
    const int h = hit(pt);
    if (h == kScreen && proc_.page == 5 && proc_.detailTab == 0) {
        const int b = toneBand_;
        beginEdit(kBandDb[b]); proc_.resetShown(kBandDb[b]); endEdit(kBandDb[b]);
        repaint();
        return;
    }
    if (h == kScreen && proc_.page == 4) {
        bool edge = false;
        const int b = blockAt(pt, edge);
        if (b >= 0 && !dragMoved_) proc_.removeMark(b);
        return;
    }
    if (h >= 0 && h < kNumControls && kControls[h].type == Knob && kControls[h].kind != kSel && paramOf(h) >= 0) {
        const int p = paramOf(h);
        beginEdit(p);
        proc_.resetShown(p);
        endEdit(p);
        repaint();
    }
}

void LacquerEditor::mouseWheelMove(const juce::MouseEvent& e, const juce::MouseWheelDetails& wheel) {
    const auto pt = e.position / s_;
    const int h = hit(pt);
    const float d = wheel.deltaY * (wheel.isReversed ? -1.0f : 1.0f);
    if (h == kScreen && proc_.page == 2) {
        const int n = int(proc_.result().moves.size());
        moveScroll_ = juce::jlimit(0, std::max(0, n - movesShown()), moveScroll_ + (d < 0 ? 1 : -1));
        repaintPts({kFaceX, kFaceY, kFaceW, kFaceH});
        return;
    }
    if (h >= 0 && h < kNumControls && kControls[h].type == Knob && kControls[h].kind != kSel && paramOf(h) >= 0) {
        const int p = paramOf(h);
        proc_.setShown(p, lq::fromNorm(p, juce::jlimit(0.0f, 1.0f, norm(h) + d * (e.mods.isShiftDown() ? 0.03f : 0.2f))));
        followDetail(p);
        bubble_ = h; bubbleUntil_ = clock_ + 1.2;
        repaintPts(boundsOf(h).expanded(40, 46));
    }
}

bool LacquerEditor::isInterestedInFileDrag(const juce::StringArray& files) {
    for (const auto& f : files)
        if (juce::File(f).hasFileExtension("wav;aif;aiff;flac;mp3;m4a;caf;ogg")) { dropHover_ = true; repaintPts({kFaceX, kFaceY, kFaceW, kFaceH}); return true; }
    return false;
}

void LacquerEditor::filesDropped(const juce::StringArray& files, int, int) {
    dropHover_ = false;
    juce::StringArray audio;
    for (const auto& f : files) if (juce::File(f).hasFileExtension("wav;aif;aiff;flac;mp3;m4a;caf;ogg")) audio.add(f);
    if (audio.isEmpty()) return;
    proc_.addReferences(audio);
    proc_.setSong(lq::aTarget, 1.0f);
    proc_.page = 0;
    repaint();
}
