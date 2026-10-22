#include "render/path_areas.hpp"

#include <algorithm>

#include "core/command_bus.hpp"
#include "core/pyconv.hpp"
#include "core/pyops.hpp"
#include "render/path_raster.hpp"

namespace genko::render::path_areas {

using core::Json;
using core::OpError;

namespace {

// The area's "antialias" (default true for a path: its edge grey); false: two-valued.
bool smooth(const Json& area) {
    const auto it = area.find("antialias");
    if (it == area.end() || it->is_null()) return true;
    if (!it->is_boolean()) throw OpError("antialias must be true or false");
    return it->get<bool>();
}

Image coverage(const core::vector::Path& path, const Json& area, const area_kinds::Context& c) {
    Image mask = path_raster::path_mask(path, c.dpi, c.size);
    if (!smooth(area)) mask = mask.point([](int v) { return v >= 128 ? 255 : 0; });
    return mask;
}

const core::Layer& layer_of(const core::Page& page, const Json& id, std::string_view kind) {
    if (id.is_string()) {
        for (const core::Layer& layer : page.layers)
            if (layer.id == id.get_ref<const std::string&>()) return layer;
    }
    throw OpError(std::string(kind) + ": no layer " + core::py_str(id));
}

}  // namespace

bool is_work(const Json* name) { return name == nullptr || name->is_null() || (name->is_string() && name->get_ref<const std::string&>() == "work"); }

const core::vector::Path& named_path(const core::Page& page, const Json& name) {
    if (is_work(&name)) {
        if (!page.work_path) throw OpError("the page has no work path");
        return *page.work_path;
    }
    if (!name.is_string()) throw OpError("a path's name is a string (or \"work\")");
    const std::string wanted = core::py_strip(name.get<std::string>());
    for (const core::vector::NamedPath& p : page.paths)
        if (p.name == wanted) return p.path;
    throw OpError("no path named " + core::py_repr_str(wanted));
}

Image path(const Json& value, const Json& area, const area_kinds::Context& c) {
    core::vector::Path p;
    try {
        p = core::vector::path_from_json(value);
    } catch (const core::Error& error) {
        throw OpError(std::string("path: ") + error.what());
    }
    return coverage(p, area, c);
}

Image path_name(const Json& value, const Json& area, const area_kinds::Context& c) { return coverage(named_path(c.page, value), area, c); }

Image shape_part(const Json& value, const Json& area, const area_kinds::Context& c) {
    if (!value.is_object()) throw OpError("shape_part is {\"layer_id\", \"id\"}");
    const Json layer_id = value.value("layer_id", Json());
    const core::Layer& layer = layer_of(c.page, layer_id, "shape_part");
    const Json id = value.value("id", Json());
    for (const core::vector::ShapePart& part : layer.shapes->items)
        if (id.is_string() && part.id == id.get_ref<const std::string&>()) return coverage(part.path, area, c);
    throw OpError("shape_part: the layer " + layer.id + " has no shape part " + core::py_str(id));
}

Image vector_mask(const Json& value, const Json& area, const area_kinds::Context& c) {
    const core::Layer& layer = layer_of(c.page, value, "vector_mask");
    if (!layer.vector_mask) throw OpError("vector_mask: the layer " + layer.id + " has no vector mask");
    const core::vector::Path& p = layer.vector_mask->path;
    if (p.empty()) return Image::create("L", c.size, Ink(p.inverted ? 0 : 255));
    return coverage(p, area, c);
}

}  // namespace genko::render::path_areas
