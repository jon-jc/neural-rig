#pragma once

/**
    The status strip along the bottom of the window.

    Three things the plugin knows and the user cannot otherwise find out:

    - what rate it is running at, and how much latency the chain is adding.
      NAM captures are not zero-latency and a chain of them compounds, so a
      player wondering why their timing feels off has somewhere to look.
    - what the TONE3000 API last said. This matters more than it sounds: the
      search endpoint is rate-limited hard enough that users will meet it, and
      "TONE3000 is rate-limiting us" in the status bar is the difference
      between a plugin that looks broken and one that is briefly waiting.
    - whether a network operation is in flight, so a slow search reads as slow
      rather than as nothing happening.

    Pure output. The host pushes values in; this never reaches back.
*/

#include <string>

#include "Colors.h"
#include "IControl.h"

using namespace iplug;
using namespace iplug::igraphics;

namespace nr::shell
{

class StatusBarControl : public IControl
{
public:
  explicit StatusBarControl(const IRECT& bounds)
  : IControl(bounds)
  {
    mIgnoreMouse = true;
  }

  void SetTransport(double sampleRate, int latencySamples)
  {
    if (sampleRate == mSampleRate && latencySamples == mLatencySamples)
      return;

    mSampleRate = sampleRate;
    mLatencySamples = latencySamples;
    SetDirty(false);
  }

  void SetApiStatus(const std::string& message, bool working)
  {
    if (message == mApiStatus && working == mWorking)
      return;

    mApiStatus = message;
    mWorking = working;
    SetDirty(false);
  }

  void Draw(IGraphics& g) override
  {
    g.FillRect(PluginColors::CHASSIS, mRECT);
    g.FillRect(PluginColors::PANEL_HI.WithOpacity(0.5f), mRECT.GetFromTop(1.f));

    const auto inner = mRECT.GetPadded(-14.f);

    DrawField(g, inner.GetFromLeft(150.f), "MODE", ModeText().c_str());
    DrawField(g, inner.GetFromLeft(300.f).GetFromRight(140.f), "LATENCY", LatencyText().c_str());

    // The API line gets the rest of the width: it is the only field whose
    // content is unbounded, and truncating a rate-limit message would defeat
    // the point of showing it.
    DrawField(g, inner.GetReducedFromLeft(310.f), "API STATUS",
              mApiStatus.empty() ? "Idle" : mApiStatus.c_str(), mWorking);
  }

private:
  std::string ModeText() const
  {
    if (mSampleRate <= 0.0)
      return "--";

    char buffer[32];
    snprintf(buffer, sizeof(buffer), "Native %.1f kHz", mSampleRate / 1000.0);
    return buffer;
  }

  std::string LatencyText() const
  {
    if (mLatencySamples <= 0)
      return "None";

    // Samples alone mean nothing without the rate, and milliseconds are what
    // a player actually feels, so show both.
    char buffer[48];

    if (mSampleRate > 0.0)
      snprintf(buffer, sizeof(buffer), "%d smp  ·  %.1f ms", mLatencySamples,
               1000.0 * static_cast<double>(mLatencySamples) / mSampleRate);
    else
      snprintf(buffer, sizeof(buffer), "%d smp", mLatencySamples);

    return buffer;
  }

  void DrawField(IGraphics& g, const IRECT& rect, const char* caption, const char* value, bool active = false)
  {
    const IText captionText(9.5f, PluginColors::INK_DIM, nullptr, EAlign::Near, EVAlign::Top);
    g.DrawText(captionText, caption, rect);

    const IText valueText(12.f, active ? PluginColors::AMBER : PluginColors::INK_MUTED, nullptr, EAlign::Near,
                          EVAlign::Bottom);
    g.DrawText(valueText, value, rect);
  }

  double mSampleRate = 0.0;
  int mLatencySamples = 0;
  std::string mApiStatus;
  bool mWorking = false;
};

} // namespace nr::shell
