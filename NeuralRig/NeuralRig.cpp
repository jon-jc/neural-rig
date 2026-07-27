#include <algorithm> // std::clamp, std::min
#include <cmath> // pow
#include <filesystem>
#include <iostream>
#include <utility>

#include "Colors.h"
#include "../NeuralAmpModelerCore/NAM/activations.h"
#include "../NeuralAmpModelerCore/NAM/get_dsp.h"
// clang-format off
// These includes need to happen in this order or else the latter won't know
// a bunch of stuff.
#include "NeuralRig.h"
#include "IPlug_include_in_plug_src.h"
// clang-format on
#include "architecture.hpp"

#include "NeuralRigControls.h"
#include "T3KBrowserControl.h"
#include "Theme.h"

using namespace iplug;
using namespace igraphics;

const double kDCBlockerFrequency = 5.0;

// Styles
const IVColorSpec colorSpec{
  DEFAULT_BGCOLOR, // Background
  PluginColors::NAM_THEMECOLOR, // Foreground
  PluginColors::NAM_THEMECOLOR.WithOpacity(0.3f), // Pressed
  PluginColors::NAM_THEMECOLOR.WithOpacity(0.4f), // Frame
  PluginColors::MOUSEOVER, // Highlight
  DEFAULT_SHCOLOR, // Shadow
  PluginColors::NAM_THEMECOLOR, // Extra 1
  COLOR_RED, // Extra 2 --> color for clipping in meters
  PluginColors::NAM_THEMECOLOR.WithContrast(0.1f), // Extra 3
};

const IVStyle style =
  IVStyle{true, // Show label
          true, // Show value
          colorSpec,
          {DEFAULT_TEXT_SIZE + 3.f, EVAlign::Middle, PluginColors::NAM_THEMEFONTCOLOR}, // Knob label text5
          {DEFAULT_TEXT_SIZE + 3.f, EVAlign::Bottom, PluginColors::NAM_THEMEFONTCOLOR}, // Knob value text
          DEFAULT_HIDE_CURSOR,
          DEFAULT_DRAW_FRAME,
          false,
          DEFAULT_EMBOSS,
          0.2f,
          2.f,
          DEFAULT_SHADOW_OFFSET,
          DEFAULT_WIDGET_FRAC,
          DEFAULT_WIDGET_ANGLE};
// Michroma is wide and geometric; letting it run large with the amber accent
// makes the wordmark carry the header on its own.
const IVStyle titleStyle = DEFAULT_STYLE
                             .WithValueText(IText(28, PluginColors::INK, "Michroma-Regular"))
                             .WithDrawFrame(false)
                             .WithDrawShadows(false);
const IVStyle radioButtonStyle =
  style
    .WithColor(EVColor::kON, PluginColors::NAM_THEMECOLOR) // Pressed buttons and their labels
    .WithColor(EVColor::kOFF, PluginColors::NAM_THEMECOLOR.WithOpacity(0.1f)) // Unpressed buttons
    .WithColor(EVColor::kX1, PluginColors::NAM_THEMECOLOR.WithOpacity(0.6f)); // Unpressed buttons' labels

EMsgBoxResult _ShowMessageBox(iplug::igraphics::IGraphics* pGraphics, const char* str, const char* caption,
                              EMsgBoxType type)
{
#ifdef OS_MAC
  // macOS is backwards?
  return pGraphics->ShowMessageBox(caption, str, type);
#else
  return pGraphics->ShowMessageBox(str, caption, type);
#endif
}

const std::string kCalibrateInputParamName = "CalibrateInput";
const bool kDefaultCalibrateInput = false;
const std::string kInputCalibrationLevelParamName = "InputCalibrationLevel";
const double kDefaultInputCalibrationLevel = 12.0;


NeuralRig::NeuralRig(const InstanceInfo& info)
: Plugin(info, MakeConfig(kNumParams, kNumPresets))
{
  _InitToneStack();
  nam::activations::Activation::enable_fast_tanh();
  GetParam(kInputLevel)->InitGain("Input", 0.0, -20.0, 20.0, 0.1);
  GetParam(kToneBass)->InitDouble("Bass", 5.0, 0.0, 10.0, 0.1);
  GetParam(kToneMid)->InitDouble("Middle", 5.0, 0.0, 10.0, 0.1);
  GetParam(kToneTreble)->InitDouble("Treble", 5.0, 0.0, 10.0, 0.1);
  GetParam(kOutputLevel)->InitGain("Output", 0.0, -40.0, 40.0, 0.1);
  GetParam(kNoiseGateThreshold)->InitGain("Threshold", -80.0, -100.0, 0.0, 0.1);
  GetParam(kNoiseGateActive)->InitBool("NoiseGateActive", true);
  GetParam(kEQActive)->InitBool("ToneStack", true);
  GetParam(kOutputMode)->InitEnum("OutputMode", 1, {"Raw", "Normalized", "Calibrated"}); // TODO DRY w/ control
  GetParam(kIRToggle)->InitBool("IRToggle", true);
  GetParam(kCalibrateInput)->InitBool(kCalibrateInputParamName.c_str(), kDefaultCalibrateInput);
  GetParam(kInputCalibrationLevel)
    ->InitDouble(kInputCalibrationLevelParamName.c_str(), kDefaultInputCalibrationLevel, -60.0, 60.0, 0.1, "dBu");
  GetParam(kSlim)->InitDouble("Slim", 0.0, 0.0, 1.0, 0.01);
  // Enable per capture slot. Default on, so loading into a slot makes it
  // audible without a second click.
  for (size_t slot = 0; slot < kNumSlots; slot++)
  {
    const std::string name = "Slot" + std::to_string(slot + 1) + "Active";
    GetParam(SlotActiveParam(slot))->InitBool(name.c_str(), true);
  }

  // Pedal FX. Off by default: a capture should sound like the capture until
  // the user asks for something on top of it.
  GetParam(kDriveActive)->InitBool("DriveActive", false);
  GetParam(kDriveAmount)->InitDouble("Drive", 3.0, 0.0, 10.0, 0.01);
  GetParam(kDelayActive)->InitBool("DelayActive", false);
  GetParam(kDelayTime)->InitDouble("Time", 350.0, 20.0, 1500.0, 1.0, "ms");
  GetParam(kDelayMix)->InitDouble("Mix", 25.0, 0.0, 100.0, 0.1, "%");
  GetParam(kReverbActive)->InitBool("ReverbActive", false);
  GetParam(kReverbAmount)->InitDouble("Reverb", 25.0, 0.0, 100.0, 0.1, "%");

  mNoiseGateTrigger.AddListener(&mNoiseGateGain);

  mMakeGraphicsFunc = [&]() {

#ifdef OS_IOS
    auto scaleFactor = GetScaleForScreen(PLUG_WIDTH, PLUG_HEIGHT) * 0.85f;
#else
    auto scaleFactor = 1.0f;
#endif

    return MakeGraphics(*this, PLUG_WIDTH, PLUG_HEIGHT, PLUG_FPS, scaleFactor);
  };

  mLayoutFunc = [&](IGraphics* pGraphics) {
    pGraphics->AttachCornerResizer(EUIResizerMode::Scale, false);
    pGraphics->AttachTextEntryControl();
    pGraphics->EnableMouseOver(true);
    pGraphics->EnableTooltips(true);
    pGraphics->EnableMultiTouch(true);

    pGraphics->LoadFont("Roboto-Regular", ROBOTO_FN);
    pGraphics->LoadFont("Michroma-Regular", MICHROMA_FN);

    const auto gearSVG = pGraphics->LoadSVG(GEAR_FN);
    const auto fileSVG = pGraphics->LoadSVG(FILE_FN);
    const auto globeSVG = pGraphics->LoadSVG(GLOBE_ICON_FN);
    const auto crossSVG = pGraphics->LoadSVG(CLOSE_BUTTON_FN);
    const auto rightArrowSVG = pGraphics->LoadSVG(RIGHT_ARROW_FN);
    const auto leftArrowSVG = pGraphics->LoadSVG(LEFT_ARROW_FN);
    const auto modelIconSVG = pGraphics->LoadSVG(MODEL_ICON_FN);
    const auto irIconOnSVG = pGraphics->LoadSVG(IR_ICON_ON_FN);
    const auto irIconOffSVG = pGraphics->LoadSVG(IR_ICON_OFF_FN);
    const auto slimIconSVG = pGraphics->LoadSVG(SLIMMABLE_ICON_FN);

    const auto backgroundBitmap = pGraphics->LoadBitmap(BACKGROUND_FN);
    const auto fileBackgroundBitmap = pGraphics->LoadBitmap(FILEBACKGROUND_FN);
    const auto inputLevelBackgroundBitmap = pGraphics->LoadBitmap(INPUTLEVELBACKGROUND_FN);
    const auto linesBitmap = pGraphics->LoadBitmap(LINES_FN);
    const auto knobBackgroundBitmap = pGraphics->LoadBitmap(KNOBBACKGROUND_FN);
    const auto switchHandleBitmap = pGraphics->LoadBitmap(SLIDESWITCHHANDLE_FN);
    const auto meterBackgroundBitmap = pGraphics->LoadBitmap(METERBACKGROUND_FN);

    const auto b = pGraphics->GetBounds();

    // Every section is carved off a running remainder rather than positioned by
    // hand, so nothing can overlap however the numbers are tuned. Reading top to
    // bottom here is reading the signal path. The browser floats above all of
    // it, so it takes no space here.
    auto remaining = b.GetPadded(-14.f);

    const auto headerArea = remaining.GetFromTop(46.f);
    remaining = remaining.GetReducedFromTop(46.f + 8.f);

    const auto titleArea = headerArea.GetFromLeft(320.f);
    const auto settingsButtonArea = headerArea.GetFromRight(34.f).GetMidVPadded(17.f);

    // Section heights are derived from what they hold rather than guessed. The
    // FX group previously got less than a knob row plus a toggle row, so the
    // toggles drew over the knobs and the value readouts fell off the window.
    const auto knobBlockHeight = NAM_KNOB_HEIGHT + 34.f; // knob + its value text
    // A switch plus its caption below it. The switch itself needs to stay a
    // comfortable click target, so this is generous rather than tight -- the
    // previous 28px left the caption drawing over the switch and made both
    // awkward to hit.
    const auto switchHeight = 26.f;
    const auto toggleBlockHeight = switchHeight + 30.f;
    const auto groupChrome = 28.f; // border and label

    // --- Amp: knobs across the full width, with meters bracketing them -------
    const auto ampGroupHeight = knobBlockHeight + toggleBlockHeight + groupChrome;
    const auto ampGroup = remaining.GetFromTop(ampGroupHeight);
    remaining = remaining.GetReducedFromTop(ampGroupHeight + 10.f);

    const auto ampInner = ampGroup.GetPadded(-12.f).GetReducedFromTop(14.f);
    const auto inputMeterArea = ampInner.GetFromLeft(22.f).GetMidVPadded(78.f);
    const auto outputMeterArea = ampInner.GetFromRight(22.f).GetMidVPadded(78.f);

    const auto knobsArea = ampInner.GetReducedFromLeft(44.f).GetReducedFromRight(44.f).GetFromTop(NAM_KNOB_HEIGHT);
    const auto inputKnobArea = knobsArea.GetGridCell(0, kInputLevel, 1, numKnobs).GetMidHPadded(52.f);
    const auto noiseGateArea = knobsArea.GetGridCell(0, kNoiseGateThreshold, 1, numKnobs).GetMidHPadded(52.f);
    const auto bassKnobArea = knobsArea.GetGridCell(0, kToneBass, 1, numKnobs).GetMidHPadded(52.f);
    const auto midKnobArea = knobsArea.GetGridCell(0, kToneMid, 1, numKnobs).GetMidHPadded(52.f);
    const auto trebleKnobArea = knobsArea.GetGridCell(0, kToneTreble, 1, numKnobs).GetMidHPadded(52.f);
    const auto outputKnobArea = knobsArea.GetGridCell(0, kOutputLevel, 1, numKnobs).GetMidHPadded(52.f);

    // Toggles sit under the knob they belong to. The control draws its own
    // caption inside its bounds, so it needs the whole block -- handing it only
    // the switch height is what made the label collide with the switch and both
    // awkward to hit.
    const auto toggleRow = ampInner.GetFromBottom(toggleBlockHeight);
    const auto ngToggleArea = toggleRow.GetGridCell(0, kNoiseGateThreshold, 1, numKnobs).GetMidHPadded(44.f);
    const auto eqToggleArea = toggleRow.GetGridCell(0, kToneMid, 1, numKnobs).GetMidHPadded(44.f);

    // --- Capture chain ------------------------------------------------------
    const auto fileHeight = 32.0f;
    const auto chainGroupHeight = fileHeight * static_cast<float>(kNumSlots) * 1.45f + groupChrome;
    const auto chainGroup = remaining.GetFromTop(chainGroupHeight);
    remaining = remaining.GetReducedFromTop(chainGroupHeight + 10.f);

    const auto chainRows = chainGroup.GetPadded(-12.f).GetReducedFromTop(16.f);
    const auto slotPitch = chainRows.H() / static_cast<float>(kNumSlots);

    auto slotRow = [&](size_t slot) {
      return chainRows.GetFromTop(slotPitch).GetVShifted(slotPitch * static_cast<float>(slot));
    };
    // Left gutter carries the slot number and the connector between stages.
    auto slotNumberArea = [&](size_t slot) { return slotRow(slot).GetFromLeft(34.f); };
    auto slotArea = [&](size_t slot) {
      return slotRow(slot).GetReducedFromLeft(38.f).GetMidVPadded(fileHeight * 0.5f);
    };

    // --- Cabinet ------------------------------------------------------------
    const auto cabinetGroupHeight = fileHeight + groupChrome + 12.f;
    const auto cabinetGroup = remaining.GetFromTop(cabinetGroupHeight);
    remaining = remaining.GetReducedFromTop(cabinetGroupHeight + 10.f);

    const auto irArea = cabinetGroup.GetPadded(-12.f).GetReducedFromTop(16.f).GetFromTop(fileHeight);
    const auto irSwitchArea = irArea.GetFromLeft(30.0f).GetHShifted(-34.0f).GetScaledAboutCentre(0.6f);

    // --- Pedal FX -----------------------------------------------------------
    // Post-amp, like a wet effects loop: drive belongs in front of a capture
    // and these do not, so they sit after the cabinet where a real rig puts
    // time-based effects.
    const auto fxGroup = remaining.GetFromTop(knobBlockHeight + toggleBlockHeight + groupChrome);
    const auto fxInner = fxGroup.GetPadded(-12.f).GetReducedFromTop(16.f);
    const auto fxKnobRow = fxInner.GetFromTop(NAM_KNOB_HEIGHT);
    const auto fxToggleRow = fxInner.GetFromBottom(toggleBlockHeight);

    const auto driveKnobArea = fxKnobRow.GetGridCell(0, 0, 1, 4).GetMidHPadded(48.f);
    const auto delayKnobArea = fxKnobRow.GetGridCell(0, 1, 1, 4).GetMidHPadded(48.f);
    const auto delayMixKnobArea = fxKnobRow.GetGridCell(0, 2, 1, 4).GetMidHPadded(48.f);
    const auto reverbKnobArea = fxKnobRow.GetGridCell(0, 3, 1, 4).GetMidHPadded(48.f);

    const auto driveToggleArea = fxToggleRow.GetGridCell(0, 0, 1, 4).GetMidHPadded(40.f);
    const auto delayToggleArea = fxToggleRow.GetGridCell(0, 1, 1, 4).GetMidHPadded(40.f);
    const auto reverbToggleArea = fxToggleRow.GetGridCell(0, 3, 1, 4).GetMidHPadded(40.f);

    // Legacy anchors, kept so the slim-model overlay lands somewhere sensible.
    const auto modelArea = slotArea(0);
    const auto slimIconArea =
      IRECT(modelArea.R + 6.f, modelArea.MH() - 14.f, modelArea.R + 6.f + 2.f * 28.f, modelArea.MH() + 14.f);
    const auto modelIconArea = titleArea.GetFromRight(30.f);

    // Model loader button
    auto makeLoadModelCompletionHandler = [&](size_t slot) {
      return [this, slot](const WDL_String& fileName, const WDL_String& path) {
        if (fileName.GetLength())
        {
          // Sets mNAMPaths[slot] and mStagedModels[slot]
          const std::string msg = _StageModel(slot, fileName);
          // TODO error messages like the IR loader.
          if (msg.size())
          {
            std::stringstream ss;
            ss << "Failed to load NAM model. Message:\n\n" << msg;
            _ShowMessageBox(GetUI(), ss.str().c_str(), "Failed to load model!", kMB_OK);
          }
          std::cout << "Loaded into slot " << (slot + 1) << ": " << fileName.Get() << std::endl;
        }
      };
    };

    // IR loader button
    auto loadIRCompletionHandler = [&](const WDL_String& fileName, const WDL_String& path) {
      if (fileName.GetLength())
      {
        mIRPath = fileName;
        const dsp::wav::LoadReturnCode retCode = _StageIR(fileName);
        if (retCode != dsp::wav::LoadReturnCode::SUCCESS)
        {
          std::stringstream message;
          message << "Failed to load IR file " << fileName.Get() << ":\n";
          message << dsp::wav::GetMsgForLoadReturnCode(retCode);

          _ShowMessageBox(GetUI(), message.str().c_str(), "Failed to load IR!", kMB_OK);
        }
      }
    };

    // Drawn rather than photographed, so it stays sharp at any editor scale and
    // the palette lives in one place.
    pGraphics->AttachControl(new nr::theme::ChassisControl(b));
    pGraphics->AttachControl(new IBitmapControl(b, linesBitmap));
    pGraphics->AttachControl(new IVLabelControl(titleArea, "NEURALRIG", titleStyle));
    pGraphics->AttachControl(new ISVGControl(modelIconArea, modelIconSVG));

#ifdef NAM_PICK_DIRECTORY
    const std::string defaultNamFileString = "Select model directory...";
    const std::string defaultIRString = "Select IR directory...";
#else
    const std::string defaultNamFileString = "Select model...";
    const std::string defaultIRString = "Select IR...";
#endif
    // Getting started page listing additional resources
    // Cabinet IRs are .wav, which the in-plugin browser does not handle -- it
    // asks TONE3000 for NAM captures only. So the IR globe still opens a web
    // page, pointed at TONE3000's IR catalogue rather than upstream's page.
    auto browseForIRs = [](IControl* pCaller) {
      WDL_String url("https://www.tone3000.com/search?formats=ir");
      pCaller->GetUI()->OpenURL(url.Get());
    };
    // Raised, titled panels rather than wire-frame groups: a filled surface a
    // step lighter than the chassis, a shadow under it and a lit top edge do
    // far more to separate sections than a drawn border does.
    pGraphics->AttachControl(new nr::theme::SectionPanelControl(ampGroup, "AMP"));
    pGraphics->AttachControl(new nr::theme::SectionPanelControl(chainGroup, "CAPTURE CHAIN"));
    pGraphics->AttachControl(new nr::theme::SectionPanelControl(cabinetGroup, "CABINET"));
    pGraphics->AttachControl(new nr::theme::SectionPanelControl(fxGroup, "PEDAL FX"));

    // The connector threading the capture slots together. Without it the rows
    // read as four unrelated file pickers rather than a signal path.
    pGraphics->AttachControl(
      new nr::theme::ChainFlowControl(chainRows.GetFromLeft(34.f), static_cast<int>(kNumSlots), slotPitch),
      kCtrlTagChainFlow);

    // Pedal FX
    pGraphics->AttachControl(new NAMKnobControl(driveKnobArea, kDriveAmount, "Drive", style, knobBackgroundBitmap));
    pGraphics->AttachControl(new NAMKnobControl(delayKnobArea, kDelayTime, "Delay", style, knobBackgroundBitmap));
    pGraphics->AttachControl(new NAMKnobControl(delayMixKnobArea, kDelayMix, "Delay Mix", style, knobBackgroundBitmap));
    pGraphics->AttachControl(new NAMKnobControl(reverbKnobArea, kReverbAmount, "Reverb", style, knobBackgroundBitmap));

    pGraphics->AttachControl(
      new NAMSwitchControl(driveToggleArea, kDriveActive, "Drive", style, switchHandleBitmap));
    pGraphics->AttachControl(
      new NAMSwitchControl(delayToggleArea, kDelayActive, "Delay", style, switchHandleBitmap));
    pGraphics->AttachControl(
      new NAMSwitchControl(reverbToggleArea, kReverbActive, "Reverb", style, switchHandleBitmap));

    for (size_t slot = 0; slot < kNumSlots; slot++)
    {
      // Slot numbers are drawn by ChainFlowControl, on its connector, so they
      // sit on the signal path rather than beside it.

      // The globe opens the in-plugin TONE3000 browser aimed at this slot, so
      // picking a capture fills the row the user clicked. Upstream sends the
      // user to a web page instead, which leaves them to download, unzip and
      // find the file by hand.
      // The globe opens the floating browser aimed at this slot, so whatever is
      // picked lands in the row that was clicked.
      auto browseForSlot = [pGraphics, slot](IControl*) {
        auto* page = pGraphics->GetControlWithTag(kCtrlTagT3KBrowser)->As<T3KBrowserPageControl>();
        page->SetTargetSlot(static_cast<int>(slot));
        page->OpenBrowser();
      };

      pGraphics->AttachControl(
        new NAMFileBrowserControl(slotArea(slot), kMsgTagClearModel, defaultNamFileString.c_str(), "nam",
                                  makeLoadModelCompletionHandler(slot), style, fileSVG, crossSVG, leftArrowSVG,
                                  rightArrowSVG, fileBackgroundBitmap, globeSVG, "Browse TONE3000", browseForSlot),
        ModelBrowserCtrlTag(slot));
    }

    auto hideSlimOverlay = [](IControl* pCaller) {
      IGraphics* ui = pCaller->GetUI();
      if (auto* backdrop = ui->GetControlWithTag(kCtrlTagSlimOverlayBackdrop))
        backdrop->Hide(true);
      if (auto* knob = ui->GetControlWithTag(kCtrlTagSlimKnob))
        knob->Hide(true);
      ui->SetAllControlsDirty();
    };
    auto showSlimOverlay = [](IControl* pCaller) {
      IGraphics* ui = pCaller->GetUI();
      if (auto* backdrop = ui->GetControlWithTag(kCtrlTagSlimOverlayBackdrop))
        backdrop->Hide(false);
      if (auto* knob = ui->GetControlWithTag(kCtrlTagSlimKnob))
        knob->Hide(false);
      ui->SetAllControlsDirty();
    };

    pGraphics
      ->AttachControl(
        new NAMSquareButtonControl(slimIconArea, DefaultClickActionFunc, slimIconSVG), kCtrlTagSlimmableIcon)
      ->SetAnimationEndActionFunction(showSlimOverlay)
      ->Hide(true);

    pGraphics->AttachControl(new ISVGSwitchControl(irSwitchArea, {irIconOffSVG, irIconOnSVG}, kIRToggle));
    pGraphics->AttachControl(
      new NAMFileBrowserControl(irArea, kMsgTagClearIR, defaultIRString.c_str(), "wav", loadIRCompletionHandler, style,
                                fileSVG, crossSVG, leftArrowSVG, rightArrowSVG, fileBackgroundBitmap, globeSVG,
                                "Get IRs from TONE3000", browseForIRs),
      kCtrlTagIRFileBrowser);
    pGraphics->AttachControl(
      new NAMSwitchControl(ngToggleArea, kNoiseGateActive, "Noise Gate", style, switchHandleBitmap));
    pGraphics->AttachControl(new NAMSwitchControl(eqToggleArea, kEQActive, "EQ", style, switchHandleBitmap));

    // The knobs
    pGraphics->AttachControl(new NAMKnobControl(inputKnobArea, kInputLevel, "", style, knobBackgroundBitmap));
    pGraphics->AttachControl(new NAMKnobControl(noiseGateArea, kNoiseGateThreshold, "", style, knobBackgroundBitmap));
    pGraphics->AttachControl(
      new NAMKnobControl(bassKnobArea, kToneBass, "", style, knobBackgroundBitmap), -1, "EQ_KNOBS");
    pGraphics->AttachControl(
      new NAMKnobControl(midKnobArea, kToneMid, "", style, knobBackgroundBitmap), -1, "EQ_KNOBS");
    pGraphics->AttachControl(
      new NAMKnobControl(trebleKnobArea, kToneTreble, "", style, knobBackgroundBitmap), -1, "EQ_KNOBS");
    pGraphics->AttachControl(new NAMKnobControl(outputKnobArea, kOutputLevel, "", style, knobBackgroundBitmap));

    // The meters
    pGraphics->AttachControl(new NAMMeterControl(inputMeterArea, meterBackgroundBitmap, style), kCtrlTagInputMeter);
    pGraphics->AttachControl(new NAMMeterControl(outputMeterArea, meterBackgroundBitmap, style), kCtrlTagOutputMeter);

    // Settings/help/about box
    pGraphics->AttachControl(new NAMCircleButtonControl(
      settingsButtonArea,
      [pGraphics](IControl* pCaller) {
        pGraphics->GetControlWithTag(kCtrlTagSettingsBox)->As<NAMSettingsPageControl>()->HideAnimated(false);
      },
      gearSVG));

    pGraphics
      ->AttachControl(new NAMSettingsPageControl(b, backgroundBitmap, inputLevelBackgroundBitmap, switchHandleBitmap,
                                                 crossSVG, style, radioButtonStyle),
                      kCtrlTagSettingsBox)
      ->Hide(true);

    // TONE3000 browser, and the button that opens it.
    {
      auto* browserPage = new T3KBrowserPageControl(b, mBrowser, style);

      browserPage->SetLoadIntoSlotFunc([this](int slot, const char* filePath) {
        // Called from a worker thread. Park the path and let OnIdle stage it on
        // the message thread rather than touching model state from here.
        std::lock_guard<std::mutex> lock(mPendingLoadMutex);
        mPendingLoads.emplace_back(slot, std::string(filePath));
      });

      // Floats above the rig, closed until asked for. A native web view always
      // draws over the IGraphics surface, so it cannot be docked without either
      // stealing space permanently or being too small to browse in; letting the
      // user move, resize and close it is what makes that liveable.
      pGraphics->AttachControl(browserPage, kCtrlTagT3KBrowser);
    }

    const auto slimKnobArea = b.GetCentredInside(100.f, NAM_KNOB_HEIGHT + 24.f);
    pGraphics->AttachControl(new NAMSlimOverlayBackdropControl(b, hideSlimOverlay), kCtrlTagSlimOverlayBackdrop)
      ->Hide(true);
    pGraphics
      ->AttachControl(new NAMKnobControl(slimKnobArea, kSlim, "Slim", style, knobBackgroundBitmap), kCtrlTagSlimKnob)
      ->Hide(true);

    pGraphics->ForAllControlsFunc([](IControl* pControl) {
      pControl->SetMouseEventsWhenDisabled(true);
      pControl->SetMouseOverWhenDisabled(true);
    });

    // pGraphics->GetControlWithTag(kCtrlTagOutNorm)->SetMouseEventsWhenDisabled(false);
    // pGraphics->GetControlWithTag(kCtrlTagCalibrateInput)->SetMouseEventsWhenDisabled(false);
  };
}

NeuralRig::~NeuralRig()
{
  _DeallocateIOPointers();
}

void NeuralRig::ProcessBlock(iplug::sample** inputs, iplug::sample** outputs, int nFrames)
{
  const size_t numChannelsExternalIn = (size_t)NInChansConnected();
  const size_t numChannelsExternalOut = (size_t)NOutChansConnected();
  const size_t numChannelsInternal = kNumChannelsInternal;
  const size_t numFrames = (size_t)nFrames;
  const double sampleRate = GetSampleRate();

  // Disable floating point denormals
  std::fenv_t fe_state;
  std::feholdexcept(&fe_state);
  disable_denormals();

  _PrepareBuffers(numChannelsInternal, numFrames);
  // Input is collapsed to mono in preparation for the NAM.
  _ProcessInput(inputs, numFrames, numChannelsExternalIn, numChannelsInternal);
  _ApplyDSPStaging();
  const bool noiseGateActive = GetParam(kNoiseGateActive)->Value();
  const bool toneStackActive = GetParam(kEQActive)->Value();

  // Noise gate trigger
  sample** triggerOutput = mInputPointers;
  if (noiseGateActive)
  {
    const double time = 0.01;
    const double threshold = GetParam(kNoiseGateThreshold)->Value(); // GetParam...
    const double ratio = 0.1; // Quadratic...
    const double openTime = 0.005;
    const double holdTime = 0.01;
    const double closeTime = 0.05;
    const dsp::noise_gate::TriggerParams triggerParams(time, threshold, ratio, openTime, holdTime, closeTime);
    mNoiseGateTrigger.SetParams(triggerParams);
    mNoiseGateTrigger.SetSampleRate(sampleRate);
    triggerOutput = mNoiseGateTrigger.Process(mInputPointers, numChannelsInternal, numFrames);
  }

  // Run the capture chain. Each loaded slot feeds the next, so the second
  // network sees the first one's actual output rather than a clean signal --
  // which is what makes a drive capture into an amp capture behave like the
  // real pairing.
  //
  // Buffers ping-pong because a capture cannot read and write the same one:
  // its resampler is stateful and would see its own output as input.
  {
    sample** chainSource = triggerOutput;
    sample** chainDest = mOutputPointers;
    bool processedAny = false;

    for (size_t slot = 0; slot < kNumSlots; slot++)
    {
      if (mModels[slot] == nullptr)
        continue;

      if (GetParam(SlotActiveParam(slot))->Bool())
      {
        mModels[slot]->process(chainSource, chainDest, nFrames);
      }
      else
      {
        // Disabled, but still loaded: carry the audio through a delay matching
        // this slot's latency rather than skipping it, so the plugin's
        // reported latency does not move when the user toggles a slot.
        for (size_t c = 0; c < numChannelsInternal; c++)
          memcpy(chainDest[c], chainSource[c], numFrames * sizeof(sample));

        _RunBypassDelay(slot, chainDest, numChannelsInternal, numFrames);
      }

      processedAny = true;
      chainSource = chainDest;
      chainDest = (chainDest == mOutputPointers) ? mChainPointers : mOutputPointers;
    }

    if (!processedAny)
    {
      _FallbackDSP(triggerOutput, mOutputPointers, numChannelsInternal, numFrames);
    }
    else if (chainSource != mOutputPointers)
    {
      // Odd number of stages left the result in the scratch buffer.
      for (size_t c = 0; c < numChannelsInternal; c++)
        memcpy(mOutputPointers[c], chainSource[c], numFrames * sizeof(sample));
    }
  }
  // Apply the noise gate after the NAM
  sample** gateGainOutput =
    noiseGateActive ? mNoiseGateGain.Process(mOutputPointers, numChannelsInternal, numFrames) : mOutputPointers;

  sample** toneStackOutPointers = (toneStackActive && mToneStack != nullptr)
                                    ? mToneStack->Process(gateGainOutput, numChannelsInternal, nFrames)
                                    : gateGainOutput;

  sample** irPointers = toneStackOutPointers;
  if (mIR != nullptr && GetParam(kIRToggle)->Value())
    irPointers = mIR->Process(toneStackOutPointers, numChannelsInternal, numFrames);

  // And the HPF for DC offset (Issue 271)
  const double highPassCutoffFreq = kDCBlockerFrequency;
  // const double lowPassCutoffFreq = 20000.0;
  const recursive_linear_filter::HighPassParams highPassParams(sampleRate, highPassCutoffFreq);
  // const recursive_linear_filter::LowPassParams lowPassParams(sampleRate, lowPassCutoffFreq);
  mHighPass.SetParams(highPassParams);
  // mLowPass.SetParams(lowPassParams);
  sample** hpfPointers = mHighPass.Process(irPointers, numChannelsInternal, numFrames);

  // Pedal FX, after the cabinet and the DC blocker. Order within the loop is
  // drive into delay into reverb, which is how the pedals would be cabled.
  {
    auto* wet = hpfPointers[0];

    if (GetParam(kDriveActive)->Bool())
    {
      mDrive.SetAmount(GetParam(kDriveAmount)->Value());
      mDrive.Process(wet, nFrames);
    }

    if (GetParam(kDelayActive)->Bool())
    {
      mDelay.SetTimeMs(GetParam(kDelayTime)->Value());
      mDelay.SetMix(GetParam(kDelayMix)->Value() * 0.01);
      mDelay.Process(wet, nFrames);
    }

    if (GetParam(kReverbActive)->Bool())
    {
      mReverb.SetMix(GetParam(kReverbAmount)->Value() * 0.01);
      mReverb.Process(wet, nFrames);
    }
  }
  // sample** lpfPointers = mLowPass.Process(hpfPointers, numChannelsInternal, numFrames);

  // restore previous floating point state
  std::feupdateenv(&fe_state);

  // Let's get outta here
  // This is where we exit mono for whatever the output requires.
  _ProcessOutput(hpfPointers, outputs, numFrames, numChannelsInternal, numChannelsExternalOut);
  // _ProcessOutput(lpfPointers, outputs, numFrames, numChannelsInternal, numChannelsExternalOut);
  // * Output of input leveling (inputs -> mInputPointers),
  // * Output of output leveling (mOutputPointers -> outputs)
  _UpdateMeters(mInputPointers, outputs, numFrames, numChannelsInternal, numChannelsExternalOut);
}

void NeuralRig::OnReset()
{
  const auto sampleRate = GetSampleRate();
  const int maxBlockSize = GetBlockSize();

  // Tail is because the HPF DC blocker has a decay.
  // 10 cycles should be enough to pass the VST3 tests checking tail behavior.
  // I'm ignoring the model & IR, but it's not the end of the world.
  const int tailCycles = 10;
  SetTailSize(tailCycles * (int)(sampleRate / kDCBlockerFrequency));
  mInputSender.Reset(sampleRate);
  mOutputSender.Reset(sampleRate);

  // FX buffers are sized here, off the audio thread, so processing never
  // allocates. 1500 ms matches the delay parameter's maximum.
  mDelay.Prepare(sampleRate, 1500.0);
  mReverb.Prepare(sampleRate);
  mReverb.Reset();
  // If there is a model or IR loaded, they need to be checked for resampling.
  _ResetModelAndIR(sampleRate, GetBlockSize());
  mToneStack->Reset(sampleRate, maxBlockSize);
  _UpdateLatency();
}

void NeuralRig::OnIdle()
{
  mInputSender.TransmitData(*this);
  mOutputSender.TransmitData(*this);

  // Destroy captures the audio thread displaced. Freeing a network's weights
  // is unbounded work, so ProcessBlock hands them over here rather than
  // deleting them mid-block.
  for (size_t slot = 0; slot < kNumSlots; slot++)
    mRetiredModels[slot] = nullptr;

  // Stage anything the browser downloaded. Done here, on the message thread,
  // because the download completes on a worker and staging touches state the
  // audio thread reads.
  {
    std::vector<std::pair<int, std::string>> pending;
    {
      std::lock_guard<std::mutex> lock(mPendingLoadMutex);
      pending.swap(mPendingLoads);
    }

    for (const auto& [slot, path] : pending)
    {
      WDL_String filePath;
      filePath.Set(path.c_str());
      _StageModel(static_cast<size_t>(slot), filePath);

      // Take the browser off TONE3000's "you can close this tab" page, which is
      // a dead end in a plugin with no tabs.
      if (auto* pGraphics = GetUI())
      {
        if (auto* page = pGraphics->GetControlWithTag(kCtrlTagT3KBrowser))
        {
          const std::string label = "slot " + std::to_string(slot + 1);
          page->As<T3KBrowserPageControl>()->OnCaptureLoaded(label.c_str());
        }
      }
    }
  }

  // Light the chain connector for whichever slots hold a capture, so the
  // signal path shows where audio actually flows.
  if (auto* pGraphics = GetUI())
  {
    if (auto* flow = pGraphics->GetControlWithTag(kCtrlTagChainFlow))
    {
      std::vector<bool> occupied(kNumSlots, false);
      bool changed = false;

      for (size_t slot = 0; slot < kNumSlots; slot++)
      {
        occupied[slot] = mNAMPaths[slot].GetLength() > 0;

        if (occupied[slot] != mFlowOccupancy[slot])
        {
          mFlowOccupancy[slot] = occupied[slot];
          changed = true;
        }
      }

      if (changed)
        static_cast<nr::theme::ChainFlowControl*>(flow)->SetOccupancy(occupied);
    }
  }

  // Repaint the browser only when something actually changed, rather than at
  // idle rate.
  if (mBrowser.ConsumeDirty())
  {
    if (auto* pGraphics = GetUI())
    {
      if (auto* page = pGraphics->GetControlWithTag(kCtrlTagT3KBrowser))
        page->As<T3KBrowserPageControl>()->Refresh();
    }
  }

  if (mNewModelLoadedInDSP)
  {
    if (auto* pGraphics = GetUI())
    {
      _UpdateControlsFromModel();
      mNewModelLoadedInDSP = false;
    }
  }
  if (mModelCleared)
  {
    if (auto* pGraphics = GetUI())
    {
      // FIXME -- need to disable only the "normalized" model
      // pGraphics->GetControlWithTag(kCtrlTagOutputMode)->SetDisabled(false);
      static_cast<NAMSettingsPageControl*>(pGraphics->GetControlWithTag(kCtrlTagSettingsBox))->ClearModelInfo();
      if (auto* p = pGraphics->GetControlWithTag(kCtrlTagSlimmableIcon))
        p->Hide(true);
      if (auto* p = pGraphics->GetControlWithTag(kCtrlTagSlimOverlayBackdrop))
        p->Hide(true);
      if (auto* p = pGraphics->GetControlWithTag(kCtrlTagSlimKnob))
        p->Hide(true);
      pGraphics->SetAllControlsDirty();
      mModelCleared = false;
    }
  }
}

bool NeuralRig::SerializeState(IByteChunk& chunk) const
{
  // If this isn't here when unserializing, then we know we're dealing with something before v0.8.0.
  WDL_String header("###NeuralRig###"); // Don't change this!
  chunk.PutStr(header.Get());
  // Plugin version, so we can load legacy serialized states in the future!
  WDL_String version(PLUG_VERSION_STR);
  chunk.PutStr(version.Get());
  // Model directory (don't serialize the model itself; we'll just load it again
  // when we unserialize)
  // One path per capture slot, in chain order, so a reopened session comes
  // back with the same rig rather than just the first amp.
  for (size_t slot = 0; slot < kNumSlots; slot++)
    chunk.PutStr(mNAMPaths[slot].Get());
  chunk.PutStr(mIRPath.Get());
  return SerializeParams(chunk);
}

int NeuralRig::UnserializeState(const IByteChunk& chunk, int startPos)
{
  // Look for the expected header. If it's there, then we'll know what to do.
  WDL_String header;
  int pos = startPos;
  pos = chunk.GetStr(header, pos);

  const char* kExpectedHeader = "###NeuralRig###";
  if (strcmp(header.Get(), kExpectedHeader) == 0)
  {
    return _UnserializeStateWithKnownVersion(chunk, pos);
  }
  else
  {
    return _UnserializeStateWithUnknownVersion(chunk, startPos);
  }
}

void NeuralRig::OnUIOpen()
{
  Plugin::OnUIOpen();

  for (size_t slot = 0; slot < kNumSlots; slot++)
  {
    if (mNAMPaths[slot].GetLength())
    {
      SendControlMsgFromDelegate(ModelBrowserCtrlTag(slot), kMsgTagLoadedModel, mNAMPaths[slot].GetLength(),
                                 mNAMPaths[slot].Get());
      // If it's not loaded yet, then mark as failed.
      // If it's yet to be loaded, then the completion handler will set us straight once it runs.
      if (mModels[slot] == nullptr && mStagedModels[slot] == nullptr)
        SendControlMsgFromDelegate(ModelBrowserCtrlTag(slot), kMsgTagLoadFailed);
    }
  }

  if (mIRPath.GetLength())
  {
    SendControlMsgFromDelegate(kCtrlTagIRFileBrowser, kMsgTagLoadedIR, mIRPath.GetLength(), mIRPath.Get());
    if (mIR == nullptr && mStagedIR == nullptr)
      SendControlMsgFromDelegate(kCtrlTagIRFileBrowser, kMsgTagLoadFailed);
  }

  if (_HaveAnyModel())
  {
    _UpdateControlsFromModel();
  }
}

void NeuralRig::OnParamChange(int paramIdx)
{
  switch (paramIdx)
  {
    // Changes to the input gain
    case kCalibrateInput:
    case kInputCalibrationLevel:
    case kInputLevel: _SetInputGain(); break;
    // Changes to the output gain
    case kOutputLevel:
    case kOutputMode: _SetOutputGain(); break;
    // Tone stack:
    case kToneBass: mToneStack->SetParam("bass", GetParam(paramIdx)->Value()); break;
    case kToneMid: mToneStack->SetParam("middle", GetParam(paramIdx)->Value()); break;
    case kToneTreble: mToneStack->SetParam("treble", GetParam(paramIdx)->Value()); break;
    case kSlim: _ApplySlimParamToLoadedNAMs(); break;
    default: break;
  }
}

void NeuralRig::OnParamChangeUI(int paramIdx, EParamSource source)
{
  if (auto pGraphics = GetUI())
  {
    bool active = GetParam(paramIdx)->Bool();

    switch (paramIdx)
    {
      case kNoiseGateActive: pGraphics->GetControlWithParamIdx(kNoiseGateThreshold)->SetDisabled(!active); break;
      case kEQActive:
        pGraphics->ForControlInGroup("EQ_KNOBS", [active](IControl* pControl) { pControl->SetDisabled(!active); });
        break;
      case kIRToggle: pGraphics->GetControlWithTag(kCtrlTagIRFileBrowser)->SetDisabled(!active); break;
      default: break;
    }
  }
}

bool NeuralRig::OnMessage(int msgTag, int ctrlTag, int dataSize, const void* pData)
{
  switch (msgTag)
  {
    case kMsgTagClearModel:
    {
      // The browser that sent this identifies which slot to empty.
      const int slot = ctrlTag - kCtrlTagModelFileBrowser;
      if (slot >= 0 && slot < static_cast<int>(kNumSlots))
        mShouldRemoveModels[slot] = true;
      return true;
    }
    case kMsgTagClearIR: mShouldRemoveIR = true; return true;
    case kMsgTagHighlightColor:
    {
      mHighLightColor.Set((const char*)pData);

      if (GetUI())
      {
        GetUI()->ForStandardControlsFunc([&](IControl* pControl) {
          if (auto* pVectorBase = pControl->As<IVectorBase>())
          {
            IColor color = IColor::FromColorCodeStr(mHighLightColor.Get());

            pVectorBase->SetColor(kX1, color);
            pVectorBase->SetColor(kPR, color.WithOpacity(0.3f));
            pVectorBase->SetColor(kFR, color.WithOpacity(0.4f));
            pVectorBase->SetColor(kX3, color.WithContrast(0.1f));
          }
          pControl->GetUI()->SetAllControlsDirty();
        });
      }

      return true;
    }
    default: return false;
  }
}

// Private methods ============================================================

void NeuralRig::_AllocateIOPointers(const size_t nChans)
{
  if (mInputPointers != nullptr)
    throw std::runtime_error("Tried to re-allocate mInputPointers without freeing");
  mInputPointers = new sample*[nChans];
  if (mInputPointers == nullptr)
    throw std::runtime_error("Failed to allocate pointer to input buffer!\n");
  if (mOutputPointers != nullptr)
    throw std::runtime_error("Tried to re-allocate mOutputPointers without freeing");
  mOutputPointers = new sample*[nChans];
  if (mOutputPointers == nullptr)
    throw std::runtime_error("Failed to allocate pointer to output buffer!\n");
  if (mChainPointers != nullptr)
    throw std::runtime_error("Tried to re-allocate mChainPointers without freeing");
  mChainPointers = new sample*[nChans];
  if (mChainPointers == nullptr)
    throw std::runtime_error("Failed to allocate pointer to chain buffer!\n");
}

void NeuralRig::_ApplyDSPStaging()
{
  // Remove marked modules. Displaced captures are handed to mRetiredModels
  // rather than destroyed here: freeing a network's weights is unbounded work
  // and this runs on the audio thread. OnIdle() collects them.
  for (size_t slot = 0; slot < kNumSlots; slot++)
  {
    if (mShouldRemoveModels[slot])
    {
      if (mRetiredModels[slot] == nullptr)
        mRetiredModels[slot] = std::move(mModels[slot]);

      mModels[slot] = nullptr;
      mNAMPaths[slot].Set("");
      mShouldRemoveModels[slot] = false;
      mModelCleared = true;
      _UpdateLatency();
      _SetInputGain();
      _SetOutputGain();
    }
  }
  if (mShouldRemoveIR)
  {
    mIR = nullptr;
    mIRPath.Set("");
    mShouldRemoveIR = false;
  }
  // Move things from staged to live
  for (size_t slot = 0; slot < kNumSlots; slot++)
  {
    if (mStagedModels[slot] != nullptr)
    {
      if (mRetiredModels[slot] == nullptr)
        mRetiredModels[slot] = std::move(mModels[slot]);

      mModels[slot] = std::move(mStagedModels[slot]);
      mStagedModels[slot] = nullptr;
      mNewModelLoadedInDSP = true;
      _UpdateLatency();
      _SetInputGain();
      _SetOutputGain();
    }
  }
  if (mStagedIR != nullptr)
  {
    mIR = std::move(mStagedIR);
    mStagedIR = nullptr;
  }
}

void NeuralRig::_DeallocateIOPointers()
{
  if (mInputPointers != nullptr)
  {
    delete[] mInputPointers;
    mInputPointers = nullptr;
  }
  if (mInputPointers != nullptr)
    throw std::runtime_error("Failed to deallocate pointer to input buffer!\n");
  if (mOutputPointers != nullptr)
  {
    delete[] mOutputPointers;
    mOutputPointers = nullptr;
  }
  if (mOutputPointers != nullptr)
    throw std::runtime_error("Failed to deallocate pointer to output buffer!\n");
  if (mChainPointers != nullptr)
  {
    delete[] mChainPointers;
    mChainPointers = nullptr;
  }
}

void NeuralRig::_FallbackDSP(iplug::sample** inputs, iplug::sample** outputs, const size_t numChannels,
                                    const size_t numFrames)
{
  for (auto c = 0; c < numChannels; c++)
    for (auto s = 0; s < numFrames; s++)
      mOutputArray[c][s] = mInputArray[c][s];
}

void NeuralRig::_ResetModelAndIR(const double sampleRate, const int maxBlockSize)
{
  // Models
  for (size_t slot = 0; slot < kNumSlots; slot++)
  {
    if (mStagedModels[slot] != nullptr)
    {
      mStagedModels[slot]->Reset(sampleRate, maxBlockSize);
    }
    else if (mModels[slot] != nullptr)
    {
      mModels[slot]->Reset(sampleRate, maxBlockSize);
    }
  }

  // IR
  if (mStagedIR != nullptr)
  {
    const double irSampleRate = mStagedIR->GetSampleRate();
    if (irSampleRate != sampleRate)
    {
      const auto irData = mStagedIR->GetData();
      mStagedIR = std::make_unique<dsp::ImpulseResponse>(irData, sampleRate);
    }
  }
  else if (mIR != nullptr)
  {
    const double irSampleRate = mIR->GetSampleRate();
    if (irSampleRate != sampleRate)
    {
      const auto irData = mIR->GetData();
      mStagedIR = std::make_unique<dsp::ImpulseResponse>(irData, sampleRate);
    }
  }
}

void NeuralRig::_SetInputGain()
{
  iplug::sample inputGainDB = GetParam(kInputLevel)->Value();
  // Input calibration keys off the first capture in the chain: that is what
  // actually receives the player's signal, so it is the only one whose trained
  // input level says anything about how hard to drive the rig.
  const ResamplingNAM* first = _FirstModel();
  if ((first != nullptr) && (const_cast<ResamplingNAM*>(first)->HasInputLevel())
      && GetParam(kCalibrateInput)->Bool())
  {
    inputGainDB += GetParam(kInputCalibrationLevel)->Value() - const_cast<ResamplingNAM*>(first)->GetInputLevel();
  }
  mInputGain = DBToAmp(inputGainDB);
}

void NeuralRig::_SetOutputGain()
{
  double gainDB = GetParam(kOutputLevel)->Value();
  // Output levelling keys off the last capture in the chain, since whatever it
  // does to the signal is what reaches the DAW. Normalising against an earlier
  // stage would be undone by everything after it.
  ResamplingNAM* last = const_cast<ResamplingNAM*>(_LastModel());
  if (last != nullptr)
  {
    const int outputMode = GetParam(kOutputMode)->Int();
    switch (outputMode)
    {
      case 1: // Normalized
        if (last->HasLoudness())
        {
          const double loudness = last->GetLoudness();
          const double targetLoudness = -18.0;
          gainDB += (targetLoudness - loudness);
        }
        break;
      case 2: // Calibrated
        if (last->HasOutputLevel())
        {
          const double inputLevel = GetParam(kInputCalibrationLevel)->Value();
          const double outputLevel = last->GetOutputLevel();
          gainDB += (outputLevel - inputLevel);
        }
        break;
      case 0: // Raw
      default: break;
    }
  }
  mOutputGain = DBToAmp(gainDB);
}

void NeuralRig::_ApplySlimParamToLoadedNAMs()
{
  const double v = GetParam(kSlim)->Value();
  auto apply = [v](ResamplingNAM* p) {
    if (p == nullptr)
      return;
    if (nam::SlimmableModel* s = p->GetSlimmableModel())
      s->SetSlimmableSize(v);
  };
  for (size_t slot = 0; slot < kNumSlots; slot++)
  {
    apply(mModels[slot].get());
    apply(mStagedModels[slot].get());
  }
}

std::string NeuralRig::_StageModel(size_t slot, const WDL_String& modelPath)
{
  if (slot >= kNumSlots)
    return "Invalid capture slot";

  WDL_String previousNAMPath = mNAMPaths[slot];
  try
  {
    auto dspPath = std::filesystem::u8path(modelPath.Get());
    std::unique_ptr<nam::DSP> model = nam::get_dsp(dspPath);

    // Check that the model has 1 input and 1 output channel
    if (model->NumInputChannels() != 1)
    {
      throw std::runtime_error("Model must have 1 input channel, but has " + std::to_string(model->NumInputChannels()));
    }
    if (model->NumOutputChannels() != 1)
    {
      throw std::runtime_error("Model must have 1 output channel, but has "
                               + std::to_string(model->NumOutputChannels()));
    }

    std::unique_ptr<ResamplingNAM> temp = std::make_unique<ResamplingNAM>(std::move(model), GetSampleRate());
    temp->Reset(GetSampleRate(), GetBlockSize());
    if (nam::SlimmableModel* slimmable = temp->GetSlimmableModel())
    {
      slimmable->SetSlimmableSize(GetParam(kSlim)->Value());
    }
    mStagedModels[slot] = std::move(temp);
    mNAMPaths[slot] = modelPath;
    SendControlMsgFromDelegate(ModelBrowserCtrlTag(slot), kMsgTagLoadedModel, mNAMPaths[slot].GetLength(),
                               mNAMPaths[slot].Get());
  }
  catch (std::runtime_error& e)
  {
    SendControlMsgFromDelegate(ModelBrowserCtrlTag(slot), kMsgTagLoadFailed);

    if (mStagedModels[slot] != nullptr)
    {
      mStagedModels[slot] = nullptr;
    }
    mNAMPaths[slot] = previousNAMPath;
    std::cerr << "Failed to read DSP module" << std::endl;
    std::cerr << e.what() << std::endl;
    return e.what();
  }
  return "";
}

dsp::wav::LoadReturnCode NeuralRig::_StageIR(const WDL_String& irPath)
{
  // FIXME it'd be better for the path to be "staged" as well. Just in case the
  // path and the model got caught on opposite sides of the fence...
  WDL_String previousIRPath = mIRPath;
  const double sampleRate = GetSampleRate();
  dsp::wav::LoadReturnCode wavState = dsp::wav::LoadReturnCode::ERROR_OTHER;
  try
  {
    auto irPathU8 = std::filesystem::u8path(irPath.Get());
    mStagedIR = std::make_unique<dsp::ImpulseResponse>(irPathU8.string().c_str(), sampleRate);
    wavState = mStagedIR->GetWavState();
  }
  catch (std::runtime_error& e)
  {
    wavState = dsp::wav::LoadReturnCode::ERROR_OTHER;
    std::cerr << "Caught unhandled exception while attempting to load IR:" << std::endl;
    std::cerr << e.what() << std::endl;
  }

  if (wavState == dsp::wav::LoadReturnCode::SUCCESS)
  {
    mIRPath = irPath;
    SendControlMsgFromDelegate(kCtrlTagIRFileBrowser, kMsgTagLoadedIR, mIRPath.GetLength(), mIRPath.Get());
  }
  else
  {
    if (mStagedIR != nullptr)
    {
      mStagedIR = nullptr;
    }
    mIRPath = previousIRPath;
    SendControlMsgFromDelegate(kCtrlTagIRFileBrowser, kMsgTagLoadFailed);
  }

  return wavState;
}

size_t NeuralRig::_GetBufferNumChannels() const
{
  // Assumes input=output (no mono->stereo effects)
  return mInputArray.size();
}

size_t NeuralRig::_GetBufferNumFrames() const
{
  if (_GetBufferNumChannels() == 0)
    return 0;
  return mInputArray[0].size();
}

void NeuralRig::_InitToneStack()
{
  // If you want to customize the tone stack, then put it here!
  mToneStack = std::make_unique<dsp::tone_stack::BasicNamToneStack>();
}
void NeuralRig::_PrepareBuffers(const size_t numChannels, const size_t numFrames)
{
  const bool updateChannels = numChannels != _GetBufferNumChannels();
  const bool updateFrames = updateChannels || (_GetBufferNumFrames() != numFrames);
  //  if (!updateChannels && !updateFrames)  // Could we do this?
  //    return;

  if (updateChannels)
  {
    _PrepareIOPointers(numChannels);
    mInputArray.resize(numChannels);
    mOutputArray.resize(numChannels);
    mChainArray.resize(numChannels);
  }
  if (updateFrames)
  {
    for (auto c = 0; c < mInputArray.size(); c++)
    {
      mInputArray[c].resize(numFrames);
      std::fill(mInputArray[c].begin(), mInputArray[c].end(), 0.0);
    }
    for (auto c = 0; c < mOutputArray.size(); c++)
    {
      mOutputArray[c].resize(numFrames);
      std::fill(mOutputArray[c].begin(), mOutputArray[c].end(), 0.0);
    }
    for (auto c = 0; c < mChainArray.size(); c++)
    {
      mChainArray[c].resize(numFrames);
      std::fill(mChainArray[c].begin(), mChainArray[c].end(), 0.0);
    }

    // Bypass delay lines, sized once here so ProcessBlock never allocates.
    // Resampling latency runs to a few hundred samples at most; this covers
    // any host rate with room to spare.
    for (size_t slot = 0; slot < kNumSlots; slot++)
    {
      if (mSlotBypassDelay[slot].size() != kBypassDelayCapacity)
      {
        mSlotBypassDelay[slot].assign(kBypassDelayCapacity, 0.0);
        mSlotBypassWrite[slot] = 0;
        mSlotBypassLength[slot] = 0;
      }
    }
  }
  // Would these ever get changed by something?
  for (auto c = 0; c < mInputArray.size(); c++)
    mInputPointers[c] = mInputArray[c].data();
  for (auto c = 0; c < mOutputArray.size(); c++)
    mOutputPointers[c] = mOutputArray[c].data();
  for (auto c = 0; c < mChainArray.size(); c++)
    mChainPointers[c] = mChainArray[c].data();
}

void NeuralRig::_RunBypassDelay(size_t slot, iplug::sample** buffers, size_t numChannels, size_t numFrames)
{
  if (slot >= kNumSlots || mSlotBypassDelay[slot].empty())
    return;

  const int capacity = static_cast<int>(mSlotBypassDelay[slot].size());
  int delaySamples = mModels[slot] != nullptr ? mModels[slot]->GetLatency() : 0;
  delaySamples = std::max(0, std::min(delaySamples, capacity - 1));

  if (delaySamples == 0)
    return;

  // A changed delay would otherwise read a region holding audio for the old
  // alignment, which clicks. Clearing costs one quiet block instead.
  if (delaySamples != mSlotBypassLength[slot])
  {
    std::fill(mSlotBypassDelay[slot].begin(), mSlotBypassDelay[slot].end(), 0.0);
    mSlotBypassLength[slot] = delaySamples;
  }

  // The chain is mono, so only channel 0 carries signal.
  auto* data = buffers[0];
  auto& line = mSlotBypassDelay[slot];
  int write = mSlotBypassWrite[slot];

  for (size_t i = 0; i < numFrames; i++)
  {
    line[static_cast<size_t>(write)] = data[i];

    int read = write - delaySamples;
    if (read < 0)
      read += capacity;

    data[i] = line[static_cast<size_t>(read)];

    if (++write >= capacity)
      write = 0;
  }

  mSlotBypassWrite[slot] = write;
  (void)numChannels;
}

void NeuralRig::_PrepareIOPointers(const size_t numChannels)
{
  _DeallocateIOPointers();
  _AllocateIOPointers(numChannels);
}

void NeuralRig::_ProcessInput(iplug::sample** inputs, const size_t nFrames, const size_t nChansIn,
                                     const size_t nChansOut)
{
  // We'll assume that the main processing is mono for now. We'll handle dual amps later.
  if (nChansOut != 1)
  {
    std::stringstream ss;
    ss << "Expected mono output, but " << nChansOut << " output channels are requested!";
    throw std::runtime_error(ss.str());
  }

  // On the standalone, we can probably assume that the user has plugged into only one input and they expect it to be
  // carried straight through. Don't apply any division over nChansIn because we're just "catching anything out there."
  // However, in a DAW, it's probably something providing stereo, and we want to take the average in order to avoid
  // doubling the loudness. (This would change w/ double mono processing)
  double gain = mInputGain;
#ifndef APP_API
  gain /= (float)nChansIn;
#endif
  // Assume _PrepareBuffers() was already called
  for (size_t c = 0; c < nChansIn; c++)
    for (size_t s = 0; s < nFrames; s++)
      if (c == 0)
        mInputArray[0][s] = gain * inputs[c][s];
      else
        mInputArray[0][s] += gain * inputs[c][s];
}

void NeuralRig::_ProcessOutput(iplug::sample** inputs, iplug::sample** outputs, const size_t nFrames,
                                      const size_t nChansIn, const size_t nChansOut)
{
  const double gain = mOutputGain;
  // Assume _PrepareBuffers() was already called
  if (nChansIn != 1)
    throw std::runtime_error("Plugin is supposed to process in mono.");
  // Broadcast the internal mono stream to all output channels.
  const size_t cin = 0;
  for (auto cout = 0; cout < nChansOut; cout++)
    for (auto s = 0; s < nFrames; s++)
#ifdef APP_API // Ensure valid output to interface
      outputs[cout][s] = std::clamp(gain * inputs[cin][s], -1.0, 1.0);
#else // In a DAW, other things may come next and should be able to handle large
      // values.
      outputs[cout][s] = gain * inputs[cin][s];
#endif
}

void NeuralRig::_UpdateControlsFromModel()
{
  // The settings page describes one capture. Show the first in the chain,
  // since that is the one whose input calibration the controls act on.
  ResamplingNAM* mModel = const_cast<ResamplingNAM*>(_FirstModel());

  if (mModel == nullptr)
  {
    return;
  }
  if (auto* pGraphics = GetUI())
  {
    ModelInfo modelInfo;
    modelInfo.sampleRate.known = true;
    modelInfo.sampleRate.value = mModel->GetEncapsulatedSampleRate();
    modelInfo.inputCalibrationLevel.known = mModel->HasInputLevel();
    modelInfo.inputCalibrationLevel.value = mModel->HasInputLevel() ? mModel->GetInputLevel() : 0.0;
    modelInfo.outputCalibrationLevel.known = mModel->HasOutputLevel();
    modelInfo.outputCalibrationLevel.value = mModel->HasOutputLevel() ? mModel->GetOutputLevel() : 0.0;

    static_cast<NAMSettingsPageControl*>(pGraphics->GetControlWithTag(kCtrlTagSettingsBox))->SetModelInfo(modelInfo);

    const bool disableInputCalibrationControls = !mModel->HasInputLevel();
    pGraphics->GetControlWithTag(kCtrlTagCalibrateInput)->SetDisabled(disableInputCalibrationControls);
    pGraphics->GetControlWithTag(kCtrlTagInputCalibrationLevel)->SetDisabled(disableInputCalibrationControls);
    {
      auto* c = static_cast<OutputModeControl*>(pGraphics->GetControlWithTag(kCtrlTagOutputMode));
      c->SetNormalizedDisable(!mModel->HasLoudness());
      c->SetCalibratedDisable(!mModel->HasOutputLevel());
    }

    if (auto* pSlimIcon = pGraphics->GetControlWithTag(kCtrlTagSlimmableIcon))
    {
      const bool show = mModel->GetSlimmableModel() != nullptr;
      pSlimIcon->Hide(!show);
    }
  }
}

void NeuralRig::_UpdateLatency()
{
  // Every loaded capture contributes its own resampling latency, so the
  // chain's total is the sum. Deliberately independent of whether a slot is
  // enabled: a disabled slot runs its audio through a matching delay instead,
  // so toggling one does not make the host re-align mid-session.
  int latency = 0;
  for (size_t slot = 0; slot < kNumSlots; slot++)
  {
    if (mModels[slot])
    {
      latency += mModels[slot]->GetLatency();
    }
  }
  // Other things that add latency here...

  // Feels weird to have to do this.
  if (GetLatency() != latency)
  {
    SetLatency(latency);
  }
}

void NeuralRig::_UpdateMeters(sample** inputPointer, sample** outputPointer, const size_t nFrames,
                                     const size_t nChansIn, const size_t nChansOut)
{
  // Right now, we didn't specify MAXNC when we initialized these, so it's 1.
  const int nChansHack = 1;
  mInputSender.ProcessBlock(inputPointer, (int)nFrames, kCtrlTagInputMeter, nChansHack);
  mOutputSender.ProcessBlock(outputPointer, (int)nFrames, kCtrlTagOutputMeter, nChansHack);
}

// HACK
#include "Unserialization.cpp"
