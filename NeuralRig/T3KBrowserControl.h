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
    TONE3000, embedded.

    A real browser inside the plugin rather than a list widget built on top of
    the API. The user gets tone3000.com itself -- artwork, demos, tags, the
    search they already know -- and picking a tone hands the id straight back to
    us to download and load. That is the flow TONE3000 designed the
    `prompt=select_tone` OAuth mode for.

    Windows uses WebView2 (Edge Chromium), macOS WKWebView. The WebView2
    *runtime* must be present, which it is on Windows 11 and anywhere Edge is
    installed; the loader is linked statically so nothing ships beside the
    plugin.
*/
class T3KBrowserPageControl : public IContainerBase
{
public:
  using LoadIntoSlotFunc = std::function<void(int slot, const char* filePath)>;

  T3KBrowserPageControl(const IRECT& bounds, nr::net::BrowserController& controller, const IVStyle& style)
  : IContainerBase(bounds)
  , mController(controller)
  , mStyle(style)
  {
    mIgnoreMouse = false;
  }

  void SetLoadIntoSlotFunc(LoadIntoSlotFunc func) { mLoadIntoSlot = std::move(func); }

  void SetTargetSlot(int slot)
  {
    mTargetSlot = std::max(0, std::min(slot, 3));

    if (mSlotButton != nullptr)
      mSlotButton->SetLabelStr(("Slot " + std::to_string(mTargetSlot + 1)).c_str());
  }

  /// Fully opaque. A translucent modal over a busy amp panel is unreadable.
  void Draw(IGraphics& g) override
  {
    g.FillRect(PluginColors::NAM_1, mRECT);
    g.FillRect(PluginColors::NAM_2, mRECT.GetFromTop(kHeaderHeight));
    g.DrawLine(PluginColors::NAM_THEMECOLOR, mRECT.L, mRECT.T + kHeaderHeight, mRECT.R,
               mRECT.T + kHeaderHeight, nullptr, 2.f);

    IContainerBase::Draw(g);
  }

  bool OnKeyDown(float x, float y, const IKeyPress& key) override
  {
    if (key.VK == kVK_ESCAPE)
    {
      Close();
      return true;
    }

    return false;
  }

  /// Collapses the browser out of sight, or brings it back.
  ///
  /// IControl::Hide() cannot help here: the web view is a native OS window
  /// layered over the IGraphics surface, so it keeps drawing regardless of what
  /// the control thinks its visibility is. Moving it outside the window is the
  /// lever the platform actually gives us.
  void SetCollapsed(bool collapsed)
  {
    if (mCollapsed == collapsed || mWebView == nullptr)
      return;

    mCollapsed = collapsed;

    const auto parked = IRECT(mContentBounds.L, mContentBounds.B + 4000.f, mContentBounds.R,
                              mContentBounds.B + 4000.f + mContentBounds.H());

    mWebView->SetTargetAndDrawRECTs(collapsed ? parked : mContentBounds);
    mWebView->OnResize();

    if (mCollapseButton != nullptr)
      mCollapseButton->SetLabelStr(collapsed ? "SHOW" : "HIDE");

    SetDirty(false);
  }

  bool IsCollapsed() const { return mCollapsed; }

  void OnAttached() override
  {
    auto remaining = GetRECT();

    auto header = remaining.GetFromTop(kHeaderHeight);
    remaining = remaining.GetReducedFromTop(kHeaderHeight);
    mContentBounds = remaining;

    const auto headerPad = header.GetPadded(-12.f);

    AddChildControl(new ITextControl(headerPad.GetFromLeft(200.f), "TONE3000",
                                     IText(22.f, EAlign::Near, COLOR_WHITE)));

    mStatusLabel = new ITextControl(headerPad.GetReducedFromLeft(210.f).GetReducedFromRight(330.f), "",
                                    IText(13.f, EAlign::Near, PluginColors::HELP_TEXT));
    AddChildControl(mStatusLabel);

    auto buttons = headerPad.GetFromRight(320.f);

    AddChildControl(new IVButtonControl(
      buttons.GetFromLeft(70.f), [this](IControl*) { NavigateHome(); }, "HOME", mStyle));

    AddChildControl(new ITextControl(buttons.GetFromLeft(150.f).GetFromRight(72.f), "Load into",
                                     IText(13.f, EAlign::Far, PluginColors::HELP_TEXT)));

    mSlotButton = new IVButtonControl(
      buttons.GetFromLeft(250.f).GetFromRight(94.f),
      [this](IControl*) {
        mTargetSlot = (mTargetSlot + 1) % 4;
        mSlotButton->SetLabelStr(("Slot " + std::to_string(mTargetSlot + 1)).c_str());
      },
      "Slot 1", mStyle);
    AddChildControl(mSlotButton);

    AddChildControl(new IVButtonControl(
      buttons.GetFromRight(140.f).GetFromLeft(68.f), [this](IControl*) { NavigateHome(); }, "RELOAD", mStyle));

    mCollapseButton = new IVButtonControl(
      buttons.GetFromRight(64.f), [this](IControl*) { SetCollapsed(!mCollapsed); }, "HIDE", mStyle);
    AddChildControl(mCollapseButton);

    // The browser fills everything below the header.
    mWebView = new IWebViewControl(
      mContentBounds, true, [this](IWebViewControl* pCaller) { OnWebViewReady(pCaller); },
      [this](IWebViewControl*, const char* json) { (void)json; });

    AddChildControl(mWebView);
  }

  /// Starts the loopback listener and points the browser at TONE3000's own
  /// selection flow, so the user browses the real site.
  void Open()
  {
    mRedirectUri = mLoopback.Start();

    if (mRedirectUri.empty())
    {
      SetStatus("Could not open a local port for sign-in");
      return;
    }

    mPkce = nr::net::CreatePkcePair();
    SetStatus("Browse TONE3000 and choose a capture");

    if (mWebView != nullptr)
      mWebView->LoadURL(SelectToneUrl().c_str());

    StartWaitingForSelection();
  }

  void Close()
  {
    mLoopback.Stop();
    Hide(true);
  }

  /// Called once a capture has actually been staged. TONE3000 leaves the view
  /// on a "you can close this tab" page, which is a dead end inside a plugin
  /// that has no tabs -- send it back to browsing so the next pick is one
  /// click away.
  ///
  /// The PKCE pair is deliberately reused: the listener is still waiting on the
  /// same state value, and regenerating it here would make the next selection
  /// fail its own security check.
  void OnCaptureLoaded(const char* slotDescription)
  {
    if (mStatusLabel != nullptr)
      mStatusLabel->SetStr((std::string("Loaded into ") + slotDescription + " \xE2\x80\xA2 pick another").c_str());

    NavigateHome();
  }

  /// Polled from the plugin's idle callback so status text tracks the
  /// controller without the worker touching IGraphics.
  void Refresh()
  {
    const auto snapshot = mController.GetSnapshot();

    if (!snapshot.message.empty())
      mStatusLabel->SetStr(snapshot.message.c_str());

    SetDirty(false);
  }

private:
  static constexpr float kHeaderHeight = NR_BROWSER_HEADER_HEIGHT;
  static constexpr int kSelectionTimeoutMs = 600000; // ten minutes of browsing

  void OnWebViewReady(IWebViewControl* pCaller)
  {
    // IWebViewControl::OnAttached places the native view at unscaled
    // coordinates, while every later update multiplies by the draw scale. On a
    // scaled display those disagree and the view lands outside its column,
    // over the rig. Forcing a resize once the view exists reconciles them.
    pCaller->OnResize();

    if (!mRedirectUri.empty())
      pCaller->LoadURL(SelectToneUrl().c_str());
  }

  std::string SelectToneUrl() const
  {
    // prompt=select_tone puts TONE3000 into its "pick one and hand it back"
    // mode, which is exactly what an in-plugin browser wants.
    return mController.BuildSelectToneUrl(mPkce, mRedirectUri);
  }

  void NavigateHome()
  {
    if (mWebView != nullptr)
      mWebView->LoadURL(SelectToneUrl().c_str());
  }

  void SetStatus(const std::string& text)
  {
    if (mStatusLabel != nullptr)
      mStatusLabel->SetStr(text.c_str());
  }

  /// Waits for TONE3000 to redirect to the loopback listener with a tone id,
  /// then downloads that tone into the chosen slot.
  void StartWaitingForSelection()
  {
    const int slot = mTargetSlot;

    mController.AwaitToneSelection(mLoopback, mPkce, mRedirectUri, kSelectionTimeoutMs,
                                   [this, slot](bool success, std::string pathOrError) {
                                     if (success && mLoadIntoSlot)
                                       mLoadIntoSlot(slot, pathOrError.c_str());
                                   });
  }

  nr::net::BrowserController& mController;
  IVStyle mStyle;
  LoadIntoSlotFunc mLoadIntoSlot;

  IWebViewControl* mWebView = nullptr;
  IVButtonControl* mSlotButton = nullptr;
  IVButtonControl* mCollapseButton = nullptr;
  ITextControl* mStatusLabel = nullptr;

  IRECT mContentBounds;
  bool mCollapsed = false;

  nr::net::LoopbackServer mLoopback;
  nr::net::PkcePair mPkce;
  std::string mRedirectUri;

  int mTargetSlot = 0;
};
