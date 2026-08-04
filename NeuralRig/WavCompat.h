#pragma once

/**
    Makes valid-but-unusual WAV files loadable.

    AudioDSPTools' reader is stricter than the WAV spec in two ways that real
    impulse responses hit constantly:

      1. It refuses WAVE_FORMAT_EXTENSIBLE (0xFFFE) unless the file also carries
         a `fact` chunk -- "Tried to read data chunk before fact chunk". `fact`
         is required for *compressed* formats, not for extensible PCM, so this
         rejects perfectly ordinary files.
      2. It refuses any `fmt ` chunk with more than 4 bytes beyond the standard
         16 -- "More than 4 extra bytes in fmt chunk" -- which every extensible
         header has, since the extension block alone is 24 bytes.

    A great many commercial IR packs export exactly this: extensible, 24-bit,
    no fact chunk. Both restrictions are in a submodule, so fixing them there
    would never reach CI or a clean checkout.

    The conversion is lossless and touches only the header. An extensible
    header carries a sub-format GUID whose first field is the real format code
    (1 = PCM, 3 = IEEE float); the file is rewritten with that as its
    audioFormat and a plain 16-byte `fmt ` chunk. Sample data is copied
    untouched, so the audio is bit-identical.
*/

#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace nr::wavcompat
{

/// Rewrites an extensible-format WAV as plain PCM beside the original.
///
/// Returns the path that should be loaded: the rewritten file when a fix was
/// needed and succeeded, otherwise the original path unchanged. Never modifies
/// the source, which may be a file from the user's own library.
inline std::string MakeLoadable(const std::string& path)
{
  constexpr uint16_t kExtensible = 0xFFFE;
  constexpr uint32_t kStandardFmtSize = 16;

  std::ifstream in(std::filesystem::u8path(path), std::ios::binary);

  if (!in)
    return path;

  const std::vector<char> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  in.close();

  if (bytes.size() < 44 || std::memcmp(bytes.data(), "RIFF", 4) != 0 || std::memcmp(bytes.data() + 8, "WAVE", 4) != 0)
    return path;

  const auto chunkSizeAt = [&bytes](size_t at) {
    uint32_t size = 0;
    std::memcpy(&size, bytes.data() + at + 4, 4);
    return size;
  };

  // Locate the fmt chunk and note whether a fact chunk is present. A file that
  // already has one loads fine, so leave it alone.
  size_t fmtAt = 0;
  bool hasFact = false;

  for (size_t pos = 12; pos + 8 <= bytes.size();)
  {
    const uint32_t size = chunkSizeAt(pos);

    if (std::memcmp(bytes.data() + pos, "fmt ", 4) == 0)
      fmtAt = pos;
    else if (std::memcmp(bytes.data() + pos, "fact", 4) == 0)
      hasFact = true;

    // Chunks are word-aligned: an odd size is followed by a pad byte.
    pos += 8 + size + (size % 2);
  }

  if (fmtAt == 0)
    return path;

  const uint32_t fmtSize = chunkSizeAt(fmtAt);

  if (fmtAt + 8 + fmtSize > bytes.size())
    return path;

  uint16_t format = 0;
  std::memcpy(&format, bytes.data() + fmtAt + 8, 2);

  // Anything over the standard 16 bytes trips the reader, whether or not the
  // file is extensible: a Broadcast WAVE with a plain PCM header and a small
  // extension fails on "More than 4 extra bytes in fmt chunk" exactly like an
  // extensible one does. So the test is the chunk's size, not its format.
  const bool oversizedFmt = fmtSize > kStandardFmtSize;
  const bool needsFactWorkaround = format == kExtensible && !hasFact;

  if (!oversizedFmt && !needsFactWorkaround)
    return path;

  uint16_t resolvedFormat = format;

  if (format == kExtensible)
  {
    // The sub-format GUID's first field is the real format code. Without the
    // full extension block there is nothing to resolve it to.
    if (fmtSize < 40)
      return path;

    std::memcpy(&resolvedFormat, bytes.data() + fmtAt + 8 + kStandardFmtSize + 8, 2);
  }

  // 1 is PCM and 3 is IEEE float. Anything else is genuinely compressed, and
  // rewriting its header would misrepresent the data.
  if (resolvedFormat != 1 && resolvedFormat != 3)
    return path;

  const uint16_t subFormat = resolvedFormat;

  std::vector<char> fixed;
  fixed.reserve(bytes.size());

  const auto append = [&fixed](const void* data, size_t size) {
    const auto* p = static_cast<const char*>(data);
    fixed.insert(fixed.end(), p, p + size);
  };

  append("RIFF", 4);
  const size_t riffSizeAt = fixed.size();
  append("\0\0\0\0", 4); // patched once the length is known
  append("WAVE", 4);

  for (size_t pos = 12; pos + 8 <= bytes.size();)
  {
    const uint32_t size = chunkSizeAt(pos);
    const size_t advance = 8 + size + (size % 2);

    if (pos == fmtAt)
    {
      // A standard 16-byte fmt chunk: the real format code, then the fourteen
      // bytes after it, and none of the extension.
      append("fmt ", 4);
      append(&kStandardFmtSize, 4);
      append(&subFormat, 2);
      append(bytes.data() + fmtAt + 10, kStandardFmtSize - 2);
    }
    else if (pos + advance <= bytes.size())
    {
      append(bytes.data() + pos, advance);
    }
    else
    {
      // Truncated trailing chunk; copy what is actually there.
      append(bytes.data() + pos, bytes.size() - pos);
    }

    pos += advance;
  }

  const uint32_t riffSize = static_cast<uint32_t>(fixed.size() - 8);
  std::memcpy(fixed.data() + riffSizeAt, &riffSize, 4);

  const auto fixedPath = std::filesystem::u8path(path).replace_extension(".pcm.wav");

  std::ofstream out(fixedPath, std::ios::binary);

  if (!out)
    return path;

  out.write(fixed.data(), static_cast<std::streamsize>(fixed.size()));

  return out.good() ? fixedPath.string() : path;
}

} // namespace nr::wavcompat
