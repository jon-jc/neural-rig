#include <array>
#include <cmath>
#include <vector>

#include <juce_core/juce_core.h>

#include "dsp/RigChain.h"

namespace
{
constexpr double hostRate = 44100.0; // deliberately not the capture's rate, so
                                     // resampling latency is non-zero
constexpr int blockSize = 256;

juce::File exampleModel()
{
    return juce::File { juce::String { NR_TEST_MODELS_DIR } }.getChildFile("wavenet.nam");
}

std::unique_ptr<nr::dsp::NamModel> loadPrepared()
{
    juce::String error;
    auto model = nr::dsp::NamModel::loadFromFile(exampleModel(), error);

    if (model != nullptr)
        model->prepare(hostRate, blockSize);

    return model;
}

using Settings = std::array<nr::dsp::NodeSettings, nr::dsp::RigChain::numSlots>;

/** Sends an impulse through the chain and returns the index of the loudest
    output sample — i.e. the delay the chain actually imposes. */
int measureImpulsePosition(nr::dsp::RigChain& chain, const Settings& settings, int searchLength)
{
    std::vector<float> block(static_cast<size_t>(blockSize));
    auto bestIndex = -1;
    auto bestMagnitude = 0.0f;
    auto emitted = 0;
    auto sent = false;

    while (emitted < searchLength)
    {
        std::fill(block.begin(), block.end(), 0.0f);

        if (! sent)
        {
            block[0] = 1.0f;
            sent = true;
        }

        chain.process(block.data(), blockSize, settings);

        for (int i = 0; i < blockSize; ++i)
        {
            const auto magnitude = std::abs(block[static_cast<size_t>(i)]);

            if (magnitude > bestMagnitude)
            {
                bestMagnitude = magnitude;
                bestIndex = emitted + i;
            }
        }

        emitted += blockSize;
    }

    return bestMagnitude > 0.0f ? bestIndex : -1;
}

class RigChainTests final : public juce::UnitTest
{
public:
    RigChainTests()
        : juce::UnitTest("RigChain", "dsp")
    {
    }

    void runTest() override
    {
        if (! exampleModel().existsAsFile())
        {
            beginTest("example capture available");
            expect(false, "wavenet.nam missing; cannot exercise RigChain");
            return;
        }

        beginTest("an empty chain is transparent and adds no latency");
        {
            nr::dsp::RigChain chain;
            chain.prepare(hostRate, blockSize);

            expectEquals(chain.latencySamples(), 0);

            Settings settings;
            std::vector<float> block(static_cast<size_t>(blockSize));

            for (int i = 0; i < blockSize; ++i)
                block[static_cast<size_t>(i)] = 0.5f * std::sin(static_cast<float>(i) * 0.1f);

            const auto original = block;
            chain.process(block.data(), blockSize, settings);

            for (int i = 0; i < blockSize; ++i)
                expectWithinAbsoluteError(block[static_cast<size_t>(i)], original[static_cast<size_t>(i)], 1.0e-6f);
        }

        beginTest("latency is the sum of the loaded slots");
        {
            nr::dsp::RigChain chain;
            chain.prepare(hostRate, blockSize);

            auto first = loadPrepared();
            expect(first != nullptr);
            const auto singleLatency = first->latencySamples();
            expect(singleLatency > 0, "resampling to 44.1k should cost latency");

            chain.stage(0, std::move(first));
            Settings settings;
            std::vector<float> block(static_cast<size_t>(blockSize), 0.0f);
            chain.process(block.data(), blockSize, settings); // let the slot pick it up

            expectEquals(chain.latencySamples(), singleLatency);

            chain.stage(2, loadPrepared());
            chain.process(block.data(), blockSize, settings);

            expectEquals(chain.latencySamples(), singleLatency * 2,
                         "two captures should report twice the latency");
        }

        beginTest("bypassing a slot does not change reported latency");
        {
            // If bypass changed the figure, the host would re-align mid-session
            // and the whole track would appear to jump.
            nr::dsp::RigChain chain;
            chain.prepare(hostRate, blockSize);
            chain.stage(0, loadPrepared());

            Settings settings;
            std::vector<float> block(static_cast<size_t>(blockSize), 0.0f);
            chain.process(block.data(), blockSize, settings);

            const auto active = chain.latencySamples();

            settings[0].bypassed = true;
            chain.process(block.data(), blockSize, settings);

            expectEquals(chain.latencySamples(), active,
                         "bypass must not move the reported latency");
        }

        beginTest("a bypassed slot delays by exactly its own latency");
        {
            nr::dsp::RigChain chain;
            chain.prepare(hostRate, blockSize);
            chain.stage(0, loadPrepared());

            Settings settings;
            std::vector<float> warmup(static_cast<size_t>(blockSize), 0.0f);
            chain.process(warmup.data(), blockSize, settings);

            const auto expected = chain.latencySamples();

            settings[0].bypassed = true;
            chain.reset();

            const auto position = measureImpulsePosition(chain, settings, blockSize * 12);

            expect(position >= 0, "the impulse never came out");
            expectEquals(position, expected,
                         "a bypassed slot should delay by its latency, not skip it");
        }

        beginTest("clearing a slot removes its latency");
        {
            nr::dsp::RigChain chain;
            chain.prepare(hostRate, blockSize);
            chain.stage(0, loadPrepared());

            Settings settings;
            std::vector<float> block(static_cast<size_t>(blockSize), 0.0f);
            chain.process(block.data(), blockSize, settings);
            expect(chain.latencySamples() > 0);

            chain.clear(0);
            chain.process(block.data(), blockSize, settings);

            expectEquals(chain.latencySamples(), 0);
            chain.collectRetired();
        }

        beginTest("reordering preserves total latency");
        {
            nr::dsp::RigChain chain;
            chain.prepare(hostRate, blockSize);
            chain.stage(0, loadPrepared());
            chain.stage(1, loadPrepared());

            Settings settings;
            std::vector<float> block(static_cast<size_t>(blockSize), 0.0f);
            chain.process(block.data(), blockSize, settings);

            const auto before = chain.latencySamples();
            const auto* firstBefore = chain.modelAt(0);
            const auto* secondBefore = chain.modelAt(1);

            chain.requestSwap(0, 1);
            chain.process(block.data(), blockSize, settings); // swap applies here

            expect(chain.modelAt(0) == secondBefore, "slot 0 should now hold the other capture");
            expect(chain.modelAt(1) == firstBefore, "slot 1 should now hold the first capture");
            expectEquals(chain.latencySamples(), before, "a sum does not care about order");
        }

        beginTest("slot indices outside the rig are ignored");
        {
            nr::dsp::RigChain chain;
            chain.prepare(hostRate, blockSize);

            // Must not crash or corrupt anything.
            chain.stage(-1, loadPrepared());
            chain.stage(nr::dsp::RigChain::numSlots, loadPrepared());
            chain.clear(99);
            chain.requestSwap(0, 99);

            Settings settings;
            std::vector<float> block(static_cast<size_t>(blockSize), 0.0f);
            chain.process(block.data(), blockSize, settings);

            expectEquals(chain.latencySamples(), 0);
        }

        beginTest("output stays finite through a full chain");
        {
            nr::dsp::RigChain chain;
            chain.prepare(hostRate, blockSize);

            for (int i = 0; i < nr::dsp::RigChain::numSlots; ++i)
                chain.stage(i, loadPrepared());

            Settings settings;
            for (auto& setting : settings)
                setting.gainDb = 3.0f;

            std::vector<float> block(static_cast<size_t>(blockSize));
            juce::Random random { 99 };

            for (int b = 0; b < 30; ++b)
            {
                for (int i = 0; i < blockSize; ++i)
                    block[static_cast<size_t>(i)] = 0.2f * (random.nextFloat() * 2.0f - 1.0f);

                chain.process(block.data(), blockSize, settings);

                for (int i = 0; i < blockSize; ++i)
                    expect(std::isfinite(block[static_cast<size_t>(i)]),
                           "four chained captures produced a non-finite sample");
            }

            chain.collectRetired();
        }
    }
};

RigChainTests rigChainTests;
} // namespace
