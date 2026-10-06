// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/Orca/Disc.h"

#include <memory>
#include <optional>

#include <fmt/format.h>

#include "Common/FileUtil.h"
#include "Common/IOFile.h"
#include "Core/Orca/Profile.h"
#include "DiscIO/Volume.h"

namespace Orca
{
DiscCheck CheckDisc(const std::string& path, std::string_view game_id)
{
  const auto refuse = [](std::string sentence) {
    return DiscCheck{false, "disc_revision", std::move(sentence)};
  };
  if (path.empty() || !File::Exists(path) || File::IsDirectory(path))
    return refuse("That disc image can't be found.");

  const std::unique_ptr<DiscIO::Volume> volume = DiscIO::CreateVolume(path);
  if (!volume)
    return refuse("That file isn't a disc image Orca can read.");

  const DiscIO::Partition partition = volume->GetGamePartition();
  const std::string id = volume->GetGameID(partition);
  const u16 revision = volume->GetRevision(partition).value_or(0);
  if (id != game_id)
  {
    return refuse(fmt::format("That disc{} isn't Super Smash Bros. Brawl for the USA ({}).",
                              id.empty() ? "" : fmt::format(" ({})", id), game_id));
  }

  std::string error;
  const std::optional<Profile> profile = LoadProfile(id, revision, &error);
  if (!profile)
  {
    if (error.find("revision") == std::string::npos)
      return DiscCheck{false, "profile", error};
    return refuse(fmt::format("That Brawl disc is revision {}; Orca plays Rev 2 (the latest USA "
                              "disc), so both players have the same game.",
                              revision));
  }
  return DiscCheck{true, {}, {}};
}

DiscCheck CheckBootFile(const std::string& path)
{
  if (path.empty() || !File::Exists(path))
    return DiscCheck{false, "disc_missing", path};
  // A folder (an extracted disc) is left to the boot's own checks.
  if (!File::IsDirectory(path) && !File::IOFile(path, "rb").IsOpen())
    return DiscCheck{false, "disc_unreadable", path};
  return DiscCheck{true, {}, {}};
}
}  // namespace Orca
