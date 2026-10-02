#include "render/ops_registry.hpp"

#include "render/ops_3d.hpp"

namespace genko::render {

void register_render_ops(core::OpRegistry& registry) {
    register_tone_ops(registry);    // tones: add, set, delete (M3)
    register_effect_ops(registry);  // effect lines: add, edit, delete, to a layer (M3)
    register_3d_ops(registry);      // 3D: figures, heads, hands, models, guides, scenes, the camera and the light (M3)
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
