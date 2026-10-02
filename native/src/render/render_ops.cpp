#include "render/render_ops.hpp"

namespace genko::render {

void register_render_ops(core::OpRegistry& registry) {
    register_tone_ops(registry);
    register_effect_ops(registry);
}

const core::OpRegistry& registry_with_render_ops() {
    static const core::OpRegistry registry = [] {
        core::OpRegistry r = core::OpRegistry::builtin();
        register_render_ops(r);
        return r;
    }();
    return registry;
}

}  // namespace genko::render
