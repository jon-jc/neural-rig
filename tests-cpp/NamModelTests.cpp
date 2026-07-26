#include <cmath>
#include <vector>

#include <juce_core/juce_core.h>

#include "dsp/NamModel.h"

namespace
{
juce::File modelsDirectory()
{
    return juce::File { juce::String { NR_TEST_MODELS_DIR } };
}

juce::File modelFile(const juce::String& name)
{
    return modelsDirectory().getChildFile(name);
}

/** Runs a sine through the model and reports the peak absolute output, or a
    negative value if anything non-finite came out. */
float runSineAndFindPeak(nr::dsp::NamModel& model, double hostRate, int blockSize, int blocks)
{
    std::vector<float> block(static_cast<size_t>(blockSize));
    double phase = 0.0;
    const auto increment = juce::MathConstants<double>::twoPi * 220.0 / hostRate;
    auto peak = 0.0f;

    for (int b = 0; b < blocks; ++b)
    {
        for (int i = 0; i < blockSize; ++i)
        {
            block[static_cast<size_t>(i)] = 0.25f * static_cast<float>(std::sin(phase));
            phase += increment;
        }

        model.process(block.data(), blockSize);

        for (const auto value : block)
        {
            if (! std::isfinite(value))
                return -1.0f;

            peak = juce::jmax(peak, std::abs(value));
        }
    }

    return peak;
}

class NamModelTests final : public juce::UnitTest
{
public:
    NamModelTests()
        : juce::UnitTest("NamModel", "dsp")
    {
    }

    void runTest() override
    {
        beginTest("bundled example captures are present");
        {
            expect(modelsDirectory().isDirectory(),
                   "example model directory missing: " + modelsDirectory().getFullPathName());
        }

        beginTest("every bundled architecture loads");
        {
            // One per architecture family NAM ships, so a regression in any of
            // them shows up here rather than in someone's session.
            for (const auto* modelName : { "wavenet.nam", "lstm.nam", "wavenet_a2_max.nam",
                                           "slimmable_wavenet.nam", "wavenet_a1_standard.nam" })
            {
                const auto file = modelFile(modelName);

                if (! file.existsAsFile())
                {
                    logMessage("skipping absent model " + juce::String(modelName));
                    continue;
                }

                juce::String error;
                auto model = nr::dsp::NamModel::loadFromFile(file, error);

                expect(model != nullptr, juce::String(modelName) + " failed to load: " + error);

                if (model != nullptr)
                {
                    expect(error.isEmpty(), "a successful load should report no error");
                    expect(model->info().sampleRate > 0.0,
                           juce::String(modelName) + " reported a non-positive rate");
                }
            }
        }

        beginTest("a missing file fails cleanly instead of throwing");
        {
            juce::String error;
            auto model = nr::dsp::NamModel::loadFromFile(modelFile("does_not_exist.nam"), error);

            expect(model == nullptr, "a missing file must not yield a model");
            expect(error.isNotEmpty(), "a failed load must explain itself");
        }

        beginTest("a corrupt file fails cleanly instead of throwing");
        {
            // NAM parses .nam as JSON and throws on malformed input. That
            // exception must not escape into the caller — a user picking the
            // wrong file should see a message, not lose their session.
            auto temp = juce::File::createTempFile(".nam");
            temp.replaceWithText("this is definitely not a neural amp model");

            juce::String error;
            auto model = nr::dsp::NamModel::loadFromFile(temp, error);

            expect(model == nullptr, "garbage must not yield a model");
            expect(error.isNotEmpty(), "a failed load must explain itself");

            temp.deleteFile();
        }

        beginTest("processing a sine produces finite, bounded audio");
        {
            const auto file = modelFile("wavenet.nam");

            if (file.existsAsFile())
            {
                juce::String error;
                auto model = nr::dsp::NamModel::loadFromFile(file, error);
                expect(model != nullptr, error);

                if (model != nullptr)
                {
                    model->prepare(48000.0, 512);
                    const auto peak = runSineAndFindPeak(*model, 48000.0, 512, 40);

                    expect(peak >= 0.0f, "model produced NaN or infinity");
                    expect(peak < 100.0f, "model output ran away, peak " + juce::String(peak));
                }
            }
        }

        beginTest("latency is zero at the capture's own rate and non-zero otherwise");
        {
            const auto file = modelFile("wavenet.nam");

            if (file.existsAsFile())
            {
                juce::String error;
                auto model = nr::dsp::NamModel::loadFromFile(file, error);
                expect(model != nullptr, error);

                if (model != nullptr)
                {
                    const auto nativeRate = model->info().sampleRate;

                    model->prepare(nativeRate, 512);
                    expectEquals(model->latencySamples(), 0,
                                 "running at the capture's own rate must not add latency");

                    model->prepare(nativeRate * 44100.0 / 48000.0, 512);
                    expect(model->latencySamples() > 0,
                           "resampling to a different host rate must report latency");
                }
            }
        }

        beginTest("slimmable captures are detected and accept a size");
        {
            const auto slim = modelFile("slimmable_wavenet.nam");
            const auto plain = modelFile("wavenet.nam");

            if (slim.existsAsFile() && plain.existsAsFile())
            {
                juce::String error;

                auto slimModel = nr::dsp::NamModel::loadFromFile(slim, error);
                expect(slimModel != nullptr, error);

                if (slimModel != nullptr)
                {
                    expect(slimModel->info().isSlimmable, "slimmable_wavenet.nam should report as slimmable");

                    slimModel->prepare(48000.0, 512);
                    slimModel->setSlimSize(0.5);

                    const auto peak = runSineAndFindPeak(*slimModel, 48000.0, 512, 20);
                    expect(peak >= 0.0f, "slimmed model produced NaN or infinity");
                }

                auto plainModel = nr::dsp::NamModel::loadFromFile(plain, error);

                if (plainModel != nullptr)
                {
                    expect(! plainModel->info().isSlimmable, "wavenet.nam should not report as slimmable");
                    // Must be a harmless no-op rather than a crash.
                    plainModel->setSlimSize(0.25);
                }
            }
        }
    }
};

NamModelTests namModelTests;
} // namespace
