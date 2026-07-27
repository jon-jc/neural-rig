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
  std::string gear;    ///< amp, full-rig, pedal, outboard, ir
  std::string format;  ///< nam, ir, aida-x, ...
  std::string licence; ///< t3k, cc-by, ...
  std::string author;
  std::string url;
  std::vector<std::string> makes;
  std::vector<std::string> tags;
  std::vector<std::string> sizes;
  int modelsCount = 0;
  int downloadsCount = 0;
  int favouritesCount = 0;
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
  std::string sort = "best-match";
  int architecture = 0; ///< 0 = any, otherwise 1 or 2
  int page = 1;
  int pageSize = 24;
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

  /// Lists the downloadable models belonging to a tone. Blocking.
  bool ListModels(int toneId, std::vector<Model>& result, std::string& error);

  /// Downloads a model file to disk. The URL requires the bearer token, so it
  /// cannot be fetched with a plain unauthenticated request. Blocking.
  bool DownloadModel(const Model& model, const std::string& destinationPath, std::string& error);

private:
  /// Performs an authorised GET, refreshing the token once on a 401.
  bool AuthorisedGet(const std::string& path, std::string& responseBody, std::string& error);

  std::string mPublishableKey;

  mutable std::mutex mTokenMutex;
  Tokens mTokens;
};

} // namespace nr::net
