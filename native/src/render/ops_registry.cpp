#include "render/ops_registry.hpp"

#include "render/raster_ops.hpp"

namespace genko::render {

const core::OpRegistry& ops_registry() {
    static const core::OpRegistry registry = [] {
        core::OpRegistry r = core::OpRegistry::builtin();
        register_render_ops(r);
        return r;
    }();
    return registry;
}

void register_render_ops(core::OpRegistry& registry) { register_raster_ops(registry); }

}  // namespace genko::render
