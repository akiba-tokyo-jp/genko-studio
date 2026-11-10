#pragma once

#include <functional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "core/adjust.hpp"
#include "core/command_bus.hpp"
#include "core/ink.hpp"
#include "core/json.hpp"
#include "core/model.hpp"
#include "render/image.hpp"

// PC2 (補正・色管理, SPEC.md COLOR-04): the C++ build's own ops of the corrections and filter_raster's kinds of its own
// (design 3). render/ops_adjust.cpp registers every one; each group's own file writes its op's body (until it does, the
// op is refused: not_yet_ported naming it). The public `genko schema` stays Python's (as set_color_mode's).

namespace genko::render {

// set_adjustment, auto_adjust, hdr_toning, apply_image, calculations, trap and fade_layer.
void register_adjust_ops(core::OpRegistry& registry);

namespace adjust_ops {

// The ops' bodies, by group.
void set_adjustment(core::OpContext& c);  // PC2a (layer.setAdjustment): render/ops_adjust.cpp
// PC2a (image.autoTone・autoContrast・autoColor): render/ops_adjust.cpp — levels' channel records found from the layer's
// histograms laid on it by filter_raster (`filter_raster`: the op as registered, before the registry's own wrappers).
void auto_adjust(core::OpContext& c, const core::OpFunction& filter_raster);
void hdr_toning(core::OpContext& c);      // PC2c (image.adjustments.hdrToning): render/tone_ops.cpp
void apply_image(core::OpContext& c);     // PC2c (image.applyImage): render/ops_calc.cpp
void calculations(core::OpContext& c);    // PC2c (image.calculations): render/ops_calc.cpp
void trap(core::OpContext& c);            // PC2c (image.trap): render/trap.cpp
void fade_layer(core::OpContext& c);      // PC2c (edit.fade): render/ops_fade.cpp

// A filter_raster kind that looks beyond each pixel (PC2a's equalize, PC2c's local corrections): what it does to each
// kind of a layer's pixels — the 8-bit picture (RGBA, the layer's), a precise raster (its new GKCR bytes), an ink
// raster (its new GKIR bytes) —, its settings in their normal form. `area`: only inside it (as filters.within), on the
// page. An empty one: not on that kind of pixels yet (not_yet_ported).
struct LocalFilter {
    std::function<Image(const Image& rgba, const core::Json& params, const core::Page& page, const core::Document& doc)> eight;
    std::function<std::string(const core::Page& page, const core::Layer& layer, const core::Json& params, const core::Json* area,
                              const core::Document& doc)>
        precise;
    std::function<std::string(const core::Page& page, const core::InkPixels& inks, const core::Json& params, const core::Json* area,
                              const core::Document& doc)>
        inks;
    // (PC2c) The 8-bit way of a filter that reads the op's area itself (match_color's stats_in_area), used when the op
    // has one (the caller still keeps the result to it). None: `eight`.
    std::function<Image(const Image& rgba, const core::Json& params, const core::Page& page, const core::Document& doc, const core::Json& area)>
        eight_in;
};
using LocalFilters = std::vector<std::pair<std::string_view, LocalFilter>>;
LocalFilters adjust_local_filters();  // PC2a: render/ops_adjust.cpp (equalize)
LocalFilters tone_local_filters();    // PC2c: render/tone_ops.cpp (shadows_highlights, replace_color, match_color)
LocalFilters filter_local_filters();  // PC4 節 D: render/ops_filters.cpp (フィルター's kinds, by their kernels: render/fx)

// filter_raster's way of the C++ build's own: a kind of its own, or a Python kind with a native (core::native_filter).
bool handles(const core::Json& op);
// What filter_raster does with it: its settings in their normal form (core::normalize_filter: OpError for what is
// refused; a `channel` — PC1c's target, render/channels — left to filter_raster), and its function (each pixel) or its
// local filter. `python`: a Python kind whose native says nothing that changes it — filter_raster's own way of
// Python's form with `params` (its native taken out). not_yet_ported naming a kind or setting not in this build yet.
struct FilterWork {
    core::Json params;
    bool python = false;
    core::AdjustPtr function;
    const LocalFilter* local = nullptr;
};
FilterWork filter_work(const core::Document& doc, const core::Page& page, std::string_view kind, const core::Json& params);
// The work on each kind of a layer's pixels (not `python`): an 8-bit picture (the layer's RGBA; `area` is the caller's,
// filters.within — given here too for a filter that reads it, LocalFilter::eight_in, and for equalize, which counts the
// pixels inside it), a precise layer's new raster, an ink raster's new GKIR bytes.
Image filter_eight(const FilterWork& work, const core::Document& doc, const core::Page& page, const Image& rgba, std::string_view kind,
                   const core::Json* area = nullptr);
std::string filter_precise(const FilterWork& work, const core::Document& doc, const core::Page& page, const core::Layer& layer,
                           std::string_view kind, const core::Json* area);
std::string filter_inks(const FilterWork& work, const core::Document& doc, const core::Page& page, const core::InkPixels& inks,
                        std::string_view kind, const core::Json* area);

}  // namespace adjust_ops

}  // namespace genko::render
