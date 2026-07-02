#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include "PluginProcessor.h"

// Native reproduction of the MOD modgui (icon-megalo.html + stylesheet).
// Everything is drawn with paths/gradients — no bitmaps — so it stays crisp
// at any resolution / zoom. Layout mirrors the CSS pixel coordinates on a
// fixed 720x380 panel.

namespace megalo {
    const juce::Colour kPanel   { 0xff25282c };
    const juce::Colour kOrange  { 0xfff0801a };
    const juce::Colour kDialHi  { 0xff4a4f55 };
    const juce::Colour kDialLo  { 0xff2a2d31 };
    const juce::Colour kSwitchBg{ 0xff1d2024 };
    const juce::Colour kWhite   { 0xffffffff };

    juce::Font font (float height, bool bold = false);
}

// ── Rotary knob look ─────────────────────────────────────────────────────────
class MegaloLNF : public juce::LookAndFeel_V4
{
public:
    void drawRotarySlider(juce::Graphics&, int x, int y, int w, int h,
                          float pos, float startAngle, float endAngle,
                          juce::Slider&) override;
};

// ── A rotary knob + caption below (modgui .megalo-knob) ──────────────────────
class KnobControl : public juce::Component
{
public:
    KnobControl(juce::AudioProcessorValueTreeState&, const juce::String& paramID,
                const juce::String& caption, juce::LookAndFeel*);
    ~KnobControl() override;
    void resized() override;
    void paint(juce::Graphics&) override;
private:
    juce::Slider slider;
    juce::String caption;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> attachment;
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(KnobControl)
};

// ── Round LED enable toggle (modgui .megalo-led) ─────────────────────────────
class LedToggle : public juce::Button
{
public:
    LedToggle(juce::AudioProcessorValueTreeState&, const juce::String& paramID);
    void paintButton(juce::Graphics&, bool, bool) override;
private:
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> attachment;
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(LedToggle)
};

// ── 3-position LP/HP/BP segmented switch (modgui .megalo-switch3) ─────────────
class FilterTypeSwitch : public juce::Component
{
public:
    FilterTypeSwitch(juce::AudioProcessorValueTreeState&, const juce::String& paramID);
    void paint(juce::Graphics&) override;
    void mouseDown(const juce::MouseEvent&) override;
private:
    int index = 0;
    std::unique_ptr<juce::ParameterAttachment> attachment;
    juce::Rectangle<int> segs[3];
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(FilterTypeSwitch)
};

// ── 2-position Granular / Phase-Vocoder pitch-engine switch ──────────────────
class PitchEngineSwitch : public juce::Component
{
public:
    PitchEngineSwitch(juce::AudioProcessorValueTreeState&, const juce::String& paramID);
    void paint(juce::Graphics&) override;
    void mouseDown(const juce::MouseEvent&) override;
private:
    int index = 0;
    std::unique_ptr<juce::ParameterAttachment> attachment;
    juce::Rectangle<int> segs[2];
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(PitchEngineSwitch)
};

// ── Top time-marker handle: vertical line that slides horizontally ───────────
class TimeHandle : public juce::Component
{
public:
    TimeHandle(juce::AudioProcessorValueTreeState&, const juce::String& paramID,
               const juce::String& label, juce::Colour knobTint);
    void paint(juce::Graphics&) override;
    void mouseDown(const juce::MouseEvent&) override;
    void mouseDrag(const juce::MouseEvent&) override;
    void mouseUp(const juce::MouseEvent&) override;
private:
    void setFromX(int x);
    juce::RangedAudioParameter* param = nullptr;
    std::unique_ptr<juce::ParameterAttachment> attachment;
    juce::String label;
    juce::Colour tint;
    float norm = 0.5f;   // 0..1 position
    float value = 0.0f;  // denormalised, for the readout
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(TimeHandle)
};

// ── ADSR envelope editor: breakpoints dragged ON the curve itself ────────────
// A/D/R drag horizontally (they are TIMES — the old per-column vertical drag
// moved the dot sideways, which was the main source of confusion), S drags
// vertically. Times sit on a fixed per-segment axis scaled by the parameter's
// own (log-skewed) 0..1, so moving one handle never rescales the others.
class EnvelopeEditor : public juce::Component
{
public:
    explicit EnvelopeEditor(juce::AudioProcessorValueTreeState&);
    void paint(juce::Graphics&) override;
    void mouseDown(const juce::MouseEvent&) override;
    void mouseDrag(const juce::MouseEvent&) override;
    void mouseUp(const juce::MouseEvent&) override;
    void mouseMove(const juce::MouseEvent&) override;
    void mouseExit(const juce::MouseEvent&) override;

private:
    struct Handle {
        juce::RangedAudioParameter* param = nullptr;
        std::unique_ptr<juce::ParameterAttachment> attachment;
        float value = 0.0f;   // denormalised, for the readout
        float norm  = 0.0f;   // 0..1 through the parameter's (skewed) range
    };
    enum Target { None = -1, A = 0, D = 1, S = 2, R = 3 };

    void bind(Handle&, juce::AudioProcessorValueTreeState&, const char* id);

    // Fixed time axis: one segment per A/D/R phase plus the sustain hold.
    float segW()   const { return (float) getWidth() * 0.27f; }
    float holdW()  const { return (float) getWidth() * 0.19f; }
    float curveH() const;
    float yOf(float lvl) const;
    float xA() const { return segW() * hA.norm; }
    float xD() const { return xA() + segW() * hD.norm; }
    float xH() const { return xD() + holdW(); }
    float xR() const { return xH() + segW() * hR.norm; }
    juce::Point<float> dotPos(Target) const;
    Target hitTarget(juce::Point<float>) const;
    void applyDrag(const juce::MouseEvent&);
    Handle& handleFor(Target t) { return t == A ? hA : t == D ? hD : t == S ? hS : hR; }

    Handle hA, hD, hS, hR;
    Target drag  = None;
    Target hover = None;
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(EnvelopeEditor)
};

// ── Onset-threshold line: horizontal line dragged vertically, flashes on onset
class ThresholdLine : public juce::Component
{
public:
    ThresholdLine(juce::AudioProcessorValueTreeState&, const juce::String& paramID);
    void paint(juce::Graphics&) override;
    bool hitTest(int x, int y) override;   // only grab clicks near the line
    void mouseDown(const juce::MouseEvent&) override;
    void mouseDrag(const juce::MouseEvent&) override;
    void mouseUp(const juce::MouseEvent&) override;
    float getNorm() const { return norm; }
    void setFlash(float f) { flash = f; repaint(); }
private:
    void setFromY(int y);
    juce::RangedAudioParameter* param = nullptr;
    std::unique_ptr<juce::ParameterAttachment> attachment;
    float norm = 0.15f;
    float flash = 0.0f;  // 0..1 onset flash level
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(ThresholdLine)
};

// ── The central orange window: bg + waveform + midline, and it owns the
//    in-window handles (sample/skip/grain/xfade, the ADSR editor, threshold).
class WindowPanel : public juce::Component
{
public:
    explicit WindowPanel(juce::AudioProcessorValueTreeState&);
    void paint(juce::Graphics&) override;
    void resized() override;
    ThresholdLine threshold;
private:
    juce::OwnedArray<TimeHandle> topHandles;
    EnvelopeEditor envEditor;   // A, D, S, R on the curve (bottom half)
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(WindowPanel)
};

// ── The editor ───────────────────────────────────────────────────────────────
class MegaloEditor : public juce::AudioProcessorEditor, private juce::Timer
{
public:
    explicit MegaloEditor(MegaloAudioProcessor&);
    ~MegaloEditor() override;
    void paint(juce::Graphics&) override;
    void resized() override;
private:
    void timerCallback() override;

    MegaloAudioProcessor& proc;
    MegaloLNF lnf;

    WindowPanel window;

    // Bottom control row.
    juce::OwnedArray<KnobControl> knobs;
    juce::OwnedArray<LedToggle>   leds;
    FilterTypeSwitch filterType;
    PitchEngineSwitch pitchEngine;

    float flashLevel = 0.0f;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(MegaloEditor)
};
