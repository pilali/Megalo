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

// ── Bottom ADSR handle: dot that slides vertically ───────────────────────────
class EnvHandle : public juce::Component
{
public:
    EnvHandle(juce::AudioProcessorValueTreeState&, const juce::String& paramID,
              const juce::String& label);
    void paint(juce::Graphics&) override;
    void mouseDown(const juce::MouseEvent&) override;
    void mouseDrag(const juce::MouseEvent&) override;
    void mouseUp(const juce::MouseEvent&) override;
    float getNorm() const { return norm; }
private:
    void setFromY(int y);
    juce::RangedAudioParameter* param = nullptr;
    std::unique_ptr<juce::ParameterAttachment> attachment;
    juce::String label;
    float norm = 0.5f;
    float value = 0.0f;
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(EnvHandle)
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

// ── The central orange window: bg + waveform + midline + ADSR curve, and it
//    owns the in-window handles (sample/skip/grain/xfade, A/D/S/R, threshold).
class WindowPanel : public juce::Component
{
public:
    explicit WindowPanel(juce::AudioProcessorValueTreeState&);
    void paint(juce::Graphics&) override;
    void resized() override;
    ThresholdLine threshold;
private:
    juce::AudioProcessorValueTreeState& apvts;
    juce::OwnedArray<TimeHandle> topHandles;
    juce::OwnedArray<EnvHandle>  envHandles;  // A, D, S, R
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

    float flashLevel = 0.0f;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(MegaloEditor)
};
