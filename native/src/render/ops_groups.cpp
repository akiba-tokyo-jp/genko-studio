#include "render/ops_groups.hpp"

#include <array>
#include <string>

#include "core/groups.hpp"
#include "core/ids.hpp"
#include "core/ops_util.hpp"
#include "core/pyconv.hpp"
#include "render/layer_bounds.hpp"

namespace genko::render {

namespace {

void frame_from_layers(core::OpContext& c) {
    core::Json op = c.op;
    op["op"] = "group_layers";
    const std::string id = core::truthy_at(op, "id") ? core::py_str(op["id"]) : core::new_id();
    op["id"] = id;
    if (!core::truthy_at(op, "name")) op["name"] = "フレーム";
    op["group"] = true;
    core::OpContext grouping{c.doc, op, c.actor};
    core::group_layers_op(grouping);
    const std::size_t at = core::require_page(c.doc, op);
    const core::Page& page = c.doc.page(at);
    std::array<double, 4> box{0.0, 0.0, page.spec.width_mm.value(), page.spec.height_mm.value()};
    if (const auto held = layer_bounds::of_layer(c.doc, page, id); held && (*held)[2] > (*held)[0] && (*held)[3] > (*held)[1]) box = *held;
    core::vector::Subpath rect;
    rect.closed = true;
    for (const auto& [x, y] : {std::array<double, 2>{box[0], box[1]}, {box[2], box[1]}, {box[2], box[3]}, {box[0], box[3]}})
        rect.knots.push_back(core::vector::Knot::corner(x, y));
    core::vector::VectorMask mask;
    mask.path.subpaths.push_back(std::move(rect));
    core::Page& edited = c.doc.edit_page(at);
    for (core::Layer& layer : edited.layers) {
        if (layer.id == id) layer.vector_mask = std::move(mask);
    }
    const auto round4 = [](double v) { return core::py_round(v, 4); };
    c.report = core::Json{{"id", id},
                          {"frame_mm", core::Json::array({round4(box[0]), round4(box[1]), round4(box[2] - box[0]), round4(box[3] - box[1])})}};
}

}  // namespace

void register_group_ops(core::OpRegistry& registry) { registry.add("frame_from_layers", frame_from_layers); }

}  // namespace genko::render
