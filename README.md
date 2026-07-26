# NeuralRig

A guitar amp plugin built on [Neural Amp Modeler](https://github.com/sdatkinson/neural-amp-modeler).

Where the stock NAM plugin loads one capture at a time, NeuralRig is a **rig**: a
chain of NAM captures, plus a **TONE3000 browser built into the plugin** so you
can search, filter, download and audition profiles without leaving your DAW.

Formats: **VST3**, **AU**, **AAX**, **Standalone**.

---

## Tone parity

NeuralRig is derived directly from
[NeuralAmpModelerPlugin](https://github.com/sdatkinson/NeuralAmpModelerPlugin)
and keeps its signal path intact, so a single loaded capture sounds exactly
like it does in the original plugin:

```
input trim (calibrated)
  → noise gate trigger
  → NAM capture (resampled to its trained rate)
  → noise gate gain
  → tone stack (bass / middle / treble)
  → impulse response
  → DC-blocking high-pass
  → output trim
```

Two details in that chain are easy to get wrong and are worth naming, because
getting either wrong changes the tone:

- The noise gate is **split**. It triggers on the *input* — so the decision to
  open is made on the player's clean signal — but applies its gain *after* the
  model, so the capture's own noise floor is attenuated too. Gating entirely
  before the model leaves that noise floor untouched; gating entirely after it
  makes the gate stutter on the model's output.
- The **DC-blocking high-pass after the IR** is not cosmetic. Neural models can
  emit a small DC offset, and without the blocker it accumulates through
  anything downstream.

Everything runs in `double` throughout, matching iPlug2's `sample` type and the
defaults of both `NAM_SAMPLE` and `DSP_SAMPLE`.

---

## Building

Requires **Visual Studio 2022** (Windows) or **Xcode** (macOS), plus **Python 3**.

```bash
git clone --recurse-submodules https://github.com/jon-jc/neural-rig.git
cd neural-rig
```

Fetch the SDKs iPlug2 needs (once):

```bash
cd iPlug2/Dependencies/IPlug && ./download-iplug-sdks.sh && cd -
cd iPlug2/Dependencies && ./download-prebuilt-libs.sh && cd -
```

Then build:

```bash
cd NeuralRig/scripts && ./makedist-win.bat full zip     # Windows
cd NeuralRig/scripts && ./makedist-mac.sh full zip      # macOS
```

Or open `NeuralRig/NeuralRig.sln` / `NeuralRig.xcworkspace` directly.

**Linux is not supported.** iPlug2 does not target it, which is the trade made
in exchange for a permissive licence and upstream parity.

---

## Layout

Mirrors upstream's, because iPlug2's project files reference dependencies by
relative path (`..\..\iPlug2\...`) and will not resolve otherwise.

```
iPlug2/                 plugin framework (submodule)
NeuralAmpModelerCore/   NAM inference engine (submodule)
AudioDSPTools/          gate, IR, filters, resampler (submodule)
eigen/                  linear algebra (submodule)
NeuralRig/              the plugin
  config.h              plugin identity and IDs
  NeuralRig.cpp/.h      processor
  NeuralRigControls.h   IGraphics UI
  ToneStack.cpp/.h      bass / middle / treble
  projects/             Visual Studio and Xcode projects
  scripts/              build and packaging
common-win.props        shared MSBuild settings
common-mac.xcconfig     shared Xcode settings
```

---

## Licence

MIT — see `LICENSE`. Derived from NeuralAmpModelerPlugin, Copyright (c) 2022
Steven Atkinson, also MIT.

`PLUG_UNIQUE_ID` and `PLUG_MFR_ID` are deliberately distinct from upstream's so
that NeuralRig and the original NAM plugin can coexist in a host without their
registries colliding.

TONE3000 integration uses the public [TONE3000 API](https://www.tone3000.com/api).
Profiles downloaded through it stay under whatever licence their author chose;
NeuralRig surfaces that licence in the browser.
