cmake_minimum_required(VERSION 3.22)

project(NeuralRig
    VERSION 0.1.0
    DESCRIPTION "Chainable Neural Amp Modeler rig with a built-in TONE3000 profile browser"
    LANGUAGES C CXX)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set_property(GLOBAL PROPERTY USE_FOLDERS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

# Statically link the MSVC runtime so the shipped VST3 does not depend on a
# redistributable being present on the end user's machine.
if(MSVC)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

if(APPLE)
    set(CMAKE_OSX_DEPLOYMENT_TARGET "10.15" CACHE STRING "Minimum macOS version")
    set(CMAKE_OSX_ARCHITECTURES "arm64;x86_64" CACHE STRING "macOS architectures")
endif()

# --- Dependencies -----------------------------------------------------------
add_subdirectory(external/JUCE)
include(NamCore)

# --- Plugin target ----------------------------------------------------------
set(NR_FORMATS VST3 Standalone)
if(APPLE)
    list(APPEND NR_FORMATS AU)
endif()

juce_add_plugin(NeuralRig
    PRODUCT_NAME                "NeuralRig"
    COMPANY_NAME                "jon-jc"
    BUNDLE_ID                   "com.jonjc.neuralrig"
    PLUGIN_MANUFACTURER_CODE    Jnjc
    PLUGIN_CODE                 Nrg1
    FORMATS                     ${NR_FORMATS}
    VST3_CATEGORIES             Fx Distortion
    AU_MAIN_TYPE                kAudioUnitType_Effect
    IS_SYNTH                    FALSE
    NEEDS_MIDI_INPUT            FALSE
    NEEDS_MIDI_OUTPUT           FALSE
    IS_MIDI_EFFECT              FALSE
    EDITOR_WANTS_KEYBOARD_FOCUS TRUE
    COPY_PLUGIN_AFTER_BUILD     FALSE)

target_sources(NeuralRig PRIVATE
    source/PluginProcessor.cpp
    source/PluginEditor.cpp
    source/Parameters.cpp
    source/NamSupport.cpp
    source/dsp/RateAdapter.cpp
    source/dsp/NamModel.cpp
    source/dsp/ToneStack.cpp
    source/dsp/ModelSlot.cpp
    source/dsp/NoiseGate.cpp
    source/dsp/RigChain.cpp)

target_include_directories(NeuralRig PRIVATE source)

target_compile_definitions(NeuralRig PUBLIC
    JUCE_WEB_BROWSER=0          # the TONE3000 browser is native, not a webview
    JUCE_VST3_CAN_REPLACE_VST2=0)

# JUCE_USE_CURL is deliberately not set here. JUCE already defines it for the
# Linux build, and defining it again on the command line is a hard error under
# GCC's -Werror. The default is what we want anyway: curl-backed HTTPS on
# Linux, platform-native stacks elsewhere.

# MSVC needs to be told to actually silence the SYSTEM include dirs it was
# given via /external:I. Without this, warnings from NAM's headers still land
# in our build and trip /WX.
if(MSVC)
    target_compile_options(NeuralRig PRIVATE /external:W0)
endif()

target_link_libraries(NeuralRig
    PRIVATE
        nam::core
        juce::juce_audio_utils
        juce::juce_dsp
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_lto_flags
        juce::juce_recommended_warning_flags)

# Treat warnings in *our* code as errors. Vendored code is exempted in
# cmake/NamCore.cmake, which builds it with warnings off.
option(NR_WERROR "Treat warnings as errors when building NeuralRig sources" OFF)
if(NR_WERROR)
    if(MSVC)
        target_compile_options(NeuralRig PRIVATE /WX)
    else()
        target_compile_options(NeuralRig PRIVATE -Werror)
    endif()
endif()

# --- Tests ------------------------------------------------------------------
option(NR_BUILD_TESTS "Build the DSP unit tests" ON)
if(NR_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests-cpp)
endif()
