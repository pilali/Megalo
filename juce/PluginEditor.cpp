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
// f0 (Hz) → note name for the capture readout ("A2", "E3"…). A4 = 440 Hz.
juce::String noteName(float f)
{
    if (f <= 0.0f) return {};
    const int m = juce::roundToInt(69.0 + 12.0 * std::log2(f / 440.0));
    static const char* names[12] = { "C","C#","D","D#","E","F","F#","G","G#","A","A#","B" };
    return juce::String(names[((m % 12) + 12) % 12]) + juce::String(m / 12 - 1);
}

// Format a handle readout the way the modgui does (long ms collapse to s).
juce::String fmtValue(const juce::String& label, float v)
{
    if (label == "S")                        // sustain 0..1
        return juce::String(v, 2);
    if (v >= 1000.0f)
        return juce::String(v / 1000.0f, v >= 10000.0f ? 1 : 2) + " s";
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

    // Value feedback + resets: the knobs used to be silent (no way to read
    // the cutoff in Hz or a pitch in semitones without the host generic view).
    if (auto* p = apvts.getParameter(paramID)) {
        slider.setDoubleClickReturnValue(true, p->convertFrom0to1(p->getDefaultValue()));
        const auto unit = p->getLabel();
        if (unit.isNotEmpty())
            slider.setTextValueSuffix(" " + unit);
        slider.setNumDecimalPlacesToDisplay(
            apvts.getParameterRange(paramID).interval >= 1.0f ? 0 : 2);
    }
    slider.setPopupDisplayEnabled(true, false, nullptr);   // bubble while dragging
    slider.setTitle(paramID);
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
    g.setFont(megalo::font(11.0f, true));
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
        g.setFont(megalo::font(11.0f, true));
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
        g.setFont(megalo::font(11.0f, true));
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

    // Label + value stacked near the top, below the bracket caption.
    g.setColour(megalo::kWhite.withAlpha(0.9f));
    g.setFont(megalo::font(11.0f, true));
    g.drawText(label, juce::Rectangle<int>(0, 15, getWidth(), 12),
               juce::Justification::centred, false);
    g.setColour(megalo::kWhite.withAlpha(0.62f));
    g.setFont(megalo::font(10.0f));
    g.drawText(fmtValue(label, value), juce::Rectangle<int>(0, 27, getWidth(), 12),
               juce::Justification::centred, false);

    // Knob dot centered on the track line, both axes.
    const float cx = lineX + 1.0f;
    const float cy = (float) getHeight() * 0.5f;
    g.setColour(tint);
    g.fillEllipse(cx - 6.0f, cy - 6.0f, 12.0f, 12.0f);
}

// ════════════════════════════════════════════════════════════════════════════
// EnvelopeEditor — ADSR breakpoints dragged directly on the curve.
// ════════════════════════════════════════════════════════════════════════════
namespace {
constexpr int   kEnvLabelStrip = 28;    // bottom strip for the A/D/S/R readouts
constexpr float kEnvHitRadius  = 16.0f; // grab radius around a breakpoint
} // namespace

EnvelopeEditor::EnvelopeEditor(APVTS& a)
{
    bind(hA, a, "env_attack");
    bind(hD, a, "env_decay");
    bind(hS, a, "env_sustain");
    bind(hR, a, "env_release");
}

void EnvelopeEditor::bind(Handle& h, APVTS& a, const char* id)
{
    h.param = a.getParameter(id);
    if (!h.param) return;
    h.attachment = std::make_unique<juce::ParameterAttachment>(
        *h.param, [this, &h](float v) {
            h.value = v;
            h.norm  = h.param->getNormalisableRange().convertTo0to1(v);
            repaint();
        });
    h.attachment->sendInitialUpdate();
}

float EnvelopeEditor::curveH() const { return (float) getHeight() - (float) kEnvLabelStrip; }
float EnvelopeEditor::yOf(float lvl) const { return (1.0f - lvl) * curveH(); }

juce::Point<float> EnvelopeEditor::dotPos(Target t) const
{
    switch (t) {
        case A:  return { xA(), yOf(1.0f) };
        case D:  return { xD(), yOf(hS.value) };
        case S:  return { (xD() + xH()) * 0.5f, yOf(hS.value) };
        default: return { xR(), yOf(0.0f) };
    }
}

EnvelopeEditor::Target EnvelopeEditor::hitTarget(juce::Point<float> p) const
{
    Target best = None;
    float bestD = kEnvHitRadius;
    for (Target t : { A, D, S, R }) {
        const float d = dotPos(t).getDistanceFrom(p);
        if (d < bestD) { bestD = d; best = t; }
    }
    return best;
}

void EnvelopeEditor::applyDrag(const juce::MouseEvent& e)
{
    if (drag == None) return;
    Handle& h = handleFor(drag);
    if (!h.attachment) return;

    float n = 0.0f;
    switch (drag) {
        // Times: horizontal, each within its own fixed segment of the axis.
        case A: n = (float) e.x / segW();          break;
        case D: n = ((float) e.x - xA()) / segW(); break;
        case R: n = ((float) e.x - xH()) / segW(); break;
        // Sustain: vertical (level).
        default: n = 1.0f - (float) e.y / curveH(); break;
    }
    n = juce::jlimit(0.0f, 1.0f, n);
    h.attachment->setValueAsPartOfGesture(h.param->getNormalisableRange().convertFrom0to1(n));
}

void EnvelopeEditor::mouseDown(const juce::MouseEvent& e)
{
    drag = hitTarget(e.position);
    if (drag == None) return;
    if (handleFor(drag).attachment != nullptr)
        handleFor(drag).attachment->beginGesture();
    applyDrag(e);
}

void EnvelopeEditor::mouseDrag(const juce::MouseEvent& e) { applyDrag(e); }

void EnvelopeEditor::mouseUp(const juce::MouseEvent&)
{
    if (drag != None && handleFor(drag).attachment != nullptr)
        handleFor(drag).attachment->endGesture();
    drag = None;
}

void EnvelopeEditor::mouseMove(const juce::MouseEvent& e)
{
    const Target t = hitTarget(e.position);
    if (t != hover) { hover = t; repaint(); }
    setMouseCursor(t == None ? juce::MouseCursor::NormalCursor
                 : t == S    ? juce::MouseCursor::UpDownResizeCursor
                             : juce::MouseCursor::LeftRightResizeCursor);
}

void EnvelopeEditor::mouseExit(const juce::MouseEvent&)
{
    if (hover != None) { hover = None; repaint(); }
    setMouseCursor(juce::MouseCursor::NormalCursor);
}

void EnvelopeEditor::paint(juce::Graphics& g)
{
    const float w = (float) getWidth();

    // Envelope curve + fill on the fixed axis.
    juce::Path env;
    env.startNewSubPath(0.0f, yOf(0.0f));
    env.lineTo(xA(), yOf(1.0f));
    env.lineTo(xD(), yOf(hS.value));
    env.lineTo(xH(), yOf(hS.value));
    env.lineTo(xR(), yOf(0.0f));
    g.setColour(megalo::kWhite.withAlpha(0.85f));
    g.strokePath(env, juce::PathStrokeType(1.5f));

    juce::Path fill = env;
    fill.lineTo(xR(), curveH());
    fill.lineTo(0.0f, curveH());
    fill.closeSubPath();
    g.setColour(megalo::kWhite.withAlpha(0.14f));
    g.fillPath(fill);

    // Breakpoint dots (hovered/dragged one highlighted).
    for (Target t : { A, D, S, R }) {
        const auto pt = dotPos(t);
        const bool hot = (t == drag) || (drag == None && t == hover);
        g.setColour(hot ? megalo::kWhite : juce::Colour(0xffffd9a8));
        const float r = hot ? 7.0f : 6.0f;
        g.fillEllipse(pt.x - r, pt.y - r, 2.0f * r, 2.0f * r);
    }

    // Label + value strip (fixed columns so the readouts never overlap).
    const char* names[4] = { "A", "D", "S", "R" };
    const Handle* hs[4]  = { &hA, &hD, &hS, &hR };
    const int colW = (int) w / 4;
    for (int i = 0; i < 4; ++i) {
        juce::Rectangle<int> col(i * colW, (int) curveH(), colW, kEnvLabelStrip);
        g.setColour(megalo::kWhite.withAlpha(0.9f));
        g.setFont(megalo::font(11.0f, true));
        g.drawText(names[i], col.removeFromTop(14), juce::Justification::centred, false);
        g.setColour(megalo::kWhite.withAlpha(0.62f));
        g.setFont(megalo::font(10.0f));
        g.drawText(fmtValue(names[i], hs[i]->value), col, juce::Justification::centred, false);
    }
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
// When the line sits in the bottom half (low thresholds — the default 0.15
// puts it right across the ADSR editor), only the left grab-dot area still
// takes the click; everywhere else the envelope breakpoints win.
bool ThresholdLine::hitTest(int x, int y)
{
    const int lineY = juce::roundToInt((1.0f - norm) * (float) getHeight());
    if (std::abs(y - lineY) >= 6) return false;
    return lineY < getHeight() / 2 || x < 24;
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
    g.setFont(megalo::font(11.0f, true));
    g.drawText(juce::String(norm, 2), juce::Rectangle<int>(getWidth() - 50, lineY - 14, 46, 13),
               juce::Justification::centredRight, false);
}

// ════════════════════════════════════════════════════════════════════════════
// WindowPanel — orange scope window with waveform, midline, ADSR curve.
// ════════════════════════════════════════════════════════════════════════════
WindowPanel::WindowPanel(APVTS& a)
    : threshold(a, "onset_threshold"), envEditor(a)
{
    // Grain Size / Crossfade drive the granular engine (and the granular
    // fallback of the H+N build), so they are surfaced in both builds —
    // they used to exist only in the host generic view.
    topHandles.add(new TimeHandle(a, "sample_ms",      "SAMPLE", juce::Colour(0xfffff5e6)));
    topHandles.add(new TimeHandle(a, "attack_skip_ms", "SKIP",   juce::Colour(0xfffff5e6)));
    topHandles.add(new TimeHandle(a, "grain_size_ms",  "SIZE",   juce::Colour(0xffffd9a8)));
    topHandles.add(new TimeHandle(a, "grain_xfade_ms", "XFADE",  juce::Colour(0xffffd9a8)));
    for (auto* h : topHandles) addAndMakeVisible(h);

    addAndMakeVisible(envEditor);

    addAndMakeVisible(threshold);   // added last → painted on top
}

void WindowPanel::resized()
{
    auto b = getLocalBounds();
    const int halfH = b.getHeight() / 2;

    // Top handles spread across the full width (count varies with the engine).
    // 10px shorter than the half-window: a dead margin above the envelope
    // area so grabbing an ADSR breakpoint near the midline can't
    // accidentally drag SAMPLE/SKIP/SIZE/XFADE.
    const int topColW = b.getWidth() / juce::jmax(1, topHandles.size());
    for (int i = 0; i < topHandles.size(); ++i)
        topHandles[i]->setBounds(i * topColW, 0, topColW, halfH - 10);

    envEditor.setBounds(0, halfH, b.getWidth(), b.getHeight() - halfH);

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

    // (The ADSR curve and its breakpoints live in envEditor, a child
    //  component covering the bottom half.)

    // White midline.
    g.setColour(megalo::kWhite.withAlpha(0.85f));
    g.fillRect(juce::Rectangle<float>(0.0f, mid - 0.5f, b.getWidth(), 1.0f));

    // Faint grouping brackets (WINDOW = sample+skip, GRAIN = size+xfade),
    // inset from the window's top edge.
    g.setColour(megalo::kWhite.withAlpha(0.55f));
    g.setFont(megalo::font(10.0f, true));
    g.drawText("WINDOW", juce::Rectangle<int>(0, 4, getWidth() / 2, 12), juce::Justification::centred, false);
    g.drawText("GRAIN",  juce::Rectangle<int>(getWidth() / 2, 4, getWidth() / 2, 12), juce::Justification::centred, false);
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
    addKnob("dry_level",    "DRY");     // 11 (was only reachable via the host)
#ifdef MEGALO_HN_SYNTH
    addKnob("hn_brightness","BRIGHT");  // 12 TIMBRE (MegaloHN only)
    addKnob("hn_damping",   "DAMP");    // 13
    addKnob("hn_even_odd",  "EVEN");    // 14
    addKnob("hn_noise",     "NOISE");   // 15
    addKnob("hn_width",     "WIDTH");   // 16
#endif

    leds.add(new LedToggle(proc.apvts, "pitch1_enable"));
    leds.add(new LedToggle(proc.apvts, "pitch2_enable"));
    leds.add(new LedToggle(proc.apvts, "detune_enable"));
    for (auto* l : leds) addAndMakeVisible(l);

    addAndMakeVisible(filterType);
#ifndef MEGALO_HN_SYNTH
    // Granular build only: the pitch voices run on the selected engine. In
    // the MegaloHN build the switch would only affect the granular FALLBACK
    // (the additive engine replaces the pitch voices whenever the analysis
    // finds notes), so its slot shows the capture readout instead.
    addAndMakeVisible(pitchEngine);
#endif

#ifdef MEGALO_HN_SYNTH
    setSize(720, 470);   // extra row for TIMBRE
#else
    setSize(720, 380);
#endif
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

    // Capture-status readout: what the engine actually made of the last
    // freeze (the analyzed chord / the loop's fundamental / an unpitched
    // texture) — the pad's behaviour is driven by this, so show it.
    juce::String s;
    const int n = proc.padNoteCount.load(std::memory_order_relaxed);
    if (n < 0)       s = juce::String::fromUTF8("—");
    else if (n == 0) s = "NO PITCH";
    else
        for (int i = 0; i < n && i < MegaloAudioProcessor::kMaxPadNotes; ++i) {
            if (i > 0) s << "  ";
            s << noteName(proc.padF0[i].load(std::memory_order_relaxed));
        }
    if (s != padStatus) {
        padStatus = s;
        repaint(390, 8, 306, 42);   // the header status/switch area
    }
}

void MegaloEditor::paint(juce::Graphics& g)
{
    auto b = getLocalBounds().toFloat();

    // Panel background.
    g.setColour(megalo::kPanel);
    g.fillRoundedRectangle(b, 14.0f);
    g.setColour(megalo::kWhite.withAlpha(0.05f));
    g.drawRoundedRectangle(b.reduced(0.5f), 14.0f, 1.0f);

    // Brand.
    g.setColour(megalo::kOrange);
    g.setFont(megalo::font(30.0f, true));
#ifdef MEGALO_HN_SYNTH
    g.drawText("M E G A L O  H N", juce::Rectangle<int>(26, 12, 380, 36),
               juce::Justification::left, false);
#else
    g.drawText("M E G A L O", juce::Rectangle<int>(26, 12, 360, 36),
               juce::Justification::left, false);
#endif

#ifndef MEGALO_HN_SYNTH
    // Pitch-engine switch caption (top-right, above the switch).
    g.setColour(megalo::kWhite.withAlpha(0.55f));
    g.setFont(megalo::font(10.0f, true));
    g.drawText("PITCH ENGINE", juce::Rectangle<int>(560, 12, 134, 10),
               juce::Justification::centred, false);
#endif

    // Capture-status readout. MegaloHN: in the pitch-engine slot (top-right);
    // Megalo: between the brand and the switch.
#ifdef MEGALO_HN_SYNTH
    const juce::Rectangle<int> stCap(560, 12, 134, 10), stVal(560, 24, 134, 22);
#else
    const juce::Rectangle<int> stCap(400, 12, 150, 10), stVal(400, 24, 150, 22);
#endif
    g.setColour(megalo::kWhite.withAlpha(0.55f));
    g.setFont(megalo::font(10.0f, true));
    g.drawText("CAPTURE", stCap, juce::Justification::centred, false);
    g.setColour(megalo::kWhite.withAlpha(0.9f));
    g.setFont(megalo::font(13.0f, true));
    g.drawText(padStatus, stVal, juce::Justification::centred, false);

    // Time-axis ruler above the window (0..500 ms).
    const int rulerX = 26, rulerY = 52, rulerW = 668;
    for (int i = 0; i <= 10; ++i) {
        const float fx = rulerX + rulerW * (i / 10.0f);
        const bool major = (i % 2 == 0);
        g.setColour(megalo::kWhite.withAlpha(major ? 0.5f : 0.22f));
        g.fillRect(juce::Rectangle<float>(fx, (float) (rulerY + (major ? 0 : 2)), 1.0f, major ? 4.0f : 2.0f));
    }
    g.setColour(megalo::kWhite.withAlpha(0.55f));
    g.setFont(megalo::font(9.0f, true));
    const char* labels[6] = { "0", "100", "200", "300", "400", "500 ms" };
    for (int i = 0; i < 6; ++i) {
        const float fx = rulerX + rulerW * (i / 5.0f);
        auto just = (i == 0) ? juce::Justification::left
                  : (i == 5) ? juce::Justification::right
                             : juce::Justification::centred;
        g.drawText(labels[i], juce::Rectangle<int>((int) fx - 30, rulerY - 11, 60, 11), just, false);
    }

    // Group backgrounds + titles (GLOBAL widened for the DRY knob).
    struct GroupSpec { const char* title; int x, w; };
    const GroupSpec groups[5] = {
        { "VOICE 1", 26,  104 }, { "VOICE 2", 138, 104 }, { "DETUNE", 250, 134 },
        { "FILTER", 392, 158 }, { "GLOBAL", 558, 136 }
    };
    for (auto& gr : groups) {
        juce::Rectangle<int> r(gr.x, 245, gr.w, 113);
        g.setColour(juce::Colours::black.withAlpha(0.18f));
        g.fillRoundedRectangle(r.toFloat(), 6.0f);
        g.setColour(megalo::kOrange);
        g.setFont(megalo::font(11.0f, true));
        g.drawText(gr.title, r.getX() + 8, r.getY() + 6, r.getWidth() - 26, 12,
                   juce::Justification::left, false);
    }
#ifdef MEGALO_HN_SYNTH
    // TIMBRE — full-width second row (MegaloHN only).
    {
        juce::Rectangle<int> r(26, 368, 668, 88);
        g.setColour(juce::Colours::black.withAlpha(0.18f));
        g.fillRoundedRectangle(r.toFloat(), 6.0f);
        g.setColour(megalo::kOrange);
        g.setFont(megalo::font(11.0f, true));
        g.drawText("TIMBRE", r.getX() + 8, r.getY() + 6, r.getWidth() - 26, 12,
                   juce::Justification::left, false);
    }
#endif
}

void MegaloEditor::resized()
{
#ifndef MEGALO_HN_SYNTH
    // Pitch-engine switch, top-right (under its caption, above the window).
    // The MegaloHN build shows the capture readout in this slot instead.
    pitchEngine.setBounds(560, 24, 134, 22);
#endif

    window.setBounds(26, 60, 668, 170);

    // LEDs sit at the top-right of VOICE 1 / VOICE 2 / DETUNE heads.
    const int ledGroupX[3] = { 26, 138, 250 };
    const int ledGroupW[3] = { 104, 104, 134 };
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

    layoutRow(26,  104, { 0, 1 }, 0);          // VOICE 1
    layoutRow(138, 104, { 2, 3 }, 0);          // VOICE 2
    layoutRow(250, 134, { 4, 5, 6 }, 0);       // DETUNE

    // FILTER: 3-pos switch on the left, then CUTOFF + Q.
    const int swW = 60, swH = 22;
    const int filtContentW = swW + gap + 2 * knobW + gap;
    int fx = 392 + (158 - filtContentW) / 2;
    filterType.setBounds(fx, bodyY + (knobH - swH) / 2 - 6, swW, swH);
    fx += swW + gap;
    knobs[7]->setBounds(fx, bodyY, knobW, knobH); fx += knobW + gap;
    knobs[8]->setBounds(fx, bodyY, knobW, knobH);

    layoutRow(558, 136, { 9, 10, 11 }, 0);     // GLOBAL (BASE / BLEND / DRY)

#ifdef MEGALO_HN_SYNTH
    // TIMBRE — five knobs spread across the full-width second row.
    {
        const int tBodyY = 368 + 26;
        const int n = 5, rowW = n * knobW + (n - 1) * gap;
        int x = 26 + (668 - rowW) / 2;
        for (int i = 12; i <= 16; ++i) {
            knobs[i]->setBounds(x, tBodyY, knobW, knobH);
            x += knobW + gap;
        }
    }
#endif
}
