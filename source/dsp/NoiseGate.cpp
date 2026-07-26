#include "NoiseGate.h"

#include <cmath>

namespace nr::dsp
{
namespace
{
/** Opening has to be immediate or the front of a picked note is clipped off.
    1 ms is fast enough to be inaudible on a transient without being so abrupt
    that it clicks. */
constexpr double attackSeconds = 0.001;

/** Closing is gradual so a decaying note fades rather than being cut. */
constexpr double releaseSeconds = 0.120;

/** Stay open briefly after the signal drops below the threshold, so the dip
    between two picked notes does not chatter the gate. */
constexpr double holdSeconds = 0.030;

float coefficientFor(double seconds, double sampleRate) noexcept
{
    if (seconds <= 0.0 || sampleRate <= 0.0)
        return 0.0f;

    return static_cast<float>(std::exp(-1.0 / (seconds * sampleRate)));
}
} // namespace

void NoiseGate::prepare(double sampleRate, int)
{
    currentSampleRate = sampleRate;

    attackCoefficient = coefficientFor(attackSeconds, sampleRate);
    releaseCoefficient = coefficientFor(releaseSeconds, sampleRate);
    holdSamples = static_cast<int>(holdSeconds * sampleRate);

    reset();
}

void NoiseGate::reset() noexcept
{
    // Start open: a gate that starts shut swallows the first note played after
    // the plugin loads.
    envelope = 1.0f;
    holdCounter = 0;
}

void NoiseGate::process(float* samples, int numSamples) noexcept
{
    if (! enabled || samples == nullptr || numSamples <= 0 || currentSampleRate <= 0.0)
    {
        if (! enabled)
            envelope = 1.0f;

        return;
    }

    for (int i = 0; i < numSamples; ++i)
    {
        const auto magnitude = std::abs(samples[i]);

        // Above the threshold, or still within the hold window, the gate wants
        // to be fully open; otherwise it wants to be shut.
        float target;

        if (magnitude >= threshold)
        {
            holdCounter = holdSamples;
            target = 1.0f;
        }
        else if (holdCounter > 0)
        {
            --holdCounter;
            target = 1.0f;
        }
        else
        {
            target = 0.0f;
        }

        const auto coefficient = target > envelope ? attackCoefficient : releaseCoefficient;
        envelope = target + (envelope - target) * coefficient;

        samples[i] *= envelope;
    }
}

} // namespace nr::dsp
