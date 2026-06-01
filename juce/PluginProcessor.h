#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include "megalo_dsp.h"

// Thin JUCE wrapper around the host-agnostic DSP core (src/megalo_dsp.{h,cpp}),
// the same core compiled into the LV2 plugin. Parameters use APVTS with IDs
// identical to the LV2 .ttl symbols, so state stays consistent across formats.
class MegaloAudioProcessor : public juce::AudioProcessor
{
public:
    MegaloAudioProcessor();
    ~MegaloAudioProcessor() override;

    void prepareToPlay(double sampleRate, int samplesPerBlock) override;
    void releaseResources() override {}
    bool isBusesLayoutSupported(const BusesLayout&) const override;
    void processBlock(juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    const juce::String getName() const override { return "Megalo"; }
    bool acceptsMidi() const override  { return false; }
    bool producesMidi() const override { return false; }
    bool isMidiEffect() const override { return false; }
    double getTailLengthSeconds() const override { return 0.0; }

    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram(int) override {}
    const juce::String getProgramName(int) override { return {}; }
    void changeProgramName(int, const juce::String&) override {}

    void getStateInformation(juce::MemoryBlock&) override;
    void setStateInformation(const void*, int) override;

    juce::AudioProcessorValueTreeState apvts;

private:
    static juce::AudioProcessorValueTreeState::ParameterLayout createLayout();

    MegaloDsp* dsp = nullptr;
    double currentSampleRate = 0.0;
    juce::AudioBuffer<float> monoScratch;

    // Cached raw-parameter atomics (denormalised values, ready for MegaloParams).
    std::atomic<float>* pThreshold   = nullptr;
    std::atomic<float>* pSampleMs    = nullptr;
    std::atomic<float>* pAttackSkip  = nullptr;
    std::atomic<float>* pBlend       = nullptr;
    std::atomic<float>* pGrainMs     = nullptr;
    std::atomic<float>* pGrainXfade  = nullptr;
    std::atomic<float>* pBasePitch   = nullptr;
    std::atomic<float>* pPitch1Semi  = nullptr;
    std::atomic<float>* pPitch1Lvl   = nullptr;
    std::atomic<float>* pPitch2Semi  = nullptr;
    std::atomic<float>* pPitch2Lvl   = nullptr;
    std::atomic<float>* pDetuneCt    = nullptr;
    std::atomic<float>* pChorusRate  = nullptr;
    std::atomic<float>* pDetuneBlend = nullptr;
    std::atomic<float>* pFiltType    = nullptr;
    std::atomic<float>* pFiltCutoff  = nullptr;
    std::atomic<float>* pFiltQ       = nullptr;
    std::atomic<float>* pEnvAtk      = nullptr;
    std::atomic<float>* pEnvDcy      = nullptr;
    std::atomic<float>* pEnvSus      = nullptr;
    std::atomic<float>* pEnvRel      = nullptr;
    std::atomic<float>* pDetuneEn    = nullptr;
    std::atomic<float>* pPitch1En    = nullptr;
    std::atomic<float>* pPitch2En    = nullptr;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(MegaloAudioProcessor)
};
