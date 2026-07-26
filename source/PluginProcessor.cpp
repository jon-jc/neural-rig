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

float peakDbOf(const juce::AudioBuffer<float>& buffer)
{
    float magnitude = 0.0f;

    for (int channel = 0; channel < buffer.getNumChannels(); ++channel)
        magnitude = juce::jmax(magnitude, buffer.getMagnitude(channel, 0, buffer.getNumSamples()));

    return magnitude > 0.0f ? juce::Decibels::gainToDecibels(magnitude, minimumMeterDb)
                            : minimumMeterDb;
}
} // namespace

void NeuralRigProcessor::ParameterHandles::attachTo(juce::AudioProcessorValueTreeState& tree)
{
    inputLevel = tree.getRawParameterValue(params::id::inputLevel);
    outputLevel = tree.getRawParameterValue(params::id::outputLevel);
    mix = tree.getRawParameterValue(params::id::mix);

    jassert(inputLevel != nullptr && outputLevel != nullptr && mix != nullptr);
}

NeuralRigProcessor::NeuralRigProcessor()
    : juce::AudioProcessor(BusesProperties()
                               .withInput("Input", juce::AudioChannelSet::stereo(), true)
                               .withOutput("Output", juce::AudioChannelSet::stereo(), true)),
      apvts(*this, nullptr, "NEURALRIG", params::createParameterLayout())
{
    handles.attachTo(apvts);
}

NeuralRigProcessor::~NeuralRigProcessor() = default;

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
    for (auto* smoothed : { &inputGain, &outputGain, &wetAmount })
        smoothed->reset(sampleRate, gainRampSeconds);

    updateSmoothedTargets();
    inputGain.setCurrentAndTargetValue(inputGain.getTargetValue());
    outputGain.setCurrentAndTargetValue(outputGain.getTargetValue());
    wetAmount.setCurrentAndTargetValue(wetAmount.getTargetValue());

    dryBuffer.setSize(getTotalNumOutputChannels(), maximumExpectedSamplesPerBlock, false, false, true);
    dryBuffer.clear();

    inputPeak.store(minimumMeterDb, std::memory_order_relaxed);
    outputPeak.store(minimumMeterDb, std::memory_order_relaxed);
}

void NeuralRigProcessor::releaseResources()
{
    dryBuffer.setSize(0, 0);
}

void NeuralRigProcessor::updateSmoothedTargets()
{
    inputGain.setTargetValue(juce::Decibels::decibelsToGain(handles.inputLevel->load(std::memory_order_relaxed)));
    outputGain.setTargetValue(juce::Decibels::decibelsToGain(handles.outputLevel->load(std::memory_order_relaxed)));
    wetAmount.setTargetValue(handles.mix->load(std::memory_order_relaxed) * 0.01f);
}

void NeuralRigProcessor::processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    juce::ScopedNoDenormals noDenormals;

    const auto numSamples = buffer.getNumSamples();
    const auto numInputChannels = getTotalNumInputChannels();
    const auto numOutputChannels = getTotalNumOutputChannels();

    // Feeding a mono source into a stereo bus leaves the upper channels holding
    // whatever the host last put there; clear them before they reach the meters.
    for (int channel = numInputChannels; channel < numOutputChannels; ++channel)
        buffer.clear(channel, 0, numSamples);

    if (numSamples == 0 || numOutputChannels == 0)
        return;

    updateSmoothedTargets();

    // --- Input trim ---------------------------------------------------------
    {
        auto gain = inputGain;

        for (int sample = 0; sample < numSamples; ++sample)
        {
            const auto value = gain.getNextValue();

            for (int channel = 0; channel < numOutputChannels; ++channel)
                buffer.getWritePointer(channel)[sample] *= value;
        }

        inputGain.skip(numSamples);
    }

    inputPeak.store(peakDbOf(buffer), std::memory_order_relaxed);

    // Stash the post-trim signal so the dry side of the mix control tracks the
    // input trim. Mixing against the pre-trim signal would make the blend jump
    // whenever the input knob moved.
    dryBuffer.makeCopyOf(buffer, true);

    // --- Neural chain -------------------------------------------------------
    // Milestone 2 inserts the NAM chain here. Until then the signal is passed
    // through untouched so the surrounding staging can be exercised for real.

    // --- Dry/wet ------------------------------------------------------------
    for (int sample = 0; sample < numSamples; ++sample)
    {
        const auto wet = wetAmount.getNextValue();
        const auto dry = 1.0f - wet;

        for (int channel = 0; channel < numOutputChannels; ++channel)
        {
            auto* data = buffer.getWritePointer(channel);
            data[sample] = data[sample] * wet + dryBuffer.getReadPointer(channel)[sample] * dry;
        }
    }

    // --- Output trim --------------------------------------------------------
    for (int sample = 0; sample < numSamples; ++sample)
    {
        const auto value = outputGain.getNextValue();

        for (int channel = 0; channel < numOutputChannels; ++channel)
            buffer.getWritePointer(channel)[sample] *= value;
    }

    outputPeak.store(peakDbOf(buffer), std::memory_order_relaxed);
}

juce::AudioProcessorEditor* NeuralRigProcessor::createEditor()
{
    return new NeuralRigEditor(*this);
}

void NeuralRigProcessor::getStateInformation(juce::MemoryBlock& destData)
{
    if (auto tree = apvts.copyState(); tree.isValid())
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
