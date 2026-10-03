#include "render/ops_registry.hpp"

namespace genko::render {

const core::OpRegistry& ops_registry() {
    static const core::OpRegistry registry = [] {
        core::OpRegistry r;
        const core::OpRegistry& core_ops = core::OpRegistry::builtin();
        for (const std::string& name : core_ops.names()) r.add(name, *core_ops.find(name));
        // (render's own ops are registered here as they are ported)
        return r;
    }();
    return registry;
}

}  // namespace genko::render
