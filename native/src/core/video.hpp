#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "core/json.hpp"
#include "core/model.hpp"

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

// The coordinator's decision on J-e1's review P2-1 (節 J 3.1): an agent's apply_ops through MCP
// (service::StudioService::apply_ops) keeps set_animation as the public list (ops_schema.json, `genko schema`) and Python
// have it — an fps of 1..60 ("fps is 1 to 60"), "work" an unknown key, ignored and said so. While one of these lives on
// this thread, the ops applied on it take that; the window, `genko apply` and the HTTP API's /v1/apply take timeline@1's.
class PublicAnimationOps {
public:
    PublicAnimationOps();
    ~PublicAnimationOps();
    PublicAnimationOps(const PublicAnimationOps&) = delete;
    PublicAnimationOps& operator=(const PublicAnimationOps&) = delete;
};
bool public_animation_ops();

// The work area (作業範囲, PC5 J-e1) of a page's anim: its work of the shape above, cut to the anim's frames (a book whose
// frames a build before J-e1 cut down may hold one past them); none: no work area (all the frames), not an animation.
std::optional<std::array<std::int64_t, 2>> work_area(const Page& page);
// The frames played and rendered from the work area: the work area, else 1..frames.
std::array<std::int64_t, 2> played_frames(const Page& page);
// timeline.info (the question timeline_info, `genko inspect --timeline`): the page's timeline — {"page", "animation":
// false} for a page that is not one, else its fps, frames, loop, length in seconds, work area (null: none) and the frames
// played, the tracks (each folder, its name, its cels and its exposure sheet), the camera's keys, the light table and
// whether the page names timeline@1. (The frame shown is the window's: not the book's.)
Json timeline_info(const Page& page);
// --- PC5 J-e2: the video rows (ビデオレイヤー) ---------------------------------------------------------------------------
// A video layer is an animation folder whose track has a "video" of the shape above (its folder a folder layer; the
// track's own exposure sheet "cels" empty). At a frame f of the timeline its frames[f - 1] says what shows: its altered cel
// (描き替えたフレーム: a paint layer in the folder) while show_altered, else the footage's frame of that index laid as a
// placed picture is (placement_mm, fit), else nothing (a blank frame, or past the row's end). The footage's frames are the
// book's asset files (PNG: 8 or 16 bits, RGBA; one picture held once however often it is shown).

// The footage of all the book's video rows decoded together may take this much (each footage frame w × h × 4 samples of
// its depth, a picture counted once): footage past it is refused before it is read (render/footage).
inline constexpr std::uint64_t kFootageMaxBytes = 4ull << 30;
inline constexpr std::string_view kFootageSuffix = ".png";

// Is this track a video row (its "video" of video@1's shape)?
bool is_video_row(const Json& track);
// The video row of the page's track of this folder (nullptr: none, or not a video row).
const Json* video_row(const Page& page, const Json& folder_id);
// What the video row shows at this frame of the timeline (1..).
struct VideoShown {
    enum class Kind { Nothing, Cel, Footage };
    Kind kind = Kind::Nothing;
    std::string cel;          // Cel: the altered cel's layer id
    std::int64_t index = -1;  // Footage: the footage frame's index (0..)
};
VideoShown video_shown(const Page& page, const Json& folder_id, const Json& video, std::int64_t frame);
// Where the whole footage frame lands on the page (mm): fitted (contain, cover, stretch) in its placement_mm, its pixels
// as wide as their aspect ratio (par) makes them. None for a row without footage.
std::optional<Rect> footage_placement(const Json& video);
// The bytes the footage takes decoded (each picture once): w × h × 4 × (8 bits: 1, 16 bits: 2).
std::uint64_t footage_bytes(const Json& footage);
// The bytes all the book's footage takes decoded (each picture once, across the rows): kFootageMaxBytes at most.
std::uint64_t book_footage_bytes(const Document& doc);
// The footage frames' asset refs the book's video rows name (each once; the pages and the smart objects' contents pages).
std::vector<std::string> footage_refs(const Document& doc);
// The footage's frames laid on the timeline from `start` (1..): the timeline's frame start + k shows the footage frame
// floor(k × footage fps ÷ timeline fps) while it is one of the footage's `count` (as many entries as that makes).
std::vector<std::int64_t> laid_footage(std::int64_t count, double footage_fps, double timeline_fps);

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
