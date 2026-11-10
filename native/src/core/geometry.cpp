#include "core/geometry.hpp"

namespace genko::core {

bool Rect::contains(const Num& px, const Num& py) const {
    return x <= px && px <= x + width && y <= py && py <= y + height;
}

Json rect_to_json(const Rect& rect) {
    Json out = Json::object();
    out["x"] = rect.x.json();
    out["y"] = rect.y.json();
    out["width"] = rect.width.json();
    out["height"] = rect.height.json();
    return out;
}

}  // namespace genko::core
