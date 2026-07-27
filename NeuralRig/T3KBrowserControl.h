#pragma once

#include <string>
#include <vector>

#include "IControls.h"

#include "Colors.h"
#include "net/BrowserController.h"

using namespace iplug;
using namespace igraphics;

/**
    Full-screen overlay for browsing TONE3000 captures.

    Layout is a paged list rather than a scrolling one, because IGraphics has no
    scroll view and the API is paged anyway -- prev/next maps straight onto
    `page`, so there is nothing to reconcile.

    The control owns no networking. It reads a snapshot from BrowserController
    on each refresh and calls back into it for actions, which keeps all the
    threading in one place and leaves this file about drawing.
*/
class T3KBrowserPageControl : public IContainerBase
{
public:
  /// Rows shown at once. Matches the page size asked of the API so a page maps
  /// one-to-one onto a screen.
  static constexpr int kRowsPerPage = 8;

  /// Fired when the user picks a capture. The host wires this to load the
  /// downloaded file into a chain slot.
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

  /// Preselects which chain slot a picked capture lands in. Called when the
  /// user opens the browser from a specific slot's globe, so the thing they
  /// clicked is the thing that gets filled.
  void SetTargetSlot(int slot)
  {
    mTargetSlot = std::max(0, std::min(slot, 3));

    if (mSlotButton != nullptr)
      RefreshFilterLabels();
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
    const auto content = GetRECT().GetPadded(-12.f);
    const auto titleText = IText(20.f, EAlign::Near, COLOR_WHITE);
    const auto bodyText = IText(14.f, EAlign::Near, PluginColors::HELP_TEXT);

    auto header = content.GetFromTop(28.f);
    AddChildControl(new IVLabelControl(header.GetFromLeft(260.f), "TONE3000", mStyle));

    // Close
    AddChildControl(new IVButtonControl(
      header.GetFromRight(60.f), [this](IControl*) { Hide(true); }, "CLOSE", mStyle));

    // Connect / sign out sits next to close so the session state is always in
    // the same place regardless of what else is on screen.
    mConnectButton = new IVButtonControl(
      header.GetFromRight(200.f).GetFromLeft(130.f),
      [this](IControl*) {
        if (mController.GetSnapshot().status == nr::net::BrowserController::Status::SignedOut)
          mController.SignIn();
        else
          mController.SignOut();
      },
      "CONNECT", mStyle);
    AddChildControl(mConnectButton);

    auto searchRow = content.GetFromTop(66.f).GetFromBottom(30.f);

    mSearchButton = new IVButtonControl(
      searchRow.GetFromLeft(240.f),
      [this](IControl* pCaller) {
        // Prompt for the query rather than hosting a live text field: a
        // permanently focused field would swallow keystrokes the host wants.
        GetUI()->CreateTextEntry(*pCaller, IText(14.f), pCaller->GetRECT(), mSearchText.c_str());
      },
      "Search captures...", mStyle);
    AddChildControl(mSearchButton);

    mGearButton = new IVButtonControl(
      searchRow.GetFromLeft(360.f).GetFromRight(110.f),
      [this](IControl*) {
        // Cycle rather than open a menu: five options is short enough that a
        // click-through is faster than a popup.
        mGearIndex = (mGearIndex + 1) % static_cast<int>(mGearChoices.size());
        RefreshFilterLabels();
        RunSearch(1);
      },
      "All gear", mStyle);
    AddChildControl(mGearButton);

    mSortButton = new IVButtonControl(
      searchRow.GetFromLeft(480.f).GetFromRight(110.f),
      [this](IControl*) {
        mSortIndex = (mSortIndex + 1) % static_cast<int>(mSortChoices.size());
        RefreshFilterLabels();
        RunSearch(1);
      },
      "Best match", mStyle);
    AddChildControl(mSortButton);

    AddChildControl(new IVButtonControl(
      searchRow.GetFromRight(80.f), [this](IControl*) { RunSearch(1); }, "GO", mStyle));

    // Which chain slot a picked capture lands in.
    auto slotRow = content.GetFromTop(102.f).GetFromBottom(26.f);
    AddChildControl(new IVLabelControl(slotRow.GetFromLeft(90.f), "Load into", mStyle));

    mSlotButton = new IVButtonControl(
      slotRow.GetFromLeft(180.f).GetFromRight(80.f),
      [this](IControl*) {
        mTargetSlot = (mTargetSlot + 1) % 4;
        RefreshFilterLabels();
      },
      "Slot 1", mStyle);
    AddChildControl(mSlotButton);

    mStatusLabel = new ITextControl(slotRow.GetReducedFromLeft(200.f), "", bodyText);
    AddChildControl(mStatusLabel);

    // Results
    auto list = content.GetReducedFromTop(110.f).GetReducedFromBottom(36.f);
    const auto rowHeight = list.H() / static_cast<float>(kRowsPerPage);

    for (int i = 0; i < kRowsPerPage; i++)
    {
      const auto rowBounds = list.GetFromTop(rowHeight).GetVShifted(rowHeight * static_cast<float>(i));

      auto* row = new IVButtonControl(
        rowBounds.GetPadded(-1.f), [this, i](IControl*) { PickRow(i); }, "", mStyle);

      mRowButtons.push_back(row);
      AddChildControl(row);
    }

    // Paging
    auto footer = content.GetFromBottom(28.f);

    AddChildControl(new IVButtonControl(
      footer.GetFromLeft(90.f), [this](IControl*) { RunSearch(mCurrentPage - 1); }, "PREV", mStyle));

    AddChildControl(new IVButtonControl(
      footer.GetFromLeft(190.f).GetFromRight(90.f), [this](IControl*) { RunSearch(mCurrentPage + 1); }, "NEXT",
      mStyle));

    mPageLabel = new ITextControl(footer.GetReducedFromLeft(200.f), "", bodyText);
    AddChildControl(mPageLabel);

    RefreshFilterLabels();
    Refresh();
  }

  void OnTextEntryCompletion(const char* str, int valIdx) override
  {
    if (str != nullptr)
    {
      mSearchText = str;
      mSearchButton->SetLabelStr(mSearchText.empty() ? "Search captures..." : mSearchText.c_str());
      RunSearch(1);
    }
  }

  /// Pulls the latest state from the controller. Called from the plugin's idle
  /// callback, and only when the controller reports something changed.
  void Refresh()
  {
    const auto snapshot = mController.GetSnapshot();
    mCurrentPage = snapshot.page;

    const bool signedOut = snapshot.status == nr::net::BrowserController::Status::SignedOut;
    mConnectButton->SetLabelStr(signedOut ? "CONNECT" : "SIGN OUT");

    mStatusLabel->SetStr(snapshot.message.c_str());

    for (int i = 0; i < kRowsPerPage; i++)
    {
      if (i < static_cast<int>(snapshot.rows.size()))
      {
        const auto& row = snapshot.rows[static_cast<size_t>(i)];

        std::string label = row.title;
        if (!row.author.empty())
          label += "   by " + row.author;
        if (!row.gear.empty())
          label += "   [" + row.gear + "]";

        mRowButtons[static_cast<size_t>(i)]->SetLabelStr(label.c_str());
        mRowButtons[static_cast<size_t>(i)]->SetDisabled(false);
      }
      else
      {
        mRowButtons[static_cast<size_t>(i)]->SetLabelStr("");
        mRowButtons[static_cast<size_t>(i)]->SetDisabled(true);
      }
    }

    if (snapshot.totalPages > 0)
    {
      const auto pageText = "Page " + std::to_string(snapshot.page) + " of "
                            + std::to_string(snapshot.totalPages) + "   (" + std::to_string(snapshot.total)
                            + " captures)";
      mPageLabel->SetStr(pageText.c_str());
    }
    else
    {
      mPageLabel->SetStr("");
    }

    SetDirty(false);
  }

private:
  void RefreshFilterLabels()
  {
    mGearButton->SetLabelStr(mGearLabels[static_cast<size_t>(mGearIndex)]);
    mSortButton->SetLabelStr(mSortLabels[static_cast<size_t>(mSortIndex)]);
    mSlotButton->SetLabelStr(("Slot " + std::to_string(mTargetSlot + 1)).c_str());
  }

  void RunSearch(int page)
  {
    if (page < 1)
      page = 1;

    mController.Search(mSearchText, mGearChoices[static_cast<size_t>(mGearIndex)],
                       mSortChoices[static_cast<size_t>(mSortIndex)], 0, page);
  }

  void PickRow(int rowIndex)
  {
    const int slot = mTargetSlot;

    mController.DownloadRow(rowIndex, [this, slot](bool success, std::string pathOrError) {
      // Fires on a worker thread. Hand the path to the host, which marshals to
      // the message thread before touching the DSP.
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

  // Empty gear means "no filter"; the label says so.
  int mGearIndex = 0;
  const std::vector<std::string> mGearChoices{"", "amp", "full-rig", "pedal", "outboard"};
  const std::vector<const char*> mGearLabels{"All gear", "Amp", "Full rig", "Pedal", "Outboard"};

  int mSortIndex = 0;
  const std::vector<std::string> mSortChoices{"best-match", "trending", "newest", "downloads-all-time"};
  const std::vector<const char*> mSortLabels{"Best match", "Trending", "Newest", "Downloads"};
};
