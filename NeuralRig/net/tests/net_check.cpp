// Standalone check of the TONE3000 network layer.
//
// Compiling proves nothing about correctness here: a wrong PKCE challenge is
// accepted by the compiler and rejected by the authorization server, which
// looks like "sign-in is broken" rather than "the hash is wrong". RFC 7636
// Appendix B publishes a verifier/challenge pair, so we can check exactly.

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>

#include "Platform.h"
#include "Tone3000Client.h"

using namespace nr::net;

static int gFailures = 0;

static void Check(bool condition, const char* what)
{
  printf("  %-52s %s\n", what, condition ? "ok" : "FAILED");
  if (!condition)
    gFailures++;
}

int main()
{
  printf("PKCE (RFC 7636 Appendix B test vector)\n");
  {
    // The RFC's worked example, so this validates SHA-256 and base64url
    // together, exactly as the authorization server will compute them.
    const std::string verifier = "dBjftJeZ4CVP-mB92K27uhbUJU1p1r_wW1gFWFOEjXk";
    const std::string expected = "E9Melhoa2OwvFrEMTJguCHaoeK1t8URWbuGJSstw-cM";

    uint8_t digest[32] = {};
    Sha256(verifier.data(), verifier.size(), digest);
    const auto challenge = ToBase64Url(digest, sizeof(digest));

    printf("    expected %s\n    got      %s\n", expected.c_str(), challenge.c_str());
    Check(challenge == expected, "code_challenge matches the RFC vector");
  }

  printf("base64url\n");
  {
    // Padding must be omitted and the alphabet URL-safe; both are required by
    // RFC 7636 and are the usual places an implementation goes wrong.
    Check(ToBase64Url("f", 1) == "Zg", "1 byte, no padding");
    Check(ToBase64Url("fo", 2) == "Zm8", "2 bytes, no padding");
    Check(ToBase64Url("foo", 3) == "Zm9v", "3 bytes");
    const uint8_t highBytes[] = {0xFB, 0xFF, 0xBE};
    Check(ToBase64Url(highBytes, 3) == "-_--", "URL-safe alphabet, no + or /");
  }

  printf("CSPRNG\n");
  {
    uint8_t a[32] = {}, b[32] = {};
    const bool gotA = SecureRandomBytes(a, sizeof(a));
    const bool gotB = SecureRandomBytes(b, sizeof(b));
    Check(gotA && gotB, "SecureRandomBytes succeeds");
    Check(memcmp(a, b, sizeof(a)) != 0, "two draws differ");
  }

  printf("PKCE pair generation\n");
  {
    const auto pair = CreatePkcePair();
    Check(!pair.verifier.empty(), "verifier is populated");
    Check(!pair.challenge.empty(), "challenge is populated");
    Check(!pair.state.empty(), "state is populated");
    Check(pair.verifier != pair.challenge, "challenge is not the verifier");

    uint8_t digest[32] = {};
    Sha256(pair.verifier.data(), pair.verifier.size(), digest);
    Check(pair.challenge == ToBase64Url(digest, sizeof(digest)), "challenge is S256 of the verifier");
  }

  printf("Authorize URL\n");
  {
    Tone3000Client client;
    const auto pair = CreatePkcePair();
    const auto url = client.BuildAuthorizeUrl(pair, "http://127.0.0.1:49500/callback");

    Check(url.find("code_challenge_method=S256") != std::string::npos, "declares S256");
    Check(url.find("client_id=t3k_pub_") != std::string::npos, "carries the publishable key");
    Check(url.find("127.0.0.1%3A49500") != std::string::npos, "redirect_uri is percent-encoded");
    Check(url.find("format=nam") != std::string::npos, "asks only for NAM captures");
    Check(url.find("t3k_cs_") == std::string::npos, "does NOT leak a secret key");
  }

  printf("Request paths\n");
  {
    // These are pure string building, and a wrong parameter here compiles fine
    // and fails as a 400 at runtime, so they are worth pinning down.
    SearchQuery query;
    query.text = "vox ac30";
    query.gears = {"amp", "amp-cab"};
    query.sizes = {"standard", "lite"};
    query.format = "nam";
    query.sort = "trending";
    query.architecture = 2;
    query.calibrated = true;
    query.page = 3;

    const auto path = BuildSearchPath(query);

    Check(path.find("/api/v1/tones/search?") == 0, "hits the search endpoint");
    Check(path.find("query=vox%20ac30") != std::string::npos || path.find("query=vox+ac30") != std::string::npos,
          "percent-encodes the search text");
    Check(path.find("gears=amp_amp-cab") != std::string::npos, "joins gears with underscores");
    Check(path.find("sizes=standard_lite") != std::string::npos, "joins sizes with underscores");
    Check(path.find("format=nam") != std::string::npos, "carries the format filter");
    Check(path.find("sort=trending") != std::string::npos, "carries the sort");
    Check(path.find("architecture=2") != std::string::npos, "carries the architecture");
    Check(path.find("calibrated=true") != std::string::npos, "carries calibrated when set");
    Check(path.find("page=3") != std::string::npos, "carries the page");

    // The search endpoint caps page_size at 25 while the listings allow 100.
    SearchQuery greedy;
    greedy.pageSize = 100;
    Check(BuildSearchPath(greedy).find("page_size=25") != std::string::npos,
          "clamps page_size to the search maximum");

    // calibrated filters rather than toggles, so it must be absent when off.
    SearchQuery plain;
    Check(BuildSearchPath(plain).find("calibrated") == std::string::npos,
          "omits calibrated entirely when it is off");

    Check(BuildListingPath(ToneListing::Favourited, 1, 10).find("/api/v1/tones/favorited?") == 0,
          "favourites listing uses the American spelling the API does");
    Check(BuildListingPath(ToneListing::Created, 1, 10).find("/api/v1/tones/created?") == 0,
          "created listing");
    Check(BuildListingPath(ToneListing::Downloaded, 1, 10).find("/api/v1/tones/downloaded?") == 0,
          "downloaded listing");
    Check(BuildListingPath(ToneListing::Search, 1, 10).empty(),
          "search has no listing endpoint of its own");
    Check(BuildListingPath(ToneListing::Created, 1, 500).find("page_size=100") != std::string::npos,
          "clamps listing page_size to 100");
  }

  printf("Gear vocabulary\n");
  {
    const auto gears = GearOptions();
    const auto has = [&](const char* v) {
      return std::find(gears.begin(), gears.end(), v) != gears.end();
    };

    // "full-rig" and "ir" are deprecated aliases. Sending "ir" as a gear was
    // silently reinterpreted as a format filter, which is why it appeared to
    // match far more than it should.
    Check(has("amp-cab"), "uses amp-cab");
    Check(has("cab"), "offers cab, which the rig needs for its IR slot");
    Check(!has("full-rig"), "no deprecated full-rig");
    Check(!has("ir"), "no deprecated ir");
  }

  printf("Credential store\n");
  {
    // Round-trip through the real OS store. A silently broken path here would
    // sign the user out every session, which reads as "it forgot me" rather
    // than as a bug.
    const std::string key = "net_check_scratch";
    const std::string secret = "refresh-token-\xE2\x9C\x93-with-unicode";

    Check(SecretStore(key, secret), "store a secret");

    std::string loaded;
    Check(SecretLoad(key, loaded), "load it back");
    Check(loaded == secret, "value survives the round trip");

    Check(SecretErase(key), "erase it");

    std::string afterErase;
    Check(!SecretLoad(key, afterErase), "gone after erase");
  }

  printf("Restored session semantics\n");
  {
    // A session restored from the credential store has a refresh token but no
    // access token. It must still count as signed in, and must look expired so
    // the first request exchanges the refresh token.
    Tokens restored;
    restored.refreshToken = "some-refresh-token";

    Check(!restored.IsEmpty(), "refresh token alone counts as signed in");
    Check(restored.IsExpired(), "missing access token counts as expired");

    Tokens empty;
    Check(empty.IsEmpty(), "genuinely empty tokens are empty");
  }

  printf("HTTPS transport (live)\n");
  {
    // Unauthenticated, so a 401 is the expected answer. What matters is that we
    // completed a TLS handshake and got a real HTTP status rather than a
    // transport failure.
    const auto response = HttpRequest("GET", std::string(kTone3000BaseUrl) + "/api/v1/tones/search?page_size=1");

    printf("    status %d  transport error: %s\n", response.statusCode,
           response.transportError.empty() ? "(none)" : response.transportError.c_str());

    Check(response.statusCode != 0, "reached the server over TLS");
    Check(response.statusCode == 401 || response.statusCode == 403 || response.IsSuccess(),
          "got a plausible API status");
  }

  printf("\n%s (%d failure(s))\n", gFailures == 0 ? "ALL PASSED" : "FAILURES", gFailures);
  return gFailures == 0 ? 0 : 1;
}
