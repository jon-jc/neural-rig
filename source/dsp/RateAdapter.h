#pragma once

#include <functional>
#include <vector>

#include <juce_audio_basics/juce_audio_basics.h>

namespace nr::dsp
{

/**
    Runs a mono processing callback at a sample rate that differs from the host's.

    NAM captures are trained at a specific sample rate — almost always 48 kHz.
    Feeding a 48 kHz capture audio at 44.1 kHz does not merely detune it; every
    frequency-dependent behaviour in the model lands in the wrong place, so it
    stops being a model of the amp it captured. This class keeps the model
    running at its native rate regardless of what the host is doing.

    When the two rates match it steps out of the way entirely: the callback is
    invoked directly on the caller's buffer, with no copy, no conversion and
    zero added latency. That is the common case — 48 kHz host, 48 kHz model —
    and it should cost nothing.

    Threading: prepare() allocates and must be called off the audio thread.
    process() is real-time safe and performs no allocation.
*/
class RateAdapter
{
public:
    /** The wrapped processor. Receives a mono buffer at the inner rate and
        must process exactly numSamples in place. */
    using InnerProcess = std::function<void(float* samples, int numSamples)>;

    RateAdapter() = default;

    /** Allocates for the given rates and block size. Not real-time safe.

        @param outerSampleRate  the host's sample rate
        @param innerSampleRate  the rate the callback wants to run at
        @param maxOuterBlock    largest block process() will be handed
    */
    void prepare(double outerSampleRate, double innerSampleRate, int maxOuterBlock);

    /** Clears buffers and interpolator history without deallocating, so the
        next block starts from silence. Real-time safe. */
    void resetState() noexcept;

    /** Processes numSamples in place at the outer rate, running `inner` at the
        inner rate. numSamples must not exceed the prepared maximum. */
    void process(float* samples, int numSamples, const InnerProcess& inner);

    /** Round-trip latency in outer-rate samples, for reporting to the host.
        Zero when no conversion is happening.

        This is measured rather than derived: prepare() runs an impulse through
        the assembled pipeline and finds where it comes out. The alternative —
        summing each interpolator's algorithmic latency, converting between
        rates and accounting for FIFO priming — is easy to get wrong by a few
        samples, and a few samples of mis-reported latency is exactly what
        ruins a re-amped DI when the DAW lines it back up. */
    int latencySamples() const noexcept { return latency; }

    /** True when the rates differ and conversion is actually running. */
    bool isConverting() const noexcept { return converting; }

private:
    void appendOuter(const float* source, int count) noexcept;
    int runConversion(float* destination, int numOuterSamples, const InnerProcess& inner) noexcept;
    int measureLatency();

    /** Drops the first `count` samples from a FIFO, keeping the remainder at
        the front. juce::Interpolators needs a contiguous input block, so a
        split-read ring buffer will not serve. These FIFOs hold well under a
        millisecond of audio, which makes the move cheaper than the bookkeeping
        the alternative would require. */
    static void consumeFront(std::vector<float>& fifo, int& fill, int count) noexcept;

    juce::Interpolators::WindowedSinc upsampler;   // outer rate -> inner rate
    juce::Interpolators::WindowedSinc downsampler; // inner rate -> outer rate

    std::vector<float> outerFifo; // awaiting conversion up to the inner rate
    std::vector<float> innerFifo; // produced by `inner`, awaiting conversion down
    std::vector<float> innerBlock; // scratch for one block at the inner rate

    int outerFill = 0;
    int innerFill = 0;

    double outerRate = 0.0;
    double innerRate = 0.0;

    // juce::Interpolators expresses a conversion as "input samples consumed per
    // output sample", so each direction gets its own ratio.
    double outerPerInner = 1.0; // upsampling pass: outerRate / innerRate
    double innerPerOuter = 1.0; // downsampling pass: innerRate / outerRate

    // Fractional carry, so the number of inner-rate samples produced per block
    // tracks the true ratio instead of drifting on rounding.
    double innerSampleCarry = 0.0;

    int maxOuterBlockSize = 0;
    int latency = 0;
    bool converting = false;

    JUCE_LEAK_DETECTOR(RateAdapter)
};

} // namespace nr::dsp
