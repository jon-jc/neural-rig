#include "Platform.h"

namespace nr::net
{
namespace
{
// RFC 4648 §5: the URL-safe alphabet, '+' and '/' replaced by '-' and '_'.
constexpr char kBase64UrlAlphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
} // namespace

std::string ToBase64Url(const void* data, size_t numBytes)
{
  const auto* bytes = static_cast<const uint8_t*>(data);
  std::string encoded;
  encoded.reserve(((numBytes + 2) / 3) * 4);

  size_t i = 0;
  for (; i + 2 < numBytes; i += 3)
  {
    const uint32_t triple = (uint32_t(bytes[i]) << 16) | (uint32_t(bytes[i + 1]) << 8) | bytes[i + 2];
    encoded += kBase64UrlAlphabet[(triple >> 18) & 0x3F];
    encoded += kBase64UrlAlphabet[(triple >> 12) & 0x3F];
    encoded += kBase64UrlAlphabet[(triple >> 6) & 0x3F];
    encoded += kBase64UrlAlphabet[triple & 0x3F];
  }

  // RFC 7636 requires the padding be omitted, so the tail cases just stop
  // early rather than emitting '='.
  if (i + 1 == numBytes)
  {
    const uint32_t triple = uint32_t(bytes[i]) << 16;
    encoded += kBase64UrlAlphabet[(triple >> 18) & 0x3F];
    encoded += kBase64UrlAlphabet[(triple >> 12) & 0x3F];
  }
  else if (i + 2 == numBytes)
  {
    const uint32_t triple = (uint32_t(bytes[i]) << 16) | (uint32_t(bytes[i + 1]) << 8);
    encoded += kBase64UrlAlphabet[(triple >> 18) & 0x3F];
    encoded += kBase64UrlAlphabet[(triple >> 12) & 0x3F];
    encoded += kBase64UrlAlphabet[(triple >> 6) & 0x3F];
  }

  return encoded;
}

} // namespace nr::net
