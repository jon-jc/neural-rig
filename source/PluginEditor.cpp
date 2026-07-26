#include "PluginEditor.h"

#include "NamSupport.h"

namespace nr
{
namespace
{
constexpr int editorWidth = 640;
constexpr int editorHeight = 520;
constexpr int slotRowHeight = 34;
constexpr int meterFloorDb = -60;

const juce::Colour backgroundColour { 0xff14161a };
const juce::Colour panelColour { 0xff1d2026 };
const juce::Colour rowColour { 0xff23272f };
const juce::Colour accentColour { 0xff38bdf8 };
const juce::Colour textColour { 0xffe2e8f0 };
const juce::Colour mutedTextColour { 0xff8b93a1 };
const juce::Colour errorColour { 0xffef4444 };

void styleButton(juce::Button& button)
{
    button.setColour(juce::TextButton::buttonColourId, juce::Colour { 0xff2c313a });
    button.setColour(juce::TextButton::textColourOffId, textColour);
}
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
    const auto colour = displayedDb > -1.0f    ? errorColour
                        : displayedDb > -12.0f ? juce::Colour { 0xfff59e0b }
                                               : juce::Colour { 0xff22c55e };

    g.setColour(colour);
    g.fillRoundedRectangle(filled, 2.0f);
}

// --- SlotRow ----------------------------------------------------------------

SlotRow::SlotRow(NeuralRigProcessor& p, int slotIndex)
    : audioProcessor(p),
      index(slotIndex),
      enableAttachment(p.state(), params::slotEnabledId(slotIndex), enableToggle)
{
    positionLabel.setText(juce::String(slotIndex + 1), juce::dontSendNotification);
    positionLabel.setJustificationType(juce::Justification::centred);
    positionLabel.setColour(juce::Label::textColourId, mutedTextColour);
    positionLabel.setFont(juce::FontOptions { 13.0f }.withStyle("Bold"));
    addAndMakeVisible(positionLabel);

    nameLabel.setJustificationType(juce::Justification::centredLeft);
    nameLabel.setFont(juce::FontOptions { 13.0f });
    addAndMakeVisible(nameLabel);

    loadButton.onClick = [this] { chooseFile(); };
    clearButton.onClick = [this] { audioProcessor.clearModel(index); };

    // Moving a capture earlier in the chain is a musical change, not a
    // cosmetic one: drive into amp is a different instrument from amp into
    // drive. The first row has nothing to swap with.
    moveUpButton.onClick = [this] { audioProcessor.swapSlots(index, index - 1); };
    moveUpButton.setEnabled(slotIndex > 0);

    for (auto* button : { &loadButton, &clearButton, &moveUpButton })
    {
        styleButton(*button);
        addAndMakeVisible(button);
    }

    enableToggle.setColour(juce::ToggleButton::tickColourId, accentColour);
    enableToggle.setColour(juce::ToggleButton::tickDisabledColourId, juce::Colour { 0xff3a4049 });
    addAndMakeVisible(enableToggle);

    refresh();
}

void SlotRow::chooseFile()
{
    fileChooser = std::make_unique<juce::FileChooser>(
        "Load a NAM capture into slot " + juce::String(index + 1), juce::File {}, "*.nam");

    // Not named `flags`: juce::Component already has a member by that name.
    const auto chooserFlags = juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles;

    fileChooser->launchAsync(chooserFlags, [this](const juce::FileChooser& chooser) {
        const auto file = chooser.getResult();

        // An empty result means the user cancelled, which is not an error.
        if (file != juce::File {})
            audioProcessor.loadModel(index, file);
    });
}

void SlotRow::refresh()
{
    const auto name = audioProcessor.loadedModelName(index);
    const auto error = audioProcessor.lastLoadError(index);

    if (error.isNotEmpty())
    {
        nameLabel.setColour(juce::Label::textColourId, errorColour);
        nameLabel.setText(error, juce::dontSendNotification);
    }
    else if (name.isNotEmpty())
    {
        nameLabel.setColour(juce::Label::textColourId, textColour);
        nameLabel.setText(name, juce::dontSendNotification);
    }
    else
    {
        nameLabel.setColour(juce::Label::textColourId, mutedTextColour);
        nameLabel.setText("empty", juce::dontSendNotification);
    }

    const auto occupied = name.isNotEmpty();
    clearButton.setEnabled(occupied);
    enableToggle.setEnabled(occupied);
}

void SlotRow::paint(juce::Graphics& g)
{
    g.setColour(rowColour);
    g.fillRoundedRectangle(getLocalBounds().toFloat().reduced(0.0f, 2.0f), 4.0f);
}

void SlotRow::resized()
{
    auto bounds = getLocalBounds().reduced(6, 4);

    positionLabel.setBounds(bounds.removeFromLeft(20));
    bounds.removeFromLeft(4);
    loadButton.setBounds(bounds.removeFromLeft(62));
    bounds.removeFromLeft(6);

    clearButton.setBounds(bounds.removeFromRight(28));
    bounds.removeFromRight(4);
    moveUpButton.setBounds(bounds.removeFromRight(28));
    bounds.removeFromRight(4);
    enableToggle.setBounds(bounds.removeFromRight(30));
    bounds.removeFromRight(4);

    nameLabel.setBounds(bounds);
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

    rackLabel.setText("CAPTURE CHAIN  \xe2\x80\x94  signal flows top to bottom",
                      juce::dontSendNotification);
    rackLabel.setColour(juce::Label::textColourId, mutedTextColour);
    rackLabel.setFont(juce::FontOptions { 11.0f }.withStyle("Bold"));
    addAndMakeVisible(rackLabel);

    for (int slot = 0; slot < params::numSlots; ++slot)
    {
        slotRows[static_cast<size_t>(slot)] = std::make_unique<SlotRow>(p, slot);
        addAndMakeVisible(*slotRows[static_cast<size_t>(slot)]);
    }

    for (auto* knob : { &inputKnob, &outputKnob, &mixKnob })
        addAndMakeVisible(knob);

    for (auto* meter : { &inputMeter, &outputMeter })
        addAndMakeVisible(meter);

    setSize(editorWidth, editorHeight);
    startTimerHz(30);
}

NeuralRigEditor::~NeuralRigEditor()
{
    stopTimer();
}

void NeuralRigEditor::timerCallback()
{
    inputMeter.setLevelDb(audioProcessor.inputPeakDb());
    outputMeter.setLevelDb(audioProcessor.outputPeakDb());

    // Only touch the rows when a load actually completed; comparing a counter
    // avoids rebuilding strings 30 times a second.
    if (const auto generation = audioProcessor.modelChangeCount(); generation != lastSeenModelGeneration)
    {
        lastSeenModelGeneration = generation;

        for (auto& row : slotRows)
            if (row != nullptr)
                row->refresh();
    }
}

void NeuralRigEditor::paint(juce::Graphics& g)
{
    g.fillAll(backgroundColour);

    auto panel = getLocalBounds().reduced(16).withTrimmedTop(64).toFloat();
    g.setColour(panelColour);
    g.fillRoundedRectangle(panel, 8.0f);
}

void NeuralRigEditor::resized()
{
    auto bounds = getLocalBounds().reduced(16);

    auto header = bounds.removeFromTop(56);
    titleLabel.setBounds(header.removeFromTop(32));
    subtitleLabel.setBounds(header);

    auto body = bounds.reduced(12);

    rackLabel.setBounds(body.removeFromTop(20));
    body.removeFromTop(4);

    for (auto& row : slotRows)
        if (row != nullptr)
            row->setBounds(body.removeFromTop(slotRowHeight));

    body.removeFromTop(12);

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
