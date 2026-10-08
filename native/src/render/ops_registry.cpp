#include "render/ops_registry.hpp"

#include "core/ops_util.hpp"
#include "render/color_edit.hpp"
#include "render/ops_3d.hpp"
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
