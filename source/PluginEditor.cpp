#include "PluginEditor.h"

#include "NamSupport.h"

namespace nr
{
namespace
{
constexpr int editorWidth = 620;
constexpr int editorHeight = 380;
constexpr int meterFloorDb = -60;

const juce::Colour backgroundColour { 0xff14161a };
const juce::Colour panelColour { 0xff1d2026 };
const juce::Colour accentColour { 0xff38bdf8 };
const juce::Colour textColour { 0xffe2e8f0 };
const juce::Colour mutedTextColour { 0xff8b93a1 };
} // namespace

// --- LabelledKnob -----------------------------------------------------------

LabelledKnob::LabelledKnob(juce::AudioProcessorValueTreeState& state,
                           juce::StringRef parameterId,
                           const juce::String& displayName)
    : attachment(state, parameterId, slider)
{
    slider.setSliderStyle(juce::Slider::RotaryHorizontalVerticalDrag);
    slider.setTextBoxStyle(juce::Slider::TextBoxBelow, false, 72, 18);
    slider.setColour(juce::Slider::rotarySliderFillColourId, accentColour);
    slider.setColour(juce::Slider::rotarySliderOutlineColourId, juce::Colour { 0xff2c313a });
    slider.setColour(juce::Slider::thumbColourId, textColour);
    slider.setColour(juce::Slider::textBoxTextColourId, textColour);
    slider.setColour(juce::Slider::textBoxOutlineColourId, juce::Colours::transparentBlack);
    addAndMakeVisible(slider);

    caption.setText(displayName, juce::dontSendNotification);
    caption.setJustificationType(juce::Justification::centred);
    caption.setColour(juce::Label::textColourId, mutedTextColour);
    caption.setFont(juce::FontOptions { 12.0f }.withStyle("Bold"));
    addAndMakeVisible(caption);
}

void LabelledKnob::resized()
{
    auto bounds = getLocalBounds();
    caption.setBounds(bounds.removeFromTop(16));
    slider.setBounds(bounds);
}

// --- PeakMeter --------------------------------------------------------------

void PeakMeter::setLevelDb(float db)
{
    // Instant attack, eased release: a meter that fell as fast as the signal
    // would be unreadable on percussive guitar transients.
    displayedDb = db > displayedDb ? db : displayedDb + (db - displayedDb) * 0.25f;
    repaint();
}

void PeakMeter::paint(juce::Graphics& g)
{
    auto bounds = getLocalBounds().toFloat();

    g.setColour(juce::Colour { 0xff0c0e11 });
    g.fillRoundedRectangle(bounds, 3.0f);

    const auto normalised = juce::jlimit(
        0.0f, 1.0f, juce::jmap(displayedDb, static_cast<float>(meterFloorDb), 0.0f, 0.0f, 1.0f));

    if (normalised <= 0.0f)
        return;

    auto filled = bounds.reduced(1.5f);
    filled = filled.removeFromBottom(filled.getHeight() * normalised);

    // Green up to -12 dB, amber approaching 0, red once clipping is imminent.
    const auto colour = displayedDb > -1.0f  ? juce::Colour { 0xffef4444 }
                        : displayedDb > -12.0f ? juce::Colour { 0xfff59e0b }
                                               : juce::Colour { 0xff22c55e };

    g.setColour(colour);
    g.fillRoundedRectangle(filled, 2.0f);
}

// --- NeuralRigEditor --------------------------------------------------------

NeuralRigEditor::NeuralRigEditor(NeuralRigProcessor& p)
    : juce::AudioProcessorEditor(&p),
      audioProcessor(p),
      inputKnob(p.state(), params::id::inputLevel, "INPUT"),
      outputKnob(p.state(), params::id::outputLevel, "OUTPUT"),
      mixKnob(p.state(), params::id::mix, "MIX")
{
    titleLabel.setText("NEURALRIG", juce::dontSendNotification);
    titleLabel.setColour(juce::Label::textColourId, textColour);
    titleLabel.setFont(juce::FontOptions { 26.0f }.withStyle("Bold"));
    addAndMakeVisible(titleLabel);

    subtitleLabel.setText("NAM core " + nam_support::coreVersion() + "  \xe2\x80\xa2  model format "
                              + nam_support::earliestSupportedFileVersion() + "\xe2\x80\x93"
                              + nam_support::latestSupportedFileVersion(),
                          juce::dontSendNotification);
    subtitleLabel.setColour(juce::Label::textColourId, mutedTextColour);
    subtitleLabel.setFont(juce::FontOptions { 12.0f });
    addAndMakeVisible(subtitleLabel);

    for (auto* knob : { &inputKnob, &outputKnob, &mixKnob })
        addAndMakeVisible(knob);

    for (auto* meter : { &inputMeter, &outputMeter })
        addAndMakeVisible(meter);

    modelLabel.setJustificationType(juce::Justification::centredLeft);
    modelLabel.setColour(juce::Label::textColourId, textColour);
    modelLabel.setFont(juce::FontOptions { 14.0f });
    addAndMakeVisible(modelLabel);

    loadButton.onClick = [this] { chooseModel(); };
    clearButton.onClick = [this] { audioProcessor.clearModel(); };

    for (auto* button : { &loadButton, &clearButton })
    {
        button->setColour(juce::TextButton::buttonColourId, juce::Colour { 0xff262b33 });
        button->setColour(juce::TextButton::textColourOffId, textColour);
        addAndMakeVisible(button);
    }

    refreshModelDisplay();

    setSize(editorWidth, editorHeight);
    startTimerHz(30);
}

void NeuralRigEditor::chooseModel()
{
    fileChooser = std::make_unique<juce::FileChooser>("Load a NAM capture", juce::File {}, "*.nam");

    // Not named `flags`: juce::Component already has a member by that name.
    const auto chooserFlags = juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles;

    fileChooser->launchAsync(chooserFlags, [this](const juce::FileChooser& chooser) {
        const auto file = chooser.getResult();

        // An empty result means the user cancelled, which is not an error.
        if (file != juce::File {})
            audioProcessor.loadModel(file);
    });
}

void NeuralRigEditor::refreshModelDisplay()
{
    const auto name = audioProcessor.loadedModelName();
    const auto error = audioProcessor.lastLoadError();

    if (error.isNotEmpty())
    {
        modelLabel.setColour(juce::Label::textColourId, juce::Colour { 0xffef4444 });
        modelLabel.setText(error, juce::dontSendNotification);
    }
    else if (name.isNotEmpty())
    {
        modelLabel.setColour(juce::Label::textColourId, textColour);
        modelLabel.setText(name, juce::dontSendNotification);
    }
    else
    {
        modelLabel.setColour(juce::Label::textColourId, mutedTextColour);
        modelLabel.setText("No capture loaded \xe2\x80\x94 signal passes through clean",
                           juce::dontSendNotification);
    }

    clearButton.setEnabled(name.isNotEmpty());
}

NeuralRigEditor::~NeuralRigEditor()
{
    stopTimer();
}

void NeuralRigEditor::timerCallback()
{
    inputMeter.setLevelDb(audioProcessor.inputPeakDb());
    outputMeter.setLevelDb(audioProcessor.outputPeakDb());

    // Only touch the labels when a load has actually completed; comparing a
    // counter avoids rebuilding strings 30 times a second.
    if (const auto generation = audioProcessor.modelChangeCount(); generation != lastSeenModelGeneration)
    {
        lastSeenModelGeneration = generation;
        refreshModelDisplay();
    }
}

void NeuralRigEditor::paint(juce::Graphics& g)
{
    g.fillAll(backgroundColour);

    auto panel = getLocalBounds().reduced(16).withTrimmedTop(64).toFloat();
    g.setColour(panelColour);
    g.fillRoundedRectangle(panel, 8.0f);

    g.setColour(mutedTextColour.withAlpha(0.6f));
    g.setFont(juce::FontOptions { 11.0f });
    g.drawText("Neural chain arrives in the next milestone \xe2\x80\x94 signal path is currently clean.",
               getLocalBounds().removeFromBottom(28).reduced(24, 0),
               juce::Justification::centredLeft);
}

void NeuralRigEditor::resized()
{
    auto bounds = getLocalBounds().reduced(16);

    auto header = bounds.removeFromTop(56);
    titleLabel.setBounds(header.removeFromTop(32));
    subtitleLabel.setBounds(header);

    auto modelRow = bounds.removeFromTop(38).reduced(0, 4);
    loadButton.setBounds(modelRow.removeFromLeft(130));
    modelRow.removeFromLeft(8);
    clearButton.setBounds(modelRow.removeFromRight(70));
    modelRow.removeFromRight(8);
    modelLabel.setBounds(modelRow);

    bounds.removeFromBottom(28);
    auto body = bounds.reduced(16);

    inputMeter.setBounds(body.removeFromLeft(14).reduced(0, 8));
    body.removeFromLeft(12);
    outputMeter.setBounds(body.removeFromRight(14).reduced(0, 8));
    body.removeFromRight(12);

    const auto knobWidth = body.getWidth() / 3;
    inputKnob.setBounds(body.removeFromLeft(knobWidth));
    outputKnob.setBounds(body.removeFromLeft(knobWidth));
    mixKnob.setBounds(body);
}

} // namespace nr
