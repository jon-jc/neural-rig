#include "RigChain.h"

#include <algorithm>
#include <cmath>

#include <juce_audio_basics/juce_audio_basics.h>

namespace nr::dsp
{
namespace
{
/** Headroom for the per-node delay lines. Resampling latency is a few hundred
    samples at most; this covers any host rate with room to spare. */
constexpr int maxNodeDelaySamples = 8192;

bool isEffectivelyDry(float mixPercent) noexcept
{
    return mixPercent >= 99.999f;
}
} // namespace

// --- Node -------------------------------------------------------------------

void RigChain::Node::prepareDelay(int maxDelaySamples)
{
    delayBuffer.assign(static_cast<size_t>(maxDelaySamples) + 1, 0.0f);
    resetDelay();
}

void RigChain::Node::resetDelay() noexcept
{
    std::fill(delayBuffer.begin(), delayBuffer.end(), 0.0f);
    writeIndex = 0;
    currentDelay = 0;
}

void RigChain::Node::runDelay(const float* input, float* output, int numSamples, int delaySamples) noexcept
{
    const auto capacity = static_cast<int>(delayBuffer.size());

    if (capacity <= 1)
    {
        if (input != output)
            std::copy(input, input + numSamples, output);

        return;
    }

    delaySamples = juce::jlimit(0, capacity - 1, delaySamples);

    // A changed delay would otherwise read from a region holding audio for the
    // old alignment, which clicks. Clearing costs one silent block instead.
    if (delaySamples != currentDelay)
    {
        std::fill(delayBuffer.begin(), delayBuffer.end(), 0.0f);
        currentDelay = delaySamples;
    }

    if (delaySamples == 0)
    {
        if (input != output)
            std::copy(input, input + numSamples, output);

        return;
    }

    for (int i = 0; i < numSamples; ++i)
    {
        delayBuffer[static_cast<size_t>(writeIndex)] = input[i];

        auto readIndex = writeIndex - delaySamples;

        if (readIndex < 0)
            readIndex += capacity;

        output[i] = delayBuffer[static_cast<size_t>(readIndex)];

        if (++writeIndex >= capacity)
            writeIndex = 0;
    }
}

// --- RigChain ---------------------------------------------------------------

void RigChain::prepare(double sampleRate, int maxBlockSize)
{
    currentSampleRate = sampleRate;
    currentBlockSize = maxBlockSize;

    for (auto& node : nodes)
    {
        node.prepareDelay(maxNodeDelaySamples);

        // The audio thread is stopped during prepare, so re-preparing the
        // capture already in play is safe.
        if (auto* model = node.slot.current())
            model->prepare(sampleRate, maxBlockSize);
    }

    dryScratch.assign(static_cast<size_t>(std::max(1, maxBlockSize)), 0.0f);
}

void RigChain::reset() noexcept
{
    for (auto& node : nodes)
        node.resetDelay();
}

int RigChain::latencySamples() const noexcept
{
    auto total = 0;

    for (const auto& node : nodes)
        if (const auto* model = node.slot.current())
            total += model->latencySamples();

    return total;
}

void RigChain::process(float* samples, int numSamples, const std::array<NodeSettings, numSlots>& settings)
{
    if (samples == nullptr || numSamples <= 0)
        return;

    applyPendingSwap();

    const auto scratchAvailable = numSamples <= static_cast<int>(dryScratch.size());

    for (int index = 0; index < numSlots; ++index)
    {
        auto& node = nodes[static_cast<size_t>(index)];
        auto* model = node.slot.acquire();

        // An empty slot contributes nothing at all, not even latency.
        if (model == nullptr)
            continue;

        const auto& setting = settings[static_cast<size_t>(index)];
        const auto nodeLatency = model->latencySamples();

        if (setting.bypassed)
        {
            // Carry the audio through a matching delay rather than skipping
            // the node, so the chain's total latency does not move when the
            // user toggles bypass.
            node.runDelay(samples, samples, numSamples, nodeLatency);
            continue;
        }

        const auto wantsDryBlend = ! isEffectivelyDry(setting.mixPercent) && scratchAvailable;

        // Always run the delay, even at 100% wet. It produces the dry copy the
        // blend needs, delayed to match what the model is about to add so the
        // two do not comb; and running it unconditionally keeps the line
        // primed, so toggling bypass or easing the mix off starts from real
        // history instead of silence.
        node.runDelay(samples, dryScratch.data(), numSamples, nodeLatency);

        model->process(samples, numSamples);

        if (wantsDryBlend)
        {
            const auto wet = juce::jlimit(0.0f, 1.0f, setting.mixPercent * 0.01f);
            const auto dry = 1.0f - wet;

            for (int i = 0; i < numSamples; ++i)
                samples[i] = samples[i] * wet + dryScratch[static_cast<size_t>(i)] * dry;
        }

        if (std::abs(setting.gainDb) > 1.0e-4f)
        {
            const auto gain = juce::Decibels::decibelsToGain(setting.gainDb);
            juce::FloatVectorOperations::multiply(samples, gain, numSamples);
        }
    }
}

void RigChain::stage(int slotIndex, std::unique_ptr<NamModel> model)
{
    if (! isValidSlot(slotIndex))
        return;

    nodes[static_cast<size_t>(slotIndex)].slot.stage(std::move(model));
}

void RigChain::clear(int slotIndex)
{
    if (! isValidSlot(slotIndex))
        return;

    nodes[static_cast<size_t>(slotIndex)].slot.requestClear();
}

void RigChain::requestSwap(int firstSlot, int secondSlot)
{
    if (! isValidSlot(firstSlot) || ! isValidSlot(secondSlot) || firstSlot == secondSlot)
        return;

    const auto encoded = (static_cast<std::uint32_t>(firstSlot + 1) << 8)
                         | static_cast<std::uint32_t>(secondSlot + 1);

    pendingSwap.store(encoded, std::memory_order_release);
}

void RigChain::applyPendingSwap() noexcept
{
    const auto encoded = pendingSwap.exchange(0, std::memory_order_acq_rel);

    if (encoded == 0)
        return;

    const auto first = static_cast<int>((encoded >> 8) & 0xFFu) - 1;
    const auto second = static_cast<int>(encoded & 0xFFu) - 1;

    if (! isValidSlot(first) || ! isValidSlot(second) || first == second)
        return;

    nodes[static_cast<size_t>(first)].slot.swapActiveWith(nodes[static_cast<size_t>(second)].slot);

    // Each node's delay line now holds history for the wrong capture, and the
    // required delay has probably changed too. Clearing costs one quiet block
    // and avoids a click.
    nodes[static_cast<size_t>(first)].resetDelay();
    nodes[static_cast<size_t>(second)].resetDelay();
}

void RigChain::collectRetired()
{
    for (auto& node : nodes)
        node.slot.collectRetired();
}

const NamModel* RigChain::modelAt(int slotIndex) const noexcept
{
    if (! isValidSlot(slotIndex))
        return nullptr;

    return nodes[static_cast<size_t>(slotIndex)].slot.current();
}

const NamModel* RigChain::firstLoaded() const noexcept
{
    for (const auto& node : nodes)
        if (const auto* model = node.slot.current())
            return model;

    return nullptr;
}

const NamModel* RigChain::lastLoaded() const noexcept
{
    for (auto index = static_cast<int>(nodes.size()) - 1; index >= 0; --index)
        if (const auto* model = nodes[static_cast<size_t>(index)].slot.current())
            return model;

    return nullptr;
}

} // namespace nr::dsp
