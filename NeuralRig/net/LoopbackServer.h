#pragma once

#include <map>
#include <string>

// TARGET_OS_IPHONE is only defined once this is included. Without it the iOS
// guard below would quietly evaluate false everywhere, including on iOS.
#if defined(__APPLE__)
  #include <TargetConditionals.h>
#endif

namespace nr::net
{

/**
    A one-shot HTTP listener on 127.0.0.1 that catches the OAuth redirect.

    TONE3000 always permits loopback redirect URIs, so nothing has to be
    registered for this to work. The browser the user already trusts handles the
    login; the plugin never sees their password, only the authorization code
    that comes back.

    The listener answers exactly one request, replies with a small page telling
    the user they can return to their DAW, and closes. It binds to 127.0.0.1
    explicitly rather than INADDR_ANY -- a listener on 0.0.0.0 would accept the
    authorization code from anything on the network.
*/
class LoopbackServer
{
public:
  LoopbackServer() = default;
  ~LoopbackServer();

  LoopbackServer(const LoopbackServer&) = delete;
  LoopbackServer& operator=(const LoopbackServer&) = delete;

  /// Binds to a free loopback port.
  /// @returns the redirect URI to hand the authorization server, or an empty
  ///          string if no port could be bound.
  std::string Start();

  /// Blocks until the browser hits the redirect, or the timeout expires.
  /// Worker thread only. Returns the query parameters, empty on timeout.
  std::map<std::string, std::string> WaitForRedirect(int timeoutMs);

  void Stop();

  bool IsRunning() const { return mListenSocket != -1; }

private:
  // Stored as a plain int so the header does not drag in winsock or
  // sys/socket.h; the .cpp casts as needed.
  intptr_t mListenSocket = -1;
  int mPort = 0;
};

/// Splits the query string of a URL or request target into key/value pairs,
/// percent-decoding both.
std::map<std::string, std::string> ParseQueryParameters(const std::string& target);

#if defined(__APPLE__) && TARGET_OS_IPHONE

/// The redirect URI the iOS build hands the authorization server. TONE3000
/// documents a deep link as the redirect for native apps. It has to match the
/// CFBundleURLSchemes entry in the app's Info.plist, and it has to be
/// registered in the TONE3000 account settings if any URIs are registered
/// there at all -- once the list is non-empty, only listed URIs are accepted.
inline constexpr const char* kUrlSchemeRedirectUri = "neuralrig://oauth";

/**
    Catches the OAuth redirect on iOS.

    A loopback listener cannot be used here. Opening the authorization page
    sends the app to the background, where it stops accepting connections, so
    the redirect would arrive at a socket nobody is reading. Worse, the AUv3
    extension may not open a URL at all.

    So the flow inverts. The containing app presents the authorization page
    itself -- keeping itself in the foreground -- and hands the callback URL
    here through Deliver when the session completes. This class only has to
    park the worker thread until that happens, which makes it plain portable
    C++ with no socket and no Objective-C.

    The interface deliberately matches LoopbackServer so BrowserController does
    not care which one it is holding.
*/
class UrlSchemeRedirect
{
public:
  /// @returns the deep-link redirect URI to hand the authorization server.
  std::string Start();

  /// Blocks until the app delivers a callback, Stop is called, or the timeout
  /// expires. Worker thread only. Returns the query parameters, empty if no
  /// callback arrived.
  std::map<std::string, std::string> WaitForRedirect(int timeoutMs);

  void Stop();

  bool IsRunning() const { return mRunning; }

  /// Hands a received callback URL to whichever thread is waiting. Called by
  /// the containing app from the main thread when the authentication session
  /// finishes. Safe to call with nobody waiting; the result is dropped.
  static void Deliver(const std::string& callbackUrl);

private:
  bool mRunning = false;
};

using RedirectListener = UrlSchemeRedirect;

#else

using RedirectListener = LoopbackServer;

#endif

} // namespace nr::net
