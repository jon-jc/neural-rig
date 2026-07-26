#include "Parameters.h"

namespace nr::params
{
namespace
{
/** Formats a dB value the way an amp panel would: signed, one decimal. */
juce::String formatDecibels(float value, int)
{
    return juce::String(value, 1) + " dB";
}

/** Tone controls read 0-10 like a real amp, not 0-100%. */
juce::String formatAmpDial(float value, int)
{
    return juce::String(value, 1);
}

juce::String formatPercent(float value, int)
{
    return juce::String(juce::roundToInt(value)) + " %";
}

using Attributes = juce::AudioParameterFloatAttributes;

std::unique_ptr<juce::AudioParameterFloat> makeFloat(juce::StringRef id,
                                                     const juce::String& name,
                                                     juce::NormalisableRange<float> range,
                                                     float defaultValue,
                                                     juce::String (*formatter)(float, int),
                                                     const juce::String& label = {})
{
    return std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID { id, 1 },
        name,
        range,
        defaultValue,
        Attributes().withStringFromValueFunction(formatter).withLabel(label));
}
} // namespace

juce::StringArray outputModeChoices()
{
    return { "Raw", "Normalized", "Calibrated" };
}

juce::AudioProcessorValueTreeState::ParameterLayout createParameterLayout()
{
    juce::AudioProcessorValueTreeState::ParameterLayout layout;

    // --- I/O ----------------------------------------------------------------
    // Input trim is the single most important control in a NAM rig: a capture
    // only behaves like the amp it modelled when it is fed the level it was
    // trained at. +/-20 dB covers everything from a weak single-coil DI to a
    // hot re-amp send.
    layout.add(makeFloat(id::inputLevel, "Input",
                         juce::NormalisableRange<float> { -20.0f, 20.0f, 0.1f },
                         0.0f, formatDecibels));

    layout.add(makeFloat(id::outputLevel, "Output",
                         juce::NormalisableRange<float> { -40.0f, 40.0f, 0.1f },
                         0.0f, formatDecibels));

    layout.add(makeFloat(id::mix, "Mix",
                         juce::NormalisableRange<float> { 0.0f, 100.0f, 0.1f },
                         100.0f, formatPercent));

    // --- Noise gate ---------------------------------------------------------
    layout.add(std::make_unique<juce::AudioParameterBool>(
        juce::ParameterID { id::gateEnabled, 1 }, "Gate", true));

    // Threshold is skewed so the useful -80..-50 dB region gets most of the
    // knob travel instead of being squashed against the bottom.
    layout.add(makeFloat(id::gateThreshold, "Gate Thresh",
                         juce::NormalisableRange<float> { -100.0f, 0.0f, 0.1f, 2.0f },
                         -80.0f, formatDecibels));

    // --- Tone stack ---------------------------------------------------------
    layout.add(std::make_unique<juce::AudioParameterBool>(
        juce::ParameterID { id::eqEnabled, 1 }, "EQ", true));

    const juce::NormalisableRange<float> dialRange { 0.0f, 10.0f, 0.01f };
    layout.add(makeFloat(id::bass, "Bass", dialRange, 5.0f, formatAmpDial));
    layout.add(makeFloat(id::mid, "Middle", dialRange, 5.0f, formatAmpDial));
    layout.add(makeFloat(id::treble, "Treble", dialRange, 5.0f, formatAmpDial));
    // Presence is a high shelf above the treble band. Upstream NAM has no such
    // control; nearly every amp it models does.
    layout.add(makeFloat(id::presence, "Presence", dialRange, 5.0f, formatAmpDial));

    // --- Cabinet ------------------------------------------------------------
    layout.add(std::make_unique<juce::AudioParameterBool>(
        juce::ParameterID { id::irEnabled, 1 }, "IR", true));

    // --- Levelling ----------------------------------------------------------
    layout.add(std::make_unique<juce::AudioParameterChoice>(
        juce::ParameterID { id::outputMode, 1 },
        "Output Mode",
        outputModeChoices(),
        static_cast<int>(OutputMode::normalized)));

    // When on, the input trim is offset so the capture receives the level it
    // was trained at rather than whatever the DAW happens to be sending.
    layout.add(std::make_unique<juce::AudioParameterBool>(
        juce::ParameterID { id::calibrateInput, 1 }, "Calibrate Input", false));

    // The dBu level that 0 dBFS in the DAW represents. 12 dBu matches the
    // stock NAM plugin's default, so rigs port across without a level jump.
    layout.add(makeFloat(id::inputCalibrationLevel, "Input Calibration",
                         juce::NormalisableRange<float> { -60.0f, 60.0f, 0.1f },
                         12.0f,
                         [](float value, int) { return juce::String(value, 1) + " dBu"; }));

    return layout;
}

} // namespace nr::params
