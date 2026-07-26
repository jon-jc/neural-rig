#pragma once

#include <juce_audio_basics/juce_audio_basics.h>

namespace nr::dsp
{

/**
    Downward gate placed ahead of the model.

    High-gain captures amplify whatever the pickups are picking up, so without
    a gate a quiet passage turns into hiss and hum. Gating *before* the model
    rather than after it matters: silence fed into a neural amp comes back as
    the amp's own noise floor, so cleaning up afterwards means fighting noise
    the model just generated.

    The envelope follows the signal with an instant attack and a timed release,
    with a hold time so that the gap between picked notes does not chatter the
    gate open and shut.
*/
class NoiseGate
{
public:
    void prepare(double sampleRate, int maxBlockSize);
    void reset() noexcept;

    /** Threshold in dBFS. Signal below this is attenuated. */
    void setThreshold(float thresholdDb) noexcept { threshold = juce::Decibels::decibelsToGain(thresholdDb, -120.0f); }

    void setEnabled(bool shouldBeEnabled) noexcept { enabled = shouldBeEnabled; }

    /** Processes a mono block in place. Real-time safe. */
    void process(float* samples, int numSamples) noexcept;

    /** Current gain reduction in dB (negative), for metering. */
    float gainReductionDb() const noexcept { return juce::Decibels::gainToDecibels(envelope, -100.0f); }

private:
    float threshold = 0.0f;
    float envelope = 1.0f;

    // Per-sample multipliers derived from the time constants below.
    float attackCoefficient = 1.0f;
    float releaseCoefficient = 0.0f;
    int holdSamples = 0;
    int holdCounter = 0;

    bool enabled = true;
    double currentSampleRate = 0.0;
};

} // namespace nr::dsp
