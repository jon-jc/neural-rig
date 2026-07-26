#pragma once

#include <juce_core/juce_core.h>

/**
    Thin seam between NeuralRig and NeuralAmpModelerCore.

    NAM's headers pull in Eigen and nlohmann/json, both of which are heavy and
    both of which fight with JUCE's warning settings. Keeping the include
    confined to NamSupport.cpp means the rest of the codebase compiles fast and
    stays clean; anything the plugin needs from NAM is re-exposed here in plain
    JUCE/std types.
*/
namespace nr::nam_support
{

/** Version of the linked NeuralAmpModelerCore, e.g. "0.5.5". */
juce::String coreVersion();

/** The newest .nam file format this build fully understands. */
juce::String latestSupportedFileVersion();

/** The oldest .nam file format this build will still load. */
juce::String earliestSupportedFileVersion();

} // namespace nr::nam_support
