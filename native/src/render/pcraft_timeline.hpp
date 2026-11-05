#pragma once

#include <optional>

#include "core/json.hpp"
#include "core/model.hpp"
#include "render/pcraft_production.hpp"

// PC5 J-e2 (timeline@1, SPEC.md PROD-01「タイムライン」, FILE-01「.pcraft」): a .pcraft's timeline (the document's "timeline":
// PhotoCraft v0.2.0's doc/src/video.rs Timeline — fps, duration, current, work_start (from 0, in), work_end (out); MIT,
// native/third_party/photocraft/NOTICE.md) as the page's animation, and back. Read: the page made an animation of that
// many frames (1..3000) at that speed (0.5..120: past it, kept to it and said), its work area where it is not all of the
// frames; the frame shown (current) is the window's, not read. A value not of that shape is left out, said ("タイムラインは
// 読めない形のため外しました"). Written: an animation page's speed, frames and work area (current 0). A video layer's other
// frames have no place in a .pcraft: formats/pcraft_write writes the first frame's look (render/footage) and says so.

namespace genko::render::pcraft_timeline {

void read(const core::Json& value, pcraft_production::ReadContext& context);
std::optional<core::Json> write(const core::Page& page, pcraft_production::WriteContext& context);

}  // namespace genko::render::pcraft_timeline
