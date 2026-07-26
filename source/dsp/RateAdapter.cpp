#include "RateAdapter.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace nr::dsp
{
namespace
{
/** Hosts report 44100.0 and 44100.00000001 interchangeably, so rates are
    compared with a tolerance. A direct == would also trip -Wfloat-equal. */
constexpr double rateEpsilon = 1.0e-6;

bool ratesMatch(double a, double b) noexcept
{
    return std::abs(a - b) < rateEpsilon;
}

/** Zero samples held in each FIFO ahead of the first real block. Without this
    priming the very first call can ask an interpolator for more input than has
    arrived yet. It is deliberately small — it is pure added latency. */
constexpr int primingSamples = 8;
} // namespace

void RateAdapter::consumeFront(std::vector<float>& fifo, int& fill, int count) noexcept
{
    count = juce::jlimit(0, fill, count);

    if (count == 0)
        return;

    const auto remaining = fill - count;

    if (remaining > 0)
        std::memmove(fifo.data(), fifo.data() + count, static_cast<size_t>(remaining) * sizeof(float));

    fill = remaining;
}

void RateAdapter::appendOuter(const float* source, int count) noexcept
{
    const auto capacity = static_cast<int>(outerFifo.size());
    count = std::min(count, capacity - outerFill);

    if (count <= 0)
        return;

    std::memcpy(outerFifo.data() + outerFill, source, static_cast<size_t>(count) * sizeof(float));
    outerFill += count;
}

void RateAdapter::prepare(double outerSampleRate, double innerSampleRate, int maxOuterBlock)
{
    outerRate = outerSampleRate;
    innerRate = innerSampleRate;
    maxOuterBlockSize = std::max(maxOuterBlock, 1);
    latency = 0;

    converting = outerRate > 0.0 && innerRate > 0.0 && ! ratesMatch(outerRate, innerRate);

    if (! converting)
    {
        outerFifo.clear();
        innerFifo.clear();
        innerBlock.clear();
        outerFill = innerFill = 0;
        return;
    }

    outerPerInner = outerRate / innerRate;
    innerPerOuter = innerRate / outerRate;

    // Worst case inner-rate samples generated for one outer-rate block, plus
    // slack for the fractional carry landing on the high side.
    const auto maxInnerBlock =
        static_cast<int>(std::ceil(static_cast<double>(maxOuterBlockSize) * innerPerOuter)) + 2;

    // Each FIFO holds priming, a full block, and one block of slack so that a
    // host handing us short blocks never forces a reallocation.
    const auto sincWindow = static_cast<int>(juce::Interpolators::WindowedSinc::getBaseLatency()) * 2 + 4;

    outerFifo.assign(static_cast<size_t>(maxOuterBlockSize * 2 + primingSamples + sincWindow), 0.0f);
    innerFifo.assign(static_cast<size_t>(maxInnerBlock * 2 + primingSamples + sincWindow), 0.0f);
    innerBlock.assign(static_cast<size_t>(maxInnerBlock), 0.0f);

    resetState();
    latency = measureLatency();
    resetState();
}

void RateAdapter::resetState() noexcept
{
    innerSampleCarry = 0.0;

    if (! converting)
    {
        outerFill = innerFill = 0;
        return;
    }

    upsampler.reset();
    downsampler.reset();

    std::fill(outerFifo.begin(), outerFifo.end(), 0.0f);
    std::fill(innerFifo.begin(), innerFifo.end(), 0.0f);

    // Prime both sides with silence so the first block cannot underrun.
    outerFill = primingSamples;
    innerFill = std::max(1, static_cast<int>(std::ceil(static_cast<double>(primingSamples) * innerPerOuter)));
}

int RateAdapter::runConversion(float* destination, int numOuterSamples, const InnerProcess& inner) noexcept
{
    // How many inner-rate samples this block is worth, carrying the fraction
    // forward so the two sides stay locked over time rather than drifting.
    const auto exact = static_cast<double>(numOuterSamples) * innerPerOuter + innerSampleCarry;
    auto innerCount = static_cast<int>(std::floor(exact));
    innerSampleCarry = exact - static_cast<double>(innerCount);

    innerCount = juce::jlimit(0, static_cast<int>(innerBlock.size()), innerCount);

    // The upsampler needs this many outer samples to produce innerCount.
    // Clamp rather than over-read if rounding leaves us a sample short.
    const auto outerNeeded = static_cast<int>(std::ceil(static_cast<double>(innerCount) * outerPerInner));

    if (outerNeeded > outerFill)
        innerCount = static_cast<int>(std::floor(static_cast<double>(outerFill) / outerPerInner));

    if (innerCount > 0)
    {
        const auto outerUsed = upsampler.process(outerPerInner, outerFifo.data(), innerBlock.data(), innerCount);
        consumeFront(outerFifo, outerFill, outerUsed);

        inner(innerBlock.data(), innerCount);

        const auto room = static_cast<int>(innerFifo.size()) - innerFill;
        const auto toStore = std::min(innerCount, room);

        if (toStore > 0)
        {
            std::memcpy(innerFifo.data() + innerFill, innerBlock.data(), static_cast<size_t>(toStore) * sizeof(float));
            innerFill += toStore;
        }
    }

    // Produce exactly the block the host asked for. Priming guarantees the
    // FIFO holds enough; the clamp is belt-and-braces against a pathological
    // rate ratio.
    const auto innerNeeded = static_cast<int>(std::ceil(static_cast<double>(numOuterSamples) * innerPerOuter));

    if (innerNeeded > innerFill)
        return 0;

    const auto innerUsed = downsampler.process(innerPerOuter, innerFifo.data(), destination, numOuterSamples);
    consumeFront(innerFifo, innerFill, innerUsed);

    return numOuterSamples;
}

void RateAdapter::process(float* samples, int numSamples, const InnerProcess& inner)
{
    if (numSamples <= 0)
        return;

    // Matched rates: hand the caller's buffer straight to the model. No copy,
    // no conversion, no latency.
    if (! converting)
    {
        inner(samples, numSamples);
        return;
    }

    appendOuter(samples, numSamples);

    if (runConversion(samples, numSamples, inner) == 0)
    {
        // Only reachable if the FIFOs were starved, which priming is designed
        // to prevent. Emitting silence is better than emitting stale audio.
        std::fill(samples, samples + numSamples, 0.0f);
    }
}

int RateAdapter::measureLatency()
{
    // Derive the reported latency by measurement rather than arithmetic: push
    // an impulse through the fully assembled pipeline with a pass-through
    // model and find where it comes out. Summing algorithmic latencies by hand
    // and converting between rates is easy to get wrong by a few samples, and
    // a host lining a re-amped DI back up will expose exactly that error.
    const InnerProcess passThrough = [](float*, int) {};

    const auto block = std::min(maxOuterBlockSize, 256);
    const auto sincLatency = static_cast<int>(juce::Interpolators::WindowedSinc::getBaseLatency());
    const auto searchLength =
        std::max(block * 4, static_cast<int>(std::ceil(static_cast<double>(sincLatency) * 4.0 * std::max(1.0, outerPerInner))) + block * 2);

    std::vector<float> scratch(static_cast<size_t>(block), 0.0f);

    auto bestIndex = 0;
    auto bestMagnitude = 0.0f;
    auto emitted = 0;
    auto impulseSent = false;

    while (emitted < searchLength)
    {
        std::fill(scratch.begin(), scratch.end(), 0.0f);

        if (! impulseSent)
        {
            scratch[0] = 1.0f;
            impulseSent = true;
        }

        process(scratch.data(), block, passThrough);

        for (auto i = 0; i < block; ++i)
        {
            const auto magnitude = std::abs(scratch[static_cast<size_t>(i)]);

            if (magnitude > bestMagnitude)
            {
                bestMagnitude = magnitude;
                bestIndex = emitted + i;
            }
        }

        emitted += block;
    }

    // A silent result means the impulse never made it through; reporting zero
    // is safer than reporting a garbage offset to the host.
    return bestMagnitude > 0.0f ? bestIndex : 0;
}

} // namespace nr::dsp
