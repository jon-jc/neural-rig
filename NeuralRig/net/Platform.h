#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

/**
    The small platform surface the TONE3000 integration needs.

    iPlug2 provides none of this. Its bundled jnetlib speaks plain HTTP with no
    TLS, and WDL's sha.h implements SHA-1 rather than the SHA-256 that PKCE
    requires. Rather than vendoring a TLS stack or hand-rolling crypto, both
    are taken from the operating system:

        Windows   WinHTTP + CNG (bcrypt)
        macOS     NSURLSession + CommonCrypto

    Implementations live in PlatformWin.cpp and PlatformMac.mm.
*/
namespace nr::net
{

/// SHA-256 digest of a buffer. Used for the PKCE code challenge.
void Sha256(const void* data, size_t numBytes, uint8_t digest[32]);

/// Base64url without padding, as RFC 7636 requires.
std::string ToBase64Url(const void* data, size_t numBytes);

/// Cryptographically secure random bytes, for the PKCE verifier and state.
bool SecureRandomBytes(void* buffer, size_t numBytes);

/// One HTTP header, as sent.
struct Header
{
  std::string name;
  std::string value;
};

struct HttpResponse
{
  /// HTTP status, or 0 if the request never reached the server.
  int statusCode = 0;
  std::vector<uint8_t> body;
  /// Populated only when statusCode is 0, describing the transport failure.
  std::string transportError;

  bool IsSuccess() const { return statusCode >= 200 && statusCode < 300; }
  std::string BodyAsString() const { return std::string(body.begin(), body.end()); }
};

/**
    Performs a blocking HTTPS request. Never call from the audio thread, and
    never from the UI thread either -- these take as long as the network does.

    @param method   "GET" or "POST"
    @param url      absolute https:// URL
    @param headers  additional request headers
    @param body     request body, empty for GET
*/
HttpResponse HttpRequest(const std::string& method,
                         const std::string& url,
                         const std::vector<Header>& headers = {},
                         const std::string& body = {});

/// Opens a URL in the user's default browser. Used to start the OAuth flow so
/// the plugin never handles the user's password.
bool OpenUrlInBrowser(const std::string& url);

} // namespace nr::net
