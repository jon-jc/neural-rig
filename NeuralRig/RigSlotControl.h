#pragma once

/**
    A typed slot in the rig: PEDAL, AMP or CAB.

    The chain used to be four identical file pickers stacked vertically, which
    said nothing about what belonged where. Three typed slots laid out left to
    right read as signal flow, and the type is not just a label: it seeds the
    browser's gear filter, so clicking an empty PEDAL slot opens the catalogue
    already filtered to pedals rather than to everything.

    Drawn rather than assembled from child controls, for the same reason the
    browser cards are: the card is a surface, a few strings and three small hit
    targets, and children would cost a control-tree rebuild every time a
    capture loads.
*/

#include <functional>
#include <string>

#include "Colors.h"
#include "IControl.h"

using namespace iplug;
using namespace iplug::igraphics;

namespace nr::rig
{

enum class SlotKind
{
  Pedal,
  Amp,

  /// The cabinet impulse response. Not a capture slot: it is a .wav convolved
  /// after the chain, so it has its own parameter and its own loader. It gets a
  /// card because from the player's side it is one more thing in the signal
  /// path, and having it anywhere else made it invisible.
  IR,
};

inline const char* SlotLabel(SlotKind kind)
{
  switch (kind)
  {
    case SlotKind::Pedal: return "PEDAL";
    case SlotKind::Amp: return "AMP";
    case SlotKind::IR: return "IR";
  }

  return "SLOT";
}

/// Placeholder shown while the slot is empty. Names the thing to pick and the
/// other way to fill it, because drag-and-drop is invisible otherwise.
inline const char* SlotPlaceholder(SlotKind kind)
{
  switch (kind)
  {
    case SlotKind::Pedal: return "Select a Pedal from the browser";
    case SlotKind::Amp: return "Select an Amp from the browser";
    case SlotKind::IR: return "Select an IR from the browser";
  }

  return "Select a capture from the browser";
}

inline IColor SlotAccent(SlotKind kind)
{
  switch (kind)
  {
    case SlotKind::Pedal: return PluginColors::GEAR_PEDAL;
    case SlotKind::Amp: return PluginColors::GEAR_AMP_CAB;
    case SlotKind::IR: return PluginColors::GEAR_SPACE;
  }

  return PluginColors::GEAR_OTHER;
}

/// The gear values this slot browses for, joined by the caller into the API's
/// underscore-separated form.
///
/// The amp slot asks for amp and amp-cab together. A full-rig capture is still
/// an amp as far as this slot is concerned -- it is one .nam that happens to
/// include a cabinet -- and since the cab slot is gone, these are how you get a
/// speaker in the chain without a separate IR.
///
/// The IR slot has no gear at all: an impulse response is a format.
inline std::vector<std::string> SlotGears(SlotKind kind)
{
  switch (kind)
  {
    case SlotKind::Pedal: return {"pedal"};
    case SlotKind::Amp: return {"amp", "amp-cab"};
    case SlotKind::IR: return {};
  }

  return {};
}

class RigSlotControl : public IControl
{
public:
  using SlotAction = std::function<void(int slot)>;
  using DropAction = std::function<void(int slot, const char* path)>;

  RigSlotControl(const IRECT& bounds, int slot, SlotKind kind, int activeParam, SlotAction onBrowse,
                 SlotAction onClear, DropAction onDrop)
  : IControl(bounds, activeParam)
  , mSlot(slot)
  , mKind(kind)
  , mOnBrowse(std::move(onBrowse))
  , mOnClear(std::move(onClear))
  , mOnDrop(std::move(onDrop))
  {
  }

  /// Dropping a file onto a card fills it. The empty state advertises this, so
  /// it has to work: iPlug2 already routes the platform's drop to the control
  /// under the cursor, and nothing was listening.
  void OnDrop(const char* path) override
  {
    if (mOnDrop && path != nullptr)
      mOnDrop(mSlot, path);
  }

  void OnResize() override { LayOut(); }
  void OnAttached() override { LayOut(); }

  /// Called by the host when a capture lands in or leaves this slot.
  void SetCaptureName(const char* name)
  {
    mCaptureName = name != nullptr ? name : "";
    SetDirty(false);
  }

  bool HasCapture() const { return !mCaptureName.empty(); }

  void Draw(IGraphics& g) override
  {
    const auto accent = SlotAccent(mKind);
    const bool loaded = HasCapture();
    const bool on = loaded && GetValue() > 0.5;

    // A loaded slot is outlined in its type colour; an empty one is a dashed
    // well, so the rig reads at a glance as "two filled, one waiting".
    g.FillRoundRect(PluginColors::PANEL, mRECT, 7.f);

    if (loaded)
      g.DrawRoundRect(accent.WithOpacity(on ? 1.f : 0.35f), mRECT, 7.f, nullptr, 1.5f);
    else
      g.DrawRoundRect(PluginColors::PANEL_HI, mRECT, 7.f);

    const IText label(12.f, loaded ? accent : PluginColors::INK_MUTED, nullptr, EAlign::Near, EVAlign::Middle);
    g.DrawText(label, SlotLabel(mKind), mLabelRect);

    if (loaded)
    {
      const IText title(17.f, PluginColors::INK, nullptr, EAlign::Near, EVAlign::Middle);
      g.DrawText(title, mCaptureName.c_str(), mTitleRect);

      DrawClear(g, accent);
      DrawPower(g, accent, on);
    }
    else
    {
      const IText hint(14.f, PluginColors::INK_MUTED, nullptr, EAlign::Near, EVAlign::Middle);
      g.DrawText(hint, SlotPlaceholder(mKind), mTitleRect);

      const IText sub(12.f, PluginColors::INK_DIM, nullptr, EAlign::Near, EVAlign::Middle);
      g.DrawText(sub, mKind == SlotKind::IR ? "Alternatively, drop a .wav IR here"
                                     : "Alternatively, drop a NAM file here",
                 mSubtitleRect);
    }
  }

  void OnMouseDown(float x, float y, const IMouseMod& mod) override
  {
    if (HasCapture())
    {
      if (mClearRect.Contains(x, y))
      {
        if (mOnClear)
          mOnClear(mSlot);

        return;
      }

      if (mPowerRect.Contains(x, y))
      {
        SetValue(GetValue() > 0.5 ? 0.0 : 1.0);
        SetDirty(true);
        return;
      }
    }

    // Anywhere else on the card opens the browser aimed at this slot. An empty
    // card is one big target rather than a small "browse" button, since filling
    // it is the only thing it is for.
    if (mOnBrowse)
      mOnBrowse(mSlot);
  }

  void OnMouseOver(float x, float y, const IMouseMod& mod) override
  {
    if (!mHovered)
    {
      mHovered = true;
      SetDirty(false);
    }
  }

  void OnMouseOut() override
  {
    if (mHovered)
    {
      mHovered = false;
      SetDirty(false);
    }
  }

private:
  void LayOut()
  {
    const auto inner = mRECT.GetPadded(-12.f);

    // Rows sized for the type they hold, with the clear button clear of the
    // label rather than sharing its baseline.
    mLabelRect = inner.GetFromTop(16.f);
    mClearRect = mLabelRect.GetFromRight(18.f);
    mTitleRect = inner.GetFromTop(52.f).GetReducedFromTop(22.f);
    mSubtitleRect = inner.GetFromTop(74.f).GetReducedFromTop(52.f);
    mPowerRect = inner.GetFromBottom(30.f).GetFromRight(30.f);
  }

  void DrawClear(IGraphics& g, const IColor& accent)
  {
    const auto box = mClearRect.GetPadded(-2.f);
    g.DrawRoundRect(PluginColors::INK_DIM, box, 2.f);

    const float inset = 4.f;
    g.DrawLine(PluginColors::INK_MUTED, box.L + inset, box.T + inset, box.R - inset, box.B - inset);
    g.DrawLine(PluginColors::INK_MUTED, box.R - inset, box.T + inset, box.L + inset, box.B - inset);
  }

  void DrawPower(IGraphics& g, const IColor& accent, bool on)
  {
    const auto circle = mPowerRect;
    const float radius = circle.W() * 0.5f;

    g.FillCircle(on ? accent.WithOpacity(0.22f) : PluginColors::WELL, circle.MW(), circle.MH(), radius);
    g.DrawCircle(on ? accent : PluginColors::INK_DIM, circle.MW(), circle.MH(), radius, nullptr, 1.4f);

    // The IEC power glyph: a broken ring with a stem through the gap.
    const auto colour = on ? accent : PluginColors::INK_DIM;
    const float r = radius * 0.42f;
    g.DrawArc(colour, circle.MW(), circle.MH(), r, 35.f, 325.f, nullptr, 1.4f);
    g.DrawLine(colour, circle.MW(), circle.MH() - r * 1.25f, circle.MW(), circle.MH() - r * 0.1f, nullptr, 1.4f);
  }

  int mSlot;
  SlotKind mKind;
  SlotAction mOnBrowse;
  SlotAction mOnClear;
  DropAction mOnDrop;

  std::string mCaptureName;
  bool mHovered = false;

  IRECT mLabelRect, mTitleRect, mSubtitleRect, mClearRect, mPowerRect;
};

/**
    The always-visible BROWSER handle.

    The catalogue used to be reachable only through a slot's browse button,
    which made "show me what is on TONE3000" a question you could only ask by
    first deciding which slot you were filling. This sits under the rig,
    permanently, and toggles the panel regardless of any slot.
*/
class BrowserToggleControl : public IControl
{
public:
  using Toggle = std::function<void(bool open)>;

  /// @param open  the state to start in. Opening the browser resizes the
  ///              window, which re-runs the layout and rebuilds this control,
  ///              so it cannot own the state it is showing.
  BrowserToggleControl(const IRECT& bounds, bool open, Toggle onToggle)
  : IControl(bounds)
  , mOnToggle(std::move(onToggle))
  , mOpen(open)
  {
  }

  void SetOpen(bool open)
  {
    mOpen = open;
    SetDirty(false);
  }

  void Draw(IGraphics& g) override
  {
    const auto colour = mHovered ? PluginColors::AMBER : PluginColors::INK_MUTED;

    const IText label(12.f, colour, nullptr, EAlign::Center, EVAlign::Top);
    g.DrawText(label, "BROWSER", mRECT);

    // A chevron pointing the way the panel will move, so the control says what
    // it will do rather than what state it is in.
    const float cx = mRECT.MW();
    const float cy = mRECT.B - 8.f;
    const float w = 9.f;
    const float h = mOpen ? -5.f : 5.f;

    g.DrawLine(colour, cx - w, cy - h * 0.5f, cx, cy + h * 0.5f, nullptr, 1.8f);
    g.DrawLine(colour, cx, cy + h * 0.5f, cx + w, cy - h * 0.5f, nullptr, 1.8f);
  }

  void OnMouseDown(float x, float y, const IMouseMod& mod) override
  {
    mOpen = !mOpen;

    if (mOnToggle)
      mOnToggle(mOpen);

    SetDirty(false);
  }

  void OnMouseOver(float x, float y, const IMouseMod& mod) override
  {
    mHovered = true;
    SetDirty(false);
  }

  void OnMouseOut() override
  {
    mHovered = false;
    SetDirty(false);
  }

private:
  Toggle mOnToggle;
  bool mOpen = false;
  bool mHovered = false;
};

} // namespace nr::rig
