#pragma once

#include <algorithm>
#include <cmath>
#include <vector>

/**
    The pedal FX that belong *after* an amp capture.

    Drive is the odd one out and is deliberately mild here: overdrive in front
    of a real amp is part of what the capture already contains, so a heavy
    distortion after it would be modelling the wrong signal path. What is left
    is the wet effects loop -- delay and reverb -- which is where a real rig
    puts them.

    Mono, matching the rest of the chain, and allocation-free once prepared.
*/
namespace nr::fx
{

/// Soft asymmetric saturation. tanh rather than a hard clip: a hard knee after
/// a neural capture sounds like a bug rather than a pedal.
class Drive
{
public:
  void SetAmount(double amount0to10) { mGain = 1.0 + amount0to10 * 2.4; }

  void Process(double* samples, int numFrames) const
  {
    if (mGain <= 1.0)
      return;

    // Normalise by the gain so turning drive up does not simply turn volume up.
    const double makeUp = 1.0 / std::tanh(mGain);

    for (int i = 0; i < numFrames; i++)
      samples[i] = std::tanh(samples[i] * mGain) * makeUp;
  }

private:
  double mGain = 1.0;
};

/// Single-tap delay with feedback.
class Delay
{
public:
  void Prepare(double sampleRate, double maxDelayMs)
  {
    mSampleRate = sampleRate;
    mBuffer.assign(static_cast<size_t>(sampleRate * maxDelayMs / 1000.0) + 4, 0.0);
    Reset();
  }

  void Reset()
  {
    std::fill(mBuffer.begin(), mBuffer.end(), 0.0);
    mWrite = 0;
  }

  void SetTimeMs(double milliseconds)
  {
    const auto samples = static_cast<int>(mSampleRate * milliseconds / 1000.0);
    mDelay = std::max(1, std::min(samples, static_cast<int>(mBuffer.size()) - 1));
  }

  void SetMix(double mix0to1) { mMix = std::clamp(mix0to1, 0.0, 1.0); }

  void Process(double* samples, int numFrames)
  {
    if (mBuffer.empty() || mMix <= 0.0)
      return;

    const auto capacity = static_cast<int>(mBuffer.size());

    for (int i = 0; i < numFrames; i++)
    {
      int read = mWrite - mDelay;
      if (read < 0)
        read += capacity;

      const double delayed = mBuffer[static_cast<size_t>(read)];

      // Feedback kept below unity so a long tail decays rather than building.
      mBuffer[static_cast<size_t>(mWrite)] = samples[i] + delayed * kFeedback;

      samples[i] += delayed * mMix;

      if (++mWrite >= capacity)
        mWrite = 0;
    }
  }

private:
  static constexpr double kFeedback = 0.38;

  std::vector<double> mBuffer;
  double mSampleRate = 48000.0;
  double mMix = 0.0;
  int mDelay = 1;
  int mWrite = 0;
};

/// Small Schroeder reverb: four parallel combs into two allpasses. Cheap, and
/// enough to sit a capture in a room rather than model a hall.
class Reverb
{
public:
  void Prepare(double sampleRate)
  {
    // Comb and allpass lengths in samples at 44.1k, scaled to the host rate.
    // Mutually prime so their echoes do not line up into a ringing pitch.
    static constexpr int kCombLengths[kNumCombs] = {1116, 1188, 1277, 1356};
    static constexpr int kAllpassLengths[kNumAllpasses] = {556, 441};

    const double scale = sampleRate / 44100.0;

    for (int i = 0; i < kNumCombs; i++)
      mCombs[i].Prepare(static_cast<int>(kCombLengths[i] * scale));

    for (int i = 0; i < kNumAllpasses; i++)
      mAllpasses[i].Prepare(static_cast<int>(kAllpassLengths[i] * scale));
  }

  void Reset()
  {
    for (auto& comb : mCombs)
      comb.Reset();
    for (auto& allpass : mAllpasses)
      allpass.Reset();
  }

  void SetMix(double mix0to1) { mMix = std::clamp(mix0to1, 0.0, 1.0); }

  void Process(double* samples, int numFrames)
  {
    if (mMix <= 0.0)
      return;

    for (int i = 0; i < numFrames; i++)
    {
      const double dry = samples[i];
      double wet = 0.0;

      for (auto& comb : mCombs)
        wet += comb.Process(dry);

      wet /= static_cast<double>(kNumCombs);

      for (auto& allpass : mAllpasses)
        wet = allpass.Process(wet);

      samples[i] = dry + wet * mMix;
    }
  }

private:
  static constexpr int kNumCombs = 4;
  static constexpr int kNumAllpasses = 2;

  struct Line
  {
    std::vector<double> buffer;
    int index = 0;

    void Prepare(int length)
    {
      buffer.assign(static_cast<size_t>(std::max(1, length)), 0.0);
      index = 0;
    }

    void Reset() { std::fill(buffer.begin(), buffer.end(), 0.0); }

    double& Current() { return buffer[static_cast<size_t>(index)]; }

    void Advance()
    {
      if (++index >= static_cast<int>(buffer.size()))
        index = 0;
    }
  };

  struct Comb : Line
  {
    static constexpr double kFeedback = 0.805;
    static constexpr double kDamping = 0.22;

    double filterStore = 0.0;

    double Process(double input)
    {
      const double output = Current();
      // One-pole lowpass in the feedback path, so the tail darkens as it
      // decays the way a real room does.
      filterStore = output * (1.0 - kDamping) + filterStore * kDamping;
      Current() = input + filterStore * kFeedback;
      Advance();
      return output;
    }
  };

  struct Allpass : Line
  {
    static constexpr double kFeedback = 0.5;

    double Process(double input)
    {
      const double buffered = Current();
      const double output = -input + buffered;
      Current() = input + buffered * kFeedback;
      Advance();
      return output;
    }
  };

  Comb mCombs[kNumCombs];
  Allpass mAllpasses[kNumAllpasses];
  double mMix = 0.0;
};

} // namespace nr::fx
