#pragma once

#include <memory>

#include <juce_core/juce_core.h>

namespace nr::dsp
{

/**
    Everything NeuralRig needs to know about a capture, in plain types.

    Read freely from any thread: it is filled in once at load time and never
    mutated afterwards.
*/
struct NamModelInfo
{
    juce::String name;

    /** The rate the capture was trained at. Older .nam files do not record
        one; those are reported as 48 kHz, which is what they almost certainly
        are given how NAM has been used historically. */
    double sampleRate = 48000.0;
    bool sampleRateWasAssumed = false;

    /** Loudness in dB for a standardised input, written by the trainer. Drives
        Normalized output mode. */
    bool hasLoudness = false;
    double loudnessDb = 0.0;

    /** Absolute levels in dBu, present on newer captures. Drive Calibrated
        output mode and input calibration. */
    bool hasInputLevel = false;
    double inputLevelDbu = 0.0;
    bool hasOutputLevel = false;
    double outputLevelDbu = 0.0;

    /** A2-architecture captures can trade quality for CPU at runtime, which
        matters once several of them are chained. */
    bool isSlimmable = false;
};

/**
    One loaded NAM capture, running at its own trained sample rate.

    Construction and prepare() do file I/O, allocate, and run inference to
    settle the network's state — none of which may happen on the audio thread.
    process() is the only real-time-safe entry point.

    NAM's headers drag in Eigen and nlohmann/json, so the model is held behind
    a pimpl. That keeps this header cheap to include from the rest of the
    plugin and keeps our translation units compiling under warnings-as-errors.
*/
class NamModel
{
public:
    ~NamModel();

    NamModel(const NamModel&) = delete;
    NamModel& operator=(const NamModel&) = delete;

    /** Loads a .nam file. Blocking, allocating — never call from the audio
        thread. Returns nullptr on failure, with the reason in errorMessage. */
    static std::unique_ptr<NamModel> loadFromFile(const juce::File& file, juce::String& errorMessage);

    /** Sizes buffers for the host's rate and block size, and prewarms the
        network so it does not start from a cold, unsettled state. Expensive;
        off the audio thread only. */
    void prepare(double hostSampleRate, int maxBlockSize);

    /** Runs the capture over a mono block in place. Real-time safe. */
    void process(float* samples, int numSamples);

    /** Latency introduced by rate conversion, in host samples. Zero when the
        host is already running at the capture's rate. */
    int latencySamples() const noexcept;

    const NamModelInfo& info() const noexcept { return modelInfo; }

    /** Quality/CPU trade-off for slimmable captures, 0.0 (cheapest) to 1.0
        (full). No-op on captures that do not support it.

        Thread-safe but not real-time safe: NAM reallocates internally, so this
        belongs on the message thread, not in process(). */
    void setSlimSize(double normalisedSize);

private:
    NamModel();

    struct Impl;
    std::unique_ptr<Impl> impl;
    NamModelInfo modelInfo;
};

} // namespace nr::dsp
