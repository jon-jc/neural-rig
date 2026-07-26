#include "BrowserController.h"

#include <algorithm>
#include <cctype>

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
  // One operation at a time. A second request while one is in flight is
  // dropped rather than queued: pressing Search twice means you want the
  // second result, and running both would race over the same state.
  bool expected = false;
  if (!mBusy.compare_exchange_strong(expected, true))
    return;

  std::thread([this, work = std::move(work)] {
    work();
    mBusy.store(false, std::memory_order_release);
  }).detach();
}

void BrowserController::Begin()
{
  if (mClient.LoadSavedSession())
    SetStatus(Status::Idle, "Connected");
  else
    SetStatus(Status::SignedOut, "Connect your TONE3000 account to browse captures");
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
  });
}

void BrowserController::SignOut()
{
  mClient.SignOut();

  {
    std::lock_guard<std::mutex> lock(mMutex);
    mSnapshot.rows.clear();
    mTones.clear();
    mSnapshot.page = 1;
    mSnapshot.totalPages = 0;
    mSnapshot.total = 0;
  }

  SetStatus(Status::SignedOut, "Signed out");
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

    {
      std::lock_guard<std::mutex> lock(mMutex);

      mTones = results.tones;
      mSnapshot.rows.clear();
      mSnapshot.rows.reserve(results.tones.size());

      for (const auto& tone : results.tones)
      {
        Row row;
        row.toneId = tone.id;
        row.title = tone.title;
        row.author = tone.author;
        row.gear = tone.gear;
        row.licence = tone.licence;
        row.downloads = tone.downloadsCount;
        mSnapshot.rows.push_back(std::move(row));
      }

      mSnapshot.page = results.page > 0 ? results.page : query.page;
      mSnapshot.totalPages = results.totalPages;
      mSnapshot.total = results.total;
    }

    SetStatus(results.tones.empty() ? Status::Idle : Status::Idle,
              results.tones.empty() ? "No captures matched" : "Connected");
  });
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
