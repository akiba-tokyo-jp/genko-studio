#pragma once

#include <cstdint>
#include <string_view>

#include "core/json.hpp"

// timeline@1 and video@1 (PC5 節 J、SPEC.md PROD-01「タイムライン・ビデオレイヤー」・schema-v4.md「制作の補助」): what the
// timeline (タイムライン) and the video layers (ビデオレイヤー) add inside a page's animation (`page.extra["anim"]`,
// core/anim.hpp: its keys as they are). Kept in the anim's JSON (no type of their own); checked here.
//
// timeline@1 — the anim's
//   "fps": a number of 0.5..1 or 60..120 (what no op before it makes: set_animation takes any number of 1..60, whole or
//          not — 8.5 is an anim's as before —; a value past 0.5..120 is an anim's before it too, hand-written: read as it is),
//   "work": [first, last] (the work area: frames from 1, both in, 1 <= first <= last <= kMaxFrames; none: all of them)
// video@1 — a track's "video" (a video layer is an animation folder's video row: J-e2):
//   {"footage": {"kind": "sequence" | "animation" | "image", "name": str,
//                "frames": ["sha256:…", …],                 (the footage's frames: the book's asset files, 1..kMaxFrames)
//                "fps": >0, "size_px": [w, h], "depth": 8 | 16, "alpha": "straight" | "premultiplied" | "ignore",
//                "par": >0, "source"?: {"path": str, "bytes": int, "sha256": str}},
//    "frames": [[footage frame index | null, altered cel id | null], …],   (one a frame of the timeline)
//    "show_altered": bool, "placement_mm": [x, y, w, h], "fit": "contain" | "cover" | "stretch"}
// The user's decision 4 (PC5): a video file (MP4・MOV・WebM) is never read — the footage is a numbered sequence of pictures
// ("sequence"), a moving picture (GIF・APNG・WebP: "animation") or one picture ("image").
//
// The book names timeline@1 and video@1 while a page's anim has them; a build without them opens it read-only (it would
// write out the whole timeline, and draw a video row as an empty folder). PC5 J-0 is the frame; the work area and the
// fps past 1..60 are J-e1's (core/ops_anim set_animation, app/timeline), the video layers J-e2's (render/ops_video,
// render/footage).

namespace genko::core {

struct Document;
struct Page;

inline constexpr std::string_view kTimelineFeature = "timeline@1";
inline constexpr std::string_view kVideoFeature = "video@1";
inline constexpr double kTimelineFpsMin = 0.5;
inline constexpr double kTimelineFpsMax = 120;

// Is this anim's fps one only timeline@1 holds (a number of 0.5..1 or 60..120)? Its work one ([first, last] as above)?
bool timeline_fps(const Json& fps);
bool timeline_work(const Json& work);
// Does the page's anim hold timeline@1's (a work area as above, or such an fps)? video@1's (a track's video row as above)?
// What is not so — a "work" or a "video" of another shape — is what an anim before them may hold: named by nothing, kept.
bool page_uses_timeline(const Page& page);
bool page_uses_video(const Page& page);
// core::Error("value") says what is not so in a book that names them: the anim's work area, its video rows (`video`).
void check_timeline(const Json& anim, bool video);
void check_video(const Json& video);

bool uses_timeline(const Document& doc);
bool uses_video(const Document& doc);
// The reader's check, for a book that names timeline@1 (`timeline`) or video@1 (`video`): a value of another shape is not
// read (the book opens read-only). A book that names neither holds what the anims before them held.
void validate_timeline(const Document& doc, bool timeline, bool video);

}  // namespace genko::core
