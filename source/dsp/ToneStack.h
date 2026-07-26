#pragma once

#include <juce_dsp/juce_dsp.h>

namespace nr::dsp
{

/**
    The amp panel: bass, middle, treble and presence.

    A NAM capture is a frozen snapshot of one amp at one setting — its own tone
    controls are baked in and cannot be moved. This stack sits after the model
    and gives back the adjustment a player expects from a physical amp.

    Bass, middle and treble reproduce the voicing of the stock NAM plugin
    exactly, so a rig ported from it sounds the same:

      bass      peaking,  150 Hz, Q 0.707, +/-20 dB
      middle    peaking,  425 Hz, Q 1.5 when cutting / 0.7 when boosting, +/-15 dB
      treble    peaking, 1800 Hz, Q 0.707, +/-10 dB

    Middle widens when cut and narrows when boosted; a boost at a wide Q drags
    the low mids up with it and turns honky.

    Presence is ours — upstream has no such control, and nearly every amp NAM
    models does. It is a high shelf rather than a peak, because presence on a
    real amp lifts everything above the treble band rather than a single
    region.

    All four controls read 0-10 like an amp, with 5 as flat.
*/
class ToneStack
{
public:
    void prepare(double sampleRate, int maxBlockSize, int numChannels);
    void reset();

    /** Sets the dial positions, 0-10 each. Cheap enough to call per block;
        coefficients are only recomputed when a value actually moves. */
    void setControls(float bass, float middle, float treble, float presence);

    /** Processes in place. Real-time safe. */
    void process(juce::AudioBuffer<float>& buffer);

private:
    enum Band
    {
        bassBand = 0,
        middleBand,
        trebleBand,
        presenceBand,
        numBands
    };

    void updateCoefficients();

    using Filter = juce::dsp::ProcessorDuplicator<juce::dsp::IIR::Filter<float>,
                                                  juce::dsp::IIR::Coefficients<float>>;

    std::array<Filter, numBands> filters;
    std::array<float, numBands> dials { 5.0f, 5.0f, 5.0f, 5.0f };

    double currentSampleRate = 0.0;
    bool coefficientsDirty = true;
};

} // namespace nr::dsp
