#include <cmath>
#include <vector>

#include <juce_core/juce_core.h>

#include "dsp/ToneStack.h"

namespace
{
constexpr double testSampleRate = 48000.0;
constexpr int testBlockSize = 512;

/** Steady-state gain of the stack at one frequency, in dB.

    Feeds a sine, discards the settling transient, and compares RMS out to RMS
    in. Measuring the actual response beats asserting on coefficients: it is
    what a listener hears, and it stays valid if the implementation changes. */
float measureGainDb(nr::dsp::ToneStack& stack, double frequency)
{
    constexpr int settleBlocks = 40;
    constexpr int measureBlocks = 40;

    juce::AudioBuffer<float> buffer { 1, testBlockSize };
    double phase = 0.0;
    const auto increment = juce::MathConstants<double>::twoPi * frequency / testSampleRate;

    double inputEnergy = 0.0;
    double outputEnergy = 0.0;

    for (int b = 0; b < settleBlocks + measureBlocks; ++b)
    {
        auto* data = buffer.getWritePointer(0);
        double blockInputEnergy = 0.0;

        for (int i = 0; i < testBlockSize; ++i)
        {
            const auto value = static_cast<float>(std::sin(phase));
            data[i] = value;
            blockInputEnergy += static_cast<double>(value) * value;
            phase += increment;
        }

        stack.process(buffer);

        if (b >= settleBlocks)
        {
            inputEnergy += blockInputEnergy;

            for (int i = 0; i < testBlockSize; ++i)
                outputEnergy += static_cast<double>(data[i]) * data[i];
        }
    }

    if (inputEnergy <= 0.0 || outputEnergy <= 0.0)
        return -200.0f;

    return static_cast<float>(10.0 * std::log10(outputEnergy / inputEnergy));
}

class ToneStackTests final : public juce::UnitTest
{
public:
    ToneStackTests()
        : juce::UnitTest("ToneStack", "dsp")
    {
    }

    void runTest() override
    {
        beginTest("all dials centred is flat across the band");
        {
            nr::dsp::ToneStack stack;
            stack.prepare(testSampleRate, testBlockSize, 1);
            stack.setControls(5.0f, 5.0f, 5.0f, 5.0f);

            for (const auto frequency : { 100.0, 425.0, 1800.0, 6000.0 })
            {
                stack.reset();
                const auto gain = measureGainDb(stack, frequency);

                expectWithinAbsoluteError(gain, 0.0f, 0.1f,
                                          "centred stack should be flat at " + juce::String(frequency) + " Hz, got "
                                              + juce::String(gain, 2) + " dB");
            }
        }

        beginTest("bass moves the low end and leaves the top alone");
        {
            nr::dsp::ToneStack stack;
            stack.prepare(testSampleRate, testBlockSize, 1);

            stack.setControls(10.0f, 5.0f, 5.0f, 5.0f);
            stack.reset();
            const auto boosted = measureGainDb(stack, 150.0);

            stack.setControls(0.0f, 5.0f, 5.0f, 5.0f);
            stack.reset();
            const auto cut = measureGainDb(stack, 150.0);

            stack.setControls(10.0f, 5.0f, 5.0f, 5.0f);
            stack.reset();
            const auto topEnd = measureGainDb(stack, 6000.0);

            // 4 dB per step over five steps from centre.
            expectWithinAbsoluteError(boosted, 20.0f, 0.5f, "full bass should be about +20 dB at 150 Hz");
            expectWithinAbsoluteError(cut, -20.0f, 0.5f, "zero bass should be about -20 dB at 150 Hz");
            expect(std::abs(topEnd) < 1.0f, "bass should not disturb 6 kHz");
        }

        beginTest("treble reaches its documented range");
        {
            nr::dsp::ToneStack stack;
            stack.prepare(testSampleRate, testBlockSize, 1);

            stack.setControls(5.0f, 5.0f, 10.0f, 5.0f);
            stack.reset();
            expectWithinAbsoluteError(measureGainDb(stack, 1800.0), 10.0f, 0.5f);

            stack.setControls(5.0f, 5.0f, 0.0f, 5.0f);
            stack.reset();
            expectWithinAbsoluteError(measureGainDb(stack, 1800.0), -10.0f, 0.5f);
        }

        beginTest("a middle cut is wider than a middle boost");
        {
            // The stock NAM voicing narrows the boost so it does not drag the
            // low mids up and turn honky. Compare each against its own centre
            // gain a half-octave away.
            nr::dsp::ToneStack stack;
            stack.prepare(testSampleRate, testBlockSize, 1);

            stack.setControls(5.0f, 10.0f, 5.0f, 5.0f);
            stack.reset();
            const auto boostCentre = measureGainDb(stack, 425.0);
            stack.reset();
            const auto boostEdge = measureGainDb(stack, 425.0 / 2.0);

            stack.setControls(5.0f, 0.0f, 5.0f, 5.0f);
            stack.reset();
            const auto cutCentre = measureGainDb(stack, 425.0);
            stack.reset();
            const auto cutEdge = measureGainDb(stack, 425.0 / 2.0);

            // Fraction of the centre effect still present an octave down.
            const auto boostSpread = std::abs(boostEdge / boostCentre);
            const auto cutSpread = std::abs(cutEdge / cutCentre);

            expect(boostSpread > cutSpread,
                   "the boost should stay wider-reaching than the cut at its Q; boost "
                       + juce::String(boostSpread, 3) + " vs cut " + juce::String(cutSpread, 3));
        }

        beginTest("presence lifts above the treble band as a shelf");
        {
            nr::dsp::ToneStack stack;
            stack.prepare(testSampleRate, testBlockSize, 1);
            stack.setControls(5.0f, 5.0f, 5.0f, 10.0f);

            stack.reset();
            const auto high = measureGainDb(stack, 10000.0);
            stack.reset();
            const auto low = measureGainDb(stack, 200.0);

            expect(high > 10.0f, "presence should lift the top end, got " + juce::String(high, 2) + " dB");
            expect(std::abs(low) < 1.0f, "a shelf should leave 200 Hz alone");
        }

        beginTest("output stays finite at extreme settings");
        {
            nr::dsp::ToneStack stack;
            stack.prepare(testSampleRate, testBlockSize, 1);
            stack.setControls(10.0f, 10.0f, 10.0f, 10.0f);

            juce::AudioBuffer<float> buffer { 1, testBlockSize };
            juce::Random random { 1234 };

            for (int b = 0; b < 20; ++b)
            {
                auto* data = buffer.getWritePointer(0);

                for (int i = 0; i < testBlockSize; ++i)
                    data[i] = random.nextFloat() * 2.0f - 1.0f;

                stack.process(buffer);

                for (int i = 0; i < testBlockSize; ++i)
                    expect(std::isfinite(data[i]), "tone stack produced a non-finite sample");
            }
        }
    }
};

ToneStackTests toneStackTests;
} // namespace
