#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

/**
    Parameter definitions for NeuralRig.

    Everything the host can automate is declared here exactly once. The DSP and
    the editor both look parameters up by the string IDs in nr::params::id, so
    renaming a control is a single-line change and there is no way for the two
    sides to drift apart.

    IDs are snake_case and permanent: changing one silently breaks every saved
    session that references it, so add new parameters rather than renaming old
    ones.
*/
namespace nr::params
{

namespace id
{
inline constexpr auto inputLevel = "input_level";
inline constexpr auto outputLevel = "output_level";
inline constexpr auto mix = "mix";

inline constexpr auto gateEnabled = "gate_enabled";
inline constexpr auto gateThreshold = "gate_threshold";

inline constexpr auto eqEnabled = "eq_enabled";
inline constexpr auto bass = "bass";
inline constexpr auto mid = "mid";
inline constexpr auto treble = "treble";
inline constexpr auto presence = "presence";

inline constexpr auto irEnabled = "ir_enabled";
inline constexpr auto outputMode = "output_mode";

inline constexpr auto calibrateInput = "calibrate_input";
inline constexpr auto inputCalibrationLevel = "input_calibration_level";
} // namespace id

/** How the level of the loaded model is compensated on the way out.

    NAM captures carry loudness metadata from the trainer, and newer ones also
    carry absolute input/output levels in dBu. These modes decide how much of
    that metadata to trust.
*/
enum class OutputMode
{
    raw = 0,    ///< No compensation. What the model produces is what you hear.
    normalized, ///< Compensate using the model's reported loudness so swapping
                ///< models does not change perceived volume.
    calibrated, ///< Use the model's absolute dBu input/output levels to place
                ///< it at real-world amp gain staging.
    numModes
};

/** Display names for OutputMode, in enum order. */
juce::StringArray outputModeChoices();

/** Builds the full parameter layout. Called once from the processor's ctor. */
juce::AudioProcessorValueTreeState::ParameterLayout createParameterLayout();

} // namespace nr::params
