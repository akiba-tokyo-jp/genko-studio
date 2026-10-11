#include "render/ops_registry.hpp"

#include "core/ops_util.hpp"
#include "core/studio.hpp"
#include "render/bookpages.hpp"
#include "render/color_edit.hpp"
#include "render/ops_3d.hpp"
#include "render/paper.hpp"
#include "render/placed.hpp"
#include "render/raster_ops.hpp"

namespace genko::render {

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
    // the studio ops again, place_asset and set_placement able to read an asset's picture size (render/placed)
    core::register_studio_ops(registry, core::StudioHooks{asset_picture_size});
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
            r.add(name, [op](core::OpContext& c) {
                op(c);
                color_edit::bake_marks(c.doc);
            });
        }
        return r;
    }();
    return registry;
}

}  // namespace genko::render
