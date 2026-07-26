#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

#include "Parameters.h"
#include "dsp/LevelCalibration.h"
#include "dsp/NoiseGate.h"
#include "dsp/RigChain.h"
#include "dsp/ToneStack.h"

namespace nr
{

/**
    NeuralRig's AudioProcessor.

    Signal path, all mono internally because a NAM capture is mono:

        input trim (calibrated) -> gate -> capture chain -> tone stack -> output trim

    The chain holds up to four captures in series. Stacking them is the point:
    an overdrive capture feeding an amp capture behaves far more like the real
    pairing than either alone, because the second network sees the first one's
    actual output rather than a clean guitar signal.

    The gate sits ahead of the capture deliberately. Silence fed into a neural
    amp comes back as that amp's own noise floor, so gating afterwards means
    fighting noise the model just generated.

    Model loading happens on a background thread and is handed to the audio
    thread through ModelSlot, which never allocates or frees on the audio side.
*/
class NeuralRigProcessor final : public juce::AudioProcessor,
                                 private juce::Timer
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

    /** Loads a .nam into a slot in the background and swaps it in when ready.
        Safe to call from the message thread; returns immediately. */
    void loadModel(int slot, const juce::File& file);

    /** Empties a slot, removing it from the chain. */
    void clearModel(int slot);

    /** Reorders the rig. A drive capture before an amp capture is a different
        instrument from the reverse, so this is a musical control, not a
        cosmetic one. */
    void swapSlots(int firstSlot, int secondSlot);

    /** Name of the capture in a slot, or an empty string. Message thread. */
    juce::String loadedModelName(int slot) const;

    /** Reason the last load into a slot failed, or empty. Message thread. */
    juce::String lastLoadError(int slot) const;

    /** Bumped whenever a load completes, so the editor can refresh without
        polling strings. */
    int modelChangeCount() const noexcept { return modelGeneration.load(std::memory_order_relaxed); }

    float inputPeakDb() const noexcept { return inputPeak.load(std::memory_order_relaxed); }
    float outputPeakDb() const noexcept { return outputPeak.load(std::memory_order_relaxed); }
    float gateReductionDb() const noexcept { return gateReduction.load(std::memory_order_relaxed); }

private:
    /** Caches raw atomic pointers to every parameter. Looking them up by
        string ID inside processBlock() would hash on the audio thread. */
    struct ParameterHandles
    {
        void attachTo(juce::AudioProcessorValueTreeState&);

        std::atomic<float>* inputLevel = nullptr;
        std::atomic<float>* outputLevel = nullptr;
        std::atomic<float>* mix = nullptr;
        std::atomic<float>* gateEnabled = nullptr;
        std::atomic<float>* gateThreshold = nullptr;
        std::atomic<float>* eqEnabled = nullptr;
        std::atomic<float>* bass = nullptr;
        std::atomic<float>* mid = nullptr;
        std::atomic<float>* treble = nullptr;
        std::atomic<float>* presence = nullptr;
        std::atomic<float>* outputMode = nullptr;
        std::atomic<float>* calibrateInput = nullptr;
        std::atomic<float>* inputCalibrationLevel = nullptr;

        struct Slot
        {
            std::atomic<float>* enabled = nullptr;
            std::atomic<float>* gain = nullptr;
            std::atomic<float>* mix = nullptr;
        };

        std::array<Slot, params::numSlots> slots {};
    };

    void timerCallback() override;
    void refreshGains() noexcept;
    void publishLatency();
    std::array<dsp::NodeSettings, dsp::RigChain::numSlots> readSlotSettings() const noexcept;

    juce::AudioProcessorValueTreeState apvts;
    ParameterHandles handles;

    dsp::RigChain chain;
    dsp::NoiseGate gate;
    dsp::ToneStack toneStack;

    juce::LinearSmoothedValue<float> inputGain { 1.0f };
    juce::LinearSmoothedValue<float> outputGain { 1.0f };
    juce::LinearSmoothedValue<float> wetAmount { 1.0f };

    // NAM is mono, so the chain runs on one channel and fans out at the end.
    juce::AudioBuffer<float> monoBuffer;
    juce::AudioBuffer<float> dryBuffer;

    double currentSampleRate = 0.0;
    int currentBlockSize = 0;
    int reportedLatency = -1;

    std::atomic<float> inputPeak { -100.0f };
    std::atomic<float> outputPeak { -100.0f };
    std::atomic<float> gateReduction { 0.0f };
    std::atomic<int> modelGeneration { 0 };

    // Guards the strings below, which the loader threads write and the message
    // thread reads.
    mutable juce::CriticalSection modelInfoLock;
    std::array<juce::String, params::numSlots> slotNames;
    std::array<juce::String, params::numSlots> slotErrors;

    // Serialises background loads so two rapid requests cannot race.
    juce::ThreadPool loaderPool { 1 };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(NeuralRigProcessor)
};

} // namespace nr
