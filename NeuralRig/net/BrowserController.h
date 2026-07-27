#pragma once

#include <atomic>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "LoopbackServer.h"
#include "Tone3000Client.h"

namespace nr::net
{

/**
    Drives the TONE3000 browser: sign-in, search, and downloading a capture
    into a chain slot.

    Every network call happens on a detached worker thread, because all of them
    block for as long as the network takes and the UI thread must keep drawing.
    Results are published into mutex-guarded state, and the UI polls Snapshot()
    from its idle callback. Nothing here touches IGraphics, which keeps the
    logic testable without a plugin host.

    One operation runs at a time. A second request while one is in flight is
    ignored rather than queued: the user pressing Search twice wants the second
    result, and letting both run would race over the same state.
*/
class BrowserController
{
public:
  enum class Status
  {
    SignedOut,  ///< no saved session; the user must connect
    Connecting, ///< waiting for the browser round trip
    Idle,       ///< signed in, nothing in flight
    Working,    ///< a search or download is running
    Failed      ///< last operation failed; see message
  };

  /// Which listing the browser is showing.
  enum class Tab
  {
    Browse,     ///< catalogue search, the only tab that takes filters
    Favourites, ///< tones the user starred
    Created,    ///< tones the user captured
    Recent,     ///< tones the user has downloaded before
    Local,      ///< the on-disk cache; no network involved
  };

  /// One row in the results list, flattened for display.
  struct Row
  {
    int toneId = 0;
    std::string title;
    std::string author;
    std::string gear;
    std::string licence;
    std::string format;
    std::string imageUrl; ///< card thumbnail; empty when the tone has no art
    std::vector<std::string> tags;
    int downloads = 0;
    int modelsCount = 0;

    /// Whether the star is filled. Only meaningful on the Favourites tab or
    /// after the user toggles it -- search results carry no per-user state, so
    /// false means "not known to be starred" rather than "definitely not".
    bool favourited = false;

    /// Set on the Local tab, where rows come from disk rather than the API.
    std::string localPath;
  };

  /// Everything the UI needs, copied under the lock so it can render without
  /// holding it.
  struct Snapshot
  {
    Status status = Status::SignedOut;
    std::string message;
    std::vector<Row> rows;
    Tab tab = Tab::Browse;
    std::string username; ///< empty until the identity call lands
    int page = 1;
    int totalPages = 0;
    int total = 0;
  };

  BrowserController();
  ~BrowserController();

  BrowserController(const BrowserController&) = delete;
  BrowserController& operator=(const BrowserController&) = delete;

  /// Restores a saved session if there is one. Call once when the UI opens.
  void Begin();

  /// Starts the OAuth flow: opens the user's browser and waits for the
  /// redirect on a loopback listener.
  void SignIn();

  /// Forgets the session, locally and in the credential store.
  void SignOut();

  /// Runs a catalogue search. Page is 1-based.
  void Search(const std::string& text,
              const std::string& gear,
              const std::string& sort,
              int architecture,
              int page);

  /// Re-runs the last search on a different page.
  void GoToPage(int page);

  /// Switches tabs and loads the first page of that listing. Browse re-runs the
  /// last search; Local reads the cache directory and touches no network.
  void ShowTab(Tab tab, int page = 1);

  /// Stars or unstars the tone behind a row, updating it in place so the UI
  /// does not have to refetch the whole page to redraw one star.
  void ToggleFavourite(int rowIndex);

  /// The signed-in account name, for the browser's identity strip. Fetched once
  /// after a session is restored or established.
  void FetchIdentity();

  /// Downloads the best model for a row and reports the file it landed in.
  /// The callback fires on the worker thread; the UI marshals it via its idle
  /// callback like everything else.
  using LoadCallback = std::function<void(bool success, std::string pathOrError)>;
  void DownloadRow(int rowIndex, LoadCallback onComplete);

  /// The URL an embedded browser should load to let the user pick a tone.
  std::string BuildSelectToneUrl(const PkcePair& pkce, const std::string& redirectUri) const;

  /// Waits for TONE3000 to redirect to the loopback listener with a chosen
  /// tone, then downloads it. Used by the embedded browser: the user does the
  /// browsing on tone3000.com itself and this picks up the result.
  void AwaitToneSelection(LoopbackServer& loopback,
                          const PkcePair& pkce,
                          const std::string& redirectUri,
                          int timeoutMs,
                          LoadCallback onComplete);

  /// Downloads a tone by id into the cache. Blocking; used by the above.
  bool DownloadTone(int toneId, const std::string& title, std::string& pathOrError);

  /// Handles a single redirect from the embedded browser. Called in a loop by
  /// AwaitToneSelection so one pick is not the end of the session.
  void HandleOneSelection(LoopbackServer& loopback,
                          const PkcePair& pkce,
                          const std::string& redirectUri,
                          int timeoutMs,
                          const LoadCallback& onComplete);

  /// Copies the current state for rendering.
  Snapshot GetSnapshot() const;

  /// True exactly once after the state changes, so the UI only redraws when
  /// something actually happened.
  bool ConsumeDirty();

  /// Directory downloaded captures are cached in.
  static std::string CacheDirectory();

private:
  void RunAsync(std::function<void()> work);

  /// Copies a fetched page into the snapshot. Shared by search and the
  /// per-user listings, which differ only in how they were fetched.
  void PublishPage(const TonePage& results, int fallbackPage);

  /// Fills the rows from the on-disk cache. No network, so it runs inline.
  void LoadLocalRows();

  /// The identity fetch without the threading, so code already running on a
  /// worker can call it. Going through FetchIdentity there would be dropped:
  /// RunAsync refuses to start while an operation is in flight, and the caller
  /// would be that operation.
  void RefreshIdentityBlocking();
  void SetStatus(Status status, std::string message);
  void MarkDirty();

  Tone3000Client mClient;
  LoopbackServer mLoopback;

  mutable std::mutex mMutex;
  Snapshot mSnapshot;
  std::vector<Tone> mTones; ///< full records behind the display rows
  SearchQuery mLastQuery;

  std::atomic<bool> mDirty{false};
  std::atomic<bool> mBusy{false};

  /// The most recent request that arrived while another was running. Only one
  /// is kept: if the user clicks three filters quickly they want the third, and
  /// running all three in turn would show them two sets of results they have
  /// already moved past.
  std::mutex mPendingMutex;
  std::function<void()> mPendingWork;
};

/// Human-readable label for a status, for the UI's message line.
const char* StatusLabel(BrowserController::Status status);

} // namespace nr::net
