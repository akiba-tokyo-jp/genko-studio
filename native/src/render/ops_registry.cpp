#include "render/ops_registry.hpp"

#include <algorithm>

#include "core/ops_util.hpp"
#include "core/pyops.hpp"
#include "core/studio.hpp"
#include "render/bookpages.hpp"
#include "render/color_edit.hpp"
#include "render/ops_3d.hpp"
#include "render/paper.hpp"
#include "render/placed.hpp"
#include "render/raster_ops.hpp"

namespace genko::render {

namespace {

// color-mode@1: the ops that work a paint layer's pixels and do not yet know the inks of a CMYK or Lab page (later
// steps of the colour modes, SPEC.md COLOR-02): refused there, never done in RGB or left undone without a word.
void refuse_on_ink_pages(const core::OpContext& c, const std::string& name) {
    static const std::vector<std::string_view> kPending{
        "transform_area", "delete_area", "paste", "erase_raster", "erase", "filter_raster", "smudge", "liquify",
        "merge_down", "merge_layers", "merge_visible", "convert_layer", "flood_fill", "gradient_fill", "add_shape",
        "fill_gaps", "stamp_material", "lt_convert", "put_color_raster", "render_prims",
        "trace_prims", "ruler_to_layer"};
    if (std::find(kPending.begin(), kPending.end(), name) == kPending.end()) return;
    const core::Json* page = core::get(c.op, "page");
    for (const auto& each : c.doc.pages) {
        if (!each->color) continue;
        const bool named = page != nullptr && !page->is_null() && page->is_number_integer() && each->index == core::Num(page->get<std::int64_t>());
        if (named || page == nullptr || page->is_null()) {
            core::not_yet_ported(name + " on a CMYK or Lab page is not in this build yet");
        }
    }
}

}  // namespace

void register_render_ops(core::OpRegistry& registry) {
    register_tone_ops(registry);
    register_effect_ops(registry);
    register_3d_ops(registry);
    register_raster_ops(registry);
    register_paint_ops(registry);
    // the line ops again, now able to look at a picture in a line's style (core's refuse it with not_yet_ported)
    core::register_line_ops(registry, verify_style_picture);
    // set_page_spec and import_pages again, now able to move a paint layer's pixels and to read the other book
    render::register_bookpage_ops(registry);
    // import_psd (fileops: a PSD or PSB read as layers, render/psd)
    register_file_ops(registry);
    // the colour modes of pages (color-mode@1: CMYK and Lab, render/ops_ink)
    register_ink_ops(registry);
    // the studio ops again, place_asset and set_placement able to read an asset's picture size, and those placing a
    // picture able to see it can be drawn (render/placed)
    core::register_studio_ops(registry, core::StudioHooks{asset_picture_size, check_asset_picture});
    // define_brush again, able to take a paper's picture in (BRUSH-01, render/paper)
    core::register_define_brush(registry, [](std::string_view file) {
        try {
            return paper::take_in(file);
        } catch (const core::Error& error) {
            throw core::OpError(error.what());
        }
    });
}

const core::OpRegistry& ops_registry() {
    static const core::OpRegistry registry = [] {
        core::OpRegistry r;
        core::register_core_ops(r);
        register_render_ops(r);
        // a precise paint layer takes what an op drew on it into its pixels (render/color_edit)
        for (const std::string& name : r.names()) {
            const core::OpFunction op = *r.find(name);
            r.add(name, [op, name](core::OpContext& c) {
                refuse_on_ink_pages(c, name);
                op(c);
                color_edit::bake_marks(c.doc);
            });
        }
        return r;
    }();
    return registry;
}

}  // namespace genko::render
