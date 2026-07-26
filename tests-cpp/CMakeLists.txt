# DSP unit tests.
#
# Kept in tests-cpp/ so the C++ tree stays clearly separated from anything a
# Python toolchain might try to collect.

juce_add_console_app(NeuralRigTests
    PRODUCT_NAME "NeuralRigTests")

target_sources(NeuralRigTests PRIVATE
    TestMain.cpp
    RateAdapterTests.cpp
    NamModelTests.cpp
    LevelCalibrationTests.cpp
    ToneStackTests.cpp
    ModelSlotTests.cpp
    ${CMAKE_SOURCE_DIR}/source/dsp/ToneStack.cpp
    ${CMAKE_SOURCE_DIR}/source/dsp/ModelSlot.cpp
    ${CMAKE_SOURCE_DIR}/source/dsp/RateAdapter.cpp
    ${CMAKE_SOURCE_DIR}/source/dsp/NamModel.cpp)

target_include_directories(NeuralRigTests PRIVATE "${CMAKE_SOURCE_DIR}/source")

# The NAM submodule ships real captures — A2, LSTM, WaveNet and slimmable
# variants. Testing against those beats any synthetic stand-in, and they are
# present on CI for free because the submodule is already checked out.
target_compile_definitions(NeuralRigTests PRIVATE
    JUCE_UNIT_TESTS=1
    JUCE_WEB_BROWSER=0
    JUCE_STANDALONE_APPLICATION=1
    NR_TEST_MODELS_DIR="${CMAKE_SOURCE_DIR}/external/NeuralAmpModelerCore/example_models")

target_link_libraries(NeuralRigTests
    PRIVATE
        nam::core
        juce::juce_audio_basics
        juce::juce_dsp
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_warning_flags)

if(NR_WERROR)
    if(MSVC)
        target_compile_options(NeuralRigTests PRIVATE /WX /external:W0)
    else()
        target_compile_options(NeuralRigTests PRIVATE -Werror)
    endif()
endif()

set_target_properties(NeuralRigTests PROPERTIES FOLDER "Tests")

add_test(NAME dsp COMMAND NeuralRigTests)
