#include "PluginEditor.h"
#include <cmath>
#include <cstdlib>

using APVTS = juce::AudioProcessorValueTreeState;

juce::Font megalo::font(float height, bool bold)
{
    return juce::Font(juce::FontOptions()
                          .withHeight(height)
                          .withStyle(bold ? "Bold" : "Regular"));
}

namespace {
// Format a handle readout the way the modgui does.
juce::String fmtValue(const juce::String& label, float v)
{
    if (label == "S")                        // sustain 0..1
        return juce::String(v, 2);
    return juce::String(juce::roundToInt(v)) + " ms";
}
} // namespace

// ════════════════════════════════════════════════════════════════════════════
// MegaloLNF — rotary knob: dark radial dial + orange pointer.
// ════════════════════════════════════════════════════════════════════════════
void MegaloLNF::drawRotarySlider(juce::Graphics& g, int x, int y, int w, int h,
                                 float pos, float startAngle, float endAngle,
                                 juce::Slider&)
{
    auto bounds = juce::Rectangle<float>((float) x, (float) y, (float) w, (float) h).reduced(2.0f);
    const float radius = juce::jmin(bounds.getWidth(), bounds.getHeight()) * 0.5f;
    const float cx = bounds.getCentreX();
    const float cy = bounds.getCentreY();
    auto dial = juce::Rectangle<float>(cx - radius, cy - radius, radius * 2.0f, radius * 2.0f);

    // Dial body — radial-ish gradient (highlight top-left → dark bottom-right).
    juce::ColourGradient grad(megalo::kDialHi, cx - radius * 0.4f, cy - radius * 0.4f,
                              megalo::kDialLo, cx + radius, cy + radius, true);
    g.setGradientFill(grad);
    g.fillEllipse(dial);

    // Bevel shadow + subtle rim.
    g.setColour(juce::Colours::black.withAlpha(0.4f));
    g.drawEllipse(dial.reduced(0.5f), 1.0f);
    g.setColour(megalo::kWhite.withAlpha(0.08f));
    g.drawEllipse(dial.reduced(1.5f), 1.0f);

    // Orange pointer line (0 = 12 o'clock, increasing clockwise).
    const float angle = startAngle + pos * (endAngle - startAngle);
    const juce::Point<float> base(cx + radius * 0.30f * std::sin(angle),
                                  cy - radius * 0.30f * std::cos(angle));
    const juce::Point<float> tip (cx + radius * 0.86f * std::sin(angle),
                                  cy - radius * 0.86f * std::cos(angle));
    g.setColour(megalo::kOrange);
    g.drawLine({ base, tip }, 2.4f);
}

// ════════════════════════════════════════════════════════════════════════════
// KnobControl
// ════════════════════════════════════════════════════════════════════════════
KnobControl::KnobControl(APVTS& apvts, const juce::String& paramID,
                         const juce::String& cap, juce::LookAndFeel* lnf)
    : caption(cap)
{
    slider.setSliderStyle(juce::Slider::RotaryVerticalDrag);
    slider.setTextBoxStyle(juce::Slider::NoTextBox, false, 0, 0);
    slider.setLookAndFeel(lnf);
    addAndMakeVisible(slider);
    attachment = std::make_unique<APVTS::SliderAttachment>(apvts, paramID, slider);
}

KnobControl::~KnobControl()
{
    slider.setLookAndFeel(nullptr);
}

void KnobControl::resized()
{
    const int dial = 34;
    slider.setBounds((getWidth() - dial) / 2, 0, dial, dial);
}

void KnobControl::paint(juce::Graphics& g)
{
    g.setColour(megalo::kWhite.withAlpha(0.7f));
    g.setFont(megalo::font(9.0f, true));
    g.drawText(caption, getLocalBounds().removeFromBottom(14),
               juce::Justification::centred, false);
}

// ════════════════════════════════════════════════════════════════════════════
// LedToggle
// ════════════════════════════════════════════════════════════════════════════
LedToggle::LedToggle(APVTS& apvts, const juce::String& paramID)
    : juce::Button(paramID)
{
    setClickingTogglesState(true);
    attachment = std::make_unique<APVTS::ButtonAttachment>(apvts, paramID, *this);
}

void LedToggle::paintButton(juce::Graphics& g, bool, bool)
{
    auto b = getLocalBounds().toFloat().reduced(1.0f);
    const bool on = getToggleState();
    g.setColour(on ? megalo::kOrange : megalo::kDialLo);
    g.fillEllipse(b);
    if (on) {
        g.setColour(megalo::kOrange.withAlpha(0.5f));
        g.drawEllipse(b.expanded(1.5f), 2.0f);   // glow ring
    } else {
        g.setColour(megalo::kWhite.withAlpha(0.18f));
        g.drawEllipse(b, 1.0f);
    }
}

// ════════════════════════════════════════════════════════════════════════════
// FilterTypeSwitch
// ════════════════════════════════════════════════════════════════════════════
FilterTypeSwitch::FilterTypeSwitch(APVTS& apvts, const juce::String& paramID)
{
    if (auto* p = apvts.getParameter(paramID)) {
        attachment = std::make_unique<juce::ParameterAttachment>(
            *p, [this](float v) { index = juce::jlimit(0, 2, juce::roundToInt(v)); repaint(); });
        attachment->sendInitialUpdate();
    }
}

void FilterTypeSwitch::paint(juce::Graphics& g)
{
    auto b = getLocalBounds();
    g.setColour(megalo::kSwitchBg);
    g.fillRoundedRectangle(b.toFloat(), 4.0f);
    g.setColour(megalo::kWhite.withAlpha(0.08f));
    g.drawRoundedRectangle(b.toFloat().reduced(0.5f), 4.0f, 1.0f);

    const char* lbl[3] = { "LP", "HP", "BP" };
    const int segW = b.getWidth() / 3;
    for (int i = 0; i < 3; ++i) {
        segs[i] = juce::Rectangle<int>(b.getX() + i * segW, b.getY(),
                                       (i == 2 ? b.getWidth() - 2 * segW : segW), b.getHeight());
        if (i == index) {
            g.setColour(megalo::kOrange);
            g.fillRoundedRectangle(segs[i].toFloat().reduced(1.0f), 3.0f);
        }
        g.setColour(i == index ? megalo::kPanel : megalo::kWhite.withAlpha(0.5f));
        g.setFont(megalo::font(9.0f, true));
        g.drawText(lbl[i], segs[i], juce::Justification::centred, false);
    }
}

void FilterTypeSwitch::mouseDown(const juce::MouseEvent& e)
{
    for (int i = 0; i < 3; ++i)
        if (segs[i].contains(e.getPosition()) && attachment)
            attachment->setValueAsCompleteGesture((float) i);
}

// ════════════════════════════════════════════════════════════════════════════
// PitchEngineSwitch — 2-position GRAIN / PV selector.
// ════════════════════════════════════════════════════════════════════════════
PitchEngineSwitch::PitchEngineSwitch(APVTS& apvts, const juce::String& paramID)
{
    if (auto* p = apvts.getParameter(paramID)) {
        attachment = std::make_unique<juce::ParameterAttachment>(
            *p, [this](float v) { index = juce::jlimit(0, 1, juce::roundToInt(v)); repaint(); });
        attachment->sendInitialUpdate();
    }
}

void PitchEngineSwitch::paint(juce::Graphics& g)
{
    auto b = getLocalBounds();
    g.setColour(megalo::kSwitchBg);
    g.fillRoundedRectangle(b.toFloat(), 4.0f);
    g.setColour(megalo::kWhite.withAlpha(0.08f));
    g.drawRoundedRectangle(b.toFloat().reduced(0.5f), 4.0f, 1.0f);

    const char* lbl[2] = { "GRAIN", "PV" };
    const int segW = b.getWidth() / 2;
    for (int i = 0; i < 2; ++i) {
        segs[i] = juce::Rectangle<int>(b.getX() + i * segW, b.getY(),
                                       (i == 1 ? b.getWidth() - segW : segW), b.getHeight());
        if (i == index) {
            g.setColour(megalo::kOrange);
            g.fillRoundedRectangle(segs[i].toFloat().reduced(1.0f), 3.0f);
        }
        g.setColour(i == index ? megalo::kPanel : megalo::kWhite.withAlpha(0.5f));
        g.setFont(megalo::font(9.0f, true));
        g.drawText(lbl[i], segs[i], juce::Justification::centred, false);
    }
}

void PitchEngineSwitch::mouseDown(const juce::MouseEvent& e)
{
    for (int i = 0; i < 2; ++i)
        if (segs[i].contains(e.getPosition()) && attachment)
            attachment->setValueAsCompleteGesture((float) i);
}

// ════════════════════════════════════════════════════════════════════════════
// TimeHandle — horizontal time marker (top row of the window).
// ════════════════════════════════════════════════════════════════════════════
TimeHandle::TimeHandle(APVTS& apvts, const juce::String& paramID,
                       const juce::String& lbl, juce::Colour knobTint)
    : label(lbl), tint(knobTint)
{
    param = apvts.getParameter(paramID);
    if (param) {
        attachment = std::make_unique<juce::ParameterAttachment>(
            *param, [this](float v) {
                value = v;
                norm  = param->getNormalisableRange().convertTo0to1(v);
                repaint();
            });
        attachment->sendInitialUpdate();
    }
    setMouseCursor(juce::MouseCursor::LeftRightResizeCursor);
}

void TimeHandle::setFromX(int x)
{
    if (!param) return;
    const float n = juce::jlimit(0.0f, 1.0f, (float) x / (float) juce::jmax(1, getWidth()));
    attachment->setValueAsPartOfGesture(param->getNormalisableRange().convertFrom0to1(n));
}

void TimeHandle::mouseDown(const juce::MouseEvent& e) { if (attachment) { attachment->beginGesture(); setFromX(e.x); } }
void TimeHandle::mouseDrag(const juce::MouseEvent& e) { setFromX(e.x); }
void TimeHandle::mouseUp  (const juce::MouseEvent&)   { if (attachment) attachment->endGesture(); }

void TimeHandle::paint(juce::Graphics& g)
{
    auto b = getLocalBounds();
    const float lineX = norm * (float) (b.getWidth() - 2);

    // Vertical track line.
    g.setColour(megalo::kWhite.withAlpha(0.35f));
    g.fillRect(juce::Rectangle<float>(lineX, 0.0f, 2.0f, (float) b.getHeight()));

    // Label, value, knob dot stacked near the top.
    g.setColour(megalo::kWhite.withAlpha(0.9f));
    g.setFont(megalo::font(9.0f, true));
    g.drawText(label, b.removeFromTop(12), juce::Justification::centred, false);
    g.setColour(megalo::kWhite.withAlpha(0.62f));
    g.setFont(megalo::font(8.0f));
    g.drawText(fmtValue(label, value), juce::Rectangle<int>(0, 12, getWidth(), 10),
               juce::Justification::centred, false);

    const float knobX = norm * (float) (getWidth() - 12);
    g.setColour(tint);
    g.fillEllipse(knobX, 30.0f, 12.0f, 12.0f);
}

// ════════════════════════════════════════════════════════════════════════════
// EnvHandle — vertical ADSR marker (bottom row of the window).
// ════════════════════════════════════════════════════════════════════════════
EnvHandle::EnvHandle(APVTS& apvts, const juce::String& paramID, const juce::String& lbl)
    : label(lbl)
{
    param = apvts.getParameter(paramID);
    if (param) {
        attachment = std::make_unique<juce::ParameterAttachment>(
            *param, [this](float v) {
                value = v;
                norm  = param->getNormalisableRange().convertTo0to1(v);
                repaint();
            });
        attachment->sendInitialUpdate();
    }
    setMouseCursor(juce::MouseCursor::UpDownResizeCursor);
}

void EnvHandle::setFromY(int y)
{
    if (!param) return;
    const float n = juce::jlimit(0.0f, 1.0f, 1.0f - (float) y / (float) juce::jmax(1, getHeight()));
    attachment->setValueAsPartOfGesture(param->getNormalisableRange().convertFrom0to1(n));
}

void EnvHandle::mouseDown(const juce::MouseEvent& e) { if (attachment) { attachment->beginGesture(); setFromY(e.y); } }
void EnvHandle::mouseDrag(const juce::MouseEvent& e) { setFromY(e.y); }
void EnvHandle::mouseUp  (const juce::MouseEvent&)   { if (attachment) attachment->endGesture(); }

void EnvHandle::paint(juce::Graphics& g)
{
    const float knobY = (1.0f - norm) * (float) (getHeight() - 12);
    g.setColour(juce::Colour(0xffffd9a8));
    g.fillEllipse((float) (getWidth() - 12) * 0.5f, knobY, 12.0f, 12.0f);

    g.setColour(megalo::kWhite.withAlpha(0.9f));
    g.setFont(megalo::font(9.0f, true));
    g.drawText(label, getLocalBounds().removeFromBottom(12), juce::Justification::centred, false);
    g.setColour(megalo::kWhite.withAlpha(0.62f));
    g.setFont(megalo::font(8.0f));
    g.drawText(fmtValue(label, value), juce::Rectangle<int>(0, getHeight() - 22, getWidth(), 10),
               juce::Justification::centred, false);
}

// ════════════════════════════════════════════════════════════════════════════
// ThresholdLine — horizontal line dragged vertically; flashes on onset.
// ════════════════════════════════════════════════════════════════════════════
ThresholdLine::ThresholdLine(APVTS& apvts, const juce::String& paramID)
{
    param = apvts.getParameter(paramID);
    if (param) {
        attachment = std::make_unique<juce::ParameterAttachment>(
            *param, [this](float v) {
                norm = param->getNormalisableRange().convertTo0to1(v);
                repaint();
            });
        attachment->sendInitialUpdate();
    }
    setMouseCursor(juce::MouseCursor::UpDownResizeCursor);
}

// Only grab clicks near the line, so the handles underneath stay reachable.
bool ThresholdLine::hitTest(int, int y)
{
    const int lineY = juce::roundToInt((1.0f - norm) * (float) getHeight());
    return std::abs(y - lineY) < 6;
}

void ThresholdLine::setFromY(int y)
{
    if (!param) return;
    const float n = juce::jlimit(0.0f, 1.0f, 1.0f - (float) y / (float) juce::jmax(1, getHeight()));
    attachment->setValueAsPartOfGesture(param->getNormalisableRange().convertFrom0to1(n));
}

void ThresholdLine::mouseDown(const juce::MouseEvent& e) { if (attachment) { attachment->beginGesture(); setFromY(e.y); } }
void ThresholdLine::mouseDrag(const juce::MouseEvent& e) { setFromY(e.y); }
void ThresholdLine::mouseUp  (const juce::MouseEvent&)   { if (attachment) attachment->endGesture(); }

void ThresholdLine::paint(juce::Graphics& g)
{
    const int lineY = juce::roundToInt((1.0f - norm) * (float) getHeight());

    // Flash blends white → orange while an onset pulse is held.
    auto lineCol = megalo::kWhite.withAlpha(0.85f).interpolatedWith(megalo::kOrange, flash);
    g.setColour(lineCol);

    // Dashed horizontal line.
    const float dashes[] = { 5.0f, 4.0f };
    g.drawDashedLine({ { 0.0f, (float) lineY }, { (float) getWidth(), (float) lineY } },
                     dashes, 2, 1.0f + 1.5f * flash);

    // Left grab dot.
    g.fillEllipse(6.0f, (float) lineY - 4.0f, 8.0f, 8.0f);

    // Value readout, right-aligned.
    g.setColour(megalo::kWhite.withAlpha(0.85f));
    g.setFont(megalo::font(9.0f, true));
    g.drawText(juce::String(norm, 2), juce::Rectangle<int>(getWidth() - 44, lineY - 12, 40, 11),
               juce::Justification::centredRight, false);
}

// ════════════════════════════════════════════════════════════════════════════
// WindowPanel — orange scope window with waveform, midline, ADSR curve.
// ════════════════════════════════════════════════════════════════════════════
WindowPanel::WindowPanel(APVTS& a) : threshold(a, "onset_threshold"), apvts(a)
{
    topHandles.add(new TimeHandle(a, "sample_ms",      "SAMPLE", juce::Colour(0xfffff5e6)));
    topHandles.add(new TimeHandle(a, "attack_skip_ms", "SKIP",   juce::Colour(0xfffff5e6)));
    topHandles.add(new TimeHandle(a, "grain_size_ms",  "GRAIN",  juce::Colour(0xffffe1bf)));
    topHandles.add(new TimeHandle(a, "grain_xfade_ms", "XFADE",  juce::Colour(0xffffe1bf)));
    for (auto* h : topHandles) addAndMakeVisible(h);

    envHandles.add(new EnvHandle(a, "env_attack",  "A"));
    envHandles.add(new EnvHandle(a, "env_decay",   "D"));
    envHandles.add(new EnvHandle(a, "env_sustain", "S"));
    envHandles.add(new EnvHandle(a, "env_release", "R"));
    for (auto* h : envHandles) addAndMakeVisible(h);

    addAndMakeVisible(threshold);   // added last → painted on top
}

void WindowPanel::resized()
{
    auto b = getLocalBounds();
    const int halfH = b.getHeight() / 2;
    const int colW  = b.getWidth() / 4;

    for (int i = 0; i < topHandles.size(); ++i)
        topHandles[i]->setBounds(i * colW, 0, colW, halfH);
    for (int i = 0; i < envHandles.size(); ++i)
        envHandles[i]->setBounds(i * colW, halfH, colW, b.getHeight() - halfH);

    threshold.setBounds(b);
}

void WindowPanel::paint(juce::Graphics& g)
{
    auto b = getLocalBounds().toFloat();

    // Orange body.
    g.setColour(megalo::kOrange);
    g.fillRoundedRectangle(b, 8.0f);

    const float mid = b.getHeight() * 0.5f;

    // Decorative dark waveform around the midline.
    juce::Path wave;
    const int N = 120;
    auto ampAt = [&](float t) { return (0.18f + 0.16f * std::sin(t * 6.2831f * 0.7f)) * b.getHeight(); };
    wave.startNewSubPath(0.0f, mid);
    for (int i = 0; i <= N; ++i) {
        const float t = (float) i / (float) N;
        const float x = t * b.getWidth();
        wave.lineTo(x, mid - ampAt(t) * std::sin(t * 6.2831f * 9.0f));
    }
    for (int i = N; i >= 0; --i) {
        const float t = (float) i / (float) N;
        const float x = t * b.getWidth();
        wave.lineTo(x, mid + ampAt(t) * std::sin(t * 6.2831f * 9.0f + 1.3f));
    }
    wave.closeSubPath();
    g.setColour(juce::Colour(0xff2a2d31).withAlpha(0.85f));
    g.fillPath(wave);

    // ADSR envelope curve in the bottom half (decorative, follows A/D/S/R).
    auto raw = [&](const char* id) { return apvts.getRawParameterValue(id)->load(); };
    const float A = raw("env_attack"), D = raw("env_decay");
    const float S = raw("env_sustain"), R = raw("env_release");
    const float hold = 400.0f;
    const float total = juce::jmax(1.0f, A + D + hold + R);
    auto yOf = [&](float lvl) { return b.getHeight() - lvl * (b.getHeight() * 0.5f); };
    const float w = b.getWidth();
    const float xA = A / total * w;
    const float xD = xA + D / total * w;
    const float xH = xD + hold / total * w;
    const float xR = xH + R / total * w;

    juce::Path env;
    env.startNewSubPath(0.0f, yOf(0.0f));
    env.lineTo(xA, yOf(1.0f));
    env.lineTo(xD, yOf(S));
    env.lineTo(xH, yOf(S));
    env.lineTo(xR, yOf(0.0f));
    g.setColour(megalo::kWhite.withAlpha(0.85f));
    g.strokePath(env, juce::PathStrokeType(1.5f));

    juce::Path fill = env;
    fill.lineTo(xR, b.getHeight());
    fill.lineTo(0.0f, b.getHeight());
    fill.closeSubPath();
    g.setColour(megalo::kWhite.withAlpha(0.14f));
    g.fillPath(fill);

    // White midline.
    g.setColour(megalo::kWhite.withAlpha(0.85f));
    g.fillRect(juce::Rectangle<float>(0.0f, mid - 0.5f, b.getWidth(), 1.0f));

    // Faint grouping brackets (WINDOW = sample+skip, GRAIN = grain+xfade).
    g.setColour(megalo::kWhite.withAlpha(0.55f));
    g.setFont(megalo::font(7.0f, true));
    g.drawText("WINDOW", juce::Rectangle<int>(0, 1, getWidth() / 2, 9), juce::Justification::centred, false);
    g.drawText("GRAIN",  juce::Rectangle<int>(getWidth() / 2, 1, getWidth() / 2, 9), juce::Justification::centred, false);
}

// ════════════════════════════════════════════════════════════════════════════
// MegaloEditor
// ════════════════════════════════════════════════════════════════════════════
MegaloEditor::MegaloEditor(MegaloAudioProcessor& p)
    : juce::AudioProcessorEditor(p), proc(p), window(p.apvts),
      filterType(p.apvts, "filter_type"),
      pitchEngine(p.apvts, "pitch_mode")
{
    addAndMakeVisible(window);

    // Bottom-row knobs, in modgui order (group → knob).
    auto addKnob = [&](const char* id, const char* cap) {
        knobs.add(new KnobControl(proc.apvts, id, cap, &lnf));
        addAndMakeVisible(knobs.getLast());
    };
    addKnob("pitch1_semi",  "PITCH");   // 0  VOICE 1
    addKnob("pitch1_level", "LEVEL");   // 1
    addKnob("pitch2_semi",  "PITCH");   // 2  VOICE 2
    addKnob("pitch2_level", "LEVEL");   // 3
    addKnob("detune_cents", "CENTS");   // 4  DETUNE
    addKnob("chorus_rate",  "RATE");    // 5
    addKnob("detune_blend", "BLEND");   // 6
    addKnob("filter_cutoff","CUTOFF");  // 7  FILTER
    addKnob("filter_q",     "Q");       // 8
    addKnob("base_pitch",   "BASE");    // 9  GLOBAL
    addKnob("blend",        "BLEND");   // 10

    leds.add(new LedToggle(proc.apvts, "pitch1_enable"));
    leds.add(new LedToggle(proc.apvts, "pitch2_enable"));
    leds.add(new LedToggle(proc.apvts, "detune_enable"));
    for (auto* l : leds) addAndMakeVisible(l);

    addAndMakeVisible(filterType);
    addAndMakeVisible(pitchEngine);

    setSize(720, 380);
    startTimerHz(30);
}

MegaloEditor::~MegaloEditor()
{
    stopTimer();
}

void MegaloEditor::timerCallback()
{
    const float pulse = proc.triggerPulse.load(std::memory_order_relaxed);
    flashLevel = pulse > 0.5f ? 1.0f : flashLevel * 0.78f;
    if (flashLevel < 0.01f) flashLevel = 0.0f;
    window.threshold.setFlash(flashLevel);
}

void MegaloEditor::paint(juce::Graphics& g)
{
    auto b = getLocalBounds().toFloat();

    // Panel background.
    g.setColour(megalo::kPanel);
    g.fillRoundedRectangle(b, 14.0f);
    g.setColour(megalo::kWhite.withAlpha(0.05f));
    g.drawRoundedRectangle(b.reduced(0.5f), 14.0f, 1.0f);

    // MEGALO brand.
    g.setColour(megalo::kOrange);
    g.setFont(megalo::font(30.0f, true));
    g.drawText("M E G A L O", juce::Rectangle<int>(26, 12, 360, 36),
               juce::Justification::left, false);

    // Pitch-engine switch caption (top-right, above the switch).
    g.setColour(megalo::kWhite.withAlpha(0.55f));
    g.setFont(megalo::font(8.0f, true));
    g.drawText("PITCH ENGINE", juce::Rectangle<int>(560, 12, 134, 10),
               juce::Justification::centred, false);

    // Time-axis ruler above the window (0..500 ms).
    const int rulerX = 26, rulerY = 52, rulerW = 668;
    for (int i = 0; i <= 10; ++i) {
        const float fx = rulerX + rulerW * (i / 10.0f);
        const bool major = (i % 2 == 0);
        g.setColour(megalo::kWhite.withAlpha(major ? 0.5f : 0.22f));
        g.fillRect(juce::Rectangle<float>(fx, (float) (rulerY + (major ? 0 : 2)), 1.0f, major ? 4.0f : 2.0f));
    }
    g.setColour(megalo::kWhite.withAlpha(0.55f));
    g.setFont(megalo::font(7.0f, true));
    const char* labels[6] = { "0", "100", "200", "300", "400", "500 ms" };
    for (int i = 0; i < 6; ++i) {
        const float fx = rulerX + rulerW * (i / 5.0f);
        auto just = (i == 0) ? juce::Justification::left
                  : (i == 5) ? juce::Justification::right
                             : juce::Justification::centred;
        g.drawText(labels[i], juce::Rectangle<int>((int) fx - 30, rulerY - 9, 60, 9), just, false);
    }

    // Group backgrounds + titles.
    struct GroupSpec { const char* title; int x, w; };
    const GroupSpec groups[5] = {
        { "VOICE 1", 26,  110 }, { "VOICE 2", 144, 110 }, { "DETUNE", 262, 140 },
        { "FILTER", 410, 166 }, { "GLOBAL", 584, 110 }
    };
    for (auto& gr : groups) {
        juce::Rectangle<int> r(gr.x, 245, gr.w, 123);
        g.setColour(juce::Colours::black.withAlpha(0.18f));
        g.fillRoundedRectangle(r.toFloat(), 6.0f);
        g.setColour(megalo::kOrange);
        g.setFont(megalo::font(9.0f, true));
        g.drawText(gr.title, r.getX() + 8, r.getY() + 6, r.getWidth() - 26, 12,
                   juce::Justification::left, false);
    }
}

void MegaloEditor::resized()
{
    // Pitch-engine switch, top-right (under its caption, above the window).
    pitchEngine.setBounds(560, 24, 134, 22);

    window.setBounds(26, 60, 668, 170);

    // LEDs sit at the top-right of VOICE 1 / VOICE 2 / DETUNE heads.
    const int ledGroupX[3] = { 26, 144, 262 };
    const int ledGroupW[3] = { 110, 110, 140 };
    for (int i = 0; i < leds.size(); ++i)
        leds[i]->setBounds(ledGroupX[i] + ledGroupW[i] - 18, 251, 10, 10);

    const int knobW = 40, knobH = 52, gap = 6;
    const int bodyY = 245 + 26;

    auto layoutRow = [&](int gx, int gw, std::initializer_list<int> idx, int leftPad) {
        const int n = (int) idx.size();
        const int rowW = n * knobW + (n - 1) * gap + leftPad;
        int x = gx + (gw - rowW) / 2 + leftPad;
        for (int i : idx) {
            knobs[i]->setBounds(x, bodyY, knobW, knobH);
            x += knobW + gap;
        }
    };

    layoutRow(26,  110, { 0, 1 }, 0);          // VOICE 1
    layoutRow(144, 110, { 2, 3 }, 0);          // VOICE 2
    layoutRow(262, 140, { 4, 5, 6 }, 0);       // DETUNE

    // FILTER: 3-pos switch on the left, then CUTOFF + Q.
    const int swW = 66, swH = 22;
    const int filtContentW = swW + gap + 2 * knobW + gap;
    int fx = 410 + (166 - filtContentW) / 2;
    filterType.setBounds(fx, bodyY + (knobH - swH) / 2 - 6, swW, swH);
    fx += swW + gap;
    knobs[7]->setBounds(fx, bodyY, knobW, knobH); fx += knobW + gap;
    knobs[8]->setBounds(fx, bodyY, knobW, knobH);

    layoutRow(584, 110, { 9, 10 }, 0);         // GLOBAL
}
