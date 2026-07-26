#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

#include "PluginProcessor.h"

namespace nr
{

/** A rotary control with its label, wired to an APVTS parameter. */
class LabelledKnob final : public juce::Component
{
public:
    LabelledKnob(juce::AudioProcessorValueTreeState& state,
                 juce::StringRef parameterId,
                 const juce::String& displayName);

    void resized() override;

private:
    juce::Slider slider;
    juce::Label caption;
    juce::AudioProcessorValueTreeState::SliderAttachment attachment;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(LabelledKnob)
};

/** A vertical peak meter driven by a dB value polled from the processor. */
class PeakMeter final : public juce::Component
{
public:
    // Explicit: JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR below declares a
    // deleted copy constructor, which suppresses the implicit default one.
    PeakMeter() = default;

    void setLevelDb(float db);
    void paint(juce::Graphics&) override;

private:
    float displayedDb = -100.0f;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(PeakMeter)
};

class NeuralRigEditor final : public juce::AudioProcessorEditor,
                              private juce::Timer
{
public:
    explicit NeuralRigEditor(NeuralRigProcessor&);
    ~NeuralRigEditor() override;

    void paint(juce::Graphics&) override;
    void resized() override;

private:
    void timerCallback() override;

    // Not named `processor`: AudioProcessorEditor already has a member by that
    // name, and shadowing it trips -Wshadow-field on Clang.
    NeuralRigProcessor& audioProcessor;

    juce::Label titleLabel;
    juce::Label subtitleLabel;

    LabelledKnob inputKnob;
    LabelledKnob outputKnob;
    LabelledKnob mixKnob;

    PeakMeter inputMeter;
    PeakMeter outputMeter;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(NeuralRigEditor)
};

} // namespace nr
