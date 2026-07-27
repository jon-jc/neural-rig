#pragma once

#include "IControls.h"

#include "Colors.h"

using namespace iplug;
using namespace igraphics;

/**
    The drawn furniture of NeuralRig's panel.

    None of this is bitmap art. Everything is drawn, so it stays sharp at any
    editor scale and the palette can be retuned in one place -- which is also
    why the old fixed-size background photograph had to go.
*/
namespace nr::theme
{

/// Backdrop for the whole editor: a shallow vertical gradient, lighter at the
/// top. A flat fill at this size reads as dead space; a gradient this subtle is
/// not consciously noticed but stops the panel looking like a void.
class ChassisControl : public IControl
{
public:
  explicit ChassisControl(const IRECT& bounds)
  : IControl(bounds)
  {
    mIgnoreMouse = true;
  }

  void Draw(IGraphics& g) override
  {
    IPattern gradient(EPatternType::Linear);
    gradient.SetTransform(1.f / mRECT.W(), 0.f, 0.f, 1.f / mRECT.H(), -mRECT.L / mRECT.W(),
                          -mRECT.T / mRECT.H());
    gradient.AddStop(IColor(255, 26, 26, 31), 0.f);
    gradient.AddStop(PluginColors::CHASSIS, 0.62f);
    gradient.AddStop(IColor(255, 6, 6, 8), 1.f);

    g.PathRect(mRECT);
    g.PathFill(gradient);
  }
};

/**
    A raised section panel with a title.

    Three cues sell the "machined from a block" impression, and all three are
    one line each: a soft shadow beneath, a fill a step lighter than the
    chassis, and a single highlight along the top edge where light would catch.
    The title sits on the panel rather than breaking the border, with a rule
    running off to the right to lead the eye across.
*/
class SectionPanelControl : public IControl
{
public:
  SectionPanelControl(const IRECT& bounds, const char* title)
  : IControl(bounds)
  , mTitle(title)
  {
    mIgnoreMouse = true;
  }

  void Draw(IGraphics& g) override
  {
    const auto panel = mRECT;
    constexpr float radius = 7.f;

    g.FillRoundRect(IColor(70, 0, 0, 0), panel.GetTranslated(0.f, 3.f), radius);
    g.FillRoundRect(PluginColors::PANEL, panel, radius);

    // The lit top edge. Inset horizontally so it fades out before the corners,
    // which is what a real bevel does.
    g.DrawLine(PluginColors::PANEL_HI, panel.L + radius, panel.T + 0.5f, panel.R - radius, panel.T + 0.5f,
               nullptr, 1.f);

    const auto label = mRECT.GetFromTop(kTitleHeight).GetPadded(-12.f, 0.f, -12.f, 0.f);
    const auto text = IText(11.f, EAlign::Near, PluginColors::INK_MUTED);

    g.DrawText(text, mTitle.Get(), label);

    // Rule from the end of the title to the panel edge.
    const auto titleWidth = 9.f * static_cast<float>(mTitle.GetLength());
    const auto ruleLeft = label.L + titleWidth + 10.f;

    if (ruleLeft < label.R - 8.f)
      g.DrawLine(PluginColors::AMBER.WithOpacity(0.22f), ruleLeft, label.MH(), label.R, label.MH(), nullptr,
                 1.f);
  }

  /// Vertical space the title takes, so callers can inset their contents.
  static constexpr float kTitleHeight = 22.f;

private:
  WDL_String mTitle;
};

/**
    The connector running down the left of the capture chain.

    Four file rows on their own read as a list of unrelated pickers. A line
    threading through numbered nodes reads as a signal path, which is what it
    actually is -- and it costs a few draw calls rather than any layout.
*/
class ChainFlowControl : public IControl
{
public:
  ChainFlowControl(const IRECT& bounds, int numSlots, float slotPitch)
  : IControl(bounds)
  , mNumSlots(numSlots)
  , mSlotPitch(slotPitch)
  {
    mIgnoreMouse = true;
  }

  /// Which slots hold a capture, so filled nodes can be lit and empty ones not.
  void SetOccupancy(const std::vector<bool>& occupied)
  {
    mOccupied = occupied;
    SetDirty(false);
  }

  void Draw(IGraphics& g) override
  {
    const auto x = mRECT.MW();

    for (int slot = 0; slot < mNumSlots; slot++)
    {
      const auto centreY = mRECT.T + mSlotPitch * (static_cast<float>(slot) + 0.5f);
      const bool occupied = slot < static_cast<int>(mOccupied.size()) && mOccupied[static_cast<size_t>(slot)];

      // Segment down to the next node.
      if (slot + 1 < mNumSlots)
        g.DrawLine(PluginColors::AMBER.WithOpacity(occupied ? 0.55f : 0.16f), x, centreY + kNodeRadius, x,
                   centreY + mSlotPitch - kNodeRadius, nullptr, 1.5f);

      // A loaded slot gets a filled, haloed node; an empty one a hollow ring.
      if (occupied)
      {
        g.FillCircle(PluginColors::AMBER_GLOW, x, centreY, kNodeRadius + 4.f);
        g.FillCircle(PluginColors::AMBER, x, centreY, kNodeRadius);
      }
      else
      {
        g.DrawCircle(PluginColors::AMBER.WithOpacity(0.3f), x, centreY, kNodeRadius, nullptr, 1.5f);
      }

      const auto number = std::to_string(slot + 1);
      g.DrawText(IText(10.f, EAlign::Center,
                       occupied ? PluginColors::CHASSIS : PluginColors::INK_DIM),
                 number.c_str(), IRECT(x - 10.f, centreY - 7.f, x + 10.f, centreY + 7.f));
    }
  }

private:
  static constexpr float kNodeRadius = 8.f;

  int mNumSlots;
  float mSlotPitch;
  std::vector<bool> mOccupied;
};

/// The plugin's control style: amber accents, no frames, warm type.
inline IVStyle Style()
{
  return DEFAULT_STYLE.WithColor(kFG, PluginColors::AMBER)
    .WithColor(kON, PluginColors::AMBER)
    .WithColor(kOFF, PluginColors::WELL)
    .WithColor(kBG, PluginColors::NAM_0)
    .WithColor(kFR, PluginColors::AMBER.WithOpacity(0.35f))
    .WithColor(kX1, PluginColors::AMBER)
    .WithColor(kHL, PluginColors::MOUSEOVER)
    .WithColor(kSH, PluginColors::NAM_0)
    .WithDrawFrame(false)
    .WithDrawShadows(false)
    .WithLabelText(IText(13.f, EAlign::Center, PluginColors::INK_MUTED))
    .WithValueText(IText(13.f, EAlign::Center, PluginColors::INK));
}

} // namespace nr::theme
