#pragma once

#include <filesystem>
#include <optional>
#include <string_view>

#include "core/json.hpp"

// A person's own poses for the 3D figure (Python's genko/poses.py, ポーズを素材として保存): kept with the app's
// settings (the config folder's poses.json), so every book has them. A pose is its joints and hands (the build and
// the size stay the figure's own).

namespace genko::core::poses {

// Python's genko.tokens.config_dir: $GENKO_CONFIG_DIR, else %APPDATA%\genko on Windows, else
// $XDG_CONFIG_HOME/genko or ~/.config/genko.
std::filesystem::path config_dir();

// [{name, joints, hands}], oldest first (nothing when the file is missing or not JSON).
Json user_poses();

// Keeps (or replaces) the pose `name` taken from the figure prim; PyValueError "a pose needs a name" for a blank name,
// and when the library already holds core::limits::kUserPoses other poses (Python keeps any number); core::Error("io")
// when the file cannot be written.
Json save_pose(std::string_view name, const Json& prim);

void delete_pose(std::string_view name);

std::optional<Json> find(std::string_view name);

}  // namespace genko::core::poses
