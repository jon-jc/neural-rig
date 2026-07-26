# NamCore.cmake
#
# Builds the neural inference dependencies NeuralRig sits on top of:
#
#   nr_eigen  - header-only linear algebra, consumed as an INTERFACE target
#   nam_core  - sdatkinson/NeuralAmpModelerCore, the NAM inference engine
#
# Both are vendored as git submodules under external/. NeuralAmpModelerCore
# ships no consumable CMake target of its own (its CMakeLists only builds
# benchmark and test executables), so the library target is declared here.
#
# Note on AudioDSPTools: the upstream NAM plugin uses it for its gate, IR,
# filters and resampler, and NeuralRig deliberately does not. Its
# ResamplingContainer depends on iPlug2's WDL headers, which AudioDSPTools
# does not vendor, and its other components hardcode `double` in places that
# its own DSP_SAMPLE_FLOAT switch is supposed to control. JUCE already gives us
# better-tested equivalents (juce::dsp::Convolution, juce::dsp::IIR,
# juce::Interpolators, AudioFormatManager), so we build on those instead.

set(NR_EXTERNAL_DIR "${CMAKE_CURRENT_SOURCE_DIR}/external")

foreach(_dep IN ITEMS eigen NeuralAmpModelerCore)
    if(NOT EXISTS "${NR_EXTERNAL_DIR}/${_dep}/.git")
        message(FATAL_ERROR
            "Submodule external/${_dep} is missing. Run:\n"
            "  git submodule update --init --recursive")
    endif()
endforeach()

# --- Eigen ------------------------------------------------------------------
# SYSTEM so Eigen's own warnings never reach our -Werror builds.
add_library(nr_eigen INTERFACE)
target_include_directories(nr_eigen SYSTEM INTERFACE "${NR_EXTERNAL_DIR}/eigen")

# --- NAM core ---------------------------------------------------------------
file(GLOB NAM_CORE_SOURCES CONFIGURE_DEPENDS
    "${NR_EXTERNAL_DIR}/NeuralAmpModelerCore/NAM/*.cpp"
    "${NR_EXTERNAL_DIR}/NeuralAmpModelerCore/NAM/wavenet/*.cpp")

if(NOT NAM_CORE_SOURCES)
    message(FATAL_ERROR "No NAM core sources found under external/NeuralAmpModelerCore/NAM.")
endif()

# OBJECT, not STATIC, and this is load-bearing.
#
# Each NAM architecture registers itself with a file-scope static in its own
# translation unit, e.g. in NAM/wavenet/model.cpp:
#
#     static nam::ConfigParserHelper _register_WaveNet("WaveNet", create_config);
#
# A static library only contributes object files that resolve an undefined
# symbol. NeuralRig calls nam::get_dsp() and nothing else, so the linker
# discards model.cpp, lstm.cpp, convnet.cpp and linear.cpp outright, their
# registrars never run, and the registry is empty at runtime. Every load then
# fails with "No config parser registered for architecture: WaveNet".
#
# An OBJECT library links all of its objects unconditionally, so the
# registrars survive. (Whole-archive linking would also work, but needs
# CMake 3.24+ and per-linker handling; this is portable to our stated 3.22.)
add_library(nam_core OBJECT ${NAM_CORE_SOURCES})
add_library(nam::core ALIAS nam_core)

target_compile_features(nam_core PUBLIC cxx_std_20)

# PUBLIC include dirs: NAM/dsp.h is reachable from our own headers, and it in
# turn does a bare `#include "json.hpp"`, so the nlohmann directory has to be
# visible to consumers too. json.hpp is vendored (not a submodule) inside
# NeuralAmpModelerCore, so there is nothing extra to fetch.
#
# SYSTEM matters here. Building nam_core with warnings off only covers its own
# translation units; NAM's headers are also compiled into ours, and
# activations.h alone emits C4305/C4244/C4100 that would trip -Werror in
# NeuralRig. SYSTEM demotes those to non-warnings in every consumer.
target_include_directories(nam_core SYSTEM PUBLIC
    "${NR_EXTERNAL_DIR}/NeuralAmpModelerCore"
    "${NR_EXTERNAL_DIR}/NeuralAmpModelerCore/Dependencies/nlohmann")

target_link_libraries(nam_core PUBLIC nr_eigen)

# NAM_SAMPLE_FLOAT makes nam::DSP::process() take float* rather than double*.
# JUCE hands us float buffers and NAM's inference is single-precision Eigen
# throughout, so this removes a double<->float conversion at every chain node
# for free. PUBLIC because it changes the signature of process().
target_compile_definitions(nam_core PUBLIC NAM_SAMPLE_FLOAT)

# Route models whose shape matches the A2 signature through the hand-optimised
# WaveNet kernel. Matches the upstream default.
option(NR_ENABLE_A2_FAST "Build the NAM A2 fast-path WaveNet kernel" ON)
if(NR_ENABLE_A2_FAST)
    target_compile_definitions(nam_core PUBLIC NAM_ENABLE_A2_FAST)
endif()

if(WIN32)
    target_compile_definitions(nam_core PUBLIC NOMINMAX WIN32_LEAN_AND_MEAN)
endif()

# We build our own sources with a strict warning set. Upstream does not, and we
# do not want to carry a patch queue just to keep third-party code quiet.
if(MSVC)
    target_compile_options(nam_core PRIVATE /W0 /bigobj)
else()
    target_compile_options(nam_core PRIVATE -w)
endif()

set_target_properties(nam_core PROPERTIES
    POSITION_INDEPENDENT_CODE ON
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN ON
    FOLDER "External")
