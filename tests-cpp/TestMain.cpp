#include <iostream>

#include <juce_core/juce_core.h>

/**
    Console runner for NeuralRig's DSP tests.

    Uses juce::UnitTest rather than pulling in a separate test framework: the
    code under test already depends on JUCE, and the DSP here needs a real
    audio-buffer vocabulary more than it needs fixtures or mocking.

    Exits non-zero if any test fails, so CI can gate on it.
*/
int main(int, char**)
{
    juce::UnitTestRunner runner;
    // Let a failing expectation record itself and carry on, rather than
    // tripping an assertion and killing the run at the first problem.
    runner.setAssertOnFailure(false);
    runner.runAllTests();

    int failures = 0;
    int passes = 0;

    for (int i = 0; i < runner.getNumResults(); ++i)
    {
        if (const auto* result = runner.getResult(i))
        {
            failures += result->failures;
            passes += result->passes;

            if (result->failures > 0)
            {
                std::cout << "FAILED: " << result->unitTestName << " / " << result->subcategoryName
                          << " (" << result->failures << " failure(s))\n";

                for (const auto& message : result->messages)
                    std::cout << "    " << message << "\n";
            }
        }
    }

    std::cout << "\n" << passes << " passed, " << failures << " failed\n";

    return failures > 0 ? 1 : 0;
}
