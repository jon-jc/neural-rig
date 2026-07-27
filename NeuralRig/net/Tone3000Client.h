#pragma once

#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

#include "Platform.h"

namespace nr::net
{

inline constexpr const char* kTone3000BaseUrl = "https://www.tone3000.com";

/// OAuth client_id. A publishable key is designed to ship inside the client --
/// it identifies the app and authorises nothing on its own. The matching secret
/// key must never appear in a plugin binary, which is exactly why this flow is
/// PKCE rather than a client-credentials exchange.
inline constexpr const char* kDefaultPublishableKey = "t3k_pub_JvGasxOc8UMC9mxCqLxCYWL4NX_FY0bH";

/// One PKCE challenge/verifier pair plus the CSRF state token.
struct PkcePair
{
  std::string verifier;  ///< kept locally, revealed only when redeeming the code
  std::string challenge; ///< SHA-256 of the verifier, sent in the authorize URL
  std::string state;     ///< echoed back by the server; guards against CSRF
};

/// Generates a fresh PKCE pair from the system CSPRNG.
///
/// PKCE exists because a desktop app cannot keep a secret: anything shipped in
/// the binary is readable by whoever has the binary. Instead the app proves, at
/// redemption time, that it is the same party that started the flow.
PkcePair CreatePkcePair();

/// A tone: one piece of gear someone captured, holding one or more models.
struct Tone
{
  int id = 0;
  std::string title;
  std::string description;
  std::string gear;    ///< amp, amp-cab, pedal, outboard, cab, space, experimental
  std::string format;  ///< nam, ir, aida-x, ...
  std::string licence; ///< t3k, cc-by, ...
  std::string author;
  std::string url;
  std::vector<std::string> makes;
  std::vector<std::string> tags;
  std::vector<std::string> sizes;
  std::vector<std::string> images; ///< artwork URLs, first is the card thumbnail
  int modelsCount = 0;
  int downloadsCount = 0;
  int favouritesCount = 0;

  /// Only ever true from the favourites listing or immediately after the user
  /// stars something. Search results carry no per-user state, so a false here
  /// means "not known to be favourited" rather than "definitely not".
  bool isFavourited = false;
};

/// The signed-in account, for the browser's identity strip.
struct User
{
  int id = 0;
  std::string username;
  std::string avatarUrl;
  std::string url;
};

/// A downloadable model file belonging to a tone.
struct Model
{
  int id = 0;
  int toneId = 0;
  std::string name;
  std::string modelUrl;
  std::string size;                ///< standard, lite, feather, nano, custom
  std::string architectureVersion; ///< "1", "2", "custom"
};

struct TonePage
{
  std::vector<Tone> tones;
  int page = 1;
  int pageSize = 0;
  int total = 0;
  int totalPages = 0;
};

/// Filters for the catalogue search, mirroring the public API's parameters.
struct SearchQuery
{
  std::string text;
  std::vector<std::string> gears; ///< empty means every kind of gear
  std::vector<std::string> sizes; ///< empty means every size
  std::string format;             ///< nam, ir, aida-x, ...; empty means any
  std::string sort = "best-match";
  int architecture = 0;    ///< 0 = any, otherwise 1 or 2
  bool calibrated = false; ///< true restricts to calibrated captures
  int page = 1;

  /// The search endpoint caps page_size at 25, lower than every other listing.
  int pageSize = 24;
};

/// Which listing the browser is showing. Each maps to its own endpoint; only
/// Search accepts the full filter set.
enum class ToneListing
{
  Search,     ///< /tones/search
  Favourited, ///< /tones/favorited
  Created,    ///< /tones/created
  Downloaded, ///< /tones/downloaded
};

/// Key under which the refresh token is filed in the OS credential store.
inline constexpr const char* kRefreshTokenKey = "tone3000_refresh_token";

struct Tokens
{
  std::string accessToken;
  std::string refreshToken;
  int64_t expiresAtMs = 0;

  /// A restored session has a refresh token but no access token yet, and is
  /// still "signed in" -- the access token is re-derived on first use.
  bool IsEmpty() const { return accessToken.empty() && refreshToken.empty(); }

  /// True when the access token is missing or close enough to expiry that a
  /// request might outlive it.
  bool IsExpired() const;
};

/// Filter vocabularies, for populating the browser's controls.
std::vector<std::string> GearOptions();
std::vector<std::string> SizeOptions();
std::vector<std::string> SortOptions();

/// Builds the request path for a catalogue search, including the page_size
/// clamp. Free rather than a private member so it can be tested without a
/// server: a wrong path here is a 400 at runtime and compiles perfectly.
std::string BuildSearchPath(const SearchQuery& query);

/// Builds the request path for one of the per-user listings. Returns an empty
/// string for ToneListing::Search, which has no listing endpoint of its own.
std::string BuildListingPath(ToneListing listing, int page, int pageSize);

/**
    Client for the public TONE3000 API.

    Every method blocks on network I/O and must run on a worker thread -- never
    the audio thread, and not the UI thread either.

    Authorisation is OAuth 2.0 with PKCE: the user signs in through their own
    browser, so the plugin never handles their password. Access tokens are
    refreshed automatically when they expire.
*/
class Tone3000Client
{
public:
  explicit Tone3000Client(std::string publishableKey = kDefaultPublishableKey);

  // --- Authorisation --------------------------------------------------------

  /// The URL to open in the user's browser to begin sign-in.
  std::string BuildAuthorizeUrl(const PkcePair& pkce, const std::string& redirectUri) const;

  /// Authorize URL in TONE3000's "browse and pick one" mode.
  ///
  /// With prompt=select_tone the site lets the user browse its own catalogue --
  /// artwork, demos, tags, the search they already know -- and hands back the
  /// chosen tone_id with the authorization code. That is what makes an embedded
  /// browser worth having over a list widget rebuilt on the API.
  std::string BuildSelectToneUrl(const PkcePair& pkce, const std::string& redirectUri) const;

  /// Redeems an authorization code for tokens. Blocking.
  bool ExchangeCode(const std::string& code,
                    const std::string& verifier,
                    const std::string& redirectUri,
                    std::string& error);

  /// Swaps the refresh token for a fresh access token. Blocking.
  bool RefreshTokens(std::string& error);

  bool IsConnected() const;
  Tokens GetTokens() const;
  void SetTokens(Tokens tokens);
  void ClearTokens();

  /// Reads any previously saved session from the OS credential store. Call once
  /// at startup; returns false if there was nothing saved.
  bool LoadSavedSession();

  /// Forgets the saved session and signs out.
  void SignOut();

  // --- Resources ------------------------------------------------------------

  /// Searches the catalogue. Blocking.
  bool SearchTones(const SearchQuery& query, TonePage& result, std::string& error);

  /// Fetches one of the per-user listings: favourited, created or downloaded.
  ///
  /// These take only paging -- the API applies no filters to them -- so the
  /// browser's gear and sort controls are inert on these tabs.
  bool ListTones(ToneListing listing, int page, int pageSize, TonePage& result, std::string& error);

  /// One tone by id, for refreshing a card after it is starred. Blocking.
  bool GetTone(int toneId, Tone& result, std::string& error);

  /// Stars or unstars a tone for the signed-in user. Blocking.
  bool SetFavourite(int toneId, bool favourite, std::string& error);

  /// The signed-in account. Blocking.
  bool GetCurrentUser(User& result, std::string& error);

  /// Lists the downloadable models belonging to a tone. Blocking.
  bool ListModels(int toneId, std::vector<Model>& result, std::string& error);

  /// Downloads a model file to disk. The URL requires the bearer token, so it
  /// cannot be fetched with a plain unauthenticated request. Blocking.
  bool DownloadModel(const Model& model, const std::string& destinationPath, std::string& error);

private:
  /// Performs an authorised GET, refreshing the token once on a 401.
  bool AuthorisedGet(const std::string& path, std::string& responseBody, std::string& error);

  /// The general form: sends once, refreshes on 401, sends again. Favouriting
  /// replies 200 with a body and unfavouriting replies 204 with none, so an
  /// empty body is not treated as failure.
  bool SendAuthorised(const std::string& method,
                      const std::string& path,
                      std::string& responseBody,
                      std::string& error);

  std::string mPublishableKey;

  mutable std::mutex mTokenMutex;
  Tokens mTokens;
};

} // namespace nr::net
