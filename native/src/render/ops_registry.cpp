#include "render/ops_registry.hpp"

#include "render/ops_3d.hpp"
#include "render/raster_ops.hpp"

namespace genko::render {

void register_render_ops(core::OpRegistry& registry) {
    register_tone_ops(registry);
    register_effect_ops(registry);
    register_3d_ops(registry);
    register_raster_ops(registry);
}

const core::OpRegistry& ops_registry() {
    static const core::OpRegistry registry = [] {
        core::OpRegistry r;
        core::register_core_ops(r);
        register_render_ops(r);
        return r;
    }();
    return registry;
}

}  // namespace genko::render
