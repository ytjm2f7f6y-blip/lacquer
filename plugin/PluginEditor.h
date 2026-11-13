#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_gui_basics/juce_gui_basics.h>

#include "Layout.h"
#include "PluginProcessor.h"

#include <memory>
#include <vector>

// The rendered art, loaded once per process and shared by every open window.
struct LacquerArt {
    LacquerArt();
    juce::Image panel, tray;
    juce::Image strip[4];          // large, med, (small: unused), selector
    int stripFrames[4] = {0, 0, 0, 0};
    juce::Image toggleUp, toggleDown, glass;
    juce::Image lit[layout::kNumLitGroups];
    juce::Typeface::Ptr marker, legend;
};

class LacquerEditor final : public juce::AudioProcessorEditor, public juce::FileDragAndDropTarget, private juce::Timer {
public:
    explicit LacquerEditor(LacquerProcessor&);
    ~LacquerEditor() override { stopTimer(); }

    void paint(juce::Graphics&) override;
    void resized() override {}
    void mouseMove(const juce::MouseEvent&) override;
    void mouseExit(const juce::MouseEvent&) override;
    void mouseDown(const juce::MouseEvent&) override;
    void mouseDrag(const juce::MouseEvent&) override;
    void mouseUp(const juce::MouseEvent&) override;
    void mouseDoubleClick(const juce::MouseEvent&) override;
    void mouseWheelMove(const juce::MouseEvent&, const juce::MouseWheelDetails&) override;
    bool isInterestedInFileDrag(const juce::StringArray& files) override;
    void filesDropped(const juce::StringArray& files, int x, int y) override;

    void setScale(float s);
    void setTray(bool open);
    // For the snapshot tool: run the meters on as if `frames` frames had passed, and show the help for an id.
    void tick(int frames = 1);
    void showHelp(const char* id) { hoverId_ = id; }
    void pickBlock(int i) { selMark_ = i; }
    void showBubble(int controlIndex) { bubble_ = controlIndex; bubbleUntil_ = clock_ + 60.0; }

    static constexpr float kHelpH = 80.0f;
    float helpH() const { return helpOn_ ? kHelpH : 0.0f; }

private:
    struct Needle { double pos = 0, vel = 0; };
    enum { kNone = -1, kScreen = 2000, kPresetTape = 2001, kSectTape = 2002, kLatch = 2003, kReadLufs = 2004, kReadTp = 2005 };

    void timerCallback() override;
    float trayH() const { return proc_.trayOpen ? layout::kTrayH : 0.0f; }
    float cy(const layout::Control& c) const { return c.tray ? c.y + layout::H : c.y; }

    // ---- controls
    int paramOf(int ci) const { return params_[size_t(ci)]; }
    float value(int ci) const;
    float norm(int ci) const;
    int selIndex(int ci) const;
    bool lit(int ci) const;
    int hit(juce::Point<float> pt) const;
    juce::Rectangle<float> boundsOf(int ci) const;
    void repaintPts(juce::Rectangle<float> r);
    void click(int ci, const juce::MouseEvent&);
    void beginEdit(int p);
    void endEdit(int p);
    void showSetMenu();
    void showPresetMenu();
    void renameSections();
    void stepPreset(int dir);

    // ---- the screen
    juce::Point<float> sc(float x, float y) const;   // screen coordinates (the mockup's) to points
    void drawScreen(juce::Graphics&);
    void drawBalance(juce::Graphics&);
    void drawLoudness(juce::Graphics&);
    void drawMoves(juce::Graphics&);
    void drawChecks(juce::Graphics&);
    void drawSections(juce::Graphics&);
    void drawDetail(juce::Graphics&);
    void drawLane(juce::Graphics&);
    bool detailClick(juce::Point<float> pt, const juce::MouseEvent&);
    void detailDrag(juce::Point<float> pt, const juce::MouseEvent&);
    int balanceChipAt(juce::Point<float> pt) const;
    void chooseReferences();
    void askStyleName();
    void stepChoice(int p, int dir);
    void followDetail(int p);
    void drawBubble(juce::Graphics&);
    void header(juce::Graphics&, const juce::String& l1, const juce::String& l2);
    void screenClick(juce::Point<float> pt, const juce::MouseEvent&);
    int moveRowAt(juce::Point<float> pt) const;
    int movesShown() const { return proc_.result().moves.size() > 6 ? 5 : 6; }   // room for the scroll note
    int blockAt(juce::Point<float> pt, bool& nearEdge) const;
    int softKeyAt(juce::Point<float> pt) const;
    void showKeyMenu(int markIndex);
    void showNameMenu(int key);
    void addNameItems(juce::PopupMenu& menu, int key);
    float xOfPpq(double ppq) const;
    double ppqAtX(float x) const;
    juce::String keyLabel(int key) const;

    // ---- drawing
    void drawKnob(juce::Graphics&, int ci);
    void drawMeter(juce::Graphics&, int ci);
    void drawLit(juce::Graphics&, int ci);
    void drawReadout(juce::Graphics&, const float* r, const juce::String& text);
    void drawTapes(juce::Graphics&);
    void drawHelp(juce::Graphics&);
    void drawSprite(juce::Graphics&, const juce::Image&, float cx, float cy, float wPt, float hPt, int frame = 0, int frames = 1);
    juce::Font font(float h) const;
    const juce::Image& paintedFrame(int ci, int frame);

    LacquerProcessor& proc_;
    juce::SharedResourcePointer<LacquerArt> art_;
    float s_ = 1.0f;

    struct Painted { int frame = -1; juce::Image image; };
    std::vector<Painted> painted_;
    std::vector<int> params_;
    std::vector<float> drawn_;
    int meterL_ = -1, meterP_ = -1, overLamp_ = -1;
    Needle needle_[2];
    double clock_ = 0, overUntil_ = 0, noteUntil_ = 0;
    juce::String note_;
    int seenRevision_ = -1, moveScroll_ = 0;

    int drag_ = kNone, dragMark_ = -1, pressed_ = kNone, dragParam_ = -1;
    float dragStartNorm_ = 0, dragAccum_ = 0;
    juce::Point<float> dragLast_;
    bool dragMoved_ = false, dropHover_ = false;
    int selMark_ = -1, softDown_ = -1;   // the SECTIONS screen: the picked block, the soft key held down
    int toneBand_ = 1, dragBand_ = -1;   // the DETAIL page's EQ: the picked band, the one being dragged
    float glueHist_[240]{}, limHist_[240]{};
    int histPos_ = 0;
    double outPkHold_ = -120;
    int bubble_ = -1;                    // a knob's value shown beside it while it turns
    double bubbleUntil_ = 0;
    bool helpOn_ = true;
    std::unique_ptr<juce::FileChooser> chooser_;
    juce::String hoverId_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(LacquerEditor)
};
