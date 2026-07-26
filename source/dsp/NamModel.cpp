#include "NamModel.h"

#include <algorithm>
#include <cmath>
#include <exception>

#include <NAM/get_dsp.h>
#include <NAM/slimmable.h>

#include "RateAdapter.h"

namespace nr::dsp
{
namespace
{
/** Captures predating sample-rate metadata are treated as 48 kHz, which is
    what NAM has overwhelmingly been used at. Guessing is better than the
    alternative: running an unknown-rate model at the host rate guarantees it
    is wrong whenever the host is not at 48 kHz. */
constexpr double assumedSampleRate = 48000.0;

/** Slack on the model-rate block size handed to NAM::Reset. The rate adapter
    produces a variable number of inner samples per block as the fractional
    carry advances, and NAM must be sized for the largest of them. */
constexpr int innerBlockSlack = 8;
} // namespace

struct NamModel::Impl
{
    std::unique_ptr<nam::DSP> dsp;
    RateAdapter adapter;
    double preparedHostRate = 0.0;
};

NamModel::NamModel()
    : impl(std::make_unique<Impl>())
{
}

NamModel::~NamModel() = default;

std::unique_ptr<NamModel> NamModel::loadFromFile(const juce::File& file, juce::String& errorMessage)
{
    errorMessage.clear();

    if (! file.existsAsFile())
    {
        errorMessage = "File not found: " + file.getFullPathName();
        return nullptr;
    }

    std::unique_ptr<NamModel> model { new NamModel() };

    try
    {
        // Skip the prewarm NAM would otherwise run inside get_dsp(): we do not
        // know the host's block size yet, so prepare() has to reset and
        // prewarm again anyway. Doing it twice just makes loading slower.
        nam::DspLoadOptions options;
        options.prewarm = false;

        model->impl->dsp = nam::get_dsp(std::filesystem::path(file.getFullPathName().toStdString()), options);
    }
    catch (const std::exception& e)
    {
        errorMessage = juce::String("Could not load model: ") + e.what();
        return nullptr;
    }
    catch (...)
    {
        errorMessage = "Could not load model: unknown error";
        return nullptr;
    }

    if (model->impl->dsp == nullptr)
    {
        errorMessage = "Could not load model: the file did not yield a usable network";
        return nullptr;
    }

    auto& dsp = *model->impl->dsp;
    auto& out = model->modelInfo;

    out.name = file.getFileNameWithoutExtension();

    const auto reported = dsp.GetExpectedSampleRate();
    out.sampleRateWasAssumed = reported <= 0.0;
    out.sampleRate = out.sampleRateWasAssumed ? assumedSampleRate : reported;

    out.hasLoudness = dsp.HasLoudness();
    if (out.hasLoudness)
        out.loudnessDb = dsp.GetLoudness();

    out.hasInputLevel = dsp.HasInputLevel();
    if (out.hasInputLevel)
        out.inputLevelDbu = dsp.GetInputLevel();

    out.hasOutputLevel = dsp.HasOutputLevel();
    if (out.hasOutputLevel)
        out.outputLevelDbu = dsp.GetOutputLevel();

    out.isSlimmable = dynamic_cast<nam::SlimmableModel*>(&dsp) != nullptr;

    return model;
}

void NamModel::prepare(double hostSampleRate, int maxBlockSize)
{
    if (impl->dsp == nullptr || hostSampleRate <= 0.0 || maxBlockSize <= 0)
        return;

    impl->preparedHostRate = hostSampleRate;
    impl->adapter.prepare(hostSampleRate, modelInfo.sampleRate, maxBlockSize);

    // The network runs at its own rate, so it must be sized for the block the
    // adapter will hand it, not the one the host hands us.
    const auto ratio = modelInfo.sampleRate / hostSampleRate;
    const auto maxInnerBlock =
        static_cast<int>(std::ceil(static_cast<double>(maxBlockSize) * ratio)) + innerBlockSlack;

    // Reset() prewarms by default, which is what we want here: a network
    // started cold produces a settling transient on the first notes played.
    impl->dsp->Reset(modelInfo.sampleRate, maxInnerBlock);
}

void NamModel::process(float* samples, int numSamples)
{
    if (impl->dsp == nullptr || samples == nullptr || numSamples <= 0)
        return;

    impl->adapter.process(samples, numSamples, [this](float* data, int count) {
        // NAM works in pointer-to-pointers form, indexed [channel][frame].
        // A capture is mono, so this is a single channel aliased in place.
        float* channels[1] = { data };
        impl->dsp->process(channels, channels, count);
    });
}

int NamModel::latencySamples() const noexcept
{
    return impl->adapter.latencySamples();
}

void NamModel::setSlimSize(double normalisedSize)
{
    if (impl->dsp == nullptr || ! modelInfo.isSlimmable)
        return;

    if (auto* slimmable = dynamic_cast<nam::SlimmableModel*>(impl->dsp.get()))
        slimmable->SetSlimmableSize(juce::jlimit(0.0, 1.0, normalisedSize));
}

} // namespace nr::dsp
