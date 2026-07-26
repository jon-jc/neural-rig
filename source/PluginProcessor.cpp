#include "PluginProcessor.h"

#include "PluginEditor.h"

namespace nr
{
namespace
{
/** Ramp time for every user-facing gain. Long enough to be inaudible on a fast
    knob sweep, short enough not to smear a deliberate jump. */
constexpr double gainRampSeconds = 0.02;

constexpr float minimumMeterDb = -100.0f;

/** How often the message thread reclaims retired captures and republishes
    latency. Frequent enough to feel instant, rare enough to cost nothing. */
constexpr int housekeepingHz = 8;

float peakDbOf(const float* data, int numSamples)
{
    auto magnitude = 0.0f;

    for (int i = 0; i < numSamples; ++i)
        magnitude = juce::jmax(magnitude, std::abs(data[i]));

    return magnitude > 0.0f ? juce::Decibels::gainToDecibels(magnitude, minimumMeterDb) : minimumMeterDb;
}
} // namespace

void NeuralRigProcessor::ParameterHandles::attachTo(juce::AudioProcessorValueTreeState& tree)
{
    inputLevel = tree.getRawParameterValue(params::id::inputLevel);
    outputLevel = tree.getRawParameterValue(params::id::outputLevel);
    mix = tree.getRawParameterValue(params::id::mix);
    gateEnabled = tree.getRawParameterValue(params::id::gateEnabled);
    gateThreshold = tree.getRawParameterValue(params::id::gateThreshold);
    eqEnabled = tree.getRawParameterValue(params::id::eqEnabled);
    bass = tree.getRawParameterValue(params::id::bass);
    mid = tree.getRawParameterValue(params::id::mid);
    treble = tree.getRawParameterValue(params::id::treble);
    presence = tree.getRawParameterValue(params::id::presence);
    outputMode = tree.getRawParameterValue(params::id::outputMode);
    calibrateInput = tree.getRawParameterValue(params::id::calibrateInput);
    inputCalibrationLevel = tree.getRawParameterValue(params::id::inputCalibrationLevel);

    jassert(inputLevel != nullptr && outputLevel != nullptr && mix != nullptr);
    jassert(outputMode != nullptr && inputCalibrationLevel != nullptr);
}

NeuralRigProcessor::NeuralRigProcessor()
    : juce::AudioProcessor(BusesProperties()
                               .withInput("Input", juce::AudioChannelSet::stereo(), true)
                               .withOutput("Output", juce::AudioChannelSet::stereo(), true)),
      apvts(*this, nullptr, "NEURALRIG", params::createParameterLayout())
{
    handles.attachTo(apvts);
    startTimerHz(housekeepingHz);
}

NeuralRigProcessor::~NeuralRigProcessor()
{
    stopTimer();
    // Finish any in-flight load before ModelSlot is torn down underneath it.
    loaderPool.removeAllJobs(true, 4000);
}

bool NeuralRigProcessor::isBusesLayoutSupported(const BusesLayout& layouts) const
{
    const auto& in = layouts.getMainInputChannelSet();
    const auto& out = layouts.getMainOutputChannelSet();

    if (in.isDisabled() || out.isDisabled())
        return false;

    // A guitar rig is fed mono and monitored in mono or stereo. Anything wider
    // than stereo, or narrowing from stereo to mono, is not a layout we model.
    const bool inputOk = in == juce::AudioChannelSet::mono() || in == juce::AudioChannelSet::stereo();
    const bool outputOk = out == juce::AudioChannelSet::mono() || out == juce::AudioChannelSet::stereo();

    return inputOk && outputOk && in.size() <= out.size();
}

void NeuralRigProcessor::prepareToPlay(double sampleRate, int maximumExpectedSamplesPerBlock)
{
    currentSampleRate = sampleRate;
    currentBlockSize = maximumExpectedSamplesPerBlock;

    for (auto* smoothed : { &inputGain, &outputGain, &wetAmount })
        smoothed->reset(sampleRate, gainRampSeconds);

    gate.prepare(sampleRate, maximumExpectedSamplesPerBlock);
    toneStack.prepare(sampleRate, maximumExpectedSamplesPerBlock, 1);

    monoBuffer.setSize(1, maximumExpectedSamplesPerBlock, false, false, true);
    dryBuffer.setSize(1, maximumExpectedSamplesPerBlock, false, false, true);
    monoBuffer.clear();
    dryBuffer.clear();

    // The active capture was prepared for the previous rate and block size.
    // Re-preparing here is safe: the audio thread is stopped during
    // prepareToPlay, so nothing is reading it.
    if (auto* model = modelSlot.current())
    {
        model->prepare(sampleRate, maximumExpectedSamplesPerBlock);
        publishLatencyFor(model);
    }

    refreshGainsFor(modelSlot.current());
    inputGain.setCurrentAndTargetValue(inputGain.getTargetValue());
    outputGain.setCurrentAndTargetValue(outputGain.getTargetValue());
    wetAmount.setCurrentAndTargetValue(wetAmount.getTargetValue());

    inputPeak.store(minimumMeterDb, std::memory_order_relaxed);
    outputPeak.store(minimumMeterDb, std::memory_order_relaxed);
}

void NeuralRigProcessor::releaseResources()
{
    monoBuffer.setSize(0, 0);
    dryBuffer.setSize(0, 0);
}

void NeuralRigProcessor::refreshGainsFor(const dsp::NamModel* model) noexcept
{
    dsp::CalibrationSettings settings;
    settings.inputTrimDb = handles.inputLevel->load(std::memory_order_relaxed);
    settings.outputTrimDb = handles.outputLevel->load(std::memory_order_relaxed);
    settings.calibrateInput = handles.calibrateInput->load(std::memory_order_relaxed) > 0.5f;
    settings.inputCalibrationDbu = handles.inputCalibrationLevel->load(std::memory_order_relaxed);
    settings.outputMode =
        static_cast<dsp::OutputMode>(juce::roundToInt(handles.outputMode->load(std::memory_order_relaxed)));

    const auto* info = model != nullptr ? &model->info() : nullptr;

    inputGain.setTargetValue(juce::Decibels::decibelsToGain(dsp::computeInputGainDb(settings, info)));
    outputGain.setTargetValue(juce::Decibels::decibelsToGain(dsp::computeOutputGainDb(settings, info)));
    wetAmount.setTargetValue(handles.mix->load(std::memory_order_relaxed) * 0.01f);
}

void NeuralRigProcessor::processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    juce::ScopedNoDenormals noDenormals;

    const auto numSamples = buffer.getNumSamples();
    const auto numInputChannels = getTotalNumInputChannels();
    const auto numOutputChannels = getTotalNumOutputChannels();

    for (int channel = numInputChannels; channel < numOutputChannels; ++channel)
        buffer.clear(channel, 0, numSamples);

    if (numSamples == 0 || numOutputChannels == 0 || numSamples > monoBuffer.getNumSamples())
        return;

    // Pick up any capture the loader thread has published. Never allocates.
    auto* model = modelSlot.acquire();

    refreshGainsFor(model);

    // --- Fold to mono -------------------------------------------------------
    // The rig is mono end to end; a stereo source is summed rather than run
    // twice, which would double the CPU for a capture that cannot be stereo.
    auto* mono = monoBuffer.getWritePointer(0);

    if (numInputChannels >= 2)
    {
        const auto* left = buffer.getReadPointer(0);
        const auto* right = buffer.getReadPointer(1);

        for (int i = 0; i < numSamples; ++i)
            mono[i] = 0.5f * (left[i] + right[i]);
    }
    else
    {
        juce::FloatVectorOperations::copy(mono, buffer.getReadPointer(0), numSamples);
    }

    // --- Input trim ---------------------------------------------------------
    for (int i = 0; i < numSamples; ++i)
        mono[i] *= inputGain.getNextValue();

    inputPeak.store(peakDbOf(mono, numSamples), std::memory_order_relaxed);

    // Keep the post-trim signal so the dry side of the mix tracks the input
    // knob; blending against the pre-trim signal would make the balance jump
    // whenever that knob moved.
    juce::FloatVectorOperations::copy(dryBuffer.getWritePointer(0), mono, numSamples);

    // --- Gate ---------------------------------------------------------------
    gate.setEnabled(handles.gateEnabled->load(std::memory_order_relaxed) > 0.5f);
    gate.setThreshold(handles.gateThreshold->load(std::memory_order_relaxed));
    gate.process(mono, numSamples);
    gateReduction.store(gate.gainReductionDb(), std::memory_order_relaxed);

    // --- Capture ------------------------------------------------------------
    if (model != nullptr)
        model->process(mono, numSamples);

    // --- Tone stack ---------------------------------------------------------
    if (handles.eqEnabled->load(std::memory_order_relaxed) > 0.5f)
    {
        toneStack.setControls(handles.bass->load(std::memory_order_relaxed),
                              handles.mid->load(std::memory_order_relaxed),
                              handles.treble->load(std::memory_order_relaxed),
                              handles.presence->load(std::memory_order_relaxed));

        juce::AudioBuffer<float> view { monoBuffer.getArrayOfWritePointers(), 1, numSamples };
        toneStack.process(view);
    }

    // --- Mix and output trim ------------------------------------------------
    const auto* dry = dryBuffer.getReadPointer(0);

    for (int i = 0; i < numSamples; ++i)
    {
        const auto wet = wetAmount.getNextValue();
        mono[i] = (mono[i] * wet + dry[i] * (1.0f - wet)) * outputGain.getNextValue();
    }

    outputPeak.store(peakDbOf(mono, numSamples), std::memory_order_relaxed);

    // --- Fan out ------------------------------------------------------------
    for (int channel = 0; channel < numOutputChannels; ++channel)
        juce::FloatVectorOperations::copy(buffer.getWritePointer(channel), mono, numSamples);
}

void NeuralRigProcessor::timerCallback()
{
    // Destroy anything the audio thread handed back. Freeing a network's
    // weights is unbounded work and must never happen mid-block.
    modelSlot.collectRetired();
}

void NeuralRigProcessor::publishLatencyFor(const dsp::NamModel* model)
{
    const auto latency = model != nullptr ? model->latencySamples() : 0;

    if (latency != reportedLatency)
    {
        reportedLatency = latency;
        setLatencySamples(latency);
    }
}

void NeuralRigProcessor::loadModel(const juce::File& file)
{
    // Not named sampleRate/blockSize: juce::AudioProcessor has members by
    // those names, and shadowing them is an error under our warning settings.
    const auto preparedRate = currentSampleRate > 0.0 ? currentSampleRate : 48000.0;
    const auto preparedBlock = currentBlockSize > 0 ? currentBlockSize : 512;

    loaderPool.addJob([this, file, preparedRate, preparedBlock] {
        juce::String error;
        auto model = dsp::NamModel::loadFromFile(file, error);

        if (model != nullptr)
        {
            // Prepare here, on the loader thread: this allocates and runs
            // inference to prewarm the network, so it must not touch the audio
            // thread. By the time it is staged it is ready to play.
            model->prepare(preparedRate, preparedBlock);
        }

        const auto name = model != nullptr ? model->info().name : juce::String {};

        {
            const juce::ScopedLock lock(modelInfoLock);
            modelName = name;
            loadError = error;
        }

        if (model != nullptr)
            modelSlot.stage(std::move(model));

        modelGeneration.fetch_add(1, std::memory_order_relaxed);

        juce::MessageManager::callAsync([this] {
            if (auto* staged = modelSlot.current())
                publishLatencyFor(staged);
        });
    });
}

void NeuralRigProcessor::clearModel()
{
    modelSlot.requestClear();

    {
        const juce::ScopedLock lock(modelInfoLock);
        modelName.clear();
        loadError.clear();
    }

    modelGeneration.fetch_add(1, std::memory_order_relaxed);
    publishLatencyFor(nullptr);
}

juce::String NeuralRigProcessor::loadedModelName() const
{
    const juce::ScopedLock lock(modelInfoLock);
    return modelName;
}

juce::String NeuralRigProcessor::lastLoadError() const
{
    const juce::ScopedLock lock(modelInfoLock);
    return loadError;
}

juce::AudioProcessorEditor* NeuralRigProcessor::createEditor()
{
    return new NeuralRigEditor(*this);
}

void NeuralRigProcessor::getStateInformation(juce::MemoryBlock& destData)
{
    auto tree = apvts.copyState();

    // Remember which capture was loaded so a reopened session comes back with
    // the same amp rather than a clean signal.
    if (auto* model = modelSlot.current())
        tree.setProperty("modelPath", model->info().name, nullptr);

    if (auto xml = tree.createXml())
        copyXmlToBinary(*xml, destData);
}

void NeuralRigProcessor::setStateInformation(const void* data, int sizeInBytes)
{
    auto xml = getXmlFromBinary(data, sizeInBytes);

    if (xml == nullptr || ! xml->hasTagName(apvts.state.getType()))
        return;

    apvts.replaceState(juce::ValueTree::fromXml(*xml));
}

} // namespace nr

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new nr::NeuralRigProcessor();
}
