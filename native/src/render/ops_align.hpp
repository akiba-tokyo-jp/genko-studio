#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "core/command_bus.hpp"
#include "core/json.hpp"
#include "render/panorama.hpp"
#include "render/resample.hpp"

// PC5 I-g (SPEC.md AUTO-01「自動整列・自動合成」「Photomerge」): 編集 → レイヤーを自動整列… and レイヤーを自動合成… as ops, and the
// stitching ファイル → 自動処理 → Photomerge… shares with them. PhotoCraft v0.2.0's engine/src/align_cmds.rs (43-221) and
// photo_cmds.rs (131-550), MIT (native/third_party/photocraft/NOTICE.md), on Genko's three ways of pixels (8 bits, precise, inks):
//
//   auto_align_layers {page, ids: [≥ 2 paint layers], projection?: auto | perspective | cylindrical | spherical | collage |
//                      reposition (auto), reference?: id (the bottom one), geometric?: bool (幾何学的歪曲の補正), interpolation?:
//                      bicubic | bilinear | nearest, focal_length?: mm of 35 mm film (the cylinder's and sphere's; none: found)}
//     → {reference, layout, focal, rms, aligned: [{id, model, matches, matrix: 3 × 3 of the page's mm | null}], failed: [ids]}
//     Registered on each layer's luminance shrunk to 1200 px at most (8 bits: sRGB's; precise: the page's values; inks: what they
//     mean in RGB); each layer that moves drawn again through its placement in mm (render/layer_transform: a plane's homography,
//     a cylinder's or sphere's mesh map), one at a time; the reference stays in the plane layouts. 対応が見つかりません
//     (OpError "auto-align found no matching detail …") when fewer than two join.
//   auto_blend_layers {page, ids: [≥ 2 paint layers of one kind], method?: panorama | stack (panorama), seamless?: bool (true),
//                      new_id?, name?}
//     → {method, ids, blended: id | null}
//     Each layer given a mask (at the page's mask grid, render/layer_masks) so the page shows one layer a place: the seams
//     routed where they agree (panorama: upstream's seam_blend, its seams on ≤ 1000 px) or the sharpest (stack: focus stacking),
//     worked on the masks' grid shrunk to 2048 px at most; with seamless, a layer above them blended by Laplacian pyramids
//     (render/pyramid's blend_tiled: 1024 px tiles), of their kind and depth.
// Not MCP's (design 3: neither is in agent_op). Each one change (one undo).

namespace genko::render::align {

void register_align_ops(core::OpRegistry& registry);

// What these work on at most (refused past them, before anything is held).
inline constexpr std::size_t kMaxLayers = 32;          // (auto_align_layers, auto_blend_layers, Photomerge's pictures)
inline constexpr std::size_t kRegisterSide = 1200;     // (the registration's longest side)
inline constexpr std::size_t kWorkSide = 2048;         // (auto_blend_layers' masks worked out at most so big)
inline constexpr std::size_t kSeamSide = 1000;         // (the seams' grid)
inline constexpr std::size_t kFeatures = 800;          // (corners a picture)
inline constexpr std::uint64_t kBudget = 2ULL << 30;   // (the imaging budget the ops hold their work in)

// A picture's luminance and coverage shrunk by k (box), as registration takes them.
struct Shrunk {
    std::size_t w = 0, h = 0;
    std::vector<float> luma, alpha;
};

// The registration of pictures shrunk by one factor (registration's 1200 px): the placements in the shrunk pictures' pixel
// indices; `focal_px` the focal length in those pixels (the EXIF's; none: found). None when fewer than two join.
std::optional<panorama::Alignment> register_shrunk(const std::vector<Shrunk>& shrunk, panorama::Layout layout, std::optional<std::size_t> reference,
                                                   bool geometric, std::optional<double> focal_px);
// A placement found on pictures shrunk by k, as a map of the full pictures' continuous coordinates (a pixel (x, y) covers
// [x, x + 1) × [y, y + 1)): the plane's matrix (none for a cylinder, a sphere or a distortion), the forward map, the inverse.
std::optional<resample::Homography> full_matrix(const panorama::Placement& p, double k);
panorama::XY full_forward(const panorama::Placement& p, double k, double x, double y);
std::optional<panorama::XY> full_inverse(const panorama::Placement& p, double k, double u, double v);

// --- Photomerge (ファイル → 自動処理 → Photomerge…; service/automate builds the book) ------------------------------------------

// One picture: straight RGBA floats (8 bits / 255, precise its values; `hdr` keeps a 32-bit one's above 1).
struct Source {
    std::string name;
    resample::Plane plane;
};

struct MergeOptions {
    panorama::Layout layout = panorama::Layout::Auto;
    bool blend = true;                // (画像を合成)
    bool vignette = false;            // (周辺光量補正)
    bool geometric = false;           // (幾何学的歪曲の補正)
    bool content_aware_fill = false;  // (透明な領域をコンテンツに応じて塗りつぶす)
    std::optional<double> focal35;    // (mm of 35 mm film: the EXIF's)
    std::uint64_t max_pixels = 0;     // (the panorama's area at most; 0: no limit here)
    std::uint64_t budget = 0;         // (the bytes the work may hold at most; 0: no limit here)
};

// A part of the panorama: a box of it (x0, y0: its corner in the panorama's pixels).
struct Part {
    int x0 = 0, y0 = 0;
    resample::Plane plane;
};

struct Merged {
    int width = 0, height = 0;
    std::string layout;
    std::size_t reference = 0;
    std::vector<std::size_t> placed;   // (the sources' indices, in the layers' order)
    std::vector<std::size_t> failed;
    std::vector<Part> layers;          // (each placed picture in the panorama, RGBA, over its box)
    std::vector<Part> masks;           // (with blend: each layer's mask, grey over the same box — 0 outside it)
    std::optional<resample::Plane> fill;     // (content_aware_fill: the panorama's transparent areas filled, opaque, whole)
    core::Json info = core::Json::object();  // {focal_length35, geometric_k1, rms, pairs, gains, vignette}
};

// The panorama of `sources` (two at least). core::OpError: "photomerge found no matching detail …" (対応が見つかりません), "the
// panorama would be W × H px …" (too large: max_pixels, 30000 a side or the budget).
Merged photomerge(const std::vector<Source>& sources, const MergeOptions& options);

// The bytes the stitching holds at its peak for a panorama of `canvas` pixels whose pictures cover `parts` pixels in all (the
// estimate checked against MergeOptions::budget before anything is made).
std::uint64_t photomerge_bytes(std::uint64_t canvas, std::uint64_t parts, bool blend, bool fill);

}  // namespace genko::render::align
