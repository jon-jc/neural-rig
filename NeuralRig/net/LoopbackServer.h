#pragma once

#include <map>
#include <string>

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

} // namespace nr::net
