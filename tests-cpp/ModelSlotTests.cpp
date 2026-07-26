#include <juce_core/juce_core.h>

#include "dsp/ModelSlot.h"

namespace
{
juce::File exampleModel()
{
    return juce::File { juce::String { NR_TEST_MODELS_DIR } }.getChildFile("wavenet.nam");
}

std::unique_ptr<nr::dsp::NamModel> loadExample()
{
    juce::String error;
    return nr::dsp::NamModel::loadFromFile(exampleModel(), error);
}

class ModelSlotTests final : public juce::UnitTest
{
public:
    ModelSlotTests()
        : juce::UnitTest("ModelSlot", "dsp")
    {
    }

    void runTest() override
    {
        if (! exampleModel().existsAsFile())
        {
            beginTest("example capture available");
            expect(false, "wavenet.nam missing; cannot exercise ModelSlot");
            return;
        }

        beginTest("an empty slot plays nothing");
        {
            nr::dsp::ModelSlot slot;
            expect(slot.current() == nullptr);
            expect(slot.acquire() == nullptr, "acquiring an empty slot should stay empty");
            expect(! slot.hasPendingStage());
        }

        beginTest("a staged capture becomes active on the next acquire");
        {
            nr::dsp::ModelSlot slot;
            auto model = loadExample();
            expect(model != nullptr);

            auto* raw = model.get();
            slot.stage(std::move(model));

            expect(slot.hasPendingStage(), "the capture should be waiting");
            expect(slot.current() == nullptr, "staging alone must not change what is playing");

            expect(slot.acquire() == raw, "acquire should hand back the staged capture");
            expect(! slot.hasPendingStage(), "the stage should be consumed");
            expect(slot.current() == raw);
        }

        beginTest("swapping replaces the active capture and retires the old one");
        {
            nr::dsp::ModelSlot slot;

            auto first = loadExample();
            auto* firstRaw = first.get();
            slot.stage(std::move(first));
            expect(slot.acquire() == firstRaw);

            auto second = loadExample();
            auto* secondRaw = second.get();
            expect(secondRaw != firstRaw, "the two loads should be distinct objects");

            slot.stage(std::move(second));
            expect(slot.acquire() == secondRaw, "the new capture should take over");

            // The displaced capture is now waiting for the message thread. The
            // audio thread must not have freed it — collecting is what frees.
            slot.collectRetired();
            expect(slot.current() == secondRaw, "collecting must not disturb what is playing");
        }

        beginTest("re-staging before the audio thread runs drops the unclaimed capture");
        {
            nr::dsp::ModelSlot slot;

            slot.stage(loadExample());
            auto latest = loadExample();
            auto* latestRaw = latest.get();
            slot.stage(std::move(latest)); // first one never reached the audio thread

            expect(slot.acquire() == latestRaw, "the most recent stage should win");
        }

        beginTest("staging nullptr clears the slot");
        {
            nr::dsp::ModelSlot slot;

            slot.stage(loadExample());
            expect(slot.acquire() != nullptr);

            slot.stage(nullptr);
            expect(slot.acquire() == nullptr, "staging nothing should silence the slot");

            slot.collectRetired();
        }

        beginTest("repeated swaps stay stable");
        {
            // Exercises the retire/collect cycle the way a player auditioning
            // captures in the browser would.
            nr::dsp::ModelSlot slot;

            for (int i = 0; i < 12; ++i)
            {
                auto model = loadExample();
                auto* raw = model.get();
                slot.stage(std::move(model));

                expect(slot.acquire() == raw, "swap " + juce::String(i) + " did not take");

                slot.collectRetired();
            }

            expect(slot.current() != nullptr);
        }
    }
};

ModelSlotTests modelSlotTests;
} // namespace
