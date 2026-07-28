#pragma once

/**
    The TONE3000 browser, drawn natively.

    This replaces the embedded web view. The plugin used to host a real browser
    pointed at tone3000.com, which worked but behaved like a browser: it floated
    over the rig, it had its own scrollbars and chrome, and every interaction
    was a page load. Everything here comes from the public API instead, so the
    catalogue is just another list in the plugin's own UI.

    One control draws the whole panel rather than a container holding a control
    per card. Cards are pure output plus a click target, so children would buy
    nothing and cost a rebuild of the control tree on every page change; hit
    testing an index from the y coordinate is simpler and keeps scrolling
    smooth.

    No artwork is fetched. Type is carried by colour -- a badge pill and a left
    edge per card -- which means no image decoding, no texture uploads and no
    cache to invalidate, and the list scrolls without any async state.

    Nothing here blocks. BrowserController owns the worker threads; this polls
    its snapshot from the editor's idle callback and redraws when it changes.
*/

#include <algorithm>
#include <cmath>
#include <cstring>
#include <functional>
#include <string>

#include "Colors.h"
#include "IControl.h"
#include "net/BrowserController.h"

using namespace iplug;
using namespace iplug::igraphics;

namespace nr::browser
{

/// Display label for a gear type. The API's vocabulary is lowercase and
/// hyphenated; this is what the badge shows.
inline const char* GearLabel(const std::string& gear, const std::string& format)
{
  // An IR is a format rather than a gear, but it is the distinction a user
  // cares about most when scanning a list, so it wins over the gear name.
  if (format == "ir")
    return "IR";

  if (gear == "amp")
    return "AMP";
  if (gear == "amp-cab")
    return "AMP & CAB";
  if (gear == "pedal")
    return "PEDAL";
  if (gear == "cab")
    return "CAB";
  if (gear == "outboard")
    return "OUTBOARD";
  if (gear == "space")
    return "SPACE";
  if (gear == "experimental")
    return "EXPERIMENTAL";

  return "CAPTURE";
}

/**
    One entry in the filter menu.

    This used to be "All gear" prepended to GearOptions(), with the selected
    index mapped back by subtracting one. Index arithmetic against a menu built
    somewhere else is exactly the kind of thing that silently goes wrong -- and
    did -- so the label and the values it sends now travel together and the
    chosen index is only ever used to look up a row.

    It also makes room for IR, which has no gear value at all: an impulse
    response is a *format*, so filtering for one means format=ir with the gear
    filter left open. Expressing that as a gear was impossible before.
*/
struct GearFilter
{
  const char* label;
  const char* gears;  ///< underscore-joined gear values, empty for any
  const char* format; ///< format value, empty for any
};

/// Architecture choices, with the API's default spelled out.
///
/// Omitting the architecture parameter is documented as "A1 + Custom (legacy
/// default; excludes A2)". Every search this plugin made omitted it, so A2
/// captures -- where most current TONE3000 content lives -- were invisible
/// everywhere. That is why a cab search that the website answers with a full
/// page came back nearly empty here.
struct ArchitectureFilter
{
  const char* label;
  int value; ///< 0 omits the parameter, which is the legacy A1 + Custom default
};

inline const std::vector<ArchitectureFilter>& ArchitectureFilters()
{
  static const std::vector<ArchitectureFilter> filters = {
    {"A2 models", 2},
    {"A1 + Custom", 0},
    {"A1 only", 1},
  };

  return filters;
}

inline const std::vector<GearFilter>& GearFilters()
{
  static const std::vector<GearFilter> filters = {
    {"All gear", "", ""},
    {"Amps", "amp", ""},
    {"Amp & Cab", "amp-cab", ""},
    {"Pedals", "pedal", ""},
    {"Cabs", "cab", ""},
    {"IRs", "", "ir"},
    {"Outboard", "outboard", ""},
    {"Space", "space", ""},
    {"Experimental", "experimental", ""},
  };

  return filters;
}

/// Badge colour for a gear type.
inline IColor GearColor(const std::string& gear, const std::string& format)
{
  if (format == "ir")
    return PluginColors::GEAR_CAB;

  if (gear == "amp")
    return PluginColors::GEAR_AMP;
  if (gear == "amp-cab")
    return PluginColors::GEAR_AMP_CAB;
  if (gear == "pedal")
    return PluginColors::GEAR_PEDAL;
  if (gear == "cab")
    return PluginColors::GEAR_CAB;
  if (gear == "outboard")
    return PluginColors::GEAR_OUTBOARD;
  if (gear == "space")
    return PluginColors::GEAR_SPACE;

  return PluginColors::GEAR_OTHER;
}

/**
    The panel.

    @param onLoad  fired when a card is clicked, with both the row index the
                   controller needs to download and the row itself, since a
                   Local row is already on disk and must not be downloaded
                   again. The host wires this to the slot the user is filling;
                   the panel itself knows nothing about the rig.
*/
class T3KBrowserPanel : public IControl
{
public:
  using LoadHandler = std::function<void(int rowIndex, const nr::net::BrowserController::Row& row)>;

  /// Fired when the user picks one variant of a multi-model tone.
  using ChoiceHandler =
    std::function<void(const nr::net::BrowserController::ModelChoice&, const nr::net::BrowserController::Row&)>;

  void SetChoiceHandler(ChoiceHandler onChoice) { mOnChoice = std::move(onChoice); }

  T3KBrowserPanel(const IRECT& bounds, nr::net::BrowserController& controller, LoadHandler onLoad)
  : IControl(bounds)
  , mController(controller)
  , mOnLoad(std::move(onLoad))
  {
    mIgnoreMouse = false;
  }

  /// Opens the catalogue pre-filtered to a slot's gear types.
  ///
  /// The API joins multi-select gears with underscores, and the controller
  /// takes a single gear string which it then joins -- joining a one-element
  /// list yields that element unchanged, so passing the already-joined form
  /// through works and keeps the controller's signature simple.
  void FocusGears(const std::string& joinedGears, const char* displayLabel, const std::string& format = {})
  {
    mGear = joinedGears;
    mFormat = format;
    mGearLabel = displayLabel != nullptr ? displayLabel : "All gear";
    mScrollOffset = 0.f;
    RunSearch(1);
  }

  void OnResize() override { LayOut(); }

  void OnAttached() override { LayOut(); }

  /// Polled from the editor's idle callback. Redraws only when the controller
  /// says something changed, so an idle browser costs nothing.
  void Poll()
  {
    // Startup work happens here, not in OnAttached. OnAttached runs while the
    // control tree is still being assembled, and Begin/ShowTab each spawn a
    // worker thread that publishes back into state the UI is concurrently
    // reading -- an intermittent startup crash rather than a reliable one,
    // which is exactly how it presented. By the first idle tick the editor is
    // fully built.
    if (!mStarted)
    {
      mStarted = true;
      mController.Begin();
      mController.ShowTab(nr::net::BrowserController::Tab::Browse);
      return;
    }

    if (mController.ConsumeDirty())
    {
      mSnapshot = mController.GetSnapshot();
      ClampScroll();
      SetDirty(false);

      // The variants we asked for have landed. Opening the menu here rather
      // than from the click keeps the fetch off the UI thread, which is the
      // same reason every other network call in this panel is a request now
      // and a result later.
      if (mAwaitingChoicesForRow >= 0 && mSnapshot.modelChoicesRow == mAwaitingChoicesForRow
          && !mSnapshot.modelChoices.empty())
      {
        // Disarm as the menu opens, not when something is chosen. Clearing it
        // on selection meant dismissing the menu left the request armed, and
        // the next state change -- switching tabs, a search landing -- saw the
        // same condition still true and reopened it.
        mChoiceRow = mAwaitingChoicesForRow;
        mAwaitingChoicesForRow = -1;
        ShowModelMenu();
      }
    }
  }

  void Draw(IGraphics& g) override
  {
    g.FillRect(PluginColors::CHASSIS, mRECT);
    g.FillRect(PluginColors::PANEL_HI, mRECT.GetFromTop(1.f));

    DrawHeader(g);
    DrawTabs(g);
    DrawFilters(g);
    DrawSearch(g);
    DrawList(g);
    DrawFooter(g);
  }

  void OnMouseDown(float x, float y, const IMouseMod& mod) override
  {
    using Tab = nr::net::BrowserController::Tab;

    // Identity strip: connect or sign out.
    if (mAuthRect.Contains(x, y))
    {
      if (mSnapshot.status == nr::net::BrowserController::Status::SignedOut)
        mController.SignIn();
      else
        mController.SignOut();

      return;
    }

    for (int i = 0; i < kNumTabs; i++)
    {
      if (mTabRects[i].Contains(x, y))
      {
        // Whatever variant request was in flight belongs to a row that is about
        // to be replaced -- and the menu it opened is still on screen. Now that
        // menus are drawn by IGraphics rather than the OS, clicking a tab does
        // not dismiss one: the click goes to the tab, and the menu simply stays
        // up over the new results. It has to be collapsed explicitly.
        DismissMenus();
        mScrollOffset = 0.f;
        mController.ShowTab(static_cast<Tab>(i));
        SetDirty(false);
        return;
      }
    }

    if (mGearRect.Contains(x, y))
    {
      ShowGearMenu();
      return;
    }

    if (mArchRect.Contains(x, y))
    {
      ShowArchitectureMenu();
      return;
    }

    if (mSortRect.Contains(x, y))
    {
      ShowSortMenu();
      return;
    }

    if (mSearchRect.Contains(x, y))
    {
      const IText entry = IText(14.f, PluginColors::INK, nullptr, EAlign::Near, EVAlign::Middle)
                            .WithTEColors(PluginColors::WELL, PluginColors::INK);
      // kNoValIdx: see Presets.h. Any other index makes IGraphics call
      // GetParam on this control before dispatching, which reads out of bounds.
      GetUI()->CreateTextEntry(*this, entry, mSearchRect, mQuery.c_str(), kNoValIdx);
      return;
    }

    if (mPrevRect.Contains(x, y) && mSnapshot.page > 1)
    {
      GoToPage(mSnapshot.page - 1);
      return;
    }

    if (mNextRect.Contains(x, y) && mSnapshot.page < mSnapshot.totalPages)
    {
      GoToPage(mSnapshot.page + 1);
      return;
    }

    if (!mListRect.Contains(x, y))
      return;

    const int row = RowAt(y);

    if (row < 0 || row >= static_cast<int>(mSnapshot.rows.size()))
      return;

    // The star sits inside the card, so it has to be tested before the card.
    if (StarRectFor(row).Contains(x, y))
    {
      mController.ToggleFavourite(row);
      return;
    }

    const auto& entry = mSnapshot.rows[static_cast<size_t>(row)];

    // A tone often ships several models -- the same capture at different sizes,
    // or several settings of one pedal. Picking one for the user throws that
    // away, so ask when there is a choice to make. One model, or a local file,
    // loads straight away: a menu with a single entry is just a extra click.
    if (entry.modelsCount > 1 && entry.localPath.empty())
    {
      mAwaitingChoicesForRow = row;
      mController.FetchModelChoices(row);
      SetDirty(false);
      return;
    }

    if (mOnLoad)
      mOnLoad(row, entry);
  }

  void OnMouseWheel(float x, float y, const IMouseMod& mod, float delta) override
  {
    if (!mListRect.Contains(x, y))
      return;

    mScrollOffset -= delta * kScrollStep;
    ClampScroll();
    SetDirty(false);
  }

  void OnMouseOver(float x, float y, const IMouseMod& mod) override
  {
    const int row = mListRect.Contains(x, y) ? RowAt(y) : -1;

    if (row != mHoverRow)
    {
      mHoverRow = row;
      SetDirty(false);
    }
  }

  void OnMouseOut() override
  {
    if (mHoverRow != -1)
    {
      mHoverRow = -1;
      SetDirty(false);
    }
  }

  void OnTextEntryCompletion(const char* txt, int valIdx) override
  {
    if (txt == nullptr)
      return;

    mQuery = txt;
    mScrollOffset = 0.f;
    RunSearch(1);
  }

  void OnPopupMenuSelection(IPopupMenu* pMenu, int valIdx) override
  {
    if (pMenu == nullptr || pMenu->GetChosenItemIdx() < 0)
      return;

    const int chosen = pMenu->GetChosenItemIdx();

    if (valIdx == kModelValIdx)
    {
      const int row = mChoiceRow;
      mChoiceRow = -1;

      if (chosen < 0 || chosen >= static_cast<int>(mSnapshot.modelChoices.size()) || row < 0
          || row >= static_cast<int>(mSnapshot.rows.size()))
        return;

      if (mOnChoice)
        mOnChoice(mSnapshot.modelChoices[static_cast<size_t>(chosen)], mSnapshot.rows[static_cast<size_t>(row)]);

      return;
    }

    if (valIdx == kGearValIdx)
    {
      const auto& filters = GearFilters();

      if (chosen < 0 || chosen >= static_cast<int>(filters.size()))
        return;

      const auto& filter = filters[static_cast<size_t>(chosen)];
      mGear = filter.gears;
      mFormat = filter.format;
      mGearLabel = filter.label;
    }
    else if (valIdx == kArchValIdx)
    {
      const auto& architectures = ArchitectureFilters();

      if (chosen < 0 || chosen >= static_cast<int>(architectures.size()))
        return;

      mArchitecture = architectures[static_cast<size_t>(chosen)].value;
      mArchLabel = architectures[static_cast<size_t>(chosen)].label;
    }
    else if (valIdx == kSortValIdx)
    {
      const auto sorts = nr::net::SortOptions();
      mSort = sorts[static_cast<size_t>(chosen)];
    }

    mScrollOffset = 0.f;
    RunSearch(1);
  }

private:
  static constexpr int kNumTabs = 5;
  static constexpr int kGearValIdx = 101;
  static constexpr int kSortValIdx = 102;
  static constexpr int kArchValIdx = 103;
  static constexpr int kModelValIdx = 104;
  static constexpr float kCardHeight = 64.f;
  static constexpr float kScrollStep = 42.f;

  // --- Layout ---------------------------------------------------------------

  void LayOut()
  {
    auto area = mRECT.GetPadded(-14.f);

    mHeaderRect = area.GetFromTop(26.f);
    mAuthRect = mHeaderRect.GetFromRight(96.f);

    area = area.GetReducedFromTop(34.f);
    mTabsRect = area.GetFromTop(26.f);

    // Tabs take the left two thirds; the filters share the right third with
    // the same baseline so the row reads as one strip.
    const auto tabStrip = mTabsRect.GetFromLeft(mTabsRect.W() * 0.58f);
    const float tabWidth = tabStrip.W() / static_cast<float>(kNumTabs);

    for (int i = 0; i < kNumTabs; i++)
      mTabRects[i] = tabStrip.GetFromLeft(tabWidth).GetHShifted(tabWidth * static_cast<float>(i)).GetPadded(-2.f);

    const auto filterStrip = mTabsRect.GetFromRight(mTabsRect.W() * 0.40f);
    const float pill = filterStrip.W() / 3.f;
    mGearRect = filterStrip.GetFromLeft(pill).GetPadded(-2.f);
    mArchRect = filterStrip.GetFromLeft(pill).GetHShifted(pill).GetPadded(-2.f);
    mSortRect = filterStrip.GetFromRight(pill).GetPadded(-2.f);

    area = area.GetReducedFromTop(34.f);
    mSearchRect = area.GetFromTop(30.f);

    area = area.GetReducedFromTop(38.f);
    mFooterRect = area.GetFromBottom(24.f);
    mListRect = area.GetReducedFromBottom(30.f);

    mPrevRect = mFooterRect.GetFromRight(64.f).GetHShifted(-70.f);
    mNextRect = mFooterRect.GetFromRight(64.f);
  }

  int RowAt(float y) const
  {
    return static_cast<int>((y - mListRect.T + mScrollOffset) / kCardHeight);
  }

  IRECT CardRectFor(int row) const
  {
    const float top = mListRect.T + static_cast<float>(row) * kCardHeight - mScrollOffset;
    return IRECT(mListRect.L, top, mListRect.R, top + kCardHeight).GetPadded(-3.f);
  }

  IRECT StarRectFor(int row) const { return CardRectFor(row).GetFromRight(34.f).GetPadded(-9.f); }

  float ContentHeight() const { return static_cast<float>(mSnapshot.rows.size()) * kCardHeight; }

  void ClampScroll()
  {
    const float maxScroll = std::max(0.f, ContentHeight() - mListRect.H());
    mScrollOffset = std::max(0.f, std::min(mScrollOffset, maxScroll));
  }

  // --- Actions --------------------------------------------------------------

  void RunSearch(int page)
  {
    DismissMenus();

    // Search sets the Browse tab itself. Calling ShowTab first would route back
    // into a search with the previous query, claim the one allowed in-flight
    // operation, and get this one dropped.
    mController.Search(mQuery, mGear, mSort, mArchitecture, page, mFormat);
    SetDirty(false);
  }

  void GoToPage(int page)
  {
    mScrollOffset = 0.f;

    if (mSnapshot.tab == nr::net::BrowserController::Tab::Browse)
      mController.Search(mQuery, mGear, mSort, 0, page);
    else
      mController.ShowTab(mSnapshot.tab, page);

    SetDirty(false);
  }

  void ShowGearMenu()
  {
    ReadyMenus();
    mGearMenu.Clear();

    for (const auto& filter : GearFilters())
      mGearMenu.AddItem(filter.label);

    GetUI()->CreatePopupMenu(*this, mGearMenu, mGearRect, kGearValIdx);
  }

  void ShowArchitectureMenu()
  {
    ReadyMenus();
    mArchMenu.Clear();

    for (const auto& architecture : ArchitectureFilters())
      mArchMenu.AddItem(architecture.label);

    GetUI()->CreatePopupMenu(*this, mArchMenu, mArchRect, kArchValIdx);
  }

  /// Collapses any open popup menu. Needed wherever the panel changes what it
  /// is showing underneath one.
  void DismissMenus()
  {
    mAwaitingChoicesForRow = -1;
    mChoiceRow = -1;

    // Hide rather than CollapseEverything, which is private. CreatePopupMenu
    // shows the control again on the next open, so hiding is enough to get it
    // off screen without leaving it unusable.
    if (auto* ui = GetUI())
      if (auto* menu = ui->GetPopupMenuControl())
        menu->Hide(true);
  }

  /// Un-hides the shared menu control before opening anything on it, since
  /// DismissMenus hides it.
  void ReadyMenus()
  {
    if (auto* ui = GetUI())
      if (auto* menu = ui->GetPopupMenuControl())
        menu->Hide(false);
  }

  void ShowModelMenu()
  {
    ReadyMenus();
    mModelMenu.Clear();

    for (const auto& choice : mSnapshot.modelChoices)
    {
      // Name and size together: two variants often share a name and differ
      // only in size, and vice versa.
      std::string label = choice.name.empty() ? "Model " + std::to_string(choice.id) : choice.name;

      if (!choice.size.empty())
        label += "  (" + choice.size + ")";

      mModelMenu.AddItem(label.c_str());
    }

    GetUI()->CreatePopupMenu(*this, mModelMenu, mListRect.GetFromTop(1.f).GetVShifted(RowTop(mChoiceRow)),
                             kModelValIdx);
  }

  float RowTop(int row) const { return static_cast<float>(row) * kCardHeight - mScrollOffset; }

  void ShowSortMenu()
  {
    ReadyMenus();
    mSortMenu.Clear();

    for (const auto& sort : nr::net::SortOptions())
      mSortMenu.AddItem(SortLabel(sort));

    GetUI()->CreatePopupMenu(*this, mSortMenu, mSortRect, kSortValIdx);
  }

  static const char* SortLabel(const std::string& sort)
  {
    if (sort == "best-match")
      return "Best match";
    if (sort == "trending")
      return "Trending";
    if (sort == "newest")
      return "Newest";
    if (sort == "oldest")
      return "Oldest";
    if (sort == "downloads-all-time")
      return "Most downloaded";

    return sort.c_str();
  }

  // --- Drawing --------------------------------------------------------------

  void DrawHeader(IGraphics& g)
  {
    const IText wordmark(18.f, PluginColors::AMBER, nullptr, EAlign::Near, EVAlign::Middle);
    g.DrawText(wordmark, "TONE3000", mHeaderRect);

    const IText caption(13.f, PluginColors::INK_MUTED, nullptr, EAlign::Near, EVAlign::Middle);

    const bool signedOut = mSnapshot.status == nr::net::BrowserController::Status::SignedOut;

    std::string identity;
    if (signedOut)
      identity = "Not connected";
    else if (!mSnapshot.username.empty())
      identity = "Signed in as " + mSnapshot.username;
    else
      identity = "Signed in";

    g.DrawText(caption, identity.c_str(), mHeaderRect.GetHShifted(96.f));

    // Status line, right of centre so it does not collide with the wordmark.
    if (!mSnapshot.message.empty())
    {
      const IText status(12.f, PluginColors::INK_DIM, nullptr, EAlign::Far, EVAlign::Middle);
      g.DrawText(status, mSnapshot.message.c_str(), mHeaderRect.GetReducedFromRight(104.f));
    }

    DrawPill(g, mAuthRect, signedOut ? "Connect" : "Log Out", signedOut ? PluginColors::AMBER : PluginColors::INK_MUTED,
             signedOut);
  }

  void DrawTabs(IGraphics& g)
  {
    static const char* kLabels[kNumTabs] = {"Browse", "Favorites", "Created", "Recent", "Local"};

    for (int i = 0; i < kNumTabs; i++)
    {
      const bool active = static_cast<int>(mSnapshot.tab) == i;
      DrawPill(g, mTabRects[i], kLabels[i], active ? PluginColors::AMBER : PluginColors::INK_MUTED, active);
    }
  }

  void DrawFilters(IGraphics& g)
  {
    DrawPill(g, mGearRect, mGearLabel.c_str(), PluginColors::INK_MUTED, false, true);
    DrawPill(g, mArchRect, mArchLabel.c_str(), PluginColors::INK_MUTED, false, true);
    DrawPill(g, mSortRect, SortLabel(mSort), PluginColors::INK_MUTED, false, true);
  }

  void DrawSearch(IGraphics& g)
  {
    g.FillRoundRect(PluginColors::WELL, mSearchRect, 5.f);
    g.DrawRoundRect(PluginColors::PANEL_HI, mSearchRect, 5.f);

    const bool empty = mQuery.empty();
    const IText text(14.f, empty ? PluginColors::INK_DIM : PluginColors::INK, nullptr, EAlign::Near, EVAlign::Middle);

    g.DrawText(text, empty ? "Search tones, pedals and IRs" : mQuery.c_str(),
               mSearchRect.GetReducedFromLeft(12.f).GetReducedFromRight(12.f));
  }

  void DrawList(IGraphics& g)
  {
    g.FillRoundRect(PluginColors::WELL, mListRect, 6.f);

    if (mSnapshot.rows.empty())
    {
      const IText empty(14.f, PluginColors::INK_DIM, nullptr, EAlign::Center, EVAlign::Middle);
      g.DrawText(empty,
                 mSnapshot.status == nr::net::BrowserController::Status::SignedOut
                   ? "Connect your TONE3000 account to browse captures"
                   : "Nothing to show",
                 mListRect);
      return;
    }

    // Clip so cards scrolled past the edges do not paint over the chrome.
    g.PathClipRegion(mListRect);

    const int first = std::max(0, static_cast<int>(mScrollOffset / kCardHeight));
    const int last = std::min(static_cast<int>(mSnapshot.rows.size()),
                              first + static_cast<int>(mListRect.H() / kCardHeight) + 2);

    for (int i = first; i < last; i++)
      DrawCard(g, i);

    g.PathClipRegion(IRECT());
  }

  void DrawCard(IGraphics& g, int row)
  {
    const auto& entry = mSnapshot.rows[static_cast<size_t>(row)];
    const auto card = CardRectFor(row);
    const auto accent = GearColor(entry.gear, entry.format);

    g.FillRoundRect(mHoverRow == row ? PluginColors::PANEL_HI : PluginColors::PANEL, card, 5.f);

    // Left edge in the type colour: readable as a group even before the badge
    // text resolves at small sizes.
    g.FillRoundRect(accent, card.GetFromLeft(3.f), 2.f);

    auto inner = card.GetPadded(-10.f).GetReducedFromLeft(4.f).GetReducedFromRight(30.f);

    // Badge.
    const auto badge = inner.GetFromTop(15.f).GetFromLeft(BadgeWidth(entry));
    g.FillRoundRect(accent.WithOpacity(0.18f), badge, 3.f);
    const IText badgeText(11.f, accent, nullptr, EAlign::Center, EVAlign::Middle);
    g.DrawText(badgeText, GearLabel(entry.gear, entry.format), badge);

    // Title, to the right of the badge.
    const IText title(15.f, PluginColors::INK, nullptr, EAlign::Near, EVAlign::Middle);
    g.DrawText(title, entry.title.c_str(), inner.GetFromTop(17.f).GetReducedFromLeft(badge.W() + 8.f));

    // Byline.
    const IText meta(12.f, PluginColors::INK_MUTED, nullptr, EAlign::Near, EVAlign::Middle);
    g.DrawText(meta, BylineFor(entry).c_str(), inner.GetFromBottom(14.f));

    DrawStar(g, StarRectFor(row), entry.favourited);
  }

  float BadgeWidth(const nr::net::BrowserController::Row& entry) const
  {
    // Proportional to the label so "AMP" does not get the same pill as
    // "EXPERIMENTAL". Measuring text would be exact but needs the graphics
    // context; the label set is fixed and short, so this is close enough.
    const size_t characters = strlen(GearLabel(entry.gear, entry.format));
    return 14.f + static_cast<float>(characters) * 5.6f;
  }

  std::string BylineFor(const nr::net::BrowserController::Row& entry) const
  {
    std::string byline = entry.author;

    for (size_t i = 0; i < entry.tags.size() && i < 2; i++)
      byline += "  ·  " + entry.tags[i];

    if (entry.downloads > 0)
      byline += "  ·  " + std::to_string(entry.downloads) + " downloads";

    if (entry.modelsCount > 1)
      byline += "  ·  " + std::to_string(entry.modelsCount) + " models";

    return byline;
  }

  void DrawStar(IGraphics& g, const IRECT& rect, bool filled)
  {
    const auto colour = filled ? PluginColors::AMBER : PluginColors::INK_DIM;
    const float cx = rect.MW();
    const float cy = rect.MH();
    const float outer = rect.W() * 0.5f;
    const float inner = outer * 0.45f;

    // A real five-pointed star rather than a glyph: the font in use is not
    // guaranteed to have one, and a missing glyph draws as a box.
    constexpr float kPi = 3.14159265f;

    for (int i = 0; i < 10; i++)
    {
      const float radius = (i % 2 == 0) ? outer : inner;
      const float angle = static_cast<float>(i) * kPi / 5.f - kPi * 0.5f;
      const float px = cx + radius * std::cos(angle);
      const float py = cy + radius * std::sin(angle);

      if (i == 0)
        g.PathMoveTo(px, py);
      else
        g.PathLineTo(px, py);
    }

    g.PathClose();

    if (filled)
      g.PathFill(colour);
    else
      g.PathStroke(colour, 1.2f);
  }

  void DrawFooter(IGraphics& g)
  {
    const IText meta(12.f, PluginColors::INK_MUTED, nullptr, EAlign::Near, EVAlign::Middle);

    if (mSnapshot.total > 0)
    {
      std::string summary = std::to_string(mSnapshot.total) + " results";

      if (mSnapshot.totalPages > 1)
        summary += "  ·  page " + std::to_string(mSnapshot.page) + " of " + std::to_string(mSnapshot.totalPages);

      g.DrawText(meta, summary.c_str(), mFooterRect);
    }

    if (mSnapshot.totalPages > 1)
    {
      DrawPill(g, mPrevRect, "Prev", mSnapshot.page > 1 ? PluginColors::INK_MUTED : PluginColors::INK_DIM, false, true);
      DrawPill(g, mNextRect, "Next",
               mSnapshot.page < mSnapshot.totalPages ? PluginColors::INK_MUTED : PluginColors::INK_DIM, false, true);
    }
  }

  /// A rounded label: filled when active, outlined when it is a control the
  /// user can press, plain otherwise.
  void DrawPill(IGraphics& g, const IRECT& rect, const char* label, const IColor& colour, bool active,
                bool outlined = false)
  {
    if (active)
      g.FillRoundRect(colour.WithOpacity(0.16f), rect, 4.f);
    else if (outlined)
      g.DrawRoundRect(PluginColors::PANEL_HI, rect, 4.f);

    const IText text(13.f, colour, nullptr, EAlign::Center, EVAlign::Middle);
    g.DrawText(text, label, rect);
  }

  // --- State ----------------------------------------------------------------

  nr::net::BrowserController& mController;
  LoadHandler mOnLoad;

  nr::net::BrowserController::Snapshot mSnapshot;

  std::string mQuery;
  std::string mGear;
  std::string mFormat; ///< format filter; IR is a format, not a gear
  /// A2 by default: omitting the parameter is documented to exclude A2, and
  /// that is where most current captures are.
  int mArchitecture = 2;
  std::string mArchLabel = "A2 models";

  std::string mGearLabel = "All gear"; ///< what the pill shows; mGear is the API value
  std::string mSort = "trending";

  IPopupMenu mGearMenu;
  IPopupMenu mSortMenu;
  IPopupMenu mArchMenu;
  IPopupMenu mModelMenu;

  bool mStarted = false;
  int mAwaitingChoicesForRow = -1; ///< a variant fetch is in flight for this row
  int mChoiceRow = -1;             ///< the row whose menu is currently open
  ChoiceHandler mOnChoice;
  float mScrollOffset = 0.f;
  int mHoverRow = -1;

  IRECT mHeaderRect, mAuthRect, mTabsRect, mGearRect, mArchRect, mSortRect;
  IRECT mSearchRect, mListRect, mFooterRect, mPrevRect, mNextRect;
  IRECT mTabRects[kNumTabs];
};

} // namespace nr::browser
