#pragma once

#include <algorithm>
#include <string>
#include <vector>

#include "IControls.h"

#include "Colors.h"
#include "net/BrowserController.h"

using namespace iplug;
using namespace igraphics;

/**
    Full-window modal for browsing TONE3000 captures.

    Draws its own opaque backdrop. IContainerBase paints nothing by default, so
    without this the panel is transparent and its children float over whatever
    is behind them.

    Layout is a paged list rather than a scrolling one: IGraphics has no scroll
    view, and the API is paged anyway, so prev/next maps straight onto `page`.

    Owns no networking. It reads a snapshot from BrowserController and calls
    back into it, which keeps threading in one place and leaves this file about
    drawing.
*/
class T3KBrowserPageControl : public IContainerBase
{
public:
  static constexpr int kRowsPerPage = 7;

  using LoadIntoSlotFunc = std::function<void(int slot, const char* filePath)>;

  T3KBrowserPageControl(const IRECT& bounds, nr::net::BrowserController& controller, const IVStyle& style)
  : IContainerBase(bounds)
  , mController(controller)
  , mStyle(style)
  {
    mIgnoreMouse = false;
    mHide = true;
  }

  void SetLoadIntoSlotFunc(LoadIntoSlotFunc func) { mLoadIntoSlot = std::move(func); }

  /// Preselects the slot a picked capture lands in, so opening the browser from
  /// a slot's globe fills that slot.
  void SetTargetSlot(int slot)
  {
    mTargetSlot = std::max(0, std::min(slot, 3));

    if (mSlotButton != nullptr)
      RefreshChrome();
  }

  /// Opaque backdrop. Without it the modal is see-through and unreadable.
  void Draw(IGraphics& g) override
  {
    g.FillRect(PluginColors::NAM_1.WithOpacity(0.97f), mRECT);
    g.FillRect(PluginColors::NAM_2, mRECT.GetFromTop(kHeaderHeight));
    g.DrawLine(PluginColors::NAM_THEMECOLOR, mRECT.L, mRECT.T + kHeaderHeight, mRECT.R,
               mRECT.T + kHeaderHeight, nullptr, 2.f);

    IContainerBase::Draw(g);
  }

  bool OnKeyDown(float x, float y, const IKeyPress& key) override
  {
    if (key.VK == kVK_ESCAPE)
    {
      Hide(true);
      return true;
    }

    return false;
  }

  void OnAttached() override
  {
    const auto all = GetRECT();

    // Every rect is carved off a running remainder, so sections cannot overlap
    // however the window is sized.
    auto remaining = all;

    // --- Header -------------------------------------------------------------
    auto header = remaining.GetFromTop(kHeaderHeight);
    remaining = remaining.GetReducedFromTop(kHeaderHeight);

    const auto headerPad = header.GetPadded(-kPad);
    AddChildControl(new ITextControl(headerPad.GetFromLeft(240.f), "TONE3000",
                                     IText(24.f, EAlign::Near, COLOR_WHITE)));

    auto headerButtons = headerPad.GetFromRight(260.f);
    AddChildControl(new IVButtonControl(
      headerButtons.GetFromRight(90.f), [this](IControl*) { Hide(true); }, "CLOSE", mStyle));

    mConnectButton = new IVButtonControl(
      headerButtons.GetFromLeft(150.f),
      [this](IControl*) {
        if (mController.GetSnapshot().status == nr::net::BrowserController::Status::SignedOut)
          mController.SignIn();
        else
          mController.SignOut();
      },
      "CONNECT", mStyle);
    AddChildControl(mConnectButton);

    remaining = remaining.GetPadded(-kPad);

    // --- Search row ---------------------------------------------------------
    auto searchRow = remaining.GetFromTop(kRowHeight);
    remaining = remaining.GetReducedFromTop(kRowHeight + kPad);

    mSearchButton = new IVButtonControl(
      searchRow.GetReducedFromRight(340.f),
      [this](IControl* pCaller) {
        GetUI()->CreateTextEntry(*pCaller, IText(15.f), pCaller->GetRECT(), mSearchText.c_str());
      },
      "Search captures...", mStyle);
    AddChildControl(mSearchButton);

    auto searchControls = searchRow.GetFromRight(330.f);
    mGearButton = new IVButtonControl(
      searchControls.GetFromLeft(105.f),
      [this](IControl*) {
        mGearIndex = (mGearIndex + 1) % static_cast<int>(mGearChoices.size());
        RefreshChrome();
        RunSearch(1);
      },
      "All gear", mStyle);
    AddChildControl(mGearButton);

    mSortButton = new IVButtonControl(
      searchControls.GetFromLeft(220.f).GetFromRight(105.f),
      [this](IControl*) {
        mSortIndex = (mSortIndex + 1) % static_cast<int>(mSortChoices.size());
        RefreshChrome();
        RunSearch(1);
      },
      "Best match", mStyle);
    AddChildControl(mSortButton);

    AddChildControl(new IVButtonControl(
      searchControls.GetFromRight(100.f), [this](IControl*) { RunSearch(1); }, "SEARCH", mStyle));

    // --- Status -------------------------------------------------------------
    auto statusRow = remaining.GetFromTop(24.f);
    remaining = remaining.GetReducedFromTop(24.f + kPad);

    mStatusLabel = new ITextControl(statusRow, "", IText(14.f, EAlign::Near, PluginColors::HELP_TEXT));
    AddChildControl(mStatusLabel);

    // --- Footer, carved off the bottom before the list takes the rest -------
    auto footer = remaining.GetFromBottom(kRowHeight);
    remaining = remaining.GetReducedFromBottom(kRowHeight + kPad);

    AddChildControl(new IVButtonControl(
      footer.GetFromLeft(90.f), [this](IControl*) { RunSearch(mCurrentPage - 1); }, "< PREV", mStyle));
    AddChildControl(new IVButtonControl(
      footer.GetFromLeft(190.f).GetFromRight(90.f), [this](IControl*) { RunSearch(mCurrentPage + 1); },
      "NEXT >", mStyle));

    mPageLabel = new ITextControl(footer.GetReducedFromLeft(200.f).GetReducedFromRight(220.f), "",
                                  IText(14.f, EAlign::Center, PluginColors::HELP_TEXT));
    AddChildControl(mPageLabel);

    auto slotPicker = footer.GetFromRight(210.f);
    AddChildControl(new ITextControl(slotPicker.GetFromLeft(90.f), "Load into",
                                     IText(14.f, EAlign::Far, PluginColors::HELP_TEXT)));
    mSlotButton = new IVButtonControl(
      slotPicker.GetFromRight(110.f),
      [this](IControl*) {
        mTargetSlot = (mTargetSlot + 1) % 4;
        RefreshChrome();
      },
      "Slot 1", mStyle);
    AddChildControl(mSlotButton);

    // --- Results ------------------------------------------------------------
    const auto rowPitch = remaining.H() / static_cast<float>(kRowsPerPage);

    for (int i = 0; i < kRowsPerPage; i++)
    {
      const auto rowBounds =
        remaining.GetFromTop(rowPitch).GetVShifted(rowPitch * static_cast<float>(i)).GetPadded(-2.f);

      auto* row = new IVButtonControl(
        rowBounds, [this, i](IControl*) { PickRow(i); }, "", mStyle, true, false, EVShape::Rectangle);

      mRowButtons.push_back(row);
      AddChildControl(row);
    }

    RefreshChrome();
    Refresh();
  }

  void OnTextEntryCompletion(const char* str, int valIdx) override
  {
    if (str != nullptr)
    {
      mSearchText = str;
      RefreshChrome();
      RunSearch(1);
    }
  }

  /// Pulls the latest state from the controller. Driven by the plugin's idle
  /// callback, and only when the controller reports a change.
  void Refresh()
  {
    const auto snapshot = mController.GetSnapshot();
    mCurrentPage = snapshot.page;

    const bool signedOut = snapshot.status == nr::net::BrowserController::Status::SignedOut;
    mConnectButton->SetLabelStr(signedOut ? "CONNECT" : "SIGN OUT");
    mStatusLabel->SetStr(snapshot.message.c_str());

    for (int i = 0; i < kRowsPerPage; i++)
    {
      auto* row = mRowButtons[static_cast<size_t>(i)];

      if (i < static_cast<int>(snapshot.rows.size()))
      {
        const auto& data = snapshot.rows[static_cast<size_t>(i)];

        std::string label = data.title;
        if (!data.author.empty())
          label += "   \xE2\x80\xA2   " + data.author;
        if (!data.gear.empty())
          label += "   \xE2\x80\xA2   " + data.gear;
        if (data.downloads > 0)
          label += "   \xE2\x80\xA2   " + std::to_string(data.downloads) + " downloads";

        row->SetLabelStr(label.c_str());
        row->SetDisabled(false);
        row->Hide(false);
      }
      else
      {
        // Hidden rather than shown empty, so a short page does not leave a
        // column of blank buttons.
        row->Hide(true);
      }
    }

    if (snapshot.totalPages > 0)
      mPageLabel->SetStr(("Page " + std::to_string(snapshot.page) + " / " + std::to_string(snapshot.totalPages)
                          + "   \xE2\x80\xA2   " + std::to_string(snapshot.total) + " captures")
                           .c_str());
    else
      mPageLabel->SetStr("");

    SetDirty(false);
  }

private:
  static constexpr float kHeaderHeight = 56.f;
  static constexpr float kRowHeight = 34.f;
  static constexpr float kPad = 14.f;

  void RefreshChrome()
  {
    mGearButton->SetLabelStr(mGearLabels[static_cast<size_t>(mGearIndex)]);
    mSortButton->SetLabelStr(mSortLabels[static_cast<size_t>(mSortIndex)]);
    mSlotButton->SetLabelStr(("Slot " + std::to_string(mTargetSlot + 1)).c_str());
    mSearchButton->SetLabelStr(mSearchText.empty() ? "Search captures..." : mSearchText.c_str());
  }

  void RunSearch(int page)
  {
    mController.Search(mSearchText, mGearChoices[static_cast<size_t>(mGearIndex)],
                       mSortChoices[static_cast<size_t>(mSortIndex)], 0, std::max(1, page));
  }

  void PickRow(int rowIndex)
  {
    const int slot = mTargetSlot;

    mController.DownloadRow(rowIndex, [this, slot](bool success, std::string pathOrError) {
      // Worker thread. The host marshals to the message thread before touching
      // the DSP.
      if (success && mLoadIntoSlot)
        mLoadIntoSlot(slot, pathOrError.c_str());
    });
  }

  nr::net::BrowserController& mController;
  IVStyle mStyle;
  LoadIntoSlotFunc mLoadIntoSlot;

  IVButtonControl* mConnectButton = nullptr;
  IVButtonControl* mSearchButton = nullptr;
  IVButtonControl* mGearButton = nullptr;
  IVButtonControl* mSortButton = nullptr;
  IVButtonControl* mSlotButton = nullptr;
  ITextControl* mStatusLabel = nullptr;
  ITextControl* mPageLabel = nullptr;
  std::vector<IVButtonControl*> mRowButtons;

  std::string mSearchText;
  int mCurrentPage = 1;
  int mTargetSlot = 0;

  int mGearIndex = 0;
  const std::vector<std::string> mGearChoices{"", "amp", "full-rig", "pedal", "outboard"};
  const std::vector<const char*> mGearLabels{"All gear", "Amp", "Full rig", "Pedal", "Outboard"};

  int mSortIndex = 0;
  const std::vector<std::string> mSortChoices{"best-match", "trending", "newest", "downloads-all-time"};
  const std::vector<const char*> mSortLabels{"Best match", "Trending", "Newest", "Downloads"};
};
