#pragma once

/**
    Title-bar replacements for a borderless window.

    The standalone has no OS caption any more, which removes two things the
    frame used to provide for free: somewhere to grab the window, and a way to
    close it. Both are drawn by the plugin instead.

    Only the standalone gets them. In a plugin the host owns the window, so a
    close button would be wrong and a drag handle would fight the host's own.
*/

#include <functional>

#include "Colors.h"
#include "IControl.h"

#if defined(APP_API) && defined(OS_WIN)
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

using namespace iplug;
using namespace iplug::igraphics;

namespace nr::shell
{

/// True when this build owns its window and should draw its own chrome.
inline constexpr bool kOwnsWindow =
#if defined(APP_API)
  true;
#else
  false;
#endif

#if defined(APP_API) && defined(OS_WIN)
/// The top-level window behind an IGraphics context.
inline HWND TopLevelWindow(IGraphics* pGraphics)
{
  if (pGraphics == nullptr)
    return nullptr;

  auto* child = static_cast<HWND>(pGraphics->GetWindow());
  return child != nullptr ? GetAncestor(child, GA_ROOT) : nullptr;
}
#endif

/// Drags the window when the empty part of the header is dragged, standing in
/// for the caption bar that is no longer there.
class WindowDragControl : public IControl
{
public:
  explicit WindowDragControl(const IRECT& bounds)
  : IControl(bounds)
  {
    mIgnoreMouse = !kOwnsWindow;
  }

  /// Draws nothing: this is a grab area over the header, not a visible control.
  void Draw(IGraphics& g) override {}

  void OnMouseDown(float x, float y, const IMouseMod& mod) override
  {
#if defined(APP_API) && defined(OS_WIN)
    // Hand the drag to the window manager rather than tracking the mouse
    // ourselves: it gets snapping, multi-monitor and DPI changes right, and we
    // would not.
    if (HWND window = TopLevelWindow(GetUI()))
    {
      ReleaseCapture();
      SendMessage(window, WM_NCLBUTTONDOWN, HTCAPTION, 0);
    }
#endif
  }
};

/// What a caption-bar button does. There is no maximise: the layout holds one
/// size, and offering a maximise that cannot be honoured is worse than not
/// offering it.
enum class WindowButton
{
  Minimise,
  Close,
};

/// A caption-bar button. Glyphs are drawn rather than loaded as SVGs so they
/// match the rest of the plugin's drawn chrome at any scale.
class WindowButtonControl : public IControl
{
public:
  WindowButtonControl(const IRECT& bounds, WindowButton button)
  : IControl(bounds)
  , mButton(button)
  {
    mIgnoreMouse = !kOwnsWindow;
  }

  void Draw(IGraphics& g) override
  {
    if (!kOwnsWindow)
      return;

    // Close goes red on hover, the way every title bar does; minimise stays
    // neutral, because it is not destructive.
    const bool destructive = mButton == WindowButton::Close;
    const auto highlight = destructive ? PluginColors::METER_CLIP : PluginColors::AMBER;
    const auto colour = mHovered ? (destructive ? PluginColors::METER_CLIP : PluginColors::INK) : PluginColors::INK_MUTED;

    if (mHovered)
      g.FillRoundRect(highlight.WithOpacity(0.16f), mRECT, 4.f);

    const auto glyph = mRECT.GetPadded(-9.f);

    switch (mButton)
    {
      case WindowButton::Close:
        g.DrawLine(colour, glyph.L, glyph.T, glyph.R, glyph.B, nullptr, 1.6f);
        g.DrawLine(colour, glyph.R, glyph.T, glyph.L, glyph.B, nullptr, 1.6f);
        break;

      case WindowButton::Minimise:
        g.DrawLine(colour, glyph.L, glyph.MH(), glyph.R, glyph.MH(), nullptr, 1.6f);
        break;
    }
  }

  void OnMouseDown(float x, float y, const IMouseMod& mod) override
  {
#if defined(APP_API) && defined(OS_WIN)
    HWND window = TopLevelWindow(GetUI());

    if (window == nullptr)
      return;

    switch (mButton)
    {
      // Posted rather than sent: this runs from a mouse handler, and closing
      // the window tears down the graphics context underneath us.
      case WindowButton::Close: PostMessage(window, WM_CLOSE, 0, 0); break;
      case WindowButton::Minimise: ShowWindow(window, SW_MINIMIZE); break;
    }
#endif
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
  WindowButton mButton;
  bool mHovered = false;
};

} // namespace nr::shell
