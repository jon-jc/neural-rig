#include <algorithm> // std::clamp, std::min
#include <cmath> // pow
#include <filesystem>
#include <fstream>
#include <cstring>
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
#include "Presets.h"
#include "RigSlotControl.h"
#include "StatusBar.h"
#include "T3KBrowserPanel.h"
#include "Theme.h"
#include "WavCompat.h"
#include "WindowChrome.h"

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

  // Per-stage trim. Unity by default, so loading a capture sounds like the
  // capture until the user asks for something else.
  for (size_t slot = 0; slot < kNumSlots; slot++)
  {
    const std::string name = "Slot" + std::to_string(slot + 1) + "Out";
    GetParam(SlotOutParam(slot))->InitGain(name.c_str(), 0.0, -20.0, 20.0, 0.1);
  }

  GetParam(kIROut)->InitGain("IROut", 0.0, -20.0, 20.0, 0.1);

  for (size_t slot = 0; slot < kNumSlots; slot++)
  {
    const std::string name = "Slot" + std::to_string(slot + 1) + "Drive";
    GetParam(SlotDriveParam(slot))->InitGain(name.c_str(), 0.0, -12.0, 24.0, 0.1);
  }

  // Defaults sit outside the audible band so the cabinet is untouched until
  // the user reaches for it.
  GetParam(kIRLowCut)->InitFrequency("IRLowCut", 20.0, 20.0, 800.0);
  GetParam(kIRHighCut)->InitFrequency("IRHighCut", 20000.0, 2000.0, 20000.0);

  for (size_t slot = 0; slot < kNumSlots; slot++)
  {
    // Wide open by default, so a capture sounds like itself until asked.
    const std::string tone = "Slot" + std::to_string(slot + 1) + "Tone";
    GetParam(SlotToneParam(slot))->InitFrequency(tone.c_str(), 20000.0, 800.0, 20000.0);

    const std::string mix = "Slot" + std::to_string(slot + 1) + "Mix";
    GetParam(SlotMixParam(slot))->InitPercentage(mix.c_str(), 100.0);
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

    // Draw the menus and the text entry ourselves rather than letting the OS
    // do it. Without these, CreatePopupMenu and CreateTextEntry fall back to
    // native widgets -- a white Windows menu and a white edit box dropped into
    // a near-black panel.
    // The text entry's own fill lives on the IText, not on the control, so a
    // plain IText leaves it at the platform default -- a white box.
    const IText menuText =
      IText(14.f, PluginColors::INK, "Roboto-Regular").WithTEColors(PluginColors::WELL, PluginColors::INK);

    pGraphics->AttachPopupMenuControl(menuText);
    pGraphics->AttachTextEntryControl();

    // IPopupMenuControl defaults to white panel, blue cells, black text. Left
    // alone it is a stock widget dropped into a near-black plugin.
    if (auto* menu = pGraphics->GetPopupMenuControl())
    {
      menu->SetPanelColor(PluginColors::PANEL);
      menu->SetCellBackgroundColor(PluginColors::AMBER.WithOpacity(0.22f));
      menu->SetItemColor(PluginColors::INK);
      menu->SetItemMouseoverColor(PluginColors::AMBER);
      menu->SetDisabledItemColor(PluginColors::INK_DIM);
      menu->SetSeparatorColor(PluginColors::PANEL_HI);
    }
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
    // Everything is laid out once, for the *open* window height, and never
    // re-laid out. Collapsing the browser shrinks the window instead, which is
    // why the order here matters: the browser panel is last, below the status
    // strip, so cutting the window off above it removes the browser and
    // nothing else.
    //
    // Laying out again on resize was the earlier approach and it rebuilt every
    // control -- which crashed when triggered from a control's own handler, and
    // painted a blank window when it survived.
    auto remaining = b.GetPadded(-14.f);

    const auto headerArea = remaining.GetFromTop(46.f);
    remaining = remaining.GetReducedFromTop(46.f + 8.f);

    // The header is divided into lanes that cannot overlap, rather than each
    // element being centred or anchored independently and hoping they miss.
    // Left to right: File and Options, the wordmark, the preset bar, then the
    // window buttons. The wordmark used to run to x=464 while the preset bar
    // began at x=370, so the two collided and the model icon sat on top of the
    // word PRESETS.
    const auto titleArea = headerArea.GetReducedFromLeft(140.f).GetFromLeft(286.f);
    // Caption-bar buttons, right to left in the usual order. No maximise: the
    // layout holds one size, so offering one would be a button that lies.
    const auto closeButtonArea = headerArea.GetFromRight(30.f).GetMidVPadded(15.f);

    // File and Options, where the OS menu bar used to be but inside the
    // plugin's own surface so they follow the theme.
    const auto fileMenuArea = headerArea.GetFromLeft(58.f).GetMidVPadded(13.f);
    const auto optionsMenuArea = fileMenuArea.GetHShifted(62.f);
    const auto settingsButtonArea = headerArea.GetFromRight(34.f).GetHShifted(-40.f).GetMidVPadded(17.f);
    // Between the wordmark's lane and the window buttons', not centred on the
    // window -- centring is what walked it into the title.
    const auto presetBarArea = headerArea.GetReducedFromLeft(456.f).GetReducedFromRight(146.f).GetMidVPadded(15.f);

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
    const auto slotCardHeight = 226.f;
    const auto rigGroupHeight = slotCardHeight + 14.f + knobBlockHeight + toggleBlockHeight + groupChrome + 12.f;
    const auto rigGroup = remaining.GetFromTop(rigGroupHeight);
    remaining = remaining.GetReducedFromTop(rigGroupHeight + 10.f);

    const auto rigInner = rigGroup.GetPadded(-12.f).GetReducedFromTop(6.f);

    // Meters bracket the whole rig rather than just the knobs, so input and
    // output sit at the two ends of the path they actually measure.
    const auto inputMeterArea = rigInner.GetFromLeft(20.f).GetMidVPadded(rigInner.H() * 0.42f);
    const auto outputMeterArea = rigInner.GetFromRight(20.f).GetMidVPadded(rigInner.H() * 0.42f);

    const auto stage = rigInner.GetReducedFromLeft(34.f).GetReducedFromRight(34.f);

    // Four cards: the three capture slots plus the cabinet IR, which is not a
    // capture slot but is one more thing in the signal path and was invisible
    // anywhere else.
    constexpr int kNumCards = static_cast<int>(kNumSlots) + 1;

    const auto slotRowArea = stage.GetFromTop(slotCardHeight);
    auto slotArea = [&](int card) {
      return slotRowArea.GetGridCell(0, card, 1, kNumCards).GetPadded(-5.f);
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
    // Below the knob *and* its value text. Placing it at NAM_KNOB_HEIGHT put
    // the switches on top of the readouts, which is why the captions were
    // sliced in half by the browser panel's top edge.
    // NAMSwitchControl draws its caption inside its own bounds, so each toggle
    // needs the whole block rather than just the switch: given only the switch
    // height the caption lands on top of the switch and both become awkward to
    // hit. Wider cells too -- these were 38px of usable target.
    const auto toggleRow =
      stage.GetReducedFromTop(slotCardHeight + 14.f + knobBlockHeight).GetFromTop(toggleBlockHeight);
    // GetMidHPadded(p) yields a rect 2*p wide -- it is a half-width, not an
    // inset. Passing 16 gave these a 32px box, which is narrower than the word
    // "Noise Gate" and clipped the caption at both ends.
    const auto ngToggleArea = toggleRow.GetGridCell(0, 0, 1, numKnobs).GetMidHPadded(62.f);
    const auto eqToggleArea = toggleRow.GetGridCell(0, 3, 1, numKnobs).GetMidHPadded(62.f);

    // The IR toggle belongs in the toggle row with the others, not on the cab
    // card. Putting it on the card meant it drew over the card's own clear
    // button and covered the capture name, so the cab slot could not be read
    // or cleared once something was loaded.
    const auto irSwitchArea = toggleRow.GetGridCell(0, 5, 1, numKnobs).GetMidVPadded(13.f).GetMidHPadded(13.f);
    const auto irArea = irSwitchArea;

    // The BROWSER handle, centred under the rig where the panel will appear.
    const auto browserToggleArea = remaining.GetFromTop(30.f).GetMidHPadded(60.f);
    remaining = remaining.GetReducedFromTop(36.f);

    // The status strip sits above the browser rather than below it, so the
    // catalogue is the last thing in the window and never separates the rig
    // from its readouts -- and so collapsing the window past the browser
    // leaves everything else intact.
    const auto statusBarArea = remaining.GetFromTop(38.f);

    mCollapsedHeight = static_cast<int>(statusBarArea.B + 14.f);

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

    // Decorative only. It covers the whole window, so leaving it interactive
    // put it on top of anything attached before it and swallowed those clicks
    // -- which is exactly what stopped the header drag from working.
    pGraphics->AttachControl(new IBitmapControl(b, linesBitmap))->SetIgnoreMouse(true);

    // Stands in for the caption bar the borderless window no longer has.
    // Attached after the backdrop so it is above it, but before the header's
    // own controls so those take their clicks first and only bare header space
    // starts a drag.
    pGraphics->AttachControl(new nr::shell::WindowDragControl(headerArea));
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
    // Raised, titled panels rather than wire-frame groups: a filled surface a
    // step lighter than the chassis, a shadow under it and a lit top edge do
    // far more to separate sections than a drawn border does.
    pGraphics->AttachControl(new nr::theme::SectionPanelControl(rigGroup, ""));

    // One card per stage. Clicking anywhere on a card opens the browser
    // already filtered to what can go in it, so choosing a pedal never means
    // scrolling past amps first.
    static constexpr nr::rig::SlotKind kSlotKinds[kNumCards] = {nr::rig::SlotKind::Pedal, nr::rig::SlotKind::Amp,
                                                                nr::rig::SlotKind::IR};

    for (int card = 0; card < kNumCards; card++)
    {
      const auto kind = kSlotKinds[card];
      const bool isIR = kind == nr::rig::SlotKind::IR;

      auto browseForSlot = [this, pGraphics, kind, isIR](int slotIndex) {
        mBrowserTargetSlot = slotIndex;

        auto* panel = pGraphics->GetControlWithTag(kCtrlTagT3KBrowser);

        if (panel == nullptr)
          return;

        panel->Hide(false);

        if (!mBrowserOpen)
        {
          mBrowserOpen = true;
          mPendingResizeHeight = PLUG_HEIGHT;
        }

        std::string joined;

        for (const auto& gear : nr::rig::SlotGears(kind))
          joined += joined.empty() ? gear : "_" + gear;

        // A capture slot can only load a .nam, so it says so. The IR card is
        // the mirror image: an impulse response is a format rather than a gear,
        // so format=ir with the gear left open is the only way to ask for one.
        const char* format = isIR ? "ir" : "nam";

        static_cast<nr::browser::T3KBrowserPanel*>(panel)->FocusGears(joined, nr::rig::SlotLabel(kind), format);
        pGraphics->SetAllControlsDirty();
      };

      auto clearSlot = [this, isIR](int slotIndex) {
        SendArbitraryMsgFromUI(isIR ? kMsgTagClearIR : kMsgTagClearModel,
                               isIR ? kCtrlTagIRFileBrowser : ModelBrowserCtrlTag(static_cast<size_t>(slotIndex)), 0,
                               nullptr);
      };

      auto dropOnSlot = [this](int slotIndex, const char* path) {
        std::string extension = std::filesystem::path(path).extension().string();
        std::transform(extension.begin(), extension.end(), extension.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

        // The file decides where it goes, not the card it landed on: a .wav is
        // an impulse response wherever you drop it.
        if (extension == ".wav")
        {
          WDL_String irPath;
          irPath.Set(path);
          _LoadIRWithFeedback(irPath);
        }
        else if (extension == ".nam" && slotIndex < static_cast<int>(kNumSlots))
        {
          std::lock_guard<std::mutex> lock(mPendingLoadMutex);
          mPendingLoads.push_back({slotIndex, std::string(path), {}});
        }
      };

      const int activeParam = isIR ? kIRToggle : SlotActiveParam(static_cast<size_t>(card));
      const int tag = isIR ? kCtrlTagIRFileBrowser : ModelBrowserCtrlTag(static_cast<size_t>(card));

      auto* slotCard = new nr::rig::RigSlotControl(slotArea(card), card, kind, activeParam, browseForSlot, clearSlot,
                                                  dropOnSlot);

      pGraphics->AttachControl(slotCard, tag);

      // Each stage's own controls, always on the card. Hiding them behind a
      // click meant the one thing you reach for after loading a capture was
      // invisible, and the hidden row overlapped the card's text besides.
      //
      // A pedal and an amp both want drive in and level out; a cabinet wants
      // its two cuts and a level. Three per card either way.
      struct SlotKnob
      {
        int param;
        const char* label;
      };

      // Braced, and only two entries where a stage has two: the initialiser
      // was written without braces and with a filler third element, which is
      // not valid C++ and is how a stray NUL ended up in this file.
      const size_t slotIx = static_cast<size_t>(card);

      const SlotKnob knobs[] = {
        isIR ? SlotKnob{kIRLowCut, "Low Cut"} : SlotKnob{SlotDriveParam(slotIx), "Drive"},
        isIR ? SlotKnob{kIRHighCut, "High Cut"} : SlotKnob{SlotToneParam(slotIx), "Tone"},
        isIR ? SlotKnob{kIROut, "Level"} : SlotKnob{SlotMixParam(slotIx), "Mix"},
        isIR ? SlotKnob{kIROut, ""} : SlotKnob{SlotOutParam(slotIx), "Level"},
      };

      // Drive, Tone, Mix and Level on a capture stage; three cuts on a cabinet.
      const int numKnobsHere = isIR ? 3 : 4;
      const auto knobRow = slotArea(card).GetPadded(-8.f).GetFromBottom(122.f);

      for (int k = 0; k < numKnobsHere; k++)
      {
        const auto cell = knobRow.GetGridCell(0, k, 1, numKnobsHere).GetMidHPadded(isIR ? 40.f : 34.f);
        pGraphics->AttachControl(new NAMKnobControl(cell, knobs[k].param, knobs[k].label, style, knobBackgroundBitmap));
      }
    }


    // The IR's on/off switch. There is deliberately no file-picker beside it:
    // NAMFileBrowserControl draws chrome at a fixed size whatever rect it is
    // given, so a toggle-sized rect spilled a folder icon, an arrow and a globe
    // across the CAB card, covering the capture name and its clear button. IRs
    // now arrive the same way captures do -- the browser's IR filter, or
    // dropping a .wav on a card.
    pGraphics->AttachControl(new ISVGSwitchControl(irSwitchArea, {irIconOffSVG, irIconOnSVG}, kIRToggle));
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
      const auto browserPanelArea = IRECT(b.L, statusBarArea.B + 10.f, b.R, b.B);

      auto* browserPanel = new nr::browser::T3KBrowserPanel(
        browserPanelArea, mBrowser,
        [this](int rowIndex, const nr::net::BrowserController::Row& row) {
          const int slot = mBrowserTargetSlot;

          // Local rows are already on disk. Downloading them again would ask
          // the API for a tone id the Local tab does not even have.
          if (!row.localPath.empty())
          {
            std::lock_guard<std::mutex> lock(mPendingLoadMutex);
            mPendingLoads.push_back({slot, row.localPath, row.title});
            return;
          }

          const std::string title = row.title;

          mBrowser.DownloadRow(rowIndex, [this, slot, title](bool success, std::string pathOrError) {
            if (!success)
              return;

            // Called from a worker thread. Park the path and let OnIdle stage
            // it on the message thread rather than touching model state here.
            std::lock_guard<std::mutex> lock(mPendingLoadMutex);
            mPendingLoads.push_back({slot, pathOrError, title});
          });
        });

      // Picking a specific variant takes the same route into the rig as the
      // automatic pick, so both land in the slot the same way.
      browserPanel->SetChoiceHandler(
        [this](const nr::net::BrowserController::ModelChoice& choice, const nr::net::BrowserController::Row& row) {
          const int slot = mBrowserTargetSlot;

          // The variant's own name qualifies the tone: "Marshall JCM-800"
          // alone does not say which of its forty models is loaded.
          const std::string label =
            choice.name.empty() ? row.title : row.title + " - " + choice.name;

          mBrowser.DownloadChoice(choice, row.title, [this, slot, label](bool success, std::string pathOrError) {
            if (!success)
              return;

            std::lock_guard<std::mutex> lock(mPendingLoadMutex);
            mPendingLoads.push_back({slot, pathOrError, label});
          });
        });

      pGraphics->AttachControl(browserPanel, kCtrlTagT3KBrowser)->Hide(!mBrowserOpen);

      // The handle that opens it without going through a slot.
      pGraphics->AttachControl(
        new nr::rig::BrowserToggleControl(
          browserToggleArea, mBrowserOpen,
          [this, pGraphics](bool open) {
            mBrowserOpen = open;
            mPendingResizeHeight = open ? PLUG_HEIGHT : mCollapsedHeight;
          }),
        kCtrlTagBrowserToggle);

      pGraphics->AttachControl(new nr::shell::StatusBarControl(statusBarArea), kCtrlTagStatusBar);

      // Replace the caption-bar buttons that went with the OS frame. Inert in
      // plugin builds, where the host owns the window.
      pGraphics->AttachControl(new nr::shell::WindowButtonControl(closeButtonArea, nr::shell::WindowButton::Close));

      pGraphics->AttachControl(new nr::shell::WindowMenuControl(fileMenuArea, "File", true));
      pGraphics->AttachControl(new nr::shell::WindowMenuControl(optionsMenuArea, "Options", false));

      // The browser starts closed, so the window should start collapsed.
      // Requested rather than done here: the graphics context is still being
      // built inside the layout function.
      if (!mBrowserOpen)
        mPendingResizeHeight = mCollapsedHeight;


      // Presets save the whole rig, so they go through the plugin's own
      // state serialization rather than a second format that would drift
      // from the parameter list.
      pGraphics->AttachControl(
        new nr::presets::PresetBarControl(
          presetBarArea,
          [this](const std::string& name) {
            IByteChunk chunk;
            if (SerializeState(chunk))
              nr::presets::Save(name, chunk);
          },
          [this](const std::string& name) {
            // Requested, not applied. Restoring state restages every model and
            // rewrites the paths ProcessBlock reads; doing that from inside the
            // menu callback that asked for it tears down state while this
            // handler is still using it.
            std::lock_guard<std::mutex> lock(mPendingPresetMutex);
            mPendingPresetName = name;
          }),
        kCtrlTagPresetBar);
    }

    // No slim-model overlay. It anchored its icon to the right of the first
    // card, which put it on top of the next one, and it revealed a full-size
    // "Slim" knob in the middle of the window whenever a slimmable capture
    // loaded. The slim size is still applied when a model provides it; it just
    // has no floating controls of its own.

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
        // Drive goes in front of the capture, because that is where it belongs:
        // a capture reacts to level the way the amp it was taken from did, so
        // pushing it is what changes the character. Applied to the source in
        // place -- by this point the source is either our own chain buffer or
        // the gate's output, and nothing downstream reads it again.
        const double mix = GetParam(SlotMixParam(slot))->Value() * 0.01;
        const bool blending = mix < 0.999 && !mSlotDry[slot].empty();

        // Take the dry *before* drive. A blend control should fade toward the
        // untouched signal, not toward a louder copy of it.
        if (blending)
          for (size_t c = 0; c < numChannelsInternal; c++)
            memcpy(mSlotDryPointers[slot][c], chainSource[c], numFrames * sizeof(sample));

        const double drive = DBToAmp(GetParam(SlotDriveParam(slot))->Value());

        if (drive != 1.0)
          for (size_t c = 0; c < numChannelsInternal; c++)
            for (int f = 0; f < numFrames; f++)
              chainSource[c][f] *= drive;

        mModels[slot]->process(chainSource, chainDest, nFrames);

        // Tone sits after the capture, which is where a pedal's tone control
        // actually is -- after the clipping stage, shaping what came out of it.
        const double toneHz = GetParam(SlotToneParam(slot))->Value();

        if (toneHz < 19999.0)
        {
          recursive_linear_filter::LowPassParams toneParams(GetSampleRate(), toneHz);
          mSlotTone[slot].SetParams(toneParams);
          sample** toned = mSlotTone[slot].Process(chainDest, numChannelsInternal, numFrames);

          if (toned != chainDest)
            for (size_t c = 0; c < numChannelsInternal; c++)
              memcpy(chainDest[c], toned[c], numFrames * sizeof(sample));
        }

        if (blending)
        {
          // Delay the dry by exactly this stage's latency before blending. The
          // bypass line is free here -- it only runs when the stage is off, and
          // this branch is the stage being on -- so the two share it.
          _RunBypassDelay(slot, mSlotDryPointers[slot], numChannelsInternal, numFrames);

          for (size_t c = 0; c < numChannelsInternal; c++)
            for (int f = 0; f < numFrames; f++)
              chainDest[c][f] = mix * chainDest[c][f] + (1.0 - mix) * mSlotDryPointers[slot][c][f];
        }

        // Trim this stage before it feeds the next. A gain is safe to apply
        // here because it does not change the stage's latency, so the chain's
        // reported latency is unaffected.
        const double trim = DBToAmp(GetParam(SlotOutParam(slot))->Value());

        if (trim != 1.0)
          for (size_t c = 0; c < numChannelsInternal; c++)
            for (int f = 0; f < numFrames; f++)
              chainDest[c][f] *= trim;
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
  {
    irPointers = mIR->Process(toneStackOutPointers, numChannelsInternal, numFrames);

    // Cabinet trim. Impulse responses arrive at wildly different levels, so
    // matching one to the rest of the rig otherwise means riding the master.
    const double trim = DBToAmp(GetParam(kIROut)->Value());

    if (trim != 1.0)
      for (size_t c = 0; c < numChannelsInternal; c++)
        for (int f = 0; f < numFrames; f++)
          irPointers[c][f] *= trim;

    // Low and high cut, the two controls every cab sim has, because a raw IR
    // is almost always too boomy at the bottom and too fizzy at the top.
    recursive_linear_filter::HighPassParams lowCut(GetSampleRate(), GetParam(kIRLowCut)->Value());
    recursive_linear_filter::LowPassParams highCut(GetSampleRate(), GetParam(kIRHighCut)->Value());
    mIRLowCut.SetParams(lowCut);
    mIRHighCut.SetParams(highCut);

    irPointers = mIRLowCut.Process(irPointers, numChannelsInternal, numFrames);
    irPointers = mIRHighCut.Process(irPointers, numChannelsInternal, numFrames);
  }

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

void NeuralRig::_LoadIRWithFeedback(const WDL_String& irPath)
{
  if (!irPath.GetLength())
    return;

  mIRPath = irPath;

  const dsp::wav::LoadReturnCode retCode = _StageIR(irPath);

  if (retCode == dsp::wav::LoadReturnCode::SUCCESS)
    return;

  std::stringstream message;
  message << "Failed to load IR file " << irPath.Get() << ":\n";
  message << dsp::wav::GetMsgForLoadReturnCode(retCode);

  _ShowMessageBox(GetUI(), message.str().c_str(), "Failed to load IR!", kMB_OK);
}

namespace
{
/// Turns a cache filename back into something readable.
///
/// Downloads are stored as a slug with the model id appended --
/// "marshall-jcm-800-ampete-one-mars-gain-6-679222". That is what the card
/// showed for every capture. This is only a fallback: a capture loaded in this
/// session carries its real API title, and this is for the ones that do not,
/// such as a reopened session or a file dropped from disk.
std::string PrettifyCaptureName(const std::string& stem)
{
  std::string text = stem;

  // Drop a trailing "-123456" id, but only if it really is all digits.
  const auto dash = text.find_last_of('-');

  if (dash != std::string::npos && dash + 1 < text.size()
      && text.find_first_not_of("0123456789", dash + 1) == std::string::npos)
    text.erase(dash);

  std::replace(text.begin(), text.end(), '-', ' ');
  std::replace(text.begin(), text.end(), '_', ' ');

  // Capitalise word starts. Leaves acronyms looking odd, but reads far better
  // than an unbroken slug.
  bool atWordStart = true;

  for (auto& c : text)
  {
    if (atWordStart && std::isalpha(static_cast<unsigned char>(c)))
      c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));

    atWordStart = c == ' ';
  }

  return text;
}
} // namespace

void NeuralRig::OnIdle()
{
  // Apply a requested preset here, where no control is mid-callback.
  {
    std::string presetName;
    {
      std::lock_guard<std::mutex> lock(mPendingPresetMutex);
      presetName.swap(mPendingPresetName);
    }

    if (!presetName.empty())
    {
      IByteChunk chunk;

      if (nr::presets::Load(presetName, chunk))
      {
        // Guarded: a preset written by a build with a different parameter list
        // deserialises into garbage lengths, and the reader will happily walk
        // off the end of the chunk chasing them.
        try
        {
          const int consumed = UnserializeState(chunk, 0);

          if (consumed <= 0)
            throw std::runtime_error("That preset could not be read.");

          OnParamReset(EParamSource::kPresetRecall);

          // Push the restored values out to the controls. Setting a parameter
          // does not update the control that displays it -- controls hold their
          // own copy -- so without this the rig was correctly restored while
          // every knob still showed its old position, and the next click on one
          // would write that stale value straight back over the preset.
          SendCurrentParamValuesFromDelegate();
        }
        catch (const std::exception& e)
        {
          _ShowMessageBox(GetUI(), e.what(), "Failed to load preset", kMB_OK);
        }
        catch (...)
        {
          _ShowMessageBox(GetUI(), "That preset could not be read.", "Failed to load preset", kMB_OK);
        }
      }

      if (auto* pGraphics = GetUI())
        pGraphics->SetAllControlsDirty();

      return;
    }
  }

  // Once, after the window exists. Doing it during layout is too early -- the
  // top-level window is not final yet.
  if (!mNativeMenuRemoved)
  {
    mNativeMenuRemoved = true;
    nr::shell::RemoveNativeMenu(GetUI());
  }

  // Grow or shrink the window with the browser. The layout is never re-run, so
  // nothing is rebuilt and no control is freed underneath a handler; the
  // browser simply falls outside a shorter window.
  if (mPendingResizeHeight > 0)
  {
    const int height = mPendingResizeHeight;
    mPendingResizeHeight = 0;

    if (auto* pGraphics = GetUI())
    {
      // Visibility has to move with the window. The panel is attached hidden,
      // and when the toggle stopped un-hiding it directly -- it only requests a
      // height now -- nothing else did, so the window grew to reveal a control
      // that was still hidden.
      if (auto* panel = pGraphics->GetControlWithTag(kCtrlTagT3KBrowser))
        panel->Hide(!mBrowserOpen);

      pGraphics->Resize(PLUG_WIDTH, height, pGraphics->GetDrawScale());
      pGraphics->SetAllControlsDirty();

      // Only the first time. Re-centring on every browser toggle would make
      // the window jump around under the pointer.
      if (!mWindowCentred)
      {
        mWindowCentred = true;
        nr::shell::CentreWindow(pGraphics);
      }
      else
      {
        // Opening the browser grows the window downwards from wherever it sits,
        // so make sure the part that just appeared is actually on screen.
        nr::shell::KeepOnScreen(pGraphics);
      }
    }

    return;
  }

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
    std::vector<PendingLoad> pending;
    {
      std::lock_guard<std::mutex> lock(mPendingLoadMutex);
      pending.swap(mPendingLoads);
    }

    for (const auto& [slot, path, title] : pending)
    {
      WDL_String filePath;
      filePath.Set(path.c_str());

      std::string extension = std::filesystem::path(path).extension().string();
      std::transform(extension.begin(), extension.end(), extension.begin(),
                     [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

      // A .wav is an impulse response and belongs to the cabinet. So does
      // anything aimed at the IR card, which is card index kNumSlots and
      // therefore not a capture slot at all -- staging into it would index
      // past the end of the slot arrays.
      const bool isIRTarget = slot >= static_cast<int>(kNumSlots);

      if (extension == ".wav" || isIRTarget)
      {
        mIRTitle = title;
        _LoadIRWithFeedback(filePath);
      }
      else if (slot >= 0)
      {
        // _StageModel returns why it failed, and that return value was being
        // discarded -- so a capture that would not load did nothing at all, with
        // no message anywhere. An invisible failure is indistinguishable from a
        // broken download or a broken filter, which is exactly how it looked.
        if (slot < static_cast<int>(kNumSlots))
          mSlotTitles[static_cast<size_t>(slot)] = title;

        const std::string message = _StageModel(static_cast<size_t>(slot), filePath);

        if (!message.empty())
          _ShowMessageBox(GetUI(), message.c_str(), "Failed to load capture", kMB_OK);
      }

      // Close the browser once a capture lands: the user asked for one, they
      // got it, and leaving the panel covering the rig hides the thing they
      // just changed.
      if (auto* pGraphics = GetUI())
      {
        if (mBrowserOpen)
        {
          mBrowserOpen = false;
          mPendingResizeHeight = mCollapsedHeight;
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
          // The API title when we have it; otherwise unpick the filename.
          name = !mSlotTitles[slot].empty()
                   ? mSlotTitles[slot]
                   : PrettifyCaptureName(std::filesystem::path(mNAMPaths[slot].Get()).stem().string());
        }

        static_cast<nr::rig::RigSlotControl*>(card)->SetCaptureNameAndCollapse(name.c_str());
      }
    }

    // The IR card, same idea. Tracked separately because the IR is not a
    // capture slot and does not live in mNAMPaths.
    {
      const bool occupied = mIRPath.GetLength() > 0;

      if (occupied != mIROccupancy)
      {
        mIROccupancy = occupied;

        if (auto* card = pGraphics->GetControlWithTag(kCtrlTagIRFileBrowser))
        {
          std::string name;

          if (occupied)
            name = !mIRTitle.empty()
                     ? mIRTitle
                     : PrettifyCaptureName(std::filesystem::path(mIRPath.Get()).stem().string());
          static_cast<nr::rig::RigSlotControl*>(card)->SetCaptureNameAndCollapse(name.c_str());
        }
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

    if (auto* status = pGraphics->GetControlWithTag(kCtrlTagStatusBar))
    {
      // GetSnapshot copies under the lock and does not touch the dirty flag,
      // so reading it here cannot starve the panel of its redraw.
      const auto snapshot = mBrowser.GetSnapshot();

      auto* bar = static_cast<nr::shell::StatusBarControl*>(status);
      bar->SetTransport(GetSampleRate(), GetLatency());
      bar->SetApiStatus(snapshot.message,
                        snapshot.status == nr::net::BrowserController::Status::Working);
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
      // kIRToggle deliberately has no UI case. It used to grey out the IR file
      // picker, and that control is gone -- GetControlWithTag returned null and
      // this dereferenced it, which crashed the plugin on startup as soon as
      // the parameter was initialised. The toggle still works: ProcessBlock
      // reads the parameter directly.
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
  // std::exception, not std::runtime_error.
  //
  // This caught runtime_error alone, and most of what this path throws is not
  // one. nlohmann::json has its own hierarchy -- parse_error, type_error --
  // and std::invalid_argument and std::out_of_range derive from logic_error.
  // Any of those escaped uncaught, past OnIdle, leaving no message, no staged
  // model and no failed-load notification: the capture simply did not appear.
  // A2 configs are new enough to be exactly the kind of thing that trips a
  // parse or range error rather than a runtime one.
  catch (const std::exception& e)
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
  catch (...)
  {
    // Something that is not a std::exception at all. Rare, but the alternative
    // is unwinding out of the editor's idle callback and taking the host with
    // us.
    SendControlMsgFromDelegate(ModelBrowserCtrlTag(slot), kMsgTagLoadFailed);
    mStagedModels[slot] = nullptr;
    mNAMPaths[slot] = previousNAMPath;
    return "That capture could not be read.";
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
    // Some valid IRs need their header relaxed before the loader will take
    // them. That has to happen here rather than at the call sites: an IR
    // arrives by drop, by browser, by file picker and by preset restore, and
    // normalising at only some of those means a file loads or not depending on
    // how you reached for it.
    auto irPathU8 = std::filesystem::u8path(irPath.Get());
    const std::string loadable = nr::wavcompat::MakeLoadable(irPathU8.string());
    mStagedIR = std::make_unique<dsp::ImpulseResponse>(loadable.c_str(), sampleRate);
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
      // Sized to the same block this function is preparing for, so the blend
      // never allocates on the audio thread.
      const size_t drySize = static_cast<size_t>(numFrames) * kNumChannelsInternal;

      if (mSlotDry[slot].size() != drySize)
      {
        mSlotDry[slot].assign(drySize, 0.0);

        for (size_t c = 0; c < kNumChannelsInternal; c++)
          mSlotDryPointers[slot][c] = mSlotDry[slot].data() + c * static_cast<size_t>(numFrames);
      }

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
