#include "Platform.h"

#ifdef _WIN32

  #include <windows.h>

  #include <bcrypt.h>
  #include <knownfolders.h>
  #include <shellapi.h>
  #include <shlobj.h>
  #include <wincred.h>
  #include <winhttp.h>

  #include <memory>

  #pragma comment(lib, "winhttp.lib")
  #pragma comment(lib, "bcrypt.lib")
  #pragma comment(lib, "shell32.lib")
  #pragma comment(lib, "credui.lib")
  #pragma comment(lib, "advapi32.lib")

namespace nr::net
{
namespace
{
constexpr const wchar_t* kUserAgent = L"NeuralRig";

// TONE3000's API is not slow, but a stalled connection should not hang a
// worker thread indefinitely.
constexpr int kResolveTimeoutMs = 10000;
constexpr int kConnectTimeoutMs = 15000;
constexpr int kSendTimeoutMs = 30000;
constexpr int kReceiveTimeoutMs = 60000;

std::wstring Widen(const std::string& text)
{
  if (text.empty())
    return {};

  const int needed = MultiByteToWideChar(CP_UTF8, 0, text.c_str(), (int)text.size(), nullptr, 0);
  std::wstring wide(needed, L'\0');
  MultiByteToWideChar(CP_UTF8, 0, text.c_str(), (int)text.size(), wide.data(), needed);
  return wide;
}

/// RAII for the WinHTTP handle family, which all close the same way.
struct HInternetDeleter
{
  void operator()(HINTERNET handle) const
  {
    if (handle != nullptr)
      WinHttpCloseHandle(handle);
  }
};

using ScopedHInternet = std::unique_ptr<void, HInternetDeleter>;

std::string LastErrorText(const char* stage)
{
  return std::string(stage) + " failed (WinHTTP error " + std::to_string(GetLastError()) + ")";
}
} // namespace

void Sha256(const void* data, size_t numBytes, uint8_t digest[32])
{
  memset(digest, 0, 32);

  BCRYPT_ALG_HANDLE algorithm = nullptr;
  if (!BCRYPT_SUCCESS(BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0)))
    return;

  BCryptHash(algorithm, nullptr, 0, (PUCHAR)data, (ULONG)numBytes, digest, 32);
  BCryptCloseAlgorithmProvider(algorithm, 0);
}

bool SecureRandomBytes(void* buffer, size_t numBytes)
{
  return BCRYPT_SUCCESS(
    BCryptGenRandom(nullptr, (PUCHAR)buffer, (ULONG)numBytes, BCRYPT_USE_SYSTEM_PREFERRED_RNG));
}

namespace
{
/// Credential Manager is a flat global namespace, so entries are prefixed to
/// keep NeuralRig's out of everyone else's way.
std::wstring CredentialTarget(const std::string& key)
{
  return Widen("NeuralRig/" + key);
}
} // namespace

std::string UserDataDirectory()
{
  PWSTR localAppData = nullptr;
  if (FAILED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &localAppData)))
    return {};

  const int needed =
    WideCharToMultiByte(CP_UTF8, 0, localAppData, -1, nullptr, 0, nullptr, nullptr);
  std::string path(needed > 0 ? needed - 1 : 0, '\0');
  WideCharToMultiByte(CP_UTF8, 0, localAppData, -1, path.data(), needed, nullptr, nullptr);
  CoTaskMemFree(localAppData);

  path += "\\NeuralRig";
  return EnsureDirectory(path) ? path : std::string{};
}

bool SecretStore(const std::string& key, const std::string& value)
{
  const auto target = CredentialTarget(key);

  CREDENTIALW credential = {};
  credential.Type = CRED_TYPE_GENERIC;
  credential.TargetName = const_cast<LPWSTR>(target.c_str());
  credential.CredentialBlobSize = (DWORD)value.size();
  credential.CredentialBlob = (LPBYTE)value.data();
  // LOCAL_MACHINE rather than ENTERPRISE: this must not roam to other machines
  // via a domain profile.
  credential.Persist = CRED_PERSIST_LOCAL_MACHINE;

  return CredWriteW(&credential, 0) == TRUE;
}

bool SecretLoad(const std::string& key, std::string& value)
{
  const auto target = CredentialTarget(key);

  PCREDENTIALW credential = nullptr;
  if (CredReadW(target.c_str(), CRED_TYPE_GENERIC, 0, &credential) != TRUE)
    return false;

  value.assign((const char*)credential->CredentialBlob, credential->CredentialBlobSize);
  CredFree(credential);
  return true;
}

bool SecretErase(const std::string& key)
{
  const auto target = CredentialTarget(key);
  return CredDeleteW(target.c_str(), CRED_TYPE_GENERIC, 0) == TRUE;
}

bool OpenUrlInBrowser(const std::string& url)
{
  const auto wide = Widen(url);
  const auto result = ShellExecuteW(nullptr, L"open", wide.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
  // ShellExecute returns a value > 32 on success. Yes, really.
  return (INT_PTR)result > 32;
}

void SetUrlOpener(std::function<bool(const std::string& url)>)
{
  // Nothing to install. Windows can open a URL from anywhere, so
  // OpenUrlInBrowser above is always able to do the work itself. The hook
  // exists for iOS, where only the containing app may open a URL.
}

HttpResponse HttpRequest(const std::string& method,
                         const std::string& url,
                         const std::vector<Header>& headers,
                         const std::string& body)
{
  HttpResponse response;

  const auto wideUrl = Widen(url);

  URL_COMPONENTS components = {};
  components.dwStructSize = sizeof(components);
  components.dwSchemeLength = (DWORD)-1;
  components.dwHostNameLength = (DWORD)-1;
  components.dwUrlPathLength = (DWORD)-1;
  components.dwExtraInfoLength = (DWORD)-1;

  if (!WinHttpCrackUrl(wideUrl.c_str(), (DWORD)wideUrl.size(), 0, &components))
  {
    response.transportError = "Malformed URL";
    return response;
  }

  const std::wstring host(components.lpszHostName, components.dwHostNameLength);
  std::wstring target(components.lpszUrlPath, components.dwUrlPathLength);
  if (components.dwExtraInfoLength > 0)
    target.append(components.lpszExtraInfo, components.dwExtraInfoLength);

  ScopedHInternet session(WinHttpOpen(kUserAgent, WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                                      WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0));
  if (!session)
  {
    response.transportError = LastErrorText("WinHttpOpen");
    return response;
  }

  WinHttpSetTimeouts(session.get(), kResolveTimeoutMs, kConnectTimeoutMs, kSendTimeoutMs, kReceiveTimeoutMs);

  ScopedHInternet connection(WinHttpConnect(session.get(), host.c_str(), components.nPort, 0));
  if (!connection)
  {
    response.transportError = LastErrorText("WinHttpConnect");
    return response;
  }

  const DWORD requestFlags = (components.nScheme == INTERNET_SCHEME_HTTPS) ? WINHTTP_FLAG_SECURE : 0;

  ScopedHInternet request(WinHttpOpenRequest(connection.get(), Widen(method).c_str(), target.c_str(), nullptr,
                                             WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, requestFlags));
  if (!request)
  {
    response.transportError = LastErrorText("WinHttpOpenRequest");
    return response;
  }

  std::wstring headerBlock;
  for (const auto& header : headers)
    headerBlock += Widen(header.name + ": " + header.value) + L"\r\n";

  if (!WinHttpSendRequest(request.get(),
                          headerBlock.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : headerBlock.c_str(),
                          headerBlock.empty() ? 0 : (DWORD)-1,
                          body.empty() ? WINHTTP_NO_REQUEST_DATA : (LPVOID)body.data(),
                          (DWORD)body.size(), (DWORD)body.size(), 0))
  {
    response.transportError = LastErrorText("WinHttpSendRequest");
    return response;
  }

  if (!WinHttpReceiveResponse(request.get(), nullptr))
  {
    response.transportError = LastErrorText("WinHttpReceiveResponse");
    return response;
  }

  DWORD status = 0;
  DWORD statusSize = sizeof(status);
  WinHttpQueryHeaders(request.get(), WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                      WINHTTP_HEADER_NAME_BY_INDEX, &status, &statusSize, WINHTTP_NO_HEADER_INDEX);
  response.statusCode = (int)status;

  // Model files run to hundreds of kilobytes, so read until the stream ends
  // rather than trusting Content-Length.
  for (;;)
  {
    DWORD available = 0;
    if (!WinHttpQueryDataAvailable(request.get(), &available) || available == 0)
      break;

    const size_t offset = response.body.size();
    response.body.resize(offset + available);

    DWORD read = 0;
    if (!WinHttpReadData(request.get(), response.body.data() + offset, available, &read))
    {
      response.body.resize(offset);
      break;
    }

    response.body.resize(offset + read);

    if (read == 0)
      break;
  }

  return response;
}

} // namespace nr::net

#endif // _WIN32
