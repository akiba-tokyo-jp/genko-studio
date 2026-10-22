#pragma once

#include <optional>

#include "core/command_bus.hpp"
#include "core/exif.hpp"
#include "render/ops_adjust.hpp"

// PC4 節 D (SPEC.md COLOR-08「フィルター」): フィルター's kinds as filter_raster's local filters (render/ops_filters.cpp,
// by their kernels: render/fx.hpp) and the frames of PC4's new ops.

namespace genko::render {

// render_picture (D4: 炎・額縁・木, refused — not_yet_ported — until its group writes it) and vanishing_point (D6: 消点,
// render/vanishing).
void register_filter_ops(core::OpRegistry& registry);

// (D1) While it lives, the layer filter_raster works on this thread: what a kind reads of it besides its pixels
// (ぼかし（レンズ）'s depth from its layer mask, design 2.6). A smart filter's stand-in has none (no depth from a mask).
class FilterLayer {
public:
    explicit FilterLayer(const core::Layer& layer);
    ~FilterLayer();
    FilterLayer(const FilterLayer&) = delete;
    FilterLayer& operator=(const FilterLayer&) = delete;

private:
    const core::Layer* before_;
};

// filter_raster's report for a kind of フィルター's (design 3.1): {"filter": {"kind", …the settings it was drawn with — its
// normal form}}; none for any other kind. (D6: レンズ補正's profile's name; what the kinds resolve as they run — 自動拡大・縮小,
// 広角補正's model — filter_raster lays in from fx::NoteScope.)
std::optional<core::Json> filter_report(const core::Document& doc, const core::Json& op);

// (D2, design 2.6) A smart filter's settings with its files made the book's: 置き換え's map given as a file's data
// (map_data) or as a layer of the book ({"page"?, "layer": id | "merged"}, drawn at the working 200 dpi) becomes a file the
// book holds (Document::filter_files: "map" its ref, "map_suffix" its kind) — what the smart object draws then never
// changes with that layer. OpError as filter_raster refuses the map. Other kinds' settings as they are.
core::Json keep_filter_files(core::Document& doc, const core::Page& page, std::string_view kind, core::Json params);

// What filter_raster refuses of a kind of フィルター's that a smart filter keeps (OpError): D3's フィルターギャラリー with every
// effect hidden (it would change nothing; a smart filter may hold its effects so).
void refuse_filter(const core::Document& doc, const core::Json& op);

// (D6) What the picture a layer shows says of its camera (its EXIF): a smart object's file's, else the file the page was
// opened from (file-info@1's native_source: its EXIF, and what the camera said — a RAW file's — for a focal length the
// EXIF lacks); none when neither says. What a file of the book's (an asset, "sha256:…": never changing) says is kept
// once read (a few of them), so a smart filter set again or a window drawn again does not read the file again.
std::optional<core::exif::Camera> picture_camera(const core::Document& doc, const core::Page& page, const core::Layer* layer);
// (D6, design 2.5) A kind's settings with what the picture's camera says filled in, as the op records them (lens_cmds.rs
// lens_params, wide_params): レンズ補正's profile "auto" without a focal length takes the 35 mm-equivalent one (else the
// lens's own) — core then makes it "generic" of that —, 広角補正 without a focal length the lens's and its crop factor.
// Other kinds' settings, and settings the camera says nothing to, as they are.
// `known`: the camera already read (picture_camera's answer, a window's: none read again); none: read it when needed.
core::Json resolve_filter(const core::Document& doc, const core::Page& page, const core::Layer* layer, std::string_view kind, core::Json params,
                          const std::optional<core::exif::Camera>* known = nullptr);
// filter_raster's op with resolve_filter's settings (none: nothing to fill in).
std::optional<core::Json> resolve_filter_op(const core::Document& doc, const core::Json& op);

}  // namespace genko::render
