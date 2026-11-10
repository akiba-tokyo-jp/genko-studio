#include "core/areas.hpp"

#include "core/command_bus.hpp"
#include "core/pyconv.hpp"
#include "core/pyops.hpp"

namespace genko::core {

namespace {

[[noreturn]] void no_get(const Json& value) {
    throw PyUncaught("AttributeError", "'" + py_type_name(value) + "' object has no attribute 'get'");
}

}  // namespace

Json op_area(const Json& op) {
    const Json* given = get(op, "area");
    const Json area = given != nullptr && py_truthy(*given) ? *given : Json::object();
    if (!area.is_object()) no_get(area);
    if (truthy_at(area, "poly")) {
        if (length(area["poly"]) < 3) throw OpError("an area needs at least three corners");
        return area;
    }
    if (truthy_at(area, "mask")) {
        const Json& mask = area["mask"];
        if (!mask.is_object()) no_get(mask);
        if (truthy_at(mask, "box") && truthy_at(mask, "png")) return area;
    }
    throw OpError("area is {poly: [[x, y], …]} or {mask: {box, png}}");
}

}  // namespace genko::core
