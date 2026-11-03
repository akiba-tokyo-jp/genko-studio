#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "core/json.hpp"
#include "core/model.hpp"
#include "formats/open_file.hpp"
#include "render/hdr_merge.hpp"

// ファイル → 自動処理 ▸ HDR Pro に統合… and 写真の切り抜きと角度補正 (PC5 I-h, SPEC.md AUTO-01; PhotoCraft v0.2.0's
// engine/src/photo_cmds.rs merge_to_hdr and crop_and_straighten, MIT): each makes new books in memory — no name, no folder,
// as 開く… makes one of a picture (formats::picture_book) — and never changes the pictures or the book they read. Not ops
// (an op reads no path, and these make books): the app's commands and `genko automate hdr|crop-photos` call them, as the
// person who runs them (design 8-4 (a): MCP and HTTP have no way in).

namespace genko::service::automate {

namespace fs = std::filesystem;

// --- HDR Pro に統合 ---------------------------------------------------------------------------------------------------------

// How many exposures one merge takes.
inline constexpr std::size_t kHdrMinImages = 2;
inline constexpr std::size_t kHdrMaxImages = 16;

// The exposures read from files, in the order given: each file opened as 開く… opens it (formats::open_file — any picture,
// a PSD, a .pcraft …) and taken as its page's merged picture at its depth (RGB or grey; CMYK refused), its camera's
// exposure from its EXIF; a camera RAW file developed to linear light (render::raw::develop_linear: no response curve is
// estimated for it), its exposure from what it says of its shot. One file at a time is open; the shots are held as they
// are (8 or 16 bits, or floats) in the imaging budget, the next refused past it. core::Error("value") for fewer than
// kHdrMinImages or more than kHdrMaxImages, pictures of different sizes, RAW files with pictures that are not; the
// files' own errors (opened as 開く… opens them) with the file's name before them.
std::vector<render::hdr::Shot> hdr_shots(const std::vector<fs::path>& files, const formats::OpenOptions& options,
                                         const std::function<bool()>& stopped = {});
// One file as hdr_shots reads it (its errors as they are: hdr_shots puts the file's name before them).
render::hdr::Shot hdr_shot_file(const fs::path& file, const formats::OpenOptions& options, const std::function<bool()>& stopped = {});
// Whether these exposures can be merged together: kHdrMinImages to kHdrMaxImages of one size, all of linear light or all
// not (core::Error("value") saying why), and held within the imaging budget (core::Error("image_too_large")).
void check_shots(const std::vector<render::hdr::Shot>& shots);
// A page of an open book as an exposure (開いているファイルを追加): its merged picture at its depth, the camera of the file it
// was opened from. core::Error("page_not_loaded") for a page not read yet.
render::hdr::Shot hdr_shot_of(const core::Document& doc, std::size_t page, const std::string& name);

struct HdrSettings {
    // EV of each exposure (relative: the darkest's own value does not matter); none: the camera's (EXIF), else estimated
    std::optional<std::vector<double>> ev;
    bool align = true;              // 可能な場合はソース画像を自動的に配置 (Ward's MTB, a translation)
    bool remove_ghosts = false;     // ゴーストを除去
    std::optional<std::size_t> ghost_base;  // the exposure ghosts come from (0-based; none: the best exposed)
    int depth = 32;                 // 32 (linear light, linear-srgb) | 16 | 8 (tone mapped: sRGB)
    render::hdr::Tone tone;         // 16 and 8 bits only
    std::string title;              // the new book's (empty: "HDR")
};

// The toning of 16 and 8 bits (the command line's --method and --settings): local_adaptation (ローカル適応: {"radius"
// 1..500 px, "strength" 0.1..4, "gamma" 0.1..2, "exposure" −5..5, "detail" −100..300 %, "shadow", "highlight", "vibrance",
// "saturation" −100..100 %}, clamped, PhotoCraft's defaults 7, 0.52, 1, 0, 30, 0, 0, 0, 20), exposure_gamma (露光量とガンマ:
// {"exposure" −20..20, "gamma" 0.1..10}), highlight_compression, equalize_histogram (none). core::Error("value") for a
// method, a key or a value it does not take.
render::hdr::Tone hdr_tone(std::string_view method, const core::Json& settings = core::Json::object());

struct HdrMade {
    formats::Opened book;
    // {"mode": 32|16|8, "method"?, "exposure_source": "given"|"exif"|"estimated", "ev": [...] (relative to the darkest),
    //  "offsets": [[dx, dy], …], "ghost_base", "ghost_fraction", "stops", "width", "height"}
    core::Json report = core::Json::object();
};

// The exposures merged (PhotoCraft's merge_to_hdr): their exposure values found (given, the cameras', or estimated), the
// shots aligned to the middle one, merged into linear radiance (ghosts removed when asked), then made a new book of one
// page: at 32 bits the radiance as it is in the built-in linear-srgb (values past 1 kept), at 16 or 8 bits tone mapped
// (render::hdr::tone_map) in sRGB. The shots are given up as they are used. The work's memory is held in the imaging
// budget (2 GiB, codecs::kAllocBudget): past it the merge is refused before it is made (core::Error("image_too_large")).
// render::Cancelled when `stopped` says so; core::Error("value") for settings it does not take (an EV a picture, a base
// that is not one of them, a depth not 32, 16 or 8).
HdrMade merge_to_hdr(std::vector<render::hdr::Shot> shots, const HdrSettings& settings, const std::function<bool()>& stopped = {});

// The memory a merge of exposures of width × height (`shots_bytes` as held) works with at its most, and its refusal past the
// imaging budget (core::Error("image_too_large"), before anything is made): merge_to_hdr's own check.
std::uint64_t hdr_work_bytes(std::uint32_t width, std::uint32_t height, std::uint64_t shots_bytes, const HdrSettings& settings);
void check_work(std::uint32_t width, std::uint32_t height, std::uint64_t shots_bytes, const HdrSettings& settings);

// The merge's first part, which merge_to_hdr makes its book of: the radiance (render::hdr::merge) and the report (all but
// "mode" and "method") — what the window's preview keeps while only the toning changes (its shots made small:
// render::hdr::reduced).
struct HdrRadiance {
    render::hdr::Merged merged;
    core::Json report = core::Json::object();
    std::optional<std::pair<double, double>> dpi;  // the first exposure's resolution
};
HdrRadiance hdr_radiance(std::vector<render::hdr::Shot> shots, const HdrSettings& settings, const std::function<bool()>& stopped = {});
// How the result shows (the window's preview), 8-bit RGB (sRGB): at 16 and 8 bits tone mapped as the settings say, at 32
// bits the light × 2^`view_exposure` through sRGB's curve (as 32 bit プレビューオプション shows a 32-bit page).
render::codecs::CodecImage hdr_display(const HdrRadiance& radiance, const HdrSettings& settings, double view_exposure = 0.0);

// --- 写真の切り抜きと角度補正 --------------------------------------------------------------------------------------------------

// At most this many photos are cut out of one scan (more: core::Error("value"), nothing made).
inline constexpr std::size_t kMaxPhotos = 32;

// The photos on the page (its merged picture at its depth and resolution, as 複製… takes it — formats::page_picture),
// found (render::scancrop) and each made a new book of one page, upright, at the page's depth, colours and resolution:
// "<title> のコピー <n>" in reading order. None found: none made (the caller says so). core::Error("page_not_loaded") for a
// page not read yet; `report` gets {"photos": [{"width", "height", "angle", "center"}]}.
std::vector<formats::Opened> crop_and_straighten(const core::Document& doc, std::size_t page, const formats::OpenOptions& options,
                                                 core::Json* report = nullptr);

}  // namespace genko::service::automate
