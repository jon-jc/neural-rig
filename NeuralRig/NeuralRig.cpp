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
#include "RigSlotControl.h"
#include "T3KBrowserPanel.h"
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

    // --- Rig: three typed slots across, one knob row beneath -----------------
    //
    // Previously three stacked groups -- amp knobs, then a vertical list of
    // four file rows, then a cabinet row -- which read as a form to fill in.
    // A rig reads left to right: what the signal hits first is leftmost, and
    // the controls that shape all of it sit underneath in a single row.
    const auto slotCardHeight = 146.f;
    const auto rigGroupHeight = slotCardHeight + knobBlockHeight + groupChrome + 18.f;
    const auto rigGroup = remaining.GetFromTop(rigGroupHeight);
    remaining = remaining.GetReducedFromTop(rigGroupHeight + 10.f);

    const auto rigInner = rigGroup.GetPadded(-12.f).GetReducedFromTop(6.f);

    // Meters bracket the whole rig rather than just the knobs, so input and
    // output sit at the two ends of the path they actually measure.
    const auto inputMeterArea = rigInner.GetFromLeft(20.f).GetMidVPadded(rigInner.H() * 0.42f);
    const auto outputMeterArea = rigInner.GetFromRight(20.f).GetMidVPadded(rigInner.H() * 0.42f);

    const auto stage = rigInner.GetReducedFromLeft(34.f).GetReducedFromRight(34.f);

    const auto slotRowArea = stage.GetFromTop(slotCardHeight);
    auto slotArea = [&](size_t slot) {
      return slotRowArea.GetGridCell(0, static_cast<int>(slot), 1, static_cast<int>(kNumSlots)).GetPadded(-6.f);
    };

    const auto knobsArea = stage.GetReducedFromTop(slotCardHeight + 14.f).GetFromTop(NAM_KNOB_HEIGHT);
    const auto noiseGateArea = knobsArea.GetGridCell(0, 0, 1, numKnobs).GetMidHPadded(46.f);
    const auto inputKnobArea = knobsArea.GetGridCell(0, 1, 1, numKnobs).GetMidHPadded(46.f);
    const auto bassKnobArea = knobsArea.GetGridCell(0, 2, 1, numKnobs).GetMidHPadded(46.f);
    const auto midKnobArea = knobsArea.GetGridCell(0, 3, 1, numKnobs).GetMidHPadded(46.f);
    const auto trebleKnobArea = knobsArea.GetGridCell(0, 4, 1, numKnobs).GetMidHPadded(46.f);
    const auto outputKnobArea = knobsArea.GetGridCell(0, 5, 1, numKnobs).GetMidHPadded(46.f);

    // The two toggles keep their function but move out of the knob row, which
    // in the target layout carries knobs only. They sit as small switches
    // beneath the knob each one gates.
    const auto toggleRow = stage.GetReducedFromTop(slotCardHeight + 14.f + NAM_KNOB_HEIGHT).GetFromTop(switchHeight);
    const auto ngToggleArea = toggleRow.GetGridCell(0, 0, 1, numKnobs).GetMidHPadded(38.f);
    const auto eqToggleArea = toggleRow.GetGridCell(0, 3, 1, numKnobs).GetMidHPadded(38.f);

    // The IR toggle belongs in the toggle row with the others, not on the cab
    // card. Putting it on the card meant it drew over the card's own clear
    // button and covered the capture name, so the cab slot could not be read
    // or cleared once something was loaded.
    const auto irSwitchArea = toggleRow.GetGridCell(0, 5, 1, numKnobs).GetMidHPadded(38.f);
    const auto irArea = irSwitchArea;

    // The BROWSER handle, centred under the rig where the panel will appear.
    const auto browserToggleArea = remaining.GetFromTop(30.f).GetMidHPadded(60.f);
    remaining = remaining.GetReducedFromTop(36.f);

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
    pGraphics->AttachControl(new nr::theme::SectionPanelControl(rigGroup, ""));

    // One card per stage. Clicking anywhere on a card opens the browser
    // already filtered to what can go in it, so choosing a pedal never means
    // scrolling past amps first.
    static constexpr nr::rig::SlotKind kSlotKinds[kNumSlots] = {
      nr::rig::SlotKind::Pedal, nr::rig::SlotKind::Amp, nr::rig::SlotKind::Cab};

    for (size_t slot = 0; slot < kNumSlots; slot++)
    {
      const auto kind = kSlotKinds[slot];

      auto browseForSlot = [this, pGraphics, kind](int slotIndex) {
        mBrowserTargetSlot = slotIndex;

        auto* panel = pGraphics->GetControlWithTag(kCtrlTagT3KBrowser);

        if (panel == nullptr)
          return;

        panel->Hide(false);

        const auto gears = nr::rig::SlotGears(kind);
        std::string joined;

        for (const auto& gear : gears)
          joined += joined.empty() ? gear : "_" + gear;

        static_cast<nr::browser::T3KBrowserPanel*>(panel)->FocusGears(joined, nr::rig::SlotLabel(kind));
        pGraphics->SetAllControlsDirty();
      };

      auto clearSlot = [this](int slotIndex) {
        SendArbitraryMsgFromUI(kMsgTagClearModel, ModelBrowserCtrlTag(static_cast<size_t>(slotIndex)), 0, nullptr);
      };

      pGraphics->AttachControl(new nr::rig::RigSlotControl(slotArea(slot), static_cast<int>(slot), kind,
                                                           SlotActiveParam(slot), browseForSlot, clearSlot),
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
      // Centred at roughly the size upstream laid it out for. Its contents are
      // positioned relative to a title area at the top of its own bounds, so
      // handing it the whole 1120x880 window scatters them across the panel.
      ->AttachControl(new NAMSettingsPageControl(b.GetCentredInside(640.f, 470.f), backgroundBitmap,
                                                 inputLevelBackgroundBitmap, switchHandleBitmap,
                                                 crossSVG, style, radioButtonStyle),
                      kCtrlTagSettingsBox)
      ->Hide(true);

    // TONE3000 browser. Docked across the bottom and hidden until asked for.
    //
    // The old embedded web view had to float, be movable and be resizable,
    // because a native web view draws over the IGraphics surface rather than
    // into it: it could not be docked without either stealing space permanently
    // or being too small to browse in. This one is drawn by IGraphics like
    // everything else, so it can simply take the lower half when open and
    // give it back when closed.
    {
      // Anchored under the BROWSER handle rather than to a fraction of the
      // window. A fixed 58% overlapped the bottom of the rig, hiding the cab
      // slot and the knob row behind it; deriving the top from the layout means
      // the two can never collide however the sections above are tuned.
      const auto browserPanelArea = IRECT(b.L, browserToggleArea.B + 8.f, b.R, b.B);

      auto* browserPanel = new nr::browser::T3KBrowserPanel(
        browserPanelArea, mBrowser,
        [this](int rowIndex, const nr::net::BrowserController::Row& row) {
          const int slot = mBrowserTargetSlot;

          // Local rows are already on disk. Downloading them again would ask
          // the API for a tone id the Local tab does not even have.
          if (!row.localPath.empty())
          {
            std::lock_guard<std::mutex> lock(mPendingLoadMutex);
            mPendingLoads.emplace_back(slot, row.localPath);
            return;
          }

          mBrowser.DownloadRow(rowIndex, [this, slot](bool success, std::string pathOrError) {
            if (!success)
              return;

            // Called from a worker thread. Park the path and let OnIdle stage
            // it on the message thread rather than touching model state here.
            std::lock_guard<std::mutex> lock(mPendingLoadMutex);
            mPendingLoads.emplace_back(slot, pathOrError);
          });
        });

      pGraphics->AttachControl(browserPanel, kCtrlTagT3KBrowser)->Hide(true);

      // The handle that opens it without going through a slot.
      pGraphics->AttachControl(
        new nr::rig::BrowserToggleControl(browserToggleArea, [pGraphics](bool open) {
          if (auto* panel = pGraphics->GetControlWithTag(kCtrlTagT3KBrowser))
            panel->Hide(!open);

          pGraphics->SetAllControlsDirty();
        }),
        kCtrlTagBrowserToggle);
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

      // Close the browser once a capture lands: the user asked for one, they
      // got it, and leaving the panel covering the rig hides the thing they
      // just changed.
      if (auto* pGraphics = GetUI())
      {
        if (auto* panel = pGraphics->GetControlWithTag(kCtrlTagT3KBrowser))
        {
          panel->Hide(true);
          pGraphics->SetAllControlsDirty();
        }
      }
    }
  }

  // Light the chain connector for whichever slots hold a capture, so the
  // signal path shows where audio actually flows.
  if (auto* pGraphics = GetUI())
  {
    // Push each slot's capture name into its card. The cards draw the name
    // themselves rather than owning the path, so this is the one place the
    // rig's actual contents reach the UI.
    for (size_t slot = 0; slot < kNumSlots; slot++)
    {
      const bool occupied = mNAMPaths[slot].GetLength() > 0;

      if (occupied == mFlowOccupancy[slot])
        continue;

      mFlowOccupancy[slot] = occupied;

      if (auto* card = pGraphics->GetControlWithTag(ModelBrowserCtrlTag(slot)))
      {
        std::string name;

        if (occupied)
        {
          // Show the file's stem: the full path is meaningless in a card this
          // size, and the stem is what the capture is actually called.
          name = std::filesystem::path(mNAMPaths[slot].Get()).stem().string();
        }

        static_cast<nr::rig::RigSlotControl*>(card)->SetCaptureName(name.c_str());
      }
    }
  }

  // The panel owns the dirty check: it calls ConsumeDirty itself and repaints
  // only when the controller reports a change. Testing the flag here as well
  // would swallow the notification, and whichever of the two ran first would
  // leave the other permanently stale.
  if (auto* pGraphics = GetUI())
  {
    if (auto* panel = pGraphics->GetControlWithTag(kCtrlTagT3KBrowser))
      static_cast<nr::browser::T3KBrowserPanel*>(panel)->Poll();
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
