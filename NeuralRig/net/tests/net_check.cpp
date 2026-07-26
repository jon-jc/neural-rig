// Standalone check of the TONE3000 network layer.
//
// Compiling proves nothing about correctness here: a wrong PKCE challenge is
// accepted by the compiler and rejected by the authorization server, which
// looks like "sign-in is broken" rather than "the hash is wrong". RFC 7636
// Appendix B publishes a verifier/challenge pair, so we can check exactly.

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
