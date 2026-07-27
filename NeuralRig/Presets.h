#pragma once

/**
    Chain presets: the whole rig saved under a name.

    A capture is one amp. A preset is the pedal, the amp, the cab, the tone
    stack and the levels together -- the thing you actually dial in and want
    back tomorrow.

    Storage reuses the plugin's own SerializeState rather than inventing a
    second format. That serialization already handles the slot paths, the IR
    and every parameter, and it is the code the host exercises on every session
    save, so it is the best-tested path in the plugin. A separate preset format
    would be a second thing to keep in step with the parameter list, and it
    would drift the first time someone added a parameter.

    Files live in the user data directory, one per preset, so they survive
    reinstalls and can be copied between machines by hand.
*/

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

#include "IPlugStructs.h"
#include "net/Platform.h"

namespace nr::presets
{

/// Extension for a saved chain. Not .nam: a preset is not a capture, and
/// letting the two share an extension would invite dropping one where the
/// other is expected.
inline constexpr const char* kPresetExtension = ".nrig";

inline std::string PresetDirectory()
{
  const auto directory = nr::net::UserDataDirectory() + "/presets";
  nr::net::EnsureDirectory(directory);
  return directory;
}

/// Strips anything that cannot go in a filename. Users type preset names, and
/// a name with a slash in it would otherwise write outside the preset folder.
inline std::string SanitiseName(const std::string& name)
{
  std::string safe;
  safe.reserve(name.size());

  for (const char c : name)
  {
    const bool allowed = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == ' '
                         || c == '-' || c == '_' || c == '.';
    safe += allowed ? c : '_';
  }

  // Leading dots would hide the file on Unix and a trailing dot is invalid on
  // Windows, so trim both ends rather than producing something unopenable.
  while (!safe.empty() && (safe.front() == '.' || safe.front() == ' '))
    safe.erase(safe.begin());

  while (!safe.empty() && (safe.back() == '.' || safe.back() == ' '))
    safe.pop_back();

  return safe.empty() ? "Untitled" : safe;
}

inline std::string PathFor(const std::string& name)
{
  return PresetDirectory() + "/" + SanitiseName(name) + kPresetExtension;
}

/// Every saved preset, by display name, sorted so prev/next is predictable.
inline std::vector<std::string> ListPresets()
{
  std::vector<std::string> names;
  std::error_code ec;

  for (const auto& entry : std::filesystem::directory_iterator(PresetDirectory(), ec))
  {
    if (!entry.is_regular_file(ec) || entry.path().extension() != kPresetExtension)
      continue;

    names.push_back(entry.path().stem().string());
  }

  std::sort(names.begin(), names.end());
  return names;
}

/// Writes a serialized rig to disk. Via a temporary file so an interrupted
/// write cannot leave a truncated preset that later fails to load.
inline bool Save(const std::string& name, const iplug::IByteChunk& chunk)
{
  const auto path = PathFor(name);
  const auto partial = path + ".part";

  FILE* file = fopen(partial.c_str(), "wb");

  if (file == nullptr)
    return false;

  const int size = chunk.Size();
  const size_t written = size > 0 ? fwrite(chunk.GetData(), 1, static_cast<size_t>(size), file) : 0;
  fclose(file);

  if (written != static_cast<size_t>(size))
  {
    remove(partial.c_str());
    return false;
  }

  remove(path.c_str());

  if (rename(partial.c_str(), path.c_str()) != 0)
  {
    remove(partial.c_str());
    return false;
  }

  return true;
}

/// Reads a preset back into a chunk ready for UnserializeState.
inline bool Load(const std::string& name, iplug::IByteChunk& chunk)
{
  const auto path = PathFor(name);

  FILE* file = fopen(path.c_str(), "rb");

  if (file == nullptr)
    return false;

  fseek(file, 0, SEEK_END);
  const long size = ftell(file);
  fseek(file, 0, SEEK_SET);

  if (size <= 0)
  {
    fclose(file);
    return false;
  }

  std::vector<unsigned char> bytes(static_cast<size_t>(size));
  const size_t read = fread(bytes.data(), 1, bytes.size(), file);
  fclose(file);

  if (read != bytes.size())
    return false;

  chunk.Resize(0);
  chunk.PutBytes(bytes.data(), static_cast<int>(bytes.size()));
  return true;
}

inline bool Delete(const std::string& name)
{
  return remove(PathFor(name).c_str()) == 0;
}

/**
    The preset bar in the header: current name, step buttons, and a menu.

    Stepping matters more than the menu. Auditioning presets is a comparison
    task -- you want the next one now, not a menu, a read and a click -- so
    prev/next are first-class buttons and the list is behind the name.
*/
class PresetBarControl : public iplug::igraphics::IControl
{
public:
  using NameAction = std::function<void(const std::string& name)>;

  PresetBarControl(const iplug::igraphics::IRECT& bounds, NameAction onSave, NameAction onLoad)
  : IControl(bounds)
  , mOnSave(std::move(onSave))
  , mOnLoad(std::move(onLoad))
  {
    Refresh();
  }

  /// Re-reads the directory. Called after a save, and once at startup.
  void Refresh()
  {
    mNames = ListPresets();
    SetDirty(false);
  }

  void OnResize() override { LayOut(); }
  void OnAttached() override { LayOut(); }

  void Draw(iplug::igraphics::IGraphics& g) override
  {
    using namespace iplug::igraphics;

    g.FillRoundRect(PluginColors::WELL, mRECT, 6.f);
    g.DrawRoundRect(PluginColors::PANEL_HI, mRECT, 6.f);

    const IText name(14.f, mCurrent.empty() ? PluginColors::INK_DIM : PluginColors::INK, nullptr, EAlign::Center,
                     EVAlign::Middle);
    g.DrawText(name, mCurrent.empty() ? "PRESETS" : mCurrent.c_str(), mNameRect);

    DrawChevron(g, mPrevRect, false, mIndex > 0);
    DrawChevron(g, mNextRect, true, !mNames.empty() && mIndex + 1 < static_cast<int>(mNames.size()));

    const IText save(11.f, PluginColors::AMBER, nullptr, EAlign::Center, EVAlign::Middle);
    g.DrawText(save, "SAVE", mSaveRect);
  }

  void OnMouseDown(float x, float y, const iplug::igraphics::IMouseMod& mod) override
  {
    if (mSaveRect.Contains(x, y))
    {
      const iplug::igraphics::IText entry(14.f, PluginColors::INK);
      GetUI()->CreateTextEntry(*this, entry, mNameRect, mCurrent.empty() ? "My Rig" : mCurrent.c_str(), kSaveValIdx);
      return;
    }

    if (mPrevRect.Contains(x, y))
    {
      Step(-1);
      return;
    }

    if (mNextRect.Contains(x, y))
    {
      Step(1);
      return;
    }

    if (mNameRect.Contains(x, y))
      ShowMenu();
  }

  void OnTextEntryCompletion(const char* txt, int valIdx) override
  {
    if (valIdx != kSaveValIdx || txt == nullptr || *txt == '\0')
      return;

    const std::string name = SanitiseName(txt);

    if (mOnSave)
      mOnSave(name);

    Refresh();
    SelectByName(name);
  }

  void OnPopupMenuSelection(iplug::igraphics::IPopupMenu* pMenu, int valIdx) override
  {
    if (pMenu == nullptr || pMenu->GetChosenItemIdx() < 0)
      return;

    const int chosen = pMenu->GetChosenItemIdx();

    if (chosen < 0 || chosen >= static_cast<int>(mNames.size()))
      return;

    mIndex = chosen;
    mCurrent = mNames[static_cast<size_t>(chosen)];

    if (mOnLoad)
      mOnLoad(mCurrent);

    SetDirty(false);
  }

private:
  static constexpr int kSaveValIdx = 200;

  void LayOut()
  {
    mPrevRect = mRECT.GetFromLeft(30.f);
    mNextRect = mRECT.GetFromRight(30.f).GetHShifted(-56.f);
    mSaveRect = mRECT.GetFromRight(52.f);
    mNameRect = mRECT.GetReducedFromLeft(32.f).GetReducedFromRight(90.f);
  }

  void Step(int delta)
  {
    if (mNames.empty())
      return;

    const int next = std::max(0, std::min(mIndex + delta, static_cast<int>(mNames.size()) - 1));

    if (next == mIndex && !mCurrent.empty())
      return;

    mIndex = next;
    mCurrent = mNames[static_cast<size_t>(mIndex)];

    if (mOnLoad)
      mOnLoad(mCurrent);

    SetDirty(false);
  }

  void SelectByName(const std::string& name)
  {
    const auto it = std::find(mNames.begin(), mNames.end(), name);
    mIndex = it == mNames.end() ? 0 : static_cast<int>(std::distance(mNames.begin(), it));
    mCurrent = name;
    SetDirty(false);
  }

  void ShowMenu()
  {
    mMenu.Clear();

    if (mNames.empty())
    {
      mMenu.AddItem("No presets saved yet");
      mMenu.GetItem(0)->SetEnabled(false);
    }
    else
    {
      for (const auto& name : mNames)
        mMenu.AddItem(name.c_str());
    }

    GetUI()->CreatePopupMenu(*this, mMenu, mNameRect);
  }

  void DrawChevron(iplug::igraphics::IGraphics& g, const iplug::igraphics::IRECT& rect, bool right, bool enabled)
  {
    const auto colour = enabled ? PluginColors::INK_MUTED : PluginColors::INK_DIM;
    const float cx = rect.MW();
    const float cy = rect.MH();
    const float w = right ? 4.f : -4.f;

    g.DrawLine(colour, cx - w, cy - 5.f, cx + w, cy, nullptr, 1.6f);
    g.DrawLine(colour, cx + w, cy, cx - w, cy + 5.f, nullptr, 1.6f);
  }

  NameAction mOnSave;
  NameAction mOnLoad;

  std::vector<std::string> mNames;
  std::string mCurrent;
  int mIndex = 0;

  iplug::igraphics::IPopupMenu mMenu;
  iplug::igraphics::IRECT mNameRect, mPrevRect, mNextRect, mSaveRect;
};

} // namespace nr::presets
