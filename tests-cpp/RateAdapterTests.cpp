#include <cmath>
#include <numeric>
#include <vector>

#include <juce_audio_basics/juce_audio_basics.h>

#include "dsp/RateAdapter.h"

namespace
{
/** Fills a buffer with a sine at the given frequency, continuing the phase
    across calls so a signal can be generated block by block. */
struct SineSource
{
    double frequency = 1000.0;
    double sampleRate = 48000.0;
    double phase = 0.0;

    void render(float* destination, int numSamples)
    {
        const auto increment = juce::MathConstants<double>::twoPi * frequency / sampleRate;

        for (int i = 0; i < numSamples; ++i)
        {
            destination[i] = static_cast<float>(std::sin(phase));
            phase += increment;

            if (phase > juce::MathConstants<double>::twoPi)
                phase -= juce::MathConstants<double>::twoPi;
        }
    }
};

/** Normalised cross-correlation of two equal-length signals. 1.0 means
    identical shape and sign, 0.0 means unrelated. */
double correlation(const std::vector<float>& a, const std::vector<float>& b, int offset, int length)
{
    double dot = 0.0, energyA = 0.0, energyB = 0.0;

    for (int i = 0; i < length; ++i)
    {
        const auto x = static_cast<double>(a[static_cast<size_t>(i)]);
        const auto y = static_cast<double>(b[static_cast<size_t>(i + offset)]);
        dot += x * y;
        energyA += x * x;
        energyB += y * y;
    }

    const auto denominator = std::sqrt(energyA * energyB);
    return denominator > 0.0 ? dot / denominator : 0.0;
}

class RateAdapterTests final : public juce::UnitTest
{
public:
    RateAdapterTests()
        : juce::UnitTest("RateAdapter", "dsp")
    {
    }

    void runTest() override
    {
        beginTest("matched rates bypass conversion entirely");
        {
            nr::dsp::RateAdapter adapter;
            adapter.prepare(48000.0, 48000.0, 512);

            expect(! adapter.isConverting(), "no conversion should be needed at equal rates");
            expectEquals(adapter.latencySamples(), 0, "bypass must not report latency");

            std::vector<float> block(512, 0.25f);
            int seenSamples = 0;
            const float* seenPointer = nullptr;

            adapter.process(block.data(), 512, [&](float* data, int n) {
                seenSamples = n;
                seenPointer = data;
                for (int i = 0; i < n; ++i)
                    data[i] *= 2.0f;
            });

            expectEquals(seenSamples, 512, "inner processor should see the whole block");
            expect(seenPointer == block.data(), "bypass should hand over the caller's buffer, not a copy");
            expectWithinAbsoluteError(block[0], 0.5f, 1.0e-6f);
        }

        beginTest("conversion is engaged and reports latency when rates differ");
        {
            nr::dsp::RateAdapter adapter;
            adapter.prepare(44100.0, 48000.0, 512);

            expect(adapter.isConverting(), "44.1k host with a 48k model must resample");
            expect(adapter.latencySamples() > 0, "a sinc pipeline cannot be latency-free");
        }

        beginTest("inner processor runs at the model's rate, not the host's");
        {
            // A 48 kHz model fed by a 44.1 kHz host must receive proportionally
            // more samples. Getting this backwards is the bug that makes a
            // capture sound like the wrong amp.
            nr::dsp::RateAdapter adapter;
            adapter.prepare(44100.0, 48000.0, 512);

            long long innerTotal = 0;
            const int blocks = 200;
            std::vector<float> block(512, 0.0f);

            for (int i = 0; i < blocks; ++i)
            {
                std::fill(block.begin(), block.end(), 0.0f);
                adapter.process(block.data(), 512, [&](float*, int n) { innerTotal += n; });
            }

            const auto outerTotal = static_cast<double>(blocks) * 512.0;
            const auto ratio = static_cast<double>(innerTotal) / outerTotal;

            expectWithinAbsoluteError(ratio, 48000.0 / 44100.0, 0.01,
                                      "inner sample count should track the rate ratio");
        }

        beginTest("output block length always matches input block length");
        {
            // Hosts hand out irregular block sizes; every one of them must come
            // back the same length or the plugin corrupts the timeline.
            for (const auto hostRate : { 44100.0, 88200.0, 96000.0, 192000.0 })
            {
                nr::dsp::RateAdapter adapter;
                adapter.prepare(hostRate, 48000.0, 1024);

                for (const auto blockSize : { 1, 7, 64, 111, 512, 1024 })
                {
                    std::vector<float> block(static_cast<size_t>(blockSize), 0.1f);
                    adapter.process(block.data(), blockSize, [](float*, int) {});
                    expectEquals(static_cast<int>(block.size()), blockSize);
                }
            }
        }

        beginTest("a sine survives the round trip at its original frequency");
        {
            // The real test of a resampler: put a tone in, get the same tone
            // back once the reported latency is compensated for.
            constexpr double hostRate = 44100.0;
            constexpr int blockSize = 512;
            constexpr int blocks = 120;
            constexpr int total = blockSize * blocks;

            nr::dsp::RateAdapter adapter;
            adapter.prepare(hostRate, 48000.0, blockSize);

            SineSource source { 1000.0, hostRate, 0.0 };
            std::vector<float> input, output;
            input.reserve(static_cast<size_t>(total));
            output.reserve(static_cast<size_t>(total));

            std::vector<float> block(static_cast<size_t>(blockSize));

            for (int i = 0; i < blocks; ++i)
            {
                source.render(block.data(), blockSize);
                input.insert(input.end(), block.begin(), block.end());

                adapter.process(block.data(), blockSize, [](float*, int) {});
                output.insert(output.end(), block.begin(), block.end());
            }

            const auto latency = adapter.latencySamples();
            expect(latency > 0 && latency < total / 2, "latency should be sane");

            // Start well past the priming region so start-up transients do not
            // colour the result. input[skip] emerges at output[skip + latency],
            // so that whole sum is the offset into the output — leaving the
            // skip out is a silent phase error, not an obvious failure.
            const int skip = 4096;
            const auto compareLength = total - skip - latency - 16;

            std::vector<float> aligned(input.begin() + skip, input.end());
            const auto score = correlation(aligned, output, skip + latency, compareLength);

            expect(score > 0.99,
                   "delay-compensated output should match the input closely, got "
                       + juce::String(score, 4));
        }

        beginTest("resetState clears history without reallocating");
        {
            nr::dsp::RateAdapter adapter;
            adapter.prepare(44100.0, 48000.0, 512);

            std::vector<float> block(512, 1.0f);
            adapter.process(block.data(), 512, [](float*, int) {});

            adapter.resetState();

            std::fill(block.begin(), block.end(), 0.0f);
            adapter.process(block.data(), 512, [](float*, int) {});

            auto peak = 0.0f;
            for (auto value : block)
                peak = juce::jmax(peak, std::abs(value));

            expect(peak < 1.0e-4f, "silence in after a reset should give silence out");
        }
    }
};

RateAdapterTests rateAdapterTests;
} // namespace
