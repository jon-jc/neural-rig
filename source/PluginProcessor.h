#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

#include "Parameters.h"

namespace nr
{

/**
    NeuralRig's AudioProcessor.

    At this milestone the audio path is I/O staging only: input trim, dry/wet
    mix and output trim, with the neural chain still to come. The parameter
    tree, bus layout and state serialisation are already the real ones, so
    later milestones drop the chain in without reshaping the host-facing API.
*/
class NeuralRigProcessor final : public juce::AudioProcessor
{
public:
    NeuralRigProcessor();
    ~NeuralRigProcessor() override;

    // --- AudioProcessor -----------------------------------------------------
    void prepareToPlay(double sampleRate, int maximumExpectedSamplesPerBlock) override;
    void releaseResources() override;
    bool isBusesLayoutSupported(const BusesLayout& layouts) const override;

    // We only implement the single-precision path. Without this using
    // declaration the double-precision overload is merely hidden rather than
    // inherited, which GCC reports as -Woverloaded-virtual.
    using juce::AudioProcessor::processBlock;
    void processBlock(juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    const juce::String getName() const override { return JucePlugin_Name; }
    bool acceptsMidi() const override { return false; }
    bool producesMidi() const override { return false; }
    bool isMidiEffect() const override { return false; }
    double getTailLengthSeconds() const override { return 0.0; }

    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram(int) override {}
    const juce::String getProgramName(int) override { return "Default"; }
    void changeProgramName(int, const juce::String&) override {}

    void getStateInformation(juce::MemoryBlock&) override;
    void setStateInformation(const void*, int) override;

    // --- NeuralRig ----------------------------------------------------------
    juce::AudioProcessorValueTreeState& state() noexcept { return apvts; }

    /** Peak input level in dB for the editor's meter. Written by the audio
        thread, read by the UI; relaxed atomics are fine for a meter. */
    float inputPeakDb() const noexcept { return inputPeak.load(std::memory_order_relaxed); }
    float outputPeakDb() const noexcept { return outputPeak.load(std::memory_order_relaxed); }

private:
    /** Caches raw atomic pointers to every parameter. Looking parameters up by
        string ID inside processBlock() would hash on the audio thread. */
    struct ParameterHandles
    {
        void attachTo(juce::AudioProcessorValueTreeState&);

        std::atomic<float>* inputLevel = nullptr;
        std::atomic<float>* outputLevel = nullptr;
        std::atomic<float>* mix = nullptr;
    };

    void updateSmoothedTargets();

    juce::AudioProcessorValueTreeState apvts;
    ParameterHandles handles;

    juce::LinearSmoothedValue<float> inputGain { 1.0f };
    juce::LinearSmoothedValue<float> outputGain { 1.0f };
    juce::LinearSmoothedValue<float> wetAmount { 1.0f };

    juce::AudioBuffer<float> dryBuffer;

    std::atomic<float> inputPeak { -100.0f };
    std::atomic<float> outputPeak { -100.0f };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(NeuralRigProcessor)
};

} // namespace nr
