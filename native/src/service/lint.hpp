#pragma once

#include <vector>

#include "core/json.hpp"
#include "service/layout.hpp"
#include "service/letter.hpp"
#include "studio/schemas.hpp"

// Deterministic checks on what an agent writes (Python's genko/studio/lint.py). Paths are JSON pointers into the
// input.

namespace genko::service::lint {

using core::Json;

inline constexpr std::size_t kMaxBalloonChars = 40;
inline constexpr std::size_t kMaxColumnChars = 14;
inline constexpr std::size_t kMaxBalloonsPerBeat = 3;
inline constexpr int kMaxSameShotRun = 2;
inline constexpr double kBalloonAreaRatio = 0.35;
inline constexpr double kMinPanelSideMm = 25.0;

// lint.lint_bible(bible)
std::vector<studio::Issue> lint_bible(const Json& bible);
// lint.lint_script(script, bible, page_count)
std::vector<studio::Issue> lint_script(const Json& script, const Json& bible, std::int64_t page_count);
// lint.script_index(script): beat id → {beat, scene_id, scene_first} (a later beat of the same id replaces it).
Json script_index(const Json& script);
// lint.lint_name_plan(plan, script, bible, page_count, tall)
std::vector<studio::Issue> lint_name_plan(const Json& plan, const Json& script, const Json& bible, std::int64_t page_count, bool tall = false);
// lint.lint_name_geometry(plan, layout, placements)
std::vector<studio::Issue> lint_name_geometry(const Json& plan, const layout::CompiledLayout& compiled,
                                              const std::vector<letter::BalloonPlacement>& placements);

// fonts.BUNDLED: the bundled faces (key, label), in Python's order.
const std::vector<std::pair<std::string, std::string>>& bundled_fonts();

}  // namespace genko::service::lint
