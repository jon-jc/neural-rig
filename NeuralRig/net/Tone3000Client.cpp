#include "Tone3000Client.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <sstream>

#include "json.hpp"

namespace nr::net
{
namespace
{
constexpr int kVerifierBytes = 32; // RFC 7636 allows 32-96; 32 is ample
constexpr int kStateBytes = 16;

/// Refresh this far ahead of expiry so a request cannot be caught mid-flight by
/// a token going stale.
constexpr int64_t kRefreshMarginMs = 60000;

int64_t NowMs()
{
  using namespace std::chrono;
  return duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
}

std::string RandomBase64Url(int numBytes)
{
  std::vector<uint8_t> bytes(static_cast<size_t>(numBytes));

  if (!SecureRandomBytes(bytes.data(), bytes.size()))
    return {};

  return ToBase64Url(bytes.data(), bytes.size());
}

/// Percent-encodes a query parameter value.
std::string UrlEncode(const std::string& text)
{
  static const char* kHex = "0123456789ABCDEF";
  std::string encoded;
  encoded.reserve(text.size());

  for (const unsigned char c : text)
  {
    if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~')
    {
      encoded += static_cast<char>(c);
    }
    else
    {
      encoded += '%';
      encoded += kHex[c >> 4];
      encoded += kHex[c & 0x0F];
    }
  }

  return encoded;
}

std::string Join(const std::vector<std::string>& parts, const char* separator)
{
  std::string joined;

  for (size_t i = 0; i < parts.size(); i++)
  {
    if (i > 0)
      joined += separator;
    joined += parts[i];
  }

  return joined;
}

// --- JSON helpers -----------------------------------------------------------
// The API omits fields rather than nulling them, so every accessor tolerates
// both a missing key and an explicit null.

std::string StringFrom(const nlohmann::json& object, const char* key)
{
  if (!object.contains(key) || object[key].is_null())
    return {};
  return object[key].is_string() ? object[key].get<std::string>() : object[key].dump();
}

int IntFrom(const nlohmann::json& object, const char* key)
{
  if (!object.contains(key) || !object[key].is_number())
    return 0;
  return object[key].get<int>();
}

/// Extracts "name" from each entry of an array of objects, which is how makes
/// and tags come back.
std::vector<std::string> NamesFrom(const nlohmann::json& object, const char* key)
{
  std::vector<std::string> names;

  if (object.contains(key) && object[key].is_array())
    for (const auto& entry : object[key])
      if (entry.is_object() && entry.contains("name") && entry["name"].is_string())
        names.push_back(entry["name"].get<std::string>());

  return names;
}

std::vector<std::string> StringsFrom(const nlohmann::json& object, const char* key)
{
  std::vector<std::string> values;

  if (object.contains(key) && object[key].is_array())
    for (const auto& entry : object[key])
      if (entry.is_string())
        values.push_back(entry.get<std::string>());

  return values;
}

Tone ToneFrom(const nlohmann::json& object)
{
  Tone tone;
  tone.id = IntFrom(object, "id");
  tone.title = StringFrom(object, "title");
  tone.description = StringFrom(object, "description");
  tone.gear = StringFrom(object, "gear");
  tone.format = StringFrom(object, "format");
  tone.licence = StringFrom(object, "license");
  tone.url = StringFrom(object, "url");
  tone.modelsCount = IntFrom(object, "models_count");
  tone.downloadsCount = IntFrom(object, "downloads_count");
  tone.favouritesCount = IntFrom(object, "favorites_count");
  tone.makes = NamesFrom(object, "makes");
  tone.tags = NamesFrom(object, "tags");
  tone.sizes = StringsFrom(object, "sizes");

  if (object.contains("user") && object["user"].is_object())
    tone.author = StringFrom(object["user"], "username");

  return tone;
}

Model ModelFrom(const nlohmann::json& object)
{
  Model model;
  model.id = IntFrom(object, "id");
  model.toneId = IntFrom(object, "tone_id");
  model.name = StringFrom(object, "name");
  model.modelUrl = StringFrom(object, "model_url");
  model.size = StringFrom(object, "size");
  model.architectureVersion = StringFrom(object, "architecture_version");
  return model;
}
} // namespace

bool Tokens::IsExpired() const
{
  // No access token at all counts as expired: a session restored from the
  // credential store holds only a refresh token, and the first request must
  // exchange it before doing anything else.
  if (accessToken.empty())
    return true;

  return NowMs() > expiresAtMs - kRefreshMarginMs;
}

PkcePair CreatePkcePair()
{
  PkcePair pair;
  pair.verifier = RandomBase64Url(kVerifierBytes);
  pair.state = RandomBase64Url(kStateBytes);

  uint8_t digest[32] = {};
  Sha256(pair.verifier.data(), pair.verifier.size(), digest);
  pair.challenge = ToBase64Url(digest, sizeof(digest));

  return pair;
}

std::vector<std::string> GearOptions()
{
  return {"amp", "full-rig", "pedal", "outboard", "ir"};
}

std::vector<std::string> SizeOptions()
{
  return {"standard", "lite", "feather", "nano", "custom"};
}

std::vector<std::string> SortOptions()
{
  return {"best-match", "trending", "newest", "oldest", "downloads-all-time"};
}

Tone3000Client::Tone3000Client(std::string publishableKey)
: mPublishableKey(std::move(publishableKey))
{
}

// --- Authorisation ----------------------------------------------------------

std::string Tone3000Client::BuildAuthorizeUrl(const PkcePair& pkce, const std::string& redirectUri) const
{
  std::ostringstream url;
  url << kTone3000BaseUrl << "/api/v1/oauth/authorize"
      << "?client_id=" << UrlEncode(mPublishableKey) << "&redirect_uri=" << UrlEncode(redirectUri)
      << "&response_type=code"
      << "&code_challenge=" << UrlEncode(pkce.challenge) << "&code_challenge_method=S256"
      << "&state=" << UrlEncode(pkce.state)
      // Ask only for NAM captures: the catalogue also carries IRs and other
      // formats this plugin cannot load.
      << "&format=nam";

  return url.str();
}

bool Tone3000Client::ExchangeCode(const std::string& code,
                                  const std::string& verifier,
                                  const std::string& redirectUri,
                                  std::string& error)
{
  std::ostringstream body;
  body << "grant_type=authorization_code"
       << "&code=" << UrlEncode(code) << "&code_verifier=" << UrlEncode(verifier)
       << "&redirect_uri=" << UrlEncode(redirectUri) << "&client_id=" << UrlEncode(mPublishableKey);

  const auto response = HttpRequest("POST", std::string(kTone3000BaseUrl) + "/api/v1/oauth/token",
                                    {{"Content-Type", "application/x-www-form-urlencoded"}}, body.str());

  if (response.statusCode == 0)
  {
    error = "Could not reach TONE3000: " + response.transportError;
    return false;
  }

  if (!response.IsSuccess())
  {
    error = "Sign-in failed (HTTP " + std::to_string(response.statusCode) + ").";
    return false;
  }

  const auto parsed = nlohmann::json::parse(response.BodyAsString(), nullptr, false);

  if (parsed.is_discarded() || !parsed.is_object())
  {
    error = "TONE3000 returned an unexpected response.";
    return false;
  }

  Tokens fresh;
  fresh.accessToken = StringFrom(parsed, "access_token");
  fresh.refreshToken = StringFrom(parsed, "refresh_token");
  fresh.expiresAtMs = NowMs() + static_cast<int64_t>(IntFrom(parsed, "expires_in")) * 1000;

  if (fresh.accessToken.empty())
  {
    error = "TONE3000 did not return an access token.";
    return false;
  }

  SetTokens(std::move(fresh));
  return true;
}

bool Tone3000Client::RefreshTokens(std::string& error)
{
  std::string existingRefresh;
  {
    std::lock_guard<std::mutex> lock(mTokenMutex);
    existingRefresh = mTokens.refreshToken;
  }

  if (existingRefresh.empty())
  {
    error = "Not signed in to TONE3000.";
    return false;
  }

  std::ostringstream body;
  body << "grant_type=refresh_token"
       << "&refresh_token=" << UrlEncode(existingRefresh) << "&client_id=" << UrlEncode(mPublishableKey);

  const auto response = HttpRequest("POST", std::string(kTone3000BaseUrl) + "/api/v1/oauth/token",
                                    {{"Content-Type", "application/x-www-form-urlencoded"}}, body.str());

  if (!response.IsSuccess())
  {
    // The refresh token is spent or revoked; drop it so the UI can offer a
    // fresh sign-in rather than retrying something that cannot work.
    ClearTokens();
    error = "TONE3000 session expired. Please connect again.";
    return false;
  }

  const auto parsed = nlohmann::json::parse(response.BodyAsString(), nullptr, false);

  Tokens fresh;
  if (!parsed.is_discarded() && parsed.is_object())
  {
    fresh.accessToken = StringFrom(parsed, "access_token");
    fresh.refreshToken = StringFrom(parsed, "refresh_token");
    fresh.expiresAtMs = NowMs() + static_cast<int64_t>(IntFrom(parsed, "expires_in")) * 1000;
  }

  if (fresh.accessToken.empty())
  {
    ClearTokens();
    error = "TONE3000 session expired. Please connect again.";
    return false;
  }

  SetTokens(std::move(fresh));
  return true;
}

bool Tone3000Client::IsConnected() const
{
  std::lock_guard<std::mutex> lock(mTokenMutex);
  return !mTokens.IsEmpty();
}

Tokens Tone3000Client::GetTokens() const
{
  std::lock_guard<std::mutex> lock(mTokenMutex);
  return mTokens;
}

void Tone3000Client::SetTokens(Tokens tokens)
{
  {
    std::lock_guard<std::mutex> lock(mTokenMutex);
    mTokens = std::move(tokens);
  }

  // Persist only the refresh token. The access token is short-lived and can
  // always be re-derived, so there is no reason to widen what sits at rest.
  const auto current = GetTokens();

  if (!current.refreshToken.empty())
    SecretStore(kRefreshTokenKey, current.refreshToken);
}

void Tone3000Client::ClearTokens()
{
  std::lock_guard<std::mutex> lock(mTokenMutex);
  mTokens = {};
}

bool Tone3000Client::LoadSavedSession()
{
  std::string refreshToken;

  if (!SecretLoad(kRefreshTokenKey, refreshToken) || refreshToken.empty())
    return false;

  {
    std::lock_guard<std::mutex> lock(mTokenMutex);
    mTokens = {};
    mTokens.refreshToken = std::move(refreshToken);
    // No access token yet, and expiresAtMs of 0 marks it stale, so the first
    // request refreshes before doing anything else.
  }

  return true;
}

void Tone3000Client::SignOut()
{
  ClearTokens();
  SecretErase(kRefreshTokenKey);
}

// --- Requests ---------------------------------------------------------------

bool Tone3000Client::AuthorisedGet(const std::string& path, std::string& responseBody, std::string& error)
{
  if (!IsConnected())
  {
    error = "Not signed in to TONE3000.";
    return false;
  }

  // Refresh proactively rather than waiting for a 401, so a long browse session
  // does not stall on a token expiring mid-scroll.
  if (GetTokens().IsExpired() && !RefreshTokens(error))
    return false;

  const std::string url = std::string(kTone3000BaseUrl) + path;

  auto response = HttpRequest("GET", url, {{"Authorization", "Bearer " + GetTokens().accessToken}});

  // One retry on 401 covers the race between the expiry check above and the
  // request actually landing.
  if (response.statusCode == 401)
  {
    if (!RefreshTokens(error))
      return false;

    response = HttpRequest("GET", url, {{"Authorization", "Bearer " + GetTokens().accessToken}});
  }

  if (response.statusCode == 0)
  {
    error = "Could not reach TONE3000: " + response.transportError;
    return false;
  }

  if (!response.IsSuccess())
  {
    error = "TONE3000 request failed (HTTP " + std::to_string(response.statusCode) + ").";
    return false;
  }

  responseBody = response.BodyAsString();
  return true;
}

bool Tone3000Client::SearchTones(const SearchQuery& query, TonePage& result, std::string& error)
{
  std::ostringstream path;
  path << "/api/v1/tones/search?page=" << std::max(1, query.page)
       << "&page_size=" << std::min(100, std::max(1, query.pageSize));

  if (!query.text.empty())
    path << "&query=" << UrlEncode(query.text);

  if (!query.sort.empty())
    path << "&sort=" << UrlEncode(query.sort);

  // The API joins multi-select filters with underscores rather than repeating
  // the parameter.
  if (!query.gears.empty())
    path << "&gears=" << UrlEncode(Join(query.gears, "_"));

  if (!query.sizes.empty())
    path << "&sizes=" << UrlEncode(Join(query.sizes, "_"));

  if (query.architecture > 0)
    path << "&architecture=" << query.architecture;

  std::string body;
  if (!AuthorisedGet(path.str(), body, error))
    return false;

  const auto parsed = nlohmann::json::parse(body, nullptr, false);

  if (parsed.is_discarded() || !parsed.is_object())
  {
    error = "TONE3000 returned an unexpected response.";
    return false;
  }

  result = {};
  result.page = IntFrom(parsed, "page");
  result.pageSize = IntFrom(parsed, "page_size");
  result.total = IntFrom(parsed, "total");
  result.totalPages = IntFrom(parsed, "total_pages");

  if (parsed.contains("data") && parsed["data"].is_array())
    for (const auto& entry : parsed["data"])
      result.tones.push_back(ToneFrom(entry));

  return true;
}

bool Tone3000Client::ListModels(int toneId, std::vector<Model>& result, std::string& error)
{
  const std::string path = "/api/v1/models?tone_id=" + std::to_string(toneId) + "&page_size=50";

  std::string body;
  if (!AuthorisedGet(path, body, error))
    return false;

  const auto parsed = nlohmann::json::parse(body, nullptr, false);

  if (parsed.is_discarded() || !parsed.is_object())
  {
    error = "TONE3000 returned an unexpected response.";
    return false;
  }

  result.clear();

  if (parsed.contains("data") && parsed["data"].is_array())
    for (const auto& entry : parsed["data"])
      result.push_back(ModelFrom(entry));

  return true;
}

bool Tone3000Client::DownloadModel(const Model& model, const std::string& destinationPath, std::string& error)
{
  if (model.modelUrl.empty())
  {
    error = "That model has no downloadable file.";
    return false;
  }

  if (GetTokens().IsExpired() && !RefreshTokens(error))
    return false;

  const auto response =
    HttpRequest("GET", model.modelUrl, {{"Authorization", "Bearer " + GetTokens().accessToken}});

  if (response.statusCode == 0)
  {
    error = "Download failed: " + response.transportError;
    return false;
  }

  if (!response.IsSuccess())
  {
    error = "Download failed (HTTP " + std::to_string(response.statusCode) + ").";
    return false;
  }

  // Write to a temporary file and move it into place, so an interrupted
  // download cannot leave a truncated .nam in the cache that later looks like a
  // corrupt model.
  const std::string partialPath = destinationPath + ".part";

  {
    FILE* file = fopen(partialPath.c_str(), "wb");

    if (file == nullptr)
    {
      error = "Could not write to " + partialPath;
      return false;
    }

    const size_t written = fwrite(response.body.data(), 1, response.body.size(), file);
    fclose(file);

    if (written != response.body.size())
    {
      remove(partialPath.c_str());
      error = "Could not write the downloaded model to disk.";
      return false;
    }
  }

  remove(destinationPath.c_str());

  if (rename(partialPath.c_str(), destinationPath.c_str()) != 0)
  {
    remove(partialPath.c_str());
    error = "Could not save the downloaded model.";
    return false;
  }

  return true;
}

} // namespace nr::net
