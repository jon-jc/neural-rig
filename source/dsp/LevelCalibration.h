#pragma once

#include "NamModel.h"

namespace nr::dsp
{

/** How the level of the loaded capture is compensated on the way out. */
enum class OutputMode
{
    raw = 0,    ///< No compensation. What the capture produces is what you hear.
    normalized, ///< Use the capture's reported loudness so swapping models does
                ///< not change perceived volume.
    calibrated  ///< Use absolute dBu levels to place the capture at real-world
                ///< amp gain staging.
};

/** The reference level, in dBu, that 0 dBFS in the DAW is taken to represent.
    12 dBu matches the stock NAM plugin's default, so rigs port across without
    a level jump. */
inline constexpr float defaultInputCalibrationDbu = 12.0f;

/** Target loudness for normalized mode, in dB. */
inline constexpr float normalizedTargetDb = -18.0f;

struct CalibrationSettings
{
    float inputTrimDb = 0.0f;
    float outputTrimDb = 0.0f;

    /** When set, the input trim is offset so the capture receives the level it
        was trained at rather than whatever the DAW happens to send. */
    bool calibrateInput = false;
    float inputCalibrationDbu = defaultInputCalibrationDbu;

    OutputMode outputMode = OutputMode::normalized;
};

/** Total input gain in dB.

    Input calibration only engages when the capture actually carries an input
    level; older captures do not, and inventing one would be worse than leaving
    the trim alone. */
inline float computeInputGainDb(const CalibrationSettings& settings, const NamModelInfo* model) noexcept
{
    auto gainDb = settings.inputTrimDb;

    if (model != nullptr && model->hasInputLevel && settings.calibrateInput)
        gainDb += settings.inputCalibrationDbu - static_cast<float>(model->inputLevelDbu);

    return gainDb;
}

/** Total output gain in dB.

    Each mode falls back to the plain trim when the capture lacks the metadata
    that mode depends on, so an old model is quiet-but-correct rather than
    wrong. */
inline float computeOutputGainDb(const CalibrationSettings& settings, const NamModelInfo* model) noexcept
{
    auto gainDb = settings.outputTrimDb;

    if (model == nullptr)
        return gainDb;

    switch (settings.outputMode)
    {
        case OutputMode::normalized:
            if (model->hasLoudness)
                gainDb += normalizedTargetDb - static_cast<float>(model->loudnessDb);
            break;

        case OutputMode::calibrated:
            if (model->hasOutputLevel)
                gainDb += static_cast<float>(model->outputLevelDbu) - settings.inputCalibrationDbu;
            break;

        case OutputMode::raw:
            break;
    }

    return gainDb;
}

} // namespace nr::dsp
