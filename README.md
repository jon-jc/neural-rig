# NeuralRig

A guitar amp plugin built on [Neural Amp Modeler](https://github.com/sdatkinson/neural-amp-modeler).

Where the stock NAM plugin loads one capture at a time, NeuralRig is a **rig**: a
chain of NAM captures and effects, plus a **TONE3000 browser built into the
plugin** so you can search, filter, download and audition profiles without ever
leaving your DAW.

Formats: **VST3**, **Standalone**, and **AU** on macOS.

---

## Status

| Milestone | Scope | State |
|---|---|---|
| M1 | CMake + JUCE build, CI, parameter tree, I/O staging | in review |
| M2 | Single-NAM engine: loading, resampling, calibration, gate, tone stack, IR | planned |
| M3 | Multi-node chain engine with real-time-safe hot swap | planned |
| M4 | TONE3000 in-plugin browser (OAuth, search, download, instant load) | planned |
| M5 | Effects rack: drive, comp, EQ, delay, reverb | planned |
| M6 | UI/UX polish, presets, A/B compare | planned |

Each milestone lands as its own pull request against `main`, built on Windows,
macOS and Linux by CI before merge.

---

## Building

Requires **CMake 3.22+** and a **C++20** compiler.

```bash
git clone --recurse-submodules https://github.com/jon-jc/neural-rig.git
cd neural-rig
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
```

If you cloned without `--recurse-submodules`:

```bash
git submodule update --init --recursive
```

Artefacts land in `build/NeuralRig_artefacts/Release/`.

### Platform notes

- **Windows** — Visual Studio 2022 Build Tools with the C++ workload. The MSVC
  runtime is linked statically so the VST3 needs no redistributable.
- **macOS** — builds a universal `arm64` + `x86_64` binary, deployment target 10.15.
- **Linux** — needs ALSA, X11, FreeType, Fontconfig and libcurl development
  packages; see `.github/workflows/build.yml` for the exact list.

---

## Layout

```
source/
  Parameters.*        host-facing parameter tree, defined once
  PluginProcessor.*   audio thread: I/O staging and the chain
  PluginEditor.*      UI
  NamSupport.*        the only place NAM/Eigen/json headers are included
  dsp/                signal processing
cmake/
  NamCore.cmake       builds the nam_core and eigen targets
tests-cpp/            DSP unit tests (juce::UnitTest)
external/             vendored dependencies, as git submodules
```

`NamSupport` exists deliberately: NAM's headers pull in Eigen and
nlohmann/json, which are slow to compile and noisy under JUCE's warning
settings. Confining them to one translation unit keeps the rest of the build
fast and lets us compile our own code with warnings-as-errors.

---

## Sample type

`nam_core` is compiled with `NAM_SAMPLE_FLOAT`. JUCE hands us `float` buffers
and NAM's inference is single-precision Eigen throughout, so this removes a
`double`↔`float` conversion at every node of the chain at no cost in accuracy.

## Why not AudioDSPTools?

The upstream NAM plugin gets its gate, IR, filters and resampler from
[AudioDSPTools](https://github.com/sdatkinson/AudioDSPTools). NeuralRig
deliberately does not, for two concrete reasons:

- Its `ResamplingContainer` — the one component with no JUCE equivalent —
  `#include`s `Dependencies/WDL/ptrlist.h` and `Dependencies/LanczosResampler.h`,
  which come from iPlug2 and are not vendored in the AudioDSPTools repo. It
  does not compile standalone.
- `NoiseGate`, `ImpulseResponse` and `RecursiveLinearFilter` hardcode `double`
  in signatures that its own `DSP_SAMPLE_FLOAT` switch is meant to control, so
  the library cannot actually be built single-precision.

JUCE already provides better-tested equivalents — `juce::dsp::Convolution`
(partitioned FFT, versus a direct convolution), `juce::dsp::IIR`,
`juce::Interpolators` and `AudioFormatManager` — so NeuralRig builds on those
and keeps the whole signal path in `float`.

---

## Third-party code

| Project | Licence | Use |
|---|---|---|
| [JUCE](https://github.com/juce-framework/JUCE) | GPLv3 / commercial | Plugin framework, DSP and UI |
| [NeuralAmpModelerCore](https://github.com/sdatkinson/NeuralAmpModelerCore) | MIT | NAM inference engine |
| [Eigen](https://gitlab.com/libeigen/eigen) | MPL2 | Linear algebra |
| [nlohmann/json](https://github.com/nlohmann/json) | MIT | `.nam` file parsing (vendored inside NAM core) |

All are pinned git submodules under `external/`; none of their source is
copied into this repository.

NeuralRig uses JUCE under its GPLv3 option, so **this project is GPLv3** — see
`LICENSE`. Shipping it under a closed licence would require a commercial JUCE
licence.

Captures are produced by the NAM trainer at
[sdatkinson/neural-amp-modeler](https://github.com/sdatkinson/neural-amp-modeler)
(MIT), which NeuralRig plays but does not include.

TONE3000 integration uses the public [TONE3000 API](https://www.tone3000.com/api).
Profiles downloaded through it remain under whatever licence their author
chose; NeuralRig surfaces that licence in the browser UI.
