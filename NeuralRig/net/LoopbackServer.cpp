#include "LoopbackServer.h"

#include <chrono>
#include <cstring>
#include <string>
#include <vector>

#ifdef _WIN32
  #include <winsock2.h>
  #include <ws2tcpip.h>
  #pragma comment(lib, "ws2_32.lib")
using socket_t = SOCKET;
  #define NR_INVALID_SOCKET INVALID_SOCKET
  #define NR_CLOSE_SOCKET closesocket
#else
  #include <arpa/inet.h>
  #include <netinet/in.h>
  #include <sys/socket.h>
  #include <sys/time.h>
  #include <unistd.h>
using socket_t = int;
  #define NR_INVALID_SOCKET (-1)
  #define NR_CLOSE_SOCKET close
#endif

namespace nr::net
{
namespace
{
/// Ephemeral port range to search. Staying inside the IANA dynamic range avoids
/// colliding with anything a user is likely to be running.
constexpr int kFirstPort = 49500;
constexpr int kLastPort = 49560;

/// How long a single accept() waits before we re-check the overall deadline.
constexpr int kAcceptPollMs = 250;

#ifdef _WIN32
/// Winsock needs process-wide initialisation. Reference-counted so repeated
/// sign-ins do not leave it started or tear it down under another user.
struct WinsockScope
{
  WinsockScope()
  {
    WSADATA data;
    ok = WSAStartup(MAKEWORD(2, 2), &data) == 0;
  }

  ~WinsockScope()
  {
    if (ok)
      WSACleanup();
  }

  bool ok = false;
};
#endif

const char* SuccessPage()
{
  return "HTTP/1.1 200 OK\r\n"
         "Content-Type: text/html; charset=utf-8\r\n"
         "Connection: close\r\n"
         "\r\n"
         "<!doctype html><html><head><meta charset=\"utf-8\"><title>NeuralRig</title></head>"
         "<body style=\"font-family:system-ui,sans-serif;background:#14161a;color:#e2e8f0;"
         "display:flex;align-items:center;justify-content:center;height:100vh;margin:0\">"
         "<div style=\"text-align:center\">"
         "<h1 style=\"font-weight:600\">Connected to TONE3000</h1>"
         "<p style=\"color:#8b93a1\">You can close this tab and return to your DAW.</p>"
         "</div></body></html>";
}

std::string PercentDecode(const std::string& text)
{
  std::string decoded;
  decoded.reserve(text.size());

  for (size_t i = 0; i < text.size(); i++)
  {
    if (text[i] == '+')
    {
      decoded += ' ';
    }
    else if (text[i] == '%' && i + 2 < text.size())
    {
      const auto hex = text.substr(i + 1, 2);
      decoded += static_cast<char>(strtol(hex.c_str(), nullptr, 16));
      i += 2;
    }
    else
    {
      decoded += text[i];
    }
  }

  return decoded;
}
} // namespace

std::map<std::string, std::string> ParseQueryParameters(const std::string& target)
{
  std::map<std::string, std::string> parameters;

  const auto queryStart = target.find('?');
  if (queryStart == std::string::npos)
    return parameters;

  const auto query = target.substr(queryStart + 1);

  size_t position = 0;
  while (position < query.size())
  {
    auto next = query.find('&', position);
    if (next == std::string::npos)
      next = query.size();

    const auto pair = query.substr(position, next - position);
    const auto equals = pair.find('=');

    if (equals != std::string::npos)
      parameters[PercentDecode(pair.substr(0, equals))] = PercentDecode(pair.substr(equals + 1));
    else if (!pair.empty())
      parameters[PercentDecode(pair)] = {};

    position = next + 1;
  }

  return parameters;
}

LoopbackServer::~LoopbackServer()
{
  Stop();
}

std::string LoopbackServer::Start()
{
  Stop();

#ifdef _WIN32
  static WinsockScope winsock;
  if (!winsock.ok)
    return {};
#endif

  for (int port = kFirstPort; port <= kLastPort; port++)
  {
    const socket_t handle = socket(AF_INET, SOCK_STREAM, 0);

    if (handle == NR_INVALID_SOCKET)
      continue;

    sockaddr_in address = {};
    address.sin_family = AF_INET;
    address.sin_port = htons(static_cast<uint16_t>(port));
    // Loopback only. Binding INADDR_ANY would let anything on the network
    // deliver an authorization code to us.
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    if (bind(handle, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0 && listen(handle, 1) == 0)
    {
      mListenSocket = static_cast<intptr_t>(handle);
      mPort = port;
      return "http://127.0.0.1:" + std::to_string(port) + "/callback";
    }

    NR_CLOSE_SOCKET(handle);
  }

  return {};
}

std::map<std::string, std::string> LoopbackServer::WaitForRedirect(int timeoutMs)
{
  std::map<std::string, std::string> parameters;

  if (mListenSocket == -1)
    return parameters;

  const auto listenHandle = static_cast<socket_t>(mListenSocket);
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);

  while (std::chrono::steady_clock::now() < deadline)
  {
    // Poll rather than blocking forever, so a user who abandons the sign-in
    // does not leave this thread stuck until the process exits.
    fd_set readable;
    FD_ZERO(&readable);
    FD_SET(listenHandle, &readable);

    timeval poll = {};
    poll.tv_sec = kAcceptPollMs / 1000;
    poll.tv_usec = (kAcceptPollMs % 1000) * 1000;

    const int ready = select(static_cast<int>(listenHandle) + 1, &readable, nullptr, nullptr, &poll);

    if (ready <= 0)
      continue;

    const socket_t connection = accept(listenHandle, nullptr, nullptr);

    if (connection == NR_INVALID_SOCKET)
      continue;

    // A browser sends the whole request line immediately; one read is enough to
    // see "GET /callback?... HTTP/1.1".
    char buffer[8192] = {};
    const auto received = recv(connection, buffer, sizeof(buffer) - 1, 0);

    if (received > 0)
    {
      const std::string request(buffer, static_cast<size_t>(received));
      const auto lineEnd = request.find("\r\n");
      const auto firstLine = request.substr(0, lineEnd == std::string::npos ? request.size() : lineEnd);

      // "GET /callback?code=... HTTP/1.1" -> the middle token
      const auto firstSpace = firstLine.find(' ');
      const auto lastSpace = firstLine.rfind(' ');

      if (firstSpace != std::string::npos && lastSpace != std::string::npos && lastSpace > firstSpace)
        parameters = ParseQueryParameters(firstLine.substr(firstSpace + 1, lastSpace - firstSpace - 1));
    }

    const char* page = SuccessPage();
    send(connection, page, static_cast<int>(strlen(page)), 0);
    NR_CLOSE_SOCKET(connection);

    if (!parameters.empty())
      break;
  }

  return parameters;
}

void LoopbackServer::Stop()
{
  if (mListenSocket != -1)
  {
    NR_CLOSE_SOCKET(static_cast<socket_t>(mListenSocket));
    mListenSocket = -1;
  }

  mPort = 0;
}

} // namespace nr::net
