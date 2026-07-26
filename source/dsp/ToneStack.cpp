#include "ToneStack.h"

#include <cmath>

namespace nr::dsp
{
namespace
{
/** Dial position that leaves a band flat. */
constexpr float centreDial = 5.0f;

/** dB of cut or boost at the extremes of each dial. Bass moves furthest and
    presence least, which is what makes the stack feel like an amp rather than
    four identical EQ bands. */
constexpr float bassDbPerStep = 4.0f;     // +/-20 dB
constexpr float middleDbPerStep = 3.0f;   // +/-15 dB
constexpr float trebleDbPerStep = 2.0f;   // +/-10 dB
constexpr float presenceDbPerStep = 3.0f; // +/-15 dB

constexpr float bassFrequency = 150.0f;
constexpr float middleFrequency = 425.0f;
constexpr float trebleFrequency = 1800.0f;
constexpr float presenceFrequency = 4000.0f;

constexpr float defaultQ = 0.707f;

/** A cut can afford to be wide; a boost at the same width drags the low mids
    with it and goes honky. Matches the stock NAM plugin. */
constexpr float middleCutQ = 1.5f;
constexpr float middleBoostQ = 0.7f;

float gainDbFor(float dial, float dbPerStep) noexcept
{
    return dbPerStep * (dial - centreDial);
}
} // namespace

void ToneStack::prepare(double sampleRate, int maxBlockSize, int numChannels)
{
    currentSampleRate = sampleRate;

    juce::dsp::ProcessSpec spec {};
    spec.sampleRate = sampleRate;
    spec.maximumBlockSize = static_cast<juce::uint32>(juce::jmax(1, maxBlockSize));
    spec.numChannels = static_cast<juce::uint32>(juce::jmax(1, numChannels));

    for (auto& filter : filters)
        filter.prepare(spec);

    coefficientsDirty = true;
    updateCoefficients();
}

void ToneStack::reset()
{
    for (auto& filter : filters)
        filter.reset();
}

void ToneStack::setControls(float bass, float middle, float treble, float presence)
{
    const std::array<float, numBands> incoming { bass, middle, treble, presence };

    for (size_t i = 0; i < incoming.size(); ++i)
    {
        // Recompute only on a real move: designing four biquads every block
        // would be wasted work on a rig where the dials rarely change.
        if (std::abs(incoming[i] - dials[i]) > 1.0e-4f)
        {
            dials[i] = incoming[i];
            coefficientsDirty = true;
        }
    }
}

void ToneStack::updateCoefficients()
{
    if (! coefficientsDirty || currentSampleRate <= 0.0)
        return;

    const auto middleGainDb = gainDbFor(dials[middleBand], middleDbPerStep);
    const auto middleQ = middleGainDb < 0.0f ? middleCutQ : middleBoostQ;

    // juce::dsp takes a linear gain factor, not decibels.
    const auto linear = [](float db) { return juce::Decibels::decibelsToGain(db); };

    *filters[bassBand].state = *juce::dsp::IIR::Coefficients<float>::makePeakFilter(
        currentSampleRate, bassFrequency, defaultQ, linear(gainDbFor(dials[bassBand], bassDbPerStep)));

    *filters[middleBand].state = *juce::dsp::IIR::Coefficients<float>::makePeakFilter(
        currentSampleRate, middleFrequency, middleQ, linear(middleGainDb));

    *filters[trebleBand].state = *juce::dsp::IIR::Coefficients<float>::makePeakFilter(
        currentSampleRate, trebleFrequency, defaultQ, linear(gainDbFor(dials[trebleBand], trebleDbPerStep)));

    *filters[presenceBand].state = *juce::dsp::IIR::Coefficients<float>::makeHighShelf(
        currentSampleRate, presenceFrequency, defaultQ, linear(gainDbFor(dials[presenceBand], presenceDbPerStep)));

    coefficientsDirty = false;
}

void ToneStack::process(juce::AudioBuffer<float>& buffer)
{
    if (currentSampleRate <= 0.0 || buffer.getNumSamples() == 0)
        return;

    updateCoefficients();

    juce::dsp::AudioBlock<float> block { buffer };
    juce::dsp::ProcessContextReplacing<float> context { block };

    for (auto& filter : filters)
        filter.process(context);
}

} // namespace nr::dsp
