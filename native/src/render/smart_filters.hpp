#pragma once

#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "core/command_bus.hpp"
#include "core/json.hpp"
#include "core/model.hpp"
#include "render/color_canvas.hpp"
#include "render/image.hpp"
#include "render/ink_canvas.hpp"

// smart-objects@1, PC3 A2 (docs/cpp-migration/SPEC.md COLOR-05「スマートフィルター」, ARCHITECTURE.md §4a): a smart object's
// smart filters — filter_raster's kinds kept on the smart object with their settings, laid on its picture each time it is
// drawn, never on its contents (PhotoCraft v0.2.0's smart filters: engine/src/smart_cmds.rs:236-308 — each visible filter
// on what is below it, laid over it in its blend mode at its opacity; the filter mask mixing the unfiltered and the
// filtered picture premultiplied —, MIT, third_party/photocraft/NOTICE.md).
//
// The filters work the smart object's picture of the whole page at the resolution it is drawn at (the coordinator's
// decision: the screen's, print's), as filter_raster works a layer of that page: the same code (render/color_filters,
// render/ink_filters, PC2's kinds through render/ops_adjust) on a stand-in page of the same paper and a stand-in layer
// holding the picture, its sizes in pixels the working 200 dpi's scaled to the resolution. The 8-bit page's filters go
// through 16 bits and back (an 8-bit page drawn at 200 dpi: within 1/255 of rasterize_layer and filter_raster). The ops
// that change them are render/ops_smart_filters.cpp; where they are drawn, render/smart.cpp.

namespace genko::render::smart_filters {

// One filter of the stack as it is drawn.
struct Filter {
    std::string kind;
    core::Json params = core::Json::object();
    std::string blend = "normal";
    double opacity = 1.0;
};

// The filters the smart object draws, the bottom one first: its visible ones while they are on (none otherwise).
std::vector<Filter> shown(const core::Layer& layer);

// Its filter mask while it is on: the PNG's ref (none otherwise).
std::optional<std::string> mask_ref(const core::Layer& layer);
// The filter mask's bytes (the book's assets, or Document::smart_files for one an op made); null when it is not held.
core::Bytes mask_bytes(const core::Document& doc, const std::string& ref);
// The filter mask as a picture of the page at `size` ("L", stretched over it bilinear, as a layer's mask is drawn).
Image mask_picture(const core::Document& doc, const std::string& ref, Size size);

// The stack laid on the smart object's picture of the whole page (`size` its pixels, `dpi`), each way: the 8-bit page's
// straight RGBA (sRGB), a precise page's linear light in its space, a page's inks. The filter mask mixes the result with
// the picture as it came. core::Error("value") / OpError for settings this build cannot use (a book changed by hand).
Image on_8bit(const core::Document& doc, const core::Page& page, const core::Layer& layer, const Image& rgba, int dpi);
ColorPicture on_precise(const core::Document& doc, const core::Page& page, const core::Layer& layer, const ColorPicture& picture,
                        const RgbSpace& space, int dpi);
InkPicture on_inks(const core::Document& doc, const core::Page& page, const core::Layer& layer, const InkPicture& picture, int dpi);

// An op's filter as a smart filter of a smart object on `page`: its kind one a smart object can hold (a plugin, a kind
// that reads another layer, a colour lookup of a table: OpError), its settings checked as filter_raster checks them and
// in the form the filter keeps (a kind of the C++ build's own, or a native: its normal form; else as given). The kind
// must work every way the smart object may be drawn on that page (not_yet_ported otherwise).
core::Json settled(const core::Document& doc, const core::Page& page, std::string_view kind, const core::Json& params);

// A filter mask made of an area (the op's selection: {poly} or {mask: {box, png}}), the page at 150 dpi: its PNG.
std::string mask_of_area(const core::Page& page, const core::Json& area);

// A smart filter laid on top of the smart object's (`params` settled here; normal, 100 %, shown), the area (the op's
// selection) its filter mask when it has none; its index. OpError past kFiltersMax or for what `settled` refuses. The
// ops that put one on a smart object of their own (PC3 A5: transform_layers' perspective and puppet warps, Liquify's
// strokes) and smart_filter_add go through it.
std::size_t push_filter(core::Document& doc, const core::Page& page, core::Layer& layer, const std::string& kind, const core::Json& params,
                        const std::optional<core::Json>& area);

// The ops (render/ops_smart_filters.cpp): smart_filter_add, smart_filter_set, smart_filter_move, smart_filter_delete and
// smart_filter_state (this build's own: `genko schema` and the MCP tools' list stay as they are).
void register_ops(core::OpRegistry& registry);

}  // namespace genko::render::smart_filters
