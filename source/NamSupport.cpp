#include "NamSupport.h"

#include <NAM/get_dsp.h>
#include <NAM/version.h>

namespace nr::nam_support
{

juce::String coreVersion()
{
    // Round-trip the compile-time version through NAM's own parser. Beyond
    // producing the string, this is a cheap guarantee that we are actually
    // linked against the core library rather than just seeing its headers.
    const nam::Version parsed { NEURAL_AMP_MODELER_DSP_VERSION_MAJOR,
                                NEURAL_AMP_MODELER_DSP_VERSION_MINOR,
                                NEURAL_AMP_MODELER_DSP_VERSION_PATCH };

    return juce::String { nam::ParseVersion(parsed.toString()).toString() };
}

juce::String latestSupportedFileVersion()
{
    return juce::String { nam::LATEST_FULLY_SUPPORTED_NAM_FILE_VERSION };
}

juce::String earliestSupportedFileVersion()
{
    return juce::String { nam::EARLIEST_SUPPORTED_NAM_FILE_VERSION };
}

} // namespace nr::nam_support
