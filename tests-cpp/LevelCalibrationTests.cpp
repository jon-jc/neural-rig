#include <juce_core/juce_core.h>

#include "dsp/LevelCalibration.h"

namespace
{
using namespace nr::dsp;

NamModelInfo captureWithEverything()
{
    NamModelInfo info;
    info.hasLoudness = true;
    info.loudnessDb = -12.0;
    info.hasInputLevel = true;
    info.inputLevelDbu = 15.0;
    info.hasOutputLevel = true;
    info.outputLevelDbu = 20.0;
    return info;
}

/** An older capture: no level metadata at all. */
NamModelInfo bareCapture()
{
    return {};
}

class LevelCalibrationTests final : public juce::UnitTest
{
public:
    LevelCalibrationTests()
        : juce::UnitTest("LevelCalibration", "dsp")
    {
    }

    void runTest() override
    {
        beginTest("with no model loaded, only the trims apply");
        {
            CalibrationSettings settings;
            settings.inputTrimDb = 3.0f;
            settings.outputTrimDb = -6.0f;
            settings.outputMode = OutputMode::normalized;

            expectWithinAbsoluteError(computeInputGainDb(settings, nullptr), 3.0f, 1.0e-5f);
            expectWithinAbsoluteError(computeOutputGainDb(settings, nullptr), -6.0f, 1.0e-5f);
        }

        beginTest("raw mode leaves the output trim untouched");
        {
            const auto model = captureWithEverything();
            CalibrationSettings settings;
            settings.outputTrimDb = 2.0f;
            settings.outputMode = OutputMode::raw;

            expectWithinAbsoluteError(computeOutputGainDb(settings, &model), 2.0f, 1.0e-5f);
        }

        beginTest("normalized mode pulls the capture to the target loudness");
        {
            const auto model = captureWithEverything(); // loudness -12 dB
            CalibrationSettings settings;
            settings.outputTrimDb = 0.0f;
            settings.outputMode = OutputMode::normalized;

            // target(-18) - loudness(-12) = -6
            expectWithinAbsoluteError(computeOutputGainDb(settings, &model), -6.0f, 1.0e-5f);
        }

        beginTest("normalized mode falls back to the trim without loudness metadata");
        {
            auto model = bareCapture();
            CalibrationSettings settings;
            settings.outputTrimDb = 4.0f;
            settings.outputMode = OutputMode::normalized;

            expectWithinAbsoluteError(computeOutputGainDb(settings, &model), 4.0f, 1.0e-5f,
                                      "an old capture should be left alone, not guessed at");
        }

        beginTest("calibrated mode uses the capture's absolute output level");
        {
            const auto model = captureWithEverything(); // output 20 dBu
            CalibrationSettings settings;
            settings.outputTrimDb = 0.0f;
            settings.inputCalibrationDbu = 12.0f;
            settings.outputMode = OutputMode::calibrated;

            // outputLevel(20) - inputCalibration(12) = 8
            expectWithinAbsoluteError(computeOutputGainDb(settings, &model), 8.0f, 1.0e-5f);
        }

        beginTest("input calibration offsets the trim toward the trained level");
        {
            const auto model = captureWithEverything(); // input 15 dBu
            CalibrationSettings settings;
            settings.inputTrimDb = 1.0f;
            settings.calibrateInput = true;
            settings.inputCalibrationDbu = 12.0f;

            // trim(1) + calibration(12) - modelInput(15) = -2
            expectWithinAbsoluteError(computeInputGainDb(settings, &model), -2.0f, 1.0e-5f);
        }

        beginTest("input calibration is inert when the capture has no input level");
        {
            auto model = bareCapture();
            CalibrationSettings settings;
            settings.inputTrimDb = 1.0f;
            settings.calibrateInput = true;
            settings.inputCalibrationDbu = 12.0f;

            expectWithinAbsoluteError(computeInputGainDb(settings, &model), 1.0f, 1.0e-5f);
        }

        beginTest("input calibration off means the trim is the whole story");
        {
            const auto model = captureWithEverything();
            CalibrationSettings settings;
            settings.inputTrimDb = 5.0f;
            settings.calibrateInput = false;

            expectWithinAbsoluteError(computeInputGainDb(settings, &model), 5.0f, 1.0e-5f);
        }

        beginTest("swapping captures in normalized mode holds perceived level steady");
        {
            // The point of the mode: two captures at very different loudnesses
            // should land at the same output level.
            NamModelInfo quiet;
            quiet.hasLoudness = true;
            quiet.loudnessDb = -30.0;

            NamModelInfo loud;
            loud.hasLoudness = true;
            loud.loudnessDb = -4.0;

            CalibrationSettings settings;
            settings.outputMode = OutputMode::normalized;

            const auto quietGain = computeOutputGainDb(settings, &quiet) + static_cast<float>(quiet.loudnessDb);
            const auto loudGain = computeOutputGainDb(settings, &loud) + static_cast<float>(loud.loudnessDb);

            expectWithinAbsoluteError(quietGain, loudGain, 1.0e-5f,
                                      "both captures should land on the same target");
            expectWithinAbsoluteError(quietGain, normalizedTargetDb, 1.0e-5f);
        }
    }
};

LevelCalibrationTests levelCalibrationTests;
} // namespace
