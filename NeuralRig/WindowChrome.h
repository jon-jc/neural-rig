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

/// Detaches the native menu bar from the window.
///
/// The MENU resource itself has to stay: removing it from the dialog template
/// makes iPlug2's standalone exit at startup. Detaching it afterwards keeps the
/// app happy and still gets rid of the unthemed strip, and File and Options are
/// drawn in the header instead.
inline void RemoveNativeMenu(IGraphics* pGraphics)
{
#if defined(APP_API) && defined(OS_WIN)
  if (HWND window = TopLevelWindow(pGraphics))
  {
    if (GetMenu(window) != nullptr)
    {
      RECT frame{};
      GetWindowRect(window, &frame);

      SetMenu(window, nullptr);

      // SWP_FRAMECHANGED alone leaves the window at its old height, still
      // reserving the row the menu occupied, so take that row back explicitly.
      //
      // Measured from the window rather than from IGraphics: WindowWidth() is
      // the logical size times the draw scale, which is not the platform size
      // when the two disagree -- using it shrank the window to 747x587.
      const int width = frame.right - frame.left;
      const int height = (frame.bottom - frame.top) - GetSystemMetrics(SM_CYMENU);

      SetWindowPos(window, nullptr, 0, 0, width, height,
                   SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
    }
  }
#endif
}

/// Centres the window on the screen's work area.
///
/// Against the work area rather than the full screen, so the taskbar cannot
/// push the bottom of the window off the display. Called once, after the
/// startup collapse: the window is created at its open height and centred by
/// the dialog style, so shrinking it afterwards left it sitting off-centre.
inline void CentreWindow(IGraphics* pGraphics)
{
#if defined(APP_API) && defined(OS_WIN)
  HWND window = TopLevelWindow(pGraphics);

  if (window == nullptr)
    return;

  RECT frame{};
  GetWindowRect(window, &frame);

  RECT work{};
  if (!SystemParametersInfo(SPI_GETWORKAREA, 0, &work, 0))
    return;

  const int width = frame.right - frame.left;
  const int height = frame.bottom - frame.top;

  SetWindowPos(window, nullptr, work.left + ((work.right - work.left) - width) / 2,
               work.top + ((work.bottom - work.top) - height) / 2, 0, 0,
               SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
#endif
}

/// Nudges the window back inside the work area if a resize pushed it out.
///
/// Centring happens once, at the collapsed height. Opening the browser then
/// grows the window downwards from that position, which on a shorter screen
/// puts the bottom of the catalogue below the taskbar -- the part the user
/// opened it to see. Moving rather than resizing, so the layout is untouched.
inline void KeepOnScreen(IGraphics* pGraphics)
{
#if defined(APP_API) && defined(OS_WIN)
  HWND window = TopLevelWindow(pGraphics);

  if (window == nullptr)
    return;

  RECT frame{};
  GetWindowRect(window, &frame);

  RECT work{};
  if (!SystemParametersInfo(SPI_GETWORKAREA, 0, &work, 0))
    return;

  const int width = frame.right - frame.left;
  const int height = frame.bottom - frame.top;

  int x = frame.left;
  int y = frame.top;

  if (frame.bottom > work.bottom)
    y = work.bottom - height;

  if (frame.right > work.right)
    x = work.right - width;

  // Clamp the top-left last: on a screen smaller than the window, keeping the
  // top visible matters more than the bottom, since that is where the controls
  // are.
  x = x < work.left ? work.left : x;
  y = y < work.top ? work.top : y;

  if (x != frame.left || y != frame.top)
    SetWindowPos(window, nullptr, x, y, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
#endif
}

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
    // Tracked here rather than handed to the window manager via
    // WM_NCLBUTTONDOWN/HTCAPTION. That is the usual trick, but it relies on the
    // window having a caption to pretend the click landed in, and this one has
    // none -- so it silently did nothing.
    HWND window = TopLevelWindow(GetUI());

    if (window == nullptr)
      return;

    RECT frame{};
    GetWindowRect(window, &frame);
    GetCursorPos(&mAnchor);

    mOrigin = {frame.left, frame.top};
    mDragging = true;
#endif
  }

  void OnMouseDrag(float x, float y, float dX, float dY, const IMouseMod& mod) override
  {
#if defined(APP_API) && defined(OS_WIN)
    if (!mDragging)
      return;

    HWND window = TopLevelWindow(GetUI());

    if (window == nullptr)
      return;

    // Against the screen cursor rather than accumulated deltas: the control
    // moves with the window, so its own coordinates shift under the pointer
    // and deltas would compound.
    POINT now{};
    GetCursorPos(&now);

    SetWindowPos(window, nullptr, mOrigin.x + (now.x - mAnchor.x), mOrigin.y + (now.y - mAnchor.y), 0, 0,
                 SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
#endif
  }

  void OnMouseUp(float x, float y, const IMouseMod& mod) override { mDragging = false; }

private:
#if defined(APP_API) && defined(OS_WIN)
  POINT mAnchor{};
  POINT mOrigin{};
#endif
  bool mDragging = false;
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
      // SC_MINIMIZE through the system menu rather than ShowWindow: on a
        // borderless popup ShowWindow(SW_MINIMIZE) took the window down
        // entirely instead of minimising it.
      case WindowButton::Minimise: PostMessage(window, WM_SYSCOMMAND, SC_MINIMIZE, 0); break;
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

/// The standalone's File and Options menus, drawn in the header instead of in
/// an OS menu bar. The bar was the last piece of unthemed chrome, and it sat
/// outside the plugin's own surface.
class WindowMenuControl : public IControl
{
public:
  WindowMenuControl(const IRECT& bounds, const char* label, bool isFileMenu)
  : IControl(bounds)
  , mLabel(label)
  , mIsFileMenu(isFileMenu)
  {
    mIgnoreMouse = !kOwnsWindow;
  }

  void Draw(IGraphics& g) override
  {
    if (!kOwnsWindow)
      return;

    if (mHovered)
      g.FillRoundRect(PluginColors::AMBER.WithOpacity(0.12f), mRECT, 4.f);

    const IText text(12.f, mHovered ? PluginColors::INK : PluginColors::INK_MUTED, nullptr, EAlign::Center,
                     EVAlign::Middle);
    g.DrawText(text, mLabel, mRECT);
  }

  void OnMouseDown(float x, float y, const IMouseMod& mod) override
  {
    mMenu.Clear();

    if (mIsFileMenu)
    {
      mMenu.AddItem("Audio Settings...");
      mMenu.AddItem("Quit");
    }
    else
    {
      mMenu.AddItem("About NeuralRig");
    }

    GetUI()->CreatePopupMenu(*this, mMenu, mRECT);
  }

  void OnPopupMenuSelection(IPopupMenu* pMenu, int valIdx) override
  {
#if defined(APP_API) && defined(OS_WIN)
    if (pMenu == nullptr || pMenu->GetChosenItemIdx() < 0)
      return;

    HWND window = TopLevelWindow(GetUI());

    if (window == nullptr)
      return;

    // The command ids the standalone's own handler already understands, so
    // these open exactly the dialogs the menu bar used to.
    constexpr WPARAM kAbout = 40005;
    constexpr WPARAM kPreferences = 40006;
    constexpr WPARAM kQuit = 40007;

    const int chosen = pMenu->GetChosenItemIdx();

    if (!mIsFileMenu)
      PostMessage(window, WM_COMMAND, kAbout, 0);
    else
      PostMessage(window, WM_COMMAND, chosen == 0 ? kPreferences : kQuit, 0);
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
  const char* mLabel;
  bool mIsFileMenu;
  bool mHovered = false;
  IPopupMenu mMenu;
};

} // namespace nr::shell
