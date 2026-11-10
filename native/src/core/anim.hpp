#pragma once

#include <cstdint>
#include <optional>
#include <string_view>
#include <vector>

#include "core/json.hpp"
#include "core/model.hpp"

// Animation (アニメーション, Python's genko/anim.py): a page can be a short animation. Its timeline lives in
// page.extra["anim"]:
//
//     {"fps": 12, "frames": 24, "loop": true,
//      "tracks": [{"folder": <animation folder layer id>, "cels": [[frame, cel layer id | null], …]}],
//      "camera": [{"frame": 1, "rect": [x, y, w, h] mm}, …],        (カメラワーク: the view moves between keys)
//      "light_table": [cel ids]}                                     (ライトテーブル: always shown faint)
//
// An animation folder holds cels (セル: ordinary pen or paint layers). A track's cels list is its exposure sheet
// (タイムシート): from each frame on, that cel shows (null: nothing) until the next entry. Layers outside the animation
// folders (backgrounds, the panel) show in every frame. Frames count from 1.
//
// The values are read as Python reads them (a book may hold anything there): Python's errors where it raises them
// (core/pyops.hpp).

namespace genko::core::anim {

inline constexpr std::int64_t kMaxFrames = 3000;

// The timeline (page.extra["anim"] when it is a dict); nullptr: not an animation.
const Json* spec(const Page& page);
bool is_animation(const Page& page);

// max(1, int(frames or 1))
std::int64_t frames_of(const Page& page);
// float(fps or 12)
double fps_of(const Page& page);
// spec.get("loop", True), as bool
bool loops(const Page& page);

// The folder ids of the tracks, in their order (t.get("folder") of each).
std::vector<Json> folders(const Page& page);

// The first track whose "folder" == folder_id (nullptr: none).
const Json* track(const Page& page, const Json& folder_id);

// The layers in this folder (parent_id == folder_id), top of the list first as the page holds them.
std::vector<const Layer*> cels_of(const Page& page, const Json& folder_id);

// The cel shown in this folder at this frame (null: nothing), as the exposure sheet holds it.
Json cel_at(const Page& page, const Json& folder_id, std::int64_t frame);

// The page as it looks at this frame: in each animation folder only the exposed cel shows. (A copy: the book is never
// changed.)
Page at_frame(const Page& page, std::int64_t frame);

// The camera's rect (mm) at this frame, moving evenly between its keys (nothing: no camera work).
std::optional<std::vector<double>> camera_at(const Page& page, std::int64_t frame);

// The cels shown before (direction -1) or after (+1) this frame on a track, nearest first, skipping the one shown now
// (the onion skin's neighbours; `loop`: round past the ends).
std::vector<Json> neighbours(const Page& page, const Json& folder_id, std::int64_t frame, int direction,
                             std::int64_t how_many, bool loop);

}  // namespace genko::core::anim
