#pragma once

#include <algorithm>
#include <string>

#include "IControls.h"
#include "IWebViewControl.h"

#include "Colors.h"
#include "net/BrowserController.h"
#include "net/LoopbackServer.h"
#include "net/Tone3000Client.h"

using namespace iplug;
using namespace igraphics;

/**
    TONE3000 as a floating window inside the plugin.

    A real browser rather than a list widget rebuilt on the API: the user gets
    tone3000.com itself, and picking a tone hands the id straight back through
    the `prompt=select_tone` OAuth mode so it downloads and loads into a slot.

    It floats, and can be moved, resized and closed. That is not decoration.
    The web view is a native OS window layered above the IGraphics surface, so
    it will always cover whatever is beneath it -- the platform offers no way to
    draw over it and no way to hide it. Docking it into a fixed region either
    steals space permanently or is too small to browse in. Letting the user push
    it out of the way, size it to taste, and close it entirely is the only
    arrangement where the constraint stops mattering.
*/
class T3KBrowserPageControl : public IContainerBase
{
public:
  using LoadIntoSlotFunc = std::function<void(int slot, const char* filePath)>;

  T3KBrowserPageControl(const IRECT& editorBounds, nr::net::BrowserController& controller, const IVStyle& style)
  : IContainerBase(DefaultFrame(editorBounds))
  , mEditorBounds(editorBounds)
  , mController(controller)
  , mStyle(style)
  {
    mIgnoreMouse = false;
    mHide = true;
  }

  void SetLoadIntoSlotFunc(LoadIntoSlotFunc func) { mLoadIntoSlot = std::move(func); }

  void SetTargetSlot(int slot)
  {
    mTargetSlot = std::max(0, std::min(slot, 3));

    if (mSlotButton != nullptr)
      mSlotButton->SetLabelStr(("Slot " + std::to_string(mTargetSlot + 1)).c_str());
  }

  void Draw(IGraphics& g) override
  {
    const auto frame = GetRECT();

    // A drop shadow reads the panel as floating above the rig rather than
    // punched into it.
    g.FillRoundRect(IColor(120, 0, 0, 0), frame.GetTranslated(4.f, 5.f), 8.f);
    g.FillRoundRect(PluginColors::NAM_1, frame, 8.f);
    g.FillRect(PluginColors::NAM_2, frame.GetFromTop(kTitleBarHeight));
    g.DrawRoundRect(PluginColors::NAM_THEMECOLOR, frame, 8.f, nullptr, 1.5f);

    // While the view is parked mid-gesture the panel is empty, so say what is
    // going on rather than showing a blank hole.
    if (mDraggingFrame || mResizing)
      g.DrawText(IText(15.f, PluginColors::HELP_TEXT), mResizing ? "Resizing..." : "Moving...",
                 ContentRect());

    IContainerBase::Draw(g);

    // Resize grip: three diagonal ticks in the bottom-right, the usual idiom.
    const auto grip = GripRect();
    for (int i = 1; i <= 3; i++)
    {
      const auto inset = static_cast<float>(i) * 4.f;
      g.DrawLine(PluginColors::HELP_TEXT, grip.R - inset, grip.B - 2.f, grip.R - 2.f, grip.B - inset, nullptr,
                 1.f);
    }
  }

  // --- Move and resize ------------------------------------------------------

  void OnMouseDown(float x, float y, const IMouseMod& mod) override
  {
    mDraggingFrame = GetRECT().GetFromTop(kTitleBarHeight).Contains(x, y);
    mResizing = GripRect().Contains(x, y);

    // Park the native view for the duration of the gesture. Repositioning it
    // every drag frame leaves smears: the OS window repaints on its own
    // schedule, out of step with the IGraphics surface underneath, so the two
    // disagree about where it is for a frame at a time. Dragging an empty frame
    // and putting the view back on release avoids the whole problem.
    if (mDraggingFrame || mResizing)
      PlaceWebView(true);
  }

  void OnMouseUp(float x, float y, const IMouseMod& mod) override
  {
    const bool wasGesturing = mDraggingFrame || mResizing;

    mDraggingFrame = false;
    mResizing = false;

    if (wasGesturing)
      PlaceWebView();
  }

  void OnMouseDrag(float x, float y, float dX, float dY, const IMouseMod& mod) override
  {
    if (!mDraggingFrame && !mResizing)
      return;

    auto frame = GetRECT();

    if (mResizing)
    {
      frame.R = std::min(mEditorBounds.R - 4.f, std::max(frame.L + kMinWidth, frame.R + dX));
      frame.B = std::min(mEditorBounds.B - 4.f, std::max(frame.T + kMinHeight, frame.B + dY));
    }
    else
    {
      // Keep the title bar reachable: a panel dragged off-screen cannot be
      // dragged back.
      const auto width = frame.W();
      const auto height = frame.H();

      frame.L = std::min(mEditorBounds.R - 80.f, std::max(mEditorBounds.L - width + 80.f, frame.L + dX));
      frame.T = std::min(mEditorBounds.B - kTitleBarHeight, std::max(mEditorBounds.T, frame.T + dY));
      frame.R = frame.L + width;
      frame.B = frame.T + height;
    }

    Relayout(frame);
  }

  void OnMouseOver(float x, float y, const IMouseMod& mod) override
  {
    GetUI()->SetMouseCursor(GripRect().Contains(x, y) ? ECursor::SIZENWSE : ECursor::ARROW);
  }

  bool OnKeyDown(float x, float y, const IKeyPress& key) override
  {
    if (key.VK == kVK_ESCAPE)
    {
      CloseBrowser();
      return true;
    }

    return false;
  }

  void OnAttached() override
  {
    BuildChrome();

    mWebView = new IWebViewControl(
      ContentRect(), true, [this](IWebViewControl* pCaller) { OnWebViewReady(pCaller); },
      [this](IWebViewControl*, const char* json) { (void)json; });

    AddChildControl(mWebView);
  }

  void OnRescale() override { PlaceWebView(); }

  /// Shows the panel and points it at TONE3000's selection flow.
  void OpenBrowser()
  {
    if (!mHide)
      return;

    Hide(false);

    if (mRedirectUri.empty())
    {
      mRedirectUri = mLoopback.Start();

      if (mRedirectUri.empty())
      {
        SetStatus("Could not open a local port for sign-in");
        return;
      }

      mPkce = nr::net::CreatePkcePair();
      StartWaitingForSelection();
    }

    SetStatus("Browse TONE3000 and choose a capture");
    NavigateHome();
    PlaceWebView();
  }

  /// Hides the panel. The native view is given a zero rect, which is the only
  /// way to make it stop drawing.
  void CloseBrowser()
  {
    Hide(true);
    PlaceWebView();
    SetDirty(false);
  }

  bool IsOpen() const { return !mHide; }

  /// Called once a capture has been staged. TONE3000 leaves the view on a "you
  /// can close this tab" page, a dead end in a plugin with no tabs, so send it
  /// back to browsing.
  void OnCaptureLoaded(const char* slotDescription)
  {
    SetStatus(std::string("Loaded into ") + slotDescription + " \xE2\x80\xA2 pick another");
    NavigateHome();
  }

  void Refresh()
  {
    const auto snapshot = mController.GetSnapshot();

    if (!snapshot.message.empty() && mStatusLabel != nullptr)
      mStatusLabel->SetStr(snapshot.message.c_str());

    SetDirty(false);
  }

private:
  static constexpr float kTitleBarHeight = 34.f;
  static constexpr float kGripSize = 18.f;
  static constexpr float kMinWidth = 480.f;
  static constexpr float kMinHeight = 320.f;
  static constexpr int kSelectionTimeoutMs = 600000;

  /// Opens large enough to browse in, inset from the editor edges so the frame
  /// reads as a window.
  static IRECT DefaultFrame(const IRECT& editor)
  {
    const auto width = std::min(920.f, editor.W() - 80.f);
    const auto height = std::min(640.f, editor.H() - 80.f);
    return editor.GetCentredInside(width, height);
  }

  IRECT ContentRect() const { return GetRECT().GetReducedFromTop(kTitleBarHeight).GetPadded(-3.f); }

  IRECT GripRect() const { return GetRECT().GetFromBRHC(kGripSize, kGripSize); }

  /// Moves the frame and its chrome. The native view deliberately stays parked
  /// until the gesture ends; see OnMouseDown.
  void Relayout(const IRECT& frame)
  {
    SetTargetAndDrawRECTs(frame);
    BuildChrome();
    GetUI()->SetAllControlsDirty();
  }

  /// (Re)places the title bar controls. Called on construction and on every
  /// move or resize, so the chrome follows the frame.
  void BuildChrome()
  {
    auto titleBar = GetRECT().GetFromTop(kTitleBarHeight).GetPadded(-6.f);

    if (mTitleLabel == nullptr)
    {
      mTitleLabel = new ITextControl(titleBar, "TONE3000", IText(17.f, EAlign::Near, COLOR_WHITE));
      AddChildControl(mTitleLabel);

      mStatusLabel = new ITextControl(titleBar, "", IText(12.f, EAlign::Near, PluginColors::HELP_TEXT));
      AddChildControl(mStatusLabel);

      mHomeButton = new IVButtonControl(
        titleBar, [this](IControl*) { NavigateHome(); }, "HOME", mStyle);
      AddChildControl(mHomeButton);

      mSlotButton = new IVButtonControl(
        titleBar,
        [this](IControl*) {
          mTargetSlot = (mTargetSlot + 1) % 4;
          mSlotButton->SetLabelStr(("Slot " + std::to_string(mTargetSlot + 1)).c_str());
        },
        "Slot 1", mStyle);
      AddChildControl(mSlotButton);

      mCloseButton = new IVButtonControl(
        titleBar, [this](IControl*) { CloseBrowser(); }, "CLOSE", mStyle);
      AddChildControl(mCloseButton);
    }

    auto buttons = titleBar.GetFromRight(250.f);
    mCloseButton->SetTargetAndDrawRECTs(buttons.GetFromRight(64.f));
    mSlotButton->SetTargetAndDrawRECTs(buttons.GetFromRight(140.f).GetFromLeft(70.f));
    mHomeButton->SetTargetAndDrawRECTs(buttons.GetFromLeft(64.f));

    mTitleLabel->SetTargetAndDrawRECTs(titleBar.GetFromLeft(110.f));
    mStatusLabel->SetTargetAndDrawRECTs(titleBar.GetReducedFromLeft(116.f).GetReducedFromRight(258.f));
  }

  /// Positions the native view, bypassing IWebViewControl::UpdateWebViewBounds.
  ///
  /// That helper is wrong on any scaled display, in two compounding ways.
  /// IWebView::SetWebViewBounds already multiplies x/y/w/h by the scale it is
  /// handed, but UpdateWebViewBounds pre-multiplies as well -- and it uses
  /// GetDrawScale() rather than GetTotalScale(), which is what carries the OS
  /// display scaling. The result lands at 1/scale of where it belongs.
  /// @param park  collapse the view to nothing without closing the panel, used
  ///              while the frame is being dragged or resized
  void PlaceWebView(bool park = false)
  {
    if (mWebView == nullptr || GetUI() == nullptr)
      return;

    const auto scale = GetUI()->GetTotalScale();
    const auto bounds = (mHide || park) ? IRECT(0.f, 0.f, 0.f, 0.f) : ContentRect();

    mWebView->SetTargetAndDrawRECTs(bounds);
    mWebView->SetWebViewBounds(bounds.L, bounds.T, bounds.W(), bounds.H(), scale);
  }

  void OnWebViewReady(IWebViewControl* pCaller)
  {
    PlaceWebView();

    if (!mRedirectUri.empty() && !mHide)
      pCaller->LoadURL(SelectToneUrl().c_str());
  }

  std::string SelectToneUrl() const { return mController.BuildSelectToneUrl(mPkce, mRedirectUri); }

  void NavigateHome()
  {
    if (mWebView != nullptr && !mRedirectUri.empty())
      mWebView->LoadURL(SelectToneUrl().c_str());
  }

  void SetStatus(const std::string& text)
  {
    if (mStatusLabel != nullptr)
      mStatusLabel->SetStr(text.c_str());
  }

  void StartWaitingForSelection()
  {
    mController.AwaitToneSelection(mLoopback, mPkce, mRedirectUri, kSelectionTimeoutMs,
                                   [this](bool success, std::string pathOrError) {
                                     if (success && mLoadIntoSlot)
                                       mLoadIntoSlot(mTargetSlot, pathOrError.c_str());
                                   });
  }

  IRECT mEditorBounds;
  nr::net::BrowserController& mController;
  IVStyle mStyle;
  LoadIntoSlotFunc mLoadIntoSlot;

  IWebViewControl* mWebView = nullptr;
  ITextControl* mTitleLabel = nullptr;
  ITextControl* mStatusLabel = nullptr;
  IVButtonControl* mHomeButton = nullptr;
  IVButtonControl* mSlotButton = nullptr;
  IVButtonControl* mCloseButton = nullptr;

  nr::net::LoopbackServer mLoopback;
  nr::net::PkcePair mPkce;
  std::string mRedirectUri;

  int mTargetSlot = 0;
  bool mDraggingFrame = false;
  bool mResizing = false;
};
