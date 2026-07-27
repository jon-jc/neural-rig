#include "BrowserController.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <system_error>

namespace nr::net
{
namespace
{
/// How long to wait for the user to finish signing in before giving up. Long
/// enough to find the browser window, create an account and read a consent
/// screen; short enough that an abandoned attempt does not linger forever.
constexpr int kSignInTimeoutMs = 180000;

/// Model sizes in the order we would rather have them, best first. A tone
/// usually ships several; without a preference we would take whichever the API
/// listed first, which is arbitrary.
const char* kSizePreference[] = {"standard", "lite", "feather", "nano", "custom"};

int SizeRank(const std::string& size)
{
  for (int i = 0; i < static_cast<int>(std::size(kSizePreference)); i++)
    if (size == kSizePreference[i])
      return i;

  return static_cast<int>(std::size(kSizePreference));
}

/// Turns a tone title into something safe to use as a filename.
std::string SanitiseForFilename(const std::string& text)
{
  std::string safe;
  safe.reserve(text.size());

  for (const unsigned char c : text)
  {
    if (std::isalnum(c))
      safe += static_cast<char>(std::tolower(c));
    else if (!safe.empty() && safe.back() != '-')
      safe += '-';
  }

  while (!safe.empty() && safe.back() == '-')
    safe.pop_back();

  return safe.empty() ? "capture" : safe;
}
} // namespace

const char* StatusLabel(BrowserController::Status status)
{
  switch (status)
  {
    case BrowserController::Status::SignedOut: return "Not connected";
    case BrowserController::Status::Connecting: return "Waiting for your browser...";
    case BrowserController::Status::Idle: return "Connected";
    case BrowserController::Status::Working: return "Working...";
    case BrowserController::Status::Failed: return "Failed";
  }

  return "";
}

BrowserController::BrowserController() = default;

BrowserController::~BrowserController()
{
  // Unblock a sign-in that is still waiting on the loopback listener, so the
  // worker thread can finish rather than outliving us.
  mLoopback.Stop();
}

std::string BrowserController::CacheDirectory()
{
  const auto base = UserDataDirectory();

  if (base.empty())
    return {};

#ifdef _WIN32
  const auto path = base + "\\models";
#else
  const auto path = base + "/models";
#endif

  return EnsureDirectory(path) ? path : std::string{};
}

void BrowserController::MarkDirty()
{
  mDirty.store(true, std::memory_order_release);
}

bool BrowserController::ConsumeDirty()
{
  return mDirty.exchange(false, std::memory_order_acq_rel);
}

void BrowserController::SetStatus(Status status, std::string message)
{
  {
    std::lock_guard<std::mutex> lock(mMutex);
    mSnapshot.status = status;
    mSnapshot.message = std::move(message);
  }

  MarkDirty();
}

BrowserController::Snapshot BrowserController::GetSnapshot() const
{
  std::lock_guard<std::mutex> lock(mMutex);
  return mSnapshot;
}

void BrowserController::RunAsync(std::function<void()> work)
{
  // One operation at a time, but the newest request wins rather than losing.
  //
  // This used to drop anything that arrived mid-flight, on the reasoning that
  // pressing Search twice means you want the second result. That was backwards:
  // dropping the second request means you keep the *first* result, so a browser
  // driven by clicking filters showed the previous filter's results every time.
  bool expected = false;
  if (!mBusy.compare_exchange_strong(expected, true))
  {
    std::lock_guard<std::mutex> lock(mPendingMutex);
    mPendingWork = std::move(work);
    return;
  }

  std::thread([this, work = std::move(work)] {
    work();
    mBusy.store(false, std::memory_order_release);

    // Pick up whatever arrived while we were busy. Releasing mBusy first means
    // a request landing right now starts on its own thread instead; if it beat
    // us to it, our re-submission simply becomes the next pending item.
    std::function<void()> next;
    {
      std::lock_guard<std::mutex> lock(mPendingMutex);
      next.swap(mPendingWork);
    }

    if (next)
      RunAsync(std::move(next));
  }).detach();
}

void BrowserController::Begin()
{
  if (mClient.LoadSavedSession())
  {
    SetStatus(Status::Idle, "Connected");
    FetchIdentity();
  }
  else
  {
    SetStatus(Status::SignedOut, "Connect your TONE3000 account to browse captures");
  }
}

void BrowserController::SignIn()
{
  RunAsync([this] {
    const auto redirectUri = mLoopback.Start();

    if (redirectUri.empty())
    {
      SetStatus(Status::Failed, "Could not open a local port for sign-in");
      return;
    }

    const auto pkce = CreatePkcePair();

    if (pkce.verifier.empty())
    {
      mLoopback.Stop();
      SetStatus(Status::Failed, "Could not generate secure random data");
      return;
    }

    if (!OpenUrlInBrowser(mClient.BuildAuthorizeUrl(pkce, redirectUri)))
    {
      mLoopback.Stop();
      SetStatus(Status::Failed, "Could not open your browser");
      return;
    }

    SetStatus(Status::Connecting, "Waiting for your browser...");

    const auto parameters = mLoopback.WaitForRedirect(kSignInTimeoutMs);
    mLoopback.Stop();

    if (parameters.empty())
    {
      SetStatus(Status::SignedOut, "Sign-in timed out");
      return;
    }

    const auto errorIt = parameters.find("error");
    if (errorIt != parameters.end())
    {
      SetStatus(Status::SignedOut, "TONE3000 declined: " + errorIt->second);
      return;
    }

    // Verify state before touching the code. Without this, anything that can
    // reach the loopback port could feed us an authorization code of its
    // choosing.
    const auto stateIt = parameters.find("state");
    if (stateIt == parameters.end() || stateIt->second != pkce.state)
    {
      SetStatus(Status::Failed, "Sign-in failed a security check and was abandoned");
      return;
    }

    const auto codeIt = parameters.find("code");
    if (codeIt == parameters.end() || codeIt->second.empty())
    {
      SetStatus(Status::SignedOut, "TONE3000 did not return an authorization code");
      return;
    }

    std::string error;
    if (!mClient.ExchangeCode(codeIt->second, pkce.verifier, redirectUri, error))
    {
      SetStatus(Status::Failed, error);
      return;
    }

    SetStatus(Status::Idle, "Connected");

    // Already on the worker, so call the blocking form directly.
    RefreshIdentityBlocking();
  });
}

void BrowserController::SignOut()
{
  mClient.SignOut();

  {
    std::lock_guard<std::mutex> lock(mMutex);
    mSnapshot.rows.clear();
    mTones.clear();
    mSnapshot.username.clear();
    mSnapshot.tab = Tab::Browse;
    mSnapshot.page = 1;
    mSnapshot.totalPages = 0;
    mSnapshot.total = 0;
  }

  SetStatus(Status::SignedOut, "Signed out");
}

namespace
{
/// Flattens an API record into the display row the UI renders.
BrowserController::Row RowFrom(const Tone& tone)
{
  BrowserController::Row row;
  row.toneId = tone.id;
  row.title = tone.title;
  row.author = tone.author;
  row.gear = tone.gear;
  row.licence = tone.licence;
  row.format = tone.format;
  row.tags = tone.tags;
  row.downloads = tone.downloadsCount;
  row.modelsCount = tone.modelsCount;
  row.favourited = tone.isFavourited;

  if (!tone.images.empty())
    row.imageUrl = tone.images.front();

  return row;
}
} // namespace

void BrowserController::PublishPage(const TonePage& results, int fallbackPage)
{
  std::lock_guard<std::mutex> lock(mMutex);

  mTones = results.tones;
  mSnapshot.rows.clear();
  mSnapshot.rows.reserve(results.tones.size());

  for (const auto& tone : results.tones)
    mSnapshot.rows.push_back(RowFrom(tone));

  mSnapshot.page = results.page > 0 ? results.page : fallbackPage;
  mSnapshot.totalPages = results.totalPages;
  mSnapshot.total = results.total;
}

void BrowserController::Search(const std::string& text,
                               const std::string& gear,
                               const std::string& sort,
                               int architecture,
                               int page)
{
  SearchQuery query;
  query.text = text;
  query.sort = sort.empty() ? "best-match" : sort;
  query.architecture = architecture;
  query.page = std::max(1, page);

  if (!gear.empty())
    query.gears.push_back(gear);

  {
    std::lock_guard<std::mutex> lock(mMutex);
    mLastQuery = query;

    // Searching *is* the Browse tab, so own that here. Callers used to call
    // ShowTab(Browse) first and then Search, but ShowTab routes Browse straight
    // back into GoToPage -> Search using the previous query. That first search
    // claimed mBusy and the caller's real one -- carrying the new filter -- was
    // dropped, so the results always lagged one click behind: picking the amp
    // slot showed whatever the pedal slot had just asked for.
    mSnapshot.tab = Tab::Browse;
  }

  RunAsync([this, query] {
    SetStatus(Status::Working, "Searching...");

    TonePage results;
    std::string error;

    if (!mClient.SearchTones(query, results, error))
    {
      SetStatus(Status::Failed, error);
      return;
    }

    PublishPage(results, query.page);

    SetStatus(Status::Idle, results.tones.empty() ? "No captures matched" : "Connected");
  });
}

void BrowserController::ShowTab(Tab tab, int page)
{
  {
    std::lock_guard<std::mutex> lock(mMutex);
    mSnapshot.tab = tab;
  }

  // Browse is the search tab; re-running the last query keeps the user's
  // filters rather than silently resetting them on every tab round trip.
  if (tab == Tab::Browse)
  {
    GoToPage(page);
    return;
  }

  if (tab == Tab::Local)
  {
    LoadLocalRows();
    MarkDirty();
    return;
  }

  ToneListing listing = ToneListing::Favourited;

  switch (tab)
  {
    case Tab::Favourites: listing = ToneListing::Favourited; break;
    case Tab::Created: listing = ToneListing::Created; break;
    case Tab::Recent: listing = ToneListing::Downloaded; break;
    default: break;
  }

  const int wanted = std::max(1, page);

  RunAsync([this, listing, wanted] {
    SetStatus(Status::Working, "Loading...");

    TonePage results;
    std::string error;

    if (!mClient.ListTones(listing, wanted, 24, results, error))
    {
      SetStatus(Status::Failed, error);
      return;
    }

    PublishPage(results, wanted);

    SetStatus(Status::Idle, results.tones.empty() ? "Nothing here yet" : "Connected");
  });
}

void BrowserController::ToggleFavourite(int rowIndex)
{
  int toneId = 0;
  bool wanted = false;

  {
    std::lock_guard<std::mutex> lock(mMutex);

    if (rowIndex < 0 || rowIndex >= static_cast<int>(mSnapshot.rows.size()))
      return;

    toneId = mSnapshot.rows[rowIndex].toneId;
    wanted = !mSnapshot.rows[rowIndex].favourited;
  }

  RunAsync([this, rowIndex, toneId, wanted] {
    std::string error;

    if (!mClient.SetFavourite(toneId, wanted, error))
    {
      SetStatus(Status::Failed, error);
      return;
    }

    {
      std::lock_guard<std::mutex> lock(mMutex);

      // The list can have been replaced while the request was in flight, so
      // check the row still holds the tone we starred before writing to it.
      if (rowIndex < static_cast<int>(mSnapshot.rows.size())
          && mSnapshot.rows[rowIndex].toneId == toneId)
        mSnapshot.rows[rowIndex].favourited = wanted;
    }

    SetStatus(Status::Idle, wanted ? "Saved to favourites" : "Removed from favourites");
  });
}

void BrowserController::RefreshIdentityBlocking()
{
  User user;
  std::string error;

  // A failure here is not worth surfacing: the browser works perfectly well
  // without knowing the user's name, and reporting it would replace a real
  // status message with a cosmetic complaint.
  if (!mClient.GetCurrentUser(user, error))
    return;

  {
    std::lock_guard<std::mutex> lock(mMutex);
    mSnapshot.username = user.username;
  }

  MarkDirty();
}

void BrowserController::FetchIdentity()
{
  RunAsync([this] { RefreshIdentityBlocking(); });
}

void BrowserController::LoadLocalRows()
{
  std::vector<Row> rows;
  std::error_code ec;

  // Not an error worth reporting: an empty cache is the normal state before
  // the user has downloaded anything.
  for (const auto& entry : std::filesystem::directory_iterator(CacheDirectory(), ec))
  {
    if (!entry.is_regular_file(ec))
      continue;

    const auto path = entry.path();
    const auto extension = path.extension().string();

    if (extension != ".nam" && extension != ".wav")
      continue;

    Row row;
    row.title = path.stem().string();
    row.localPath = path.string();
    row.format = extension == ".nam" ? "nam" : "ir";
    row.author = "On this machine";
    rows.push_back(std::move(row));
  }

  std::sort(rows.begin(), rows.end(), [](const Row& a, const Row& b) { return a.title < b.title; });

  std::lock_guard<std::mutex> lock(mMutex);
  mSnapshot.rows = std::move(rows);
  mSnapshot.page = 1;
  mSnapshot.totalPages = 1;
  mSnapshot.total = static_cast<int>(mSnapshot.rows.size());
  mSnapshot.message = mSnapshot.rows.empty() ? "No downloaded captures yet" : "Local cache";
}

void BrowserController::GoToPage(int page)
{
  SearchQuery query;
  {
    std::lock_guard<std::mutex> lock(mMutex);
    query = mLastQuery;
  }

  const auto gear = query.gears.empty() ? std::string{} : query.gears.front();
  Search(query.text, gear, query.sort, query.architecture, page);
}

std::string BrowserController::BuildSelectToneUrl(const PkcePair& pkce, const std::string& redirectUri) const
{
  return mClient.BuildSelectToneUrl(pkce, redirectUri);
}

void BrowserController::AwaitToneSelection(LoopbackServer& loopback,
                                           const PkcePair& pkce,
                                           const std::string& redirectUri,
                                           int timeoutMs,
                                           LoadCallback onComplete)
{
  RunAsync([this, &loopback, pkce, redirectUri, timeoutMs, onComplete = std::move(onComplete)] {
    // Keep listening for as long as the browser panel is open. The panel is
    // permanent now, so one selection must not be the end of it -- picking a
    // second capture has to work without reopening anything.
    while (loopback.IsRunning())
    {
      HandleOneSelection(loopback, pkce, redirectUri, timeoutMs, onComplete);
    }
  });
}

void BrowserController::HandleOneSelection(LoopbackServer& loopback,
                                           const PkcePair& pkce,
                                           const std::string& redirectUri,
                                           int timeoutMs,
                                           const LoadCallback& onComplete)
{
  {
    const auto parameters = loopback.WaitForRedirect(timeoutMs);

    if (parameters.empty())
    {
      // Timed out with nothing chosen, or the panel was closed underneath us.
      return;
    }

    const auto errorIt = parameters.find("error");
    if (errorIt != parameters.end())
    {
      // redirect_uri_mismatch is the one worth naming outright: it means the
      // TONE3000 app has redirect URIs registered and the loopback address is
      // not among them, which no amount of retrying will fix.
      const bool mismatch = errorIt->second.find("redirect") != std::string::npos;

      SetStatus(Status::Failed,
                mismatch ? "TONE3000 rejected the loopback address. Clear the Allowed Redirect URIs "
                           "in your TONE3000 app settings, or add " + redirectUri
                         : "TONE3000 declined: " + errorIt->second);
      return;
    }

    // Verify state before touching the code. Without this, anything able to
    // reach the loopback port could feed us a code of its choosing.
    const auto stateIt = parameters.find("state");
    if (stateIt == parameters.end() || stateIt->second != pkce.state)
    {
      SetStatus(Status::Failed, "Selection failed a security check and was abandoned");
      return;
    }

    // A code is present on first authorisation. On later selections, when the
    // user is already signed in, TONE3000 may hand back only a tone_id -- in
    // which case the saved session has to carry the request instead.
    const auto codeIt = parameters.find("code");
    if (codeIt != parameters.end() && !codeIt->second.empty())
    {
      SetStatus(Status::Working, "Signing in...");

      std::string error;
      if (!mClient.ExchangeCode(codeIt->second, pkce.verifier, redirectUri, error))
      {
        SetStatus(Status::Failed, "Sign-in failed: " + error);
        return;
      }
    }

    const auto toneIt = parameters.find("tone_id");
    if (toneIt == parameters.end() || toneIt->second.empty())
    {
      SetStatus(Status::Idle, mClient.IsConnected() ? "Signed in. Pick a capture to load."
                                                    : "Signed in, but no capture was chosen.");
      return;
    }

    if (!mClient.IsConnected())
    {
      // Without a token the model endpoints are closed to us, and silently
      // doing nothing is the worst possible answer here.
      SetStatus(Status::Failed, "Chose a capture but no TONE3000 session; press HOME and sign in again.");

      if (onComplete)
        onComplete(false, "no session");

      return;
    }

    const int toneId = std::atoi(toneIt->second.c_str());
    SetStatus(Status::Working, "Fetching capture " + toneIt->second + "...");

    std::string pathOrError;
    const bool ok = DownloadTone(toneId, "tone-" + toneIt->second, pathOrError);

    SetStatus(ok ? Status::Idle : Status::Failed,
              ok ? "Loaded into the chain" : ("Download failed: " + pathOrError));

    if (onComplete)
      onComplete(ok, pathOrError);
  }
}

bool BrowserController::DownloadTone(int toneId, const std::string& title, std::string& pathOrError)
{
  std::vector<Model> models;
  std::string error;

  if (!mClient.ListModels(toneId, models, error))
  {
    pathOrError = error;
    return false;
  }

  if (models.empty())
  {
    pathOrError = "That tone has no downloadable models";
    return false;
  }

  // Prefer the fullest size a tone offers; CPU is the user's to trade away with
  // the slim control if they want it back.
  const auto best = std::min_element(models.begin(), models.end(), [](const Model& a, const Model& b) {
    return SizeRank(a.size) < SizeRank(b.size);
  });

  const auto directory = CacheDirectory();

  if (directory.empty())
  {
    pathOrError = "Could not create the model cache directory";
    return false;
  }

#ifdef _WIN32
  const auto separator = "\\";
#else
  const auto separator = "/";
#endif

  const auto path =
    directory + separator + SanitiseForFilename(title) + "-" + std::to_string(best->id) + ".nam";

  if (!mClient.DownloadModel(*best, path, error))
  {
    pathOrError = error;
    return false;
  }

  pathOrError = path;
  return true;
}

void BrowserController::DownloadRow(int rowIndex, LoadCallback onComplete)
{
  int toneId = 0;
  std::string title;

  {
    std::lock_guard<std::mutex> lock(mMutex);

    if (rowIndex < 0 || rowIndex >= static_cast<int>(mTones.size()))
      return;

    toneId = mTones[rowIndex].id;
    title = mTones[rowIndex].title;
  }

  RunAsync([this, toneId, title, onComplete = std::move(onComplete)] {
    SetStatus(Status::Working, "Fetching capture...");

    std::vector<Model> models;
    std::string error;

    if (!mClient.ListModels(toneId, models, error))
    {
      SetStatus(Status::Failed, error);
      if (onComplete)
        onComplete(false, error);
      return;
    }

    if (models.empty())
    {
      const std::string message = "That tone has no downloadable models";
      SetStatus(Status::Failed, message);
      if (onComplete)
        onComplete(false, message);
      return;
    }

    // A tone usually ships several sizes. Prefer the fullest, since CPU is the
    // user's to trade away via the slim control if they want it back.
    const auto best = std::min_element(models.begin(), models.end(), [](const Model& a, const Model& b) {
      return SizeRank(a.size) < SizeRank(b.size);
    });

    const auto directory = CacheDirectory();

    if (directory.empty())
    {
      const std::string message = "Could not create the model cache directory";
      SetStatus(Status::Failed, message);
      if (onComplete)
        onComplete(false, message);
      return;
    }

#ifdef _WIN32
    const auto separator = "\\";
#else
    const auto separator = "/";
#endif

    const auto path =
      directory + separator + SanitiseForFilename(title) + "-" + std::to_string(best->id) + ".nam";

    if (!mClient.DownloadModel(*best, path, error))
    {
      SetStatus(Status::Failed, error);
      if (onComplete)
        onComplete(false, error);
      return;
    }

    SetStatus(Status::Idle, "Loaded " + title);

    if (onComplete)
      onComplete(true, path);
  });
}

} // namespace nr::net
