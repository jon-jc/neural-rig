#pragma once

#include "../AudioDSPTools/dsp/ImpulseResponse.h"
#include "../AudioDSPTools/dsp/NoiseGate.h"
#include "../AudioDSPTools/dsp/dsp.h"
#include "../AudioDSPTools/dsp/wav.h"
#include "../AudioDSPTools/dsp/ResamplingContainer/ResamplingContainer.h"
#include "../NeuralAmpModelerCore/NAM/dsp.h"
#include "../NeuralAmpModelerCore/NAM/slimmable.h"

#include "Colors.h"
#include "ToneStack.h"
#include "net/BrowserController.h"

#include <mutex>
#include <utility>

#include "IPlug_include_in_plug_hdr.h"
#include "ISender.h"


const int kNumPresets = 1;
// The plugin is mono inside
constexpr size_t kNumChannelsInternal = 1;

// Capture slots in the chain. Each is a full network evaluation per sample, so
// four is both a heavy load and enough for pedal into amp into a second stage.
/// Two capture slots -- pedal and amp -- plus the cabinet IR, which is not a
/// capture slot and lives outside this count.
///
/// There was a cab slot. TONE3000 has almost no NAM cab captures, because a
/// cabinet is captured as an impulse response, so the card asked for something
/// the library does not have. The IR card already covers cabinets properly.
/// Full-rig captures that include a cab are reachable from the amp slot, which
/// asks for amp-cab as well as amp.
constexpr size_t kNumSlots = 2;

// Capacity of each slot's bypass delay line, allocated once so ProcessBlock
// never allocates. Resampling latency is a few hundred samples at most.
constexpr size_t kBypassDelayCapacity = 8192;

class NAMSender : public iplug::IPeakAvgSender<>
{
public:
  NAMSender()
  : iplug::IPeakAvgSender<>(-90.0, true, 5.0f, 1.0f, 300.0f, 500.0f)
  {
  }
};

enum EParams
{
  // These need to be the first ones because I use their indices to place
  // their rects in the GUI.
  kInputLevel = 0,
  kNoiseGateThreshold,
  kToneBass,
  kToneMid,
  kToneTreble,
  kOutputLevel,
  // The rest is fine though.
  kNoiseGateActive,
  kEQActive,
  kIRToggle,
  // Input calibration
  kCalibrateInput,
  kInputCalibrationLevel,
  kOutputMode,
  kSlim,
  // One enable per capture slot. A disabled slot still passes its audio
  // through a matching delay, so the reported latency does not move.
  kSlot1Active,
  kSlot2Active,
  // Per-stage output trim, so a hot pedal capture can be brought back before it
  // hits the amp rather than only at the end of the chain.
  kSlot1Out,
  kSlot2Out,
  kIROut,
  // Drive into a stage. A capture responds to level the way the real thing
  // does, so pushing it harder is the control that actually changes character
  // rather than just loudness.
  kSlot1Drive,
  kSlot2Drive,
  // Cabinet voicing. Low and high cut are what every cab sim gives you,
  // because an IR on its own is almost always too boomy and too fizzy.
  kIRLowCut,
  kIRHighCut,
  // Tone after each capture, where a pedal's tone control actually sits: after
  // the clipping stage, not before it.
  kSlot1Tone,
  kSlot2Tone,
  // Dry blend per stage, delay-matched to that stage's latency.
  kSlot1Mix,
  kSlot2Mix,
  kNumParams
};

// Parameter index of a slot's enable toggle.
inline int SlotActiveParam(size_t slot)
{
  return kSlot1Active + static_cast<int>(slot);
}

// Parameter index of a slot's output trim.
inline int SlotOutParam(size_t slot)
{
  return kSlot1Out + static_cast<int>(slot);
}

// Parameter index of a slot's input drive.
inline int SlotDriveParam(size_t slot)
{
  return kSlot1Drive + static_cast<int>(slot);
}

// Parameter index of a slot's tone control.
inline int SlotToneParam(size_t slot)
{
  return kSlot1Tone + static_cast<int>(slot);
}

// Parameter index of a slot's dry blend.
inline int SlotMixParam(size_t slot)
{
  return kSlot1Mix + static_cast<int>(slot);
}

const int numKnobs = 6;

enum ECtrlTags
{
  // One file browser per capture slot. Kept contiguous and first so a slot
  // index maps straight onto a tag.
  kCtrlTagModelFileBrowser = 0,
  kCtrlTagModelFileBrowser2,
  kCtrlTagModelFileBrowser3,
  kCtrlTagModelFileBrowser4,
  kCtrlTagIRFileBrowser,
  kCtrlTagInputMeter,
  kCtrlTagOutputMeter,
  kCtrlTagSettingsBox,
  kCtrlTagOutputMode,
  kCtrlTagCalibrateInput,
  kCtrlTagInputCalibrationLevel,
  kCtrlTagSlimmableIcon,
  kCtrlTagSlimOverlayBackdrop,
  kCtrlTagSlimKnob,
  kCtrlTagT3KBrowser,
  kCtrlTagChainFlow,
  kCtrlTagBrowserToggle,
  kCtrlTagStatusBar,
  kCtrlTagPresetBar,
  kNumCtrlTags
};

// Control tag of a slot's model file browser.
inline int ModelBrowserCtrlTag(size_t slot)
{
  return kCtrlTagModelFileBrowser + static_cast<int>(slot);
}

enum EMsgTags
{
  // These tags are used from UI -> DSP
  kMsgTagClearModel = 0,
  kMsgTagClearIR,
  kMsgTagHighlightColor,
  // The following tags are from DSP -> UI
  kMsgTagLoadFailed,
  kMsgTagLoadedModel,
  kMsgTagLoadedIR,
  kNumMsgTags
};

// Get the sample rate of a NAM model.
// Sometimes, the model doesn't know its own sample rate; this wrapper guesses 48k based on the way that most
// people have used NAM in the past.
double GetNAMSampleRate(const std::unique_ptr<nam::DSP>& model)
{
  // Some models are from when we didn't have sample rate in the model.
  // For those, this wraps with the assumption that they're 48k models, which is probably true.
  const double assumedSampleRate = 48000.0;
  const double reportedEncapsulatedSampleRate = model->GetExpectedSampleRate();
  const double encapsulatedSampleRate =
    reportedEncapsulatedSampleRate <= 0.0 ? assumedSampleRate : reportedEncapsulatedSampleRate;
  return encapsulatedSampleRate;
};

class ResamplingNAM : public nam::DSP
{
public:
  // Resampling wrapper around the NAM models
  ResamplingNAM(std::unique_ptr<nam::DSP> encapsulated, const double expected_sample_rate)
  : nam::DSP(encapsulated->NumInputChannels(), encapsulated->NumOutputChannels(), expected_sample_rate)
  , mEncapsulated(std::move(encapsulated))
  , mResampler(GetNAMSampleRate(mEncapsulated))
  {
    // Assign the encapsulated object's processing function  to this object's member so that the resampler can use it:
    auto ProcessBlockFunc = [&](NAM_SAMPLE** input, NAM_SAMPLE** output, int numFrames) {
      mEncapsulated->process(input, output, numFrames);
    };
    mBlockProcessFunc = ProcessBlockFunc;

    // Get the other information from the encapsulated NAM so that we can tell the outside world about what we're
    // holding.
    if (mEncapsulated->HasLoudness())
    {
      SetLoudness(mEncapsulated->GetLoudness());
    }
    if (mEncapsulated->HasInputLevel())
    {
      SetInputLevel(mEncapsulated->GetInputLevel());
    }
    if (mEncapsulated->HasOutputLevel())
    {
      SetOutputLevel(mEncapsulated->GetOutputLevel());
    }

    // NOTE: prewarm samples doesn't mean anything--we can prewarm the encapsulated model as it likes and be good to
    // go.
    // _prewarm_samples = 0;

    // And be ready
    int maxBlockSize = 2048; // Conservative
    Reset(expected_sample_rate, maxBlockSize);
  };

  ~ResamplingNAM() = default;

  void prewarm() override { mEncapsulated->prewarm(); };

  void process(NAM_SAMPLE** input, NAM_SAMPLE** output, const int num_frames) override
  {
    if (num_frames > mMaxExternalBlockSize)
      // We can afford to be careful
      throw std::runtime_error("More frames were provided than the max expected!");

    if (!NeedToResample())
    {
      mEncapsulated->process(input, output, num_frames);
    }
    else
    {
      mResampler.ProcessBlock(input, output, num_frames, mBlockProcessFunc);
    }
  };

  int GetLatency() const { return NeedToResample() ? mResampler.GetLatency() : 0; };

  void Reset(const double sampleRate, const int maxBlockSize) override
  {
    mExpectedSampleRate = sampleRate;
    mMaxExternalBlockSize = maxBlockSize;
    mResampler.Reset(sampleRate, maxBlockSize);

    // Allocations in the encapsulated model (HACK)
    // Stolen some code from the resampler; it'd be nice to have these exposed as methods? :)
    const double mUpRatio = sampleRate / GetEncapsulatedSampleRate();
    const auto maxEncapsulatedBlockSize = static_cast<int>(std::ceil(static_cast<double>(maxBlockSize) / mUpRatio));
    mEncapsulated->ResetAndPrewarm(sampleRate, maxEncapsulatedBlockSize);
  };

  // So that we can let the world know if we're resampling (useful for debugging)
  double GetEncapsulatedSampleRate() const { return GetNAMSampleRate(mEncapsulated); };

  nam::SlimmableModel* GetSlimmableModel() { return dynamic_cast<nam::SlimmableModel*>(mEncapsulated.get()); }
  const nam::SlimmableModel* GetSlimmableModel() const
  {
    return dynamic_cast<const nam::SlimmableModel*>(mEncapsulated.get());
  }

private:
  bool NeedToResample() const { return GetExpectedSampleRate() != GetEncapsulatedSampleRate(); };
  // The encapsulated NAM
  std::unique_ptr<nam::DSP> mEncapsulated;

  // The resampling wrapper
  dsp::ResamplingContainer<NAM_SAMPLE, 1, 12> mResampler;

  // Used to check that we don't get too large a block to process.
  int mMaxExternalBlockSize = 0;

  // This function is defined to conform to the interface expected by the iPlug2 resampler.
  std::function<void(NAM_SAMPLE**, NAM_SAMPLE**, int)> mBlockProcessFunc;
};

class NeuralRig final : public iplug::Plugin
{
public:
  NeuralRig(const iplug::InstanceInfo& info);
  ~NeuralRig();

  void ProcessBlock(iplug::sample** inputs, iplug::sample** outputs, int nFrames) override;
  void OnReset() override;
  void OnIdle() override;


  bool SerializeState(iplug::IByteChunk& chunk) const override;
  int UnserializeState(const iplug::IByteChunk& chunk, int startPos) override;
  void OnUIOpen() override;
  bool OnHostRequestingSupportedViewConfiguration(int width, int height) override { return true; }

  void OnParamChange(int paramIdx) override;
  void OnParamChangeUI(int paramIdx, iplug::EParamSource source) override;
  bool OnMessage(int msgTag, int ctrlTag, int dataSize, const void* pData) override;

private:
  // Allocates mInputPointers and mOutputPointers
  void _AllocateIOPointers(const size_t nChans);
  // Moves DSP modules from staging area to the main area.
  // Also deletes DSP modules that are flagged for removal.
  // Exists so that we don't try to use a DSP module that's only
  // partially-instantiated.
  void _ApplyDSPStaging();
  // Deallocates mInputPointers and mOutputPointers
  void _DeallocateIOPointers();
  // Fallback that just copies inputs to outputs if mDSP doesn't hold a model.
  void _FallbackDSP(iplug::sample** inputs, iplug::sample** outputs, const size_t numChannels, const size_t numFrames);
  // Sizes based on mInputArray
  size_t _GetBufferNumChannels() const;
  size_t _GetBufferNumFrames() const;
  void _InitToneStack();
  // Loads a NAM model and stores it to mStagedNAM
  // Returns an empty string on success, or an error message on failure.
  std::string _StageModel(size_t slot, const WDL_String& dspFile);
  // Loads an IR and stores it to mStagedIR.
  // Return status code so that error messages can be relayed if
  // it wasn't successful.
  dsp::wav::LoadReturnCode _StageIR(const WDL_String& irPath);

  /// Stages an IR and tells the user if it failed. Shared by the three ways an
  /// IR can arrive -- dropped on a card, downloaded from the browser's IR
  /// filter, or restored from state -- so a bad file reports itself whichever
  /// route it came in by.
  void _LoadIRWithFeedback(const WDL_String& irPath);

  bool _HaveModel(size_t slot) const { return slot < kNumSlots && mModels[slot] != nullptr; };
  bool _HaveAnyModel() const
  {
    for (size_t slot = 0; slot < kNumSlots; slot++)
      if (mModels[slot] != nullptr)
        return true;
    return false;
  };
  // Levelling asks different questions at the two ends of the chain: input
  // calibration belongs to the first capture, since that is what receives the
  // player's signal, and output levelling to the last, since whatever it does
  // is what reaches the DAW.
  const ResamplingNAM* _FirstModel() const
  {
    for (size_t slot = 0; slot < kNumSlots; slot++)
      if (mModels[slot] != nullptr)
        return mModels[slot].get();
    return nullptr;
  };
  const ResamplingNAM* _LastModel() const
  {
    for (size_t slot = kNumSlots; slot-- > 0;)
      if (mModels[slot] != nullptr)
        return mModels[slot].get();
    return nullptr;
  };
  // Runs a disabled slot's audio through a delay matching what the model would
  // have added, so the reported latency stays put.
  void _RunBypassDelay(size_t slot, iplug::sample** buffers, size_t numChannels, size_t numFrames);
  // Prepare the input & output buffers
  void _PrepareBuffers(const size_t numChannels, const size_t numFrames);
  // Manage pointers
  void _PrepareIOPointers(const size_t nChans);
  // Copy the input buffer to the object, applying input level.
  // :param nChansIn: In from external
  // :param nChansOut: Out to the internal of the DSP routine
  void _ProcessInput(iplug::sample** inputs, const size_t nFrames, const size_t nChansIn, const size_t nChansOut);
  // Copy the output to the output buffer, applying output level.
  // :param nChansIn: In from internal
  // :param nChansOut: Out to external
  void _ProcessOutput(iplug::sample** inputs, iplug::sample** outputs, const size_t nFrames, const size_t nChansIn,
                      const size_t nChansOut);
  // Resetting for models and IRs, called by OnReset
  void _ResetModelAndIR(const double sampleRate, const int maxBlockSize);

  void _SetInputGain();
  void _SetOutputGain();
  void _ApplySlimParamToLoadedNAMs();

  // See: Unserialization.cpp
  void _UnserializeApplyConfig(nlohmann::json& config);
  // 0.7.9 and later
  int _UnserializeStateWithKnownVersion(const iplug::IByteChunk& chunk, int startPos);
  // Hopefully 0.7.3-0.7.8, but no gurantees
  int _UnserializeStateWithUnknownVersion(const iplug::IByteChunk& chunk, int startPos);

  // Update all controls that depend on a model
  void _UpdateControlsFromModel();

  // Make sure that the latency is reported correctly.
  void _UpdateLatency();

  // Update level meters
  // Called within ProcessBlock().
  // Assume _ProcessInput() and _ProcessOutput() were run immediately before.
  void _UpdateMeters(iplug::sample** inputPointer, iplug::sample** outputPointer, const size_t nFrames,
                     const size_t nChansIn, const size_t nChansOut);

  // Member data

  // Input arrays to NAM
  std::vector<std::vector<iplug::sample>> mInputArray;
  // Output from NAM
  std::vector<std::vector<iplug::sample>> mOutputArray;
  // Second buffer so consecutive captures can ping-pong. A capture cannot read
  // and write the same buffer: its resampler is stateful and would see its own
  // output as input.
  std::vector<std::vector<iplug::sample>> mChainArray;
  // Pointer versions
  iplug::sample** mInputPointers = nullptr;
  iplug::sample** mOutputPointers = nullptr;
  iplug::sample** mChainPointers = nullptr;

  // Input and output gain
  double mInputGain = 1.0;
  double mOutputGain = 1.0;

  // Noise gates
  dsp::noise_gate::Trigger mNoiseGateTrigger;
  dsp::noise_gate::Gain mNoiseGateGain;
  // The models actually being used, in chain order. Stacking captures is the
  // point of the plugin: an overdrive capture feeding an amp capture behaves
  // far more like the real pairing than either alone, because the second
  // network sees the first one's actual output rather than a clean signal.
  std::unique_ptr<ResamplingNAM> mModels[kNumSlots];
  // And the IR
  std::unique_ptr<dsp::ImpulseResponse> mIR;
  // Manages switching what DSP is being used.
  std::unique_ptr<ResamplingNAM> mStagedModels[kNumSlots];
  std::unique_ptr<dsp::ImpulseResponse> mStagedIR;
  // Models the audio thread has displaced, waiting to be destroyed off it.
  // Freeing a WaveNet's weights is unbounded work, so it must not happen in
  // ProcessBlock; OnIdle() collects these.
  std::unique_ptr<ResamplingNAM> mRetiredModels[kNumSlots];
  // Flags to take away the modules at a safe time.
  std::atomic<bool> mShouldRemoveModels[kNumSlots];
  std::atomic<bool> mShouldRemoveIR = false;

  // Carries a disabled slot's audio through a delay matching the latency it
  // would have added. Without this, toggling a slot changes the plugin's
  // reported latency and the host re-aligns mid-session -- audible as the
  // whole track jumping.
  std::vector<double> mSlotBypassDelay[kNumSlots];

  /// The dry copy of each stage's input, held so it can be delayed to match
  /// that stage's latency before being blended back in. Without the delay a
  /// dry/wet control is a comb filter whose notches move with the sample rate,
  /// which is why this could not just be a lerp.
  std::vector<iplug::sample> mSlotDry[kNumSlots];
  iplug::sample* mSlotDryPointers[kNumSlots][kNumChannelsInternal] = {};

  /// Post-capture tone, one per stage.
  recursive_linear_filter::LowPass mSlotTone[kNumSlots];
  int mSlotBypassWrite[kNumSlots] = {};
  int mSlotBypassLength[kNumSlots] = {};

  std::atomic<bool> mNewModelLoadedInDSP = false;
  std::atomic<bool> mModelCleared = false;

  // Tone stack modules
  std::unique_ptr<dsp::tone_stack::AbstractToneStack> mToneStack;

  // Post-IR filters
  recursive_linear_filter::HighPass mHighPass;

  // Cabinet voicing, applied to the IR's output.
  recursive_linear_filter::HighPass mIRLowCut;
  recursive_linear_filter::LowPass mIRHighCut;
  //  recursive_linear_filter::LowPass mLowPass;

  // Path to each slot's model.nam
  WDL_String mNAMPaths[kNumSlots];
  // Path to IR (.wav file)
  WDL_String mIRPath;

  WDL_String mHighLightColor{PluginColors::NAM_THEMECOLOR.ToColorCode()};

  // TONE3000 browsing. The controller owns all the threading; the plugin only
  // polls it from OnIdle.
  nr::net::BrowserController mBrowser;

  // Which slot the browser is filling. Set by the slot's globe button, read
  // when a card is clicked, so one panel serves every slot.
  int mBrowserTargetSlot = 0;

  /// Whether the IR card currently shows a file, so its name is only pushed
  /// when it actually changes rather than every idle tick.
  bool mIROccupancy = false;

  /// Whether the browser is open. Held here rather than in the control because
  /// resizing re-runs the layout function, which rebuilds every control.
  ///
  /// Starts open on iOS. There the canvas is a fixed slice of the device
  /// screen, so closing the browser buys back no space and only leaves the
  /// lower half empty; an iPad has room for the catalogue, and having it in
  /// reach is the point. Set here rather than in the layout function so the
  /// toggle and the panel are both built from the same value -- flipping it
  /// mid-layout left the toggle drawn closed over an open panel.
#if defined(OS_IOS)
  bool mBrowserOpen = true;
#else
  bool mBrowserOpen = false;
#endif

  /// Window height with the browser closed: the bottom of the status strip,
  /// measured by the layout rather than guessed. The browser is the last thing
  /// in the window, so collapsing is simply cutting the window off here.
  int mCollapsedHeight = PLUG_HEIGHT;

  /// Height the window should become, applied from OnIdle. Resizing from a
  /// control's own handler can tear down state that handler is still using,
  /// which is how this crashed the first time it was tried.
  int mPendingResizeHeight = 0;

  /// The native menu bar is detached once, from the first idle tick.
  bool mNativeMenuRemoved = false;

  /// The window is centred once, after the startup collapse.
  bool mWindowCentred = false;

  /// A preset waiting to be applied, and its guard. Restoring state restages
  /// models and rewrites the paths the audio thread reads, so it cannot run
  /// from inside the control callback that asked for it -- the same shape as
  /// the resize and close crashes.
  std::string mPendingPresetName;
  std::mutex mPendingPresetMutex;



  // Captures the browser has downloaded, waiting to be staged. The download
  // callback fires on a worker thread, and staging a model touches state the
  // audio thread reads, so the path is parked here and picked up on the message
  // thread in OnIdle instead.
  std::mutex mPendingLoadMutex;
  /// A file waiting to be staged, with the display name it should carry.
  ///
  /// The title comes from the API and is what the card shows. Without it the
  /// card falls back to the cache filename, which is a slug with a numeric id
  /// on the end -- readable, but not what the capture is called.
  struct PendingLoad
  {
    int slot = 0;
    std::string path;
    std::string title; ///< empty for drops and local files, which have no API record
  };

  std::vector<PendingLoad> mPendingLoads;

  /// Display names per stage, held in memory only. A reloaded session falls
  /// back to prettifying the filename rather than carrying titles through
  /// serialization, which is a format change for a cosmetic gain.
  std::string mSlotTitles[kNumSlots];
  std::string mIRTitle;

  // Last-known slot occupancy, so the chain connector is only redrawn when a
  // slot is actually filled or emptied rather than every idle tick.
  bool mFlowOccupancy[kNumSlots] = {};

  std::unordered_map<std::string, double> mNAMParams = {{"Input", 0.0}, {"Output", 0.0}};

  NAMSender mInputSender, mOutputSender;
};
