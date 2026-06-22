#include "PluginProcessor.h"
#include "PluginEditor.h"
#include "megalo_presets.h"

using APVTS = juce::AudioProcessorValueTreeState;
using Range = juce::NormalisableRange<float>;
namespace { const int kVer = 1; }

// Parameter IDs are exactly the LV2 .ttl symbols so state is portable.
APVTS::ParameterLayout MegaloAudioProcessor::createLayout()
{
    using AF = juce::AudioParameterFloat;
    using AC = juce::AudioParameterChoice;
    using AB = juce::AudioParameterBool;
    using FA = juce::AudioParameterFloatAttributes;

    APVTS::ParameterLayout p;

    auto pid = [](const char* s){ return juce::ParameterID { s, kVer }; };

    // integer-valued range (matches lv2:integer ports)
    auto intRange = [](float lo, float hi){ Range r(lo, hi, 1.0f); return r; };

    // filter_cutoff: lv2:logarithmic → skew so the centre sits musically.
    Range cutoffRange(20.0f, 20000.0f);
    cutoffRange.setSkewForCentre(1000.0f);

    p.add(std::make_unique<AF>(pid("onset_threshold"), "Onset Threshold", Range(0.0f, 1.0f), 0.15f));
    p.add(std::make_unique<AF>(pid("sample_ms"), "Sample Length", intRange(50.0f, 500.0f), 150.0f, FA{}.withLabel("ms")));
    p.add(std::make_unique<AF>(pid("attack_skip_ms"), "Attack Skip", Range(0.0f, 500.0f), 50.0f, FA{}.withLabel("ms")));
    p.add(std::make_unique<AF>(pid("blend"), "Blend", Range(0.0f, 1.0f), 0.8f));
    p.add(std::make_unique<AF>(pid("grain_size_ms"), "Grain Size", Range(5.0f, 200.0f), 100.0f, FA{}.withLabel("ms")));
    p.add(std::make_unique<AF>(pid("grain_xfade_ms"), "Grain Crossfade", Range(5.0f, 100.0f), 40.0f, FA{}.withLabel("ms")));
    p.add(std::make_unique<AF>(pid("base_pitch"), "Base Pitch", intRange(-12.0f, 12.0f), 0.0f, FA{}.withLabel("st")));
    p.add(std::make_unique<AF>(pid("pitch1_semi"), "Voice 1 Pitch", intRange(-24.0f, 24.0f), -12.0f, FA{}.withLabel("st")));
    p.add(std::make_unique<AF>(pid("pitch1_level"), "Voice 1 Level", Range(0.0f, 1.0f), 0.5f));
    p.add(std::make_unique<AF>(pid("pitch2_semi"), "Voice 2 Pitch", intRange(-24.0f, 24.0f), 12.0f, FA{}.withLabel("st")));
    p.add(std::make_unique<AF>(pid("pitch2_level"), "Voice 2 Level", Range(0.0f, 1.0f), 0.5f));
    p.add(std::make_unique<AF>(pid("detune_cents"), "Detune", Range(0.0f, 50.0f), 10.0f, FA{}.withLabel("cents")));
    p.add(std::make_unique<AF>(pid("chorus_rate"), "Chorus Rate", Range(0.1f, 8.0f), 0.5f, FA{}.withLabel("Hz")));
    p.add(std::make_unique<AF>(pid("detune_blend"), "Detune Blend", Range(0.0f, 1.0f), 0.3f));
    p.add(std::make_unique<AC>(pid("filter_type"), "Filter Type",
                               juce::StringArray { "LP", "HP", "BP" }, 0));
    p.add(std::make_unique<AF>(pid("filter_cutoff"), "Filter Cutoff", cutoffRange, 3000.0f, FA{}.withLabel("Hz")));
    p.add(std::make_unique<AF>(pid("filter_q"), "Filter Q", Range(0.1f, 10.0f), 0.7f));
    p.add(std::make_unique<AF>(pid("env_attack"), "Attack", Range(0.0f, 5000.0f), 100.0f, FA{}.withLabel("ms")));
    p.add(std::make_unique<AF>(pid("env_decay"), "Decay", Range(0.0f, 5000.0f), 200.0f, FA{}.withLabel("ms")));
    p.add(std::make_unique<AF>(pid("env_sustain"), "Sustain", Range(0.0f, 1.0f), 1.0f));
    p.add(std::make_unique<AF>(pid("env_release"), "Release", Range(0.0f, 10000.0f), 1000.0f, FA{}.withLabel("ms")));
    p.add(std::make_unique<AB>(pid("detune_enable"), "Detune Enable", false));
    p.add(std::make_unique<AB>(pid("pitch1_enable"), "Voice 1 Enable", true));
    p.add(std::make_unique<AB>(pid("pitch2_enable"), "Voice 2 Enable", false));
    // Pitch engine — runtime choice between the granular reader (default,
    // matches the LV2 MOD/native sound) and the phase vocoder.
    p.add(std::make_unique<AC>(pid("pitch_mode"), "Pitch Engine",
                               juce::StringArray { "Granular", "Phase Vocoder" }, 0));

#ifdef MEGALO_HN_SYNTH
    // H+N timbre controls (MegaloHN build only).
    p.add(std::make_unique<AF>(pid("hn_brightness"), "Brightness", Range(-1.0f, 1.0f), 0.0f));
    p.add(std::make_unique<AF>(pid("hn_damping"),    "Damping",    Range( 0.0f, 1.0f), 0.0f));
    p.add(std::make_unique<AF>(pid("hn_even_odd"),   "Even/Odd",   Range(-1.0f, 1.0f), 0.0f));
    p.add(std::make_unique<AF>(pid("hn_noise"),      "Noise",      Range( 0.0f, 1.0f), 0.4f));
    p.add(std::make_unique<AF>(pid("hn_width"),      "Stereo Width", Range(0.0f, 1.0f), 0.3f));
#endif

    return p;
}

MegaloAudioProcessor::MegaloAudioProcessor()
    : AudioProcessor(BusesProperties()
        .withInput("Input",  juce::AudioChannelSet::stereo(), true)
        .withOutput("Output", juce::AudioChannelSet::stereo(), true)),
      apvts(*this, nullptr, "PARAMS", createLayout())
{
    auto raw = [this](const char* id){ return apvts.getRawParameterValue(id); };
    pThreshold   = raw("onset_threshold");
    pSampleMs    = raw("sample_ms");
    pAttackSkip  = raw("attack_skip_ms");
    pBlend       = raw("blend");
    pGrainMs     = raw("grain_size_ms");
    pGrainXfade  = raw("grain_xfade_ms");
    pBasePitch   = raw("base_pitch");
    pPitch1Semi  = raw("pitch1_semi");
    pPitch1Lvl   = raw("pitch1_level");
    pPitch2Semi  = raw("pitch2_semi");
    pPitch2Lvl   = raw("pitch2_level");
    pDetuneCt    = raw("detune_cents");
    pChorusRate  = raw("chorus_rate");
    pDetuneBlend = raw("detune_blend");
    pFiltType    = raw("filter_type");
    pFiltCutoff  = raw("filter_cutoff");
    pFiltQ       = raw("filter_q");
    pEnvAtk      = raw("env_attack");
    pEnvDcy      = raw("env_decay");
    pEnvSus      = raw("env_sustain");
    pEnvRel      = raw("env_release");
    pDetuneEn    = raw("detune_enable");
    pPitch1En    = raw("pitch1_enable");
    pPitch2En    = raw("pitch2_enable");
    pPitchMode   = raw("pitch_mode");
#ifdef MEGALO_HN_SYNTH
    pHnBright    = raw("hn_brightness");
    pHnDamp      = raw("hn_damping");
    pHnEvenOdd   = raw("hn_even_odd");
    pHnNoise     = raw("hn_noise");
    pHnWidth     = raw("hn_width");
#endif
}

MegaloAudioProcessor::~MegaloAudioProcessor()
{
    if (dsp) megalo_dsp_free(dsp);
}

void MegaloAudioProcessor::prepareToPlay(double sampleRate, int)
{
    // (Re)create the DSP if the sample rate changed — allocation happens here,
    // off the audio thread.
    if (dsp == nullptr || sampleRate != currentSampleRate) {
        if (dsp) megalo_dsp_free(dsp);
        dsp = megalo_dsp_new(sampleRate);
        currentSampleRate = sampleRate;
    }
    if (dsp) megalo_dsp_reset(dsp);
}

bool MegaloAudioProcessor::isBusesLayoutSupported(const BusesLayout& l) const
{
    const auto out = l.getMainOutputChannelSet();
    if (out != juce::AudioChannelSet::mono() && out != juce::AudioChannelSet::stereo())
        return false;
    // Mono engine: any mono/stereo input is summed to mono. Allow mono→stereo
    // (the decorrelated stereo path) in addition to mono→mono and stereo→stereo.
    const auto in = l.getMainInputChannelSet();
    return in == juce::AudioChannelSet::mono()
        || in == juce::AudioChannelSet::stereo()
        || in.isDisabled();
}

void MegaloAudioProcessor::processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    juce::ScopedNoDenormals noDenormals;
    const int n = buffer.getNumSamples();
    if (dsp == nullptr || n == 0) return;

    const MegaloParams p {
        pThreshold->load(), pSampleMs->load(), pAttackSkip->load(), pBlend->load(),
        pGrainMs->load(), pGrainXfade->load(), pBasePitch->load(),
        pPitch1Semi->load(), pPitch1Lvl->load(), pPitch2Semi->load(), pPitch2Lvl->load(),
        pDetuneCt->load(), pChorusRate->load(), pDetuneBlend->load(),
        pFiltType->load(), pFiltCutoff->load(), pFiltQ->load(),
        pEnvAtk->load(), pEnvDcy->load(), pEnvSus->load(), pEnvRel->load(),
        pDetuneEn->load(), pPitch1En->load(), pPitch2En->load(),
        pPitchMode->load(),
        1.0f,   // dry_level: temporary LV2-only tuning control, neutral here
#ifdef MEGALO_HN_SYNTH
        pHnBright->load(), pHnDamp->load(), pHnEvenOdd->load(),
        pHnNoise->load(), pHnWidth->load()
#endif
    };

    // Mono engine (guitar): sum the input to mono, process once, fan out.
    monoScratch.setSize(1, n, false, false, true);
    float* mono = monoScratch.getWritePointer(0);
    const int numIn = getTotalNumInputChannels();
    if (numIn > 0) {
        juce::FloatVectorOperations::copy(mono, buffer.getReadPointer(0), n);
        for (int ch = 1; ch < numIn; ++ch)
            juce::FloatVectorOperations::add(mono, buffer.getReadPointer(ch), n);
        if (numIn > 1)
            juce::FloatVectorOperations::multiply(mono, 1.0f / (float) numIn, n);
    } else {
        juce::FloatVectorOperations::clear(mono, n);
    }

    const int numOut = getTotalNumOutputChannels();
    if (numOut >= 2) {
        // Decorrelated stereo. mono (scratch) is distinct from the output
        // channels, so the in≠out requirement of the stereo path is met.
        float* outL = buffer.getWritePointer(0);
        float* outR = buffer.getWritePointer(1);
        megalo_dsp_process_stereo(dsp, &p, mono, outL, outR, (uint32_t) n);
        for (int ch = 2; ch < numOut; ++ch)
            juce::FloatVectorOperations::copy(buffer.getWritePointer(ch), outL, n);
    } else {
        megalo_dsp_process(dsp, &p, mono, mono, (uint32_t) n);
        for (int ch = 0; ch < numOut; ++ch)
            juce::FloatVectorOperations::copy(buffer.getWritePointer(ch), mono, n);
    }
    triggerPulse.store(megalo_dsp_trigger(dsp), std::memory_order_relaxed);
}

juce::AudioProcessorEditor* MegaloAudioProcessor::createEditor()
{
    return new MegaloEditor(*this);
}

// ── Factory presets ────────────────────────────────────────────────────────
// Unified with the LV2 .ttl presets (juce/megalo_presets.h). Each preset is a
// list of {symbol, value} pairs; values are applied through the matching APVTS
// parameter so host automation and the editor stay in sync. The HN-only timbre
// parameters are present in the table only for the MegaloHN build.
int MegaloAudioProcessor::getNumPrograms() { return megalo::kNumPresets; }

const juce::String MegaloAudioProcessor::getProgramName(int index)
{
    if (index < 0 || index >= megalo::kNumPresets) return {};
    return megalo::kPresets[index].name;
}

void MegaloAudioProcessor::setCurrentProgram(int index)
{
    if (index < 0 || index >= megalo::kNumPresets) return;
    currentProgram = index;

    const auto& preset = megalo::kPresets[index];
    for (int i = 0; i < preset.numParams; ++i)
    {
        const auto& pp = preset.params[i];
        if (auto* param = apvts.getParameter(pp.symbol))
            param->setValueNotifyingHost(param->convertTo0to1(pp.value));
    }
}

void MegaloAudioProcessor::getStateInformation(juce::MemoryBlock& dest)
{
    if (auto xml = apvts.copyState().createXml())
        copyXmlToBinary(*xml, dest);
}

void MegaloAudioProcessor::setStateInformation(const void* data, int size)
{
    if (auto xml = getXmlFromBinary(data, size))
        if (xml->hasTagName(apvts.state.getType()))
            apvts.replaceState(juce::ValueTree::fromXml(*xml));
}

// Mandatory entry point.
juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new MegaloAudioProcessor();
}
