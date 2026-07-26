# Network layer checks

`net_check.cpp` verifies the parts of the TONE3000 integration that compile
cleanly whether or not they are correct:

- **PKCE code challenge** against the RFC 7636 Appendix B test vector. This is
  the important one. A wrong challenge is accepted by the compiler and rejected
  by the authorization server, so the symptom is "sign-in is broken" rather than
  "the hash is wrong".
- **base64url** padding and alphabet, the usual places an implementation slips.
- **CSPRNG** returns bytes and does not repeat.
- **Authorize URL** declares `S256`, carries the publishable key, percent-encodes
  the redirect URI, and — asserted explicitly — contains no secret key.
- **HTTPS transport**, live against tone3000.com. Unauthenticated, so `401` is
  the expected answer; what matters is completing a TLS handshake and getting a
  real HTTP status rather than a transport failure.

Deliberately not wired into CI: the last check depends on an external service
being reachable, and a red build caused by someone else's outage teaches you
nothing.

## Running it

Windows, from a Developer Command Prompt:

```bat
cl /nologo /EHsc /std:c++17 ^
  /I ..\ /I ..\..\..\NeuralAmpModelerCore\Dependencies\nlohmann ^
  net_check.cpp ..\PlatformWin.cpp ..\PlatformCommon.cpp ..\Tone3000Client.cpp ^
  /Fe:net_check.exe && net_check.exe
```

macOS:

```bash
clang++ -std=c++17 -ObjC++ \
  -I.. -I../../../NeuralAmpModelerCore/Dependencies/nlohmann \
  net_check.cpp ../PlatformMac.mm ../PlatformCommon.cpp ../Tone3000Client.cpp \
  -framework Foundation -framework AppKit -framework Security \
  -o net_check && ./net_check
```
