// vanishing-point@1 (PC4 節 D6): the planes a page keeps (core/vanishing.hpp). The planes' shape is PhotoCraft v0.2.0's
// engine/src/vp_cmds.rs scene (MIT, native/third_party/photocraft/NOTICE.md); Genko keeps them on the page, in mm.

#include "core/vanishing.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <string>

#include "core/command_bus.hpp"
#include "core/error.hpp"
#include "core/pyconv.hpp"
#include "core/pyops.hpp"
#include "core/smart.hpp"

namespace genko::core {

namespace {

bool finite_number(const Json& v) { return v.is_number() && !v.is_boolean() && std::isfinite(v.get<double>()); }

// Four corners [x, y] (mm) of a convex quad, not flat.
Json read_corners(const Json& value, const std::string& where) {
    const std::string wrong = where + ".corners_mm must be four corners [x, y] (mm) of a convex quad";
    if (!value.is_array() || value.size() != 4) throw OpError(wrong);
    std::array<std::array<double, 2>, 4> q{};
    Json out = Json::array();
    for (std::size_t i = 0; i < 4; ++i) {
        const Json& p = value[i];
        if (!p.is_array() || p.size() != 2 || !finite_number(p[0]) || !finite_number(p[1])) throw OpError(wrong);
        q[i] = {p[0].get<double>(), p[1].get<double>()};
        if (std::fabs(q[i][0]) > 1e6 || std::fabs(q[i][1]) > 1e6) throw OpError(wrong);
        out.push_back(Json::array({q[i][0], q[i][1]}));
    }
    // (each turn the same way, none straight: a convex quad, its area more than a hundredth of a mm²)
    int sign = 0;
    double area = 0;
    for (std::size_t i = 0; i < 4; ++i) {
        const auto& a = q[i];
        const auto& b = q[(i + 1) % 4];
        const auto& c = q[(i + 2) % 4];
        const double cross = (b[0] - a[0]) * (c[1] - b[1]) - (b[1] - a[1]) * (c[0] - b[0]);
        const int s = cross > 1e-9 ? 1 : cross < -1e-9 ? -1 : 0;
        if (s == 0 || (sign != 0 && s != sign)) throw OpError(wrong);
        sign = s;
        area += a[0] * b[1] - b[0] * a[1];
    }
    if (std::fabs(area) / 2 < 0.01) throw OpError(wrong);
    return out;
}

}  // namespace

Json read_vanishing(const Json& value) {
    const std::string where = "vanishing";
    if (!value.is_object()) throw OpError("vanishing must be {\"planes\": […], \"focal_mm\"?}");
    for (const auto& [key, v] : value.items()) {
        if (key != "planes" && key != "focal_mm") throw OpError("vanishing has no setting " + py_repr_str(key) + " (planes, focal_mm)");
    }
    const Json* planes = get(value, "planes");
    if (planes == nullptr || !planes->is_array() || planes->empty() || planes->size() > kVanishingMostPlanes)
        throw OpError("vanishing.planes must be a list of 1 to " + std::to_string(kVanishingMostPlanes) +
                      " planes: the first {\"corners_mm\": [[x, y] × 4]}, a later one its corners or {\"from\", \"edge\", \"angle\", \"depth\"}");
    Json out = Json::object();
    Json list = Json::array();
    for (std::size_t i = 0; i < planes->size(); ++i) {
        const Json& plane = (*planes)[i];
        const std::string here = where + ".planes[" + std::to_string(i) + "]";
        if (!plane.is_object()) throw OpError(here + " must be an object");
        for (const auto& [key, v] : plane.items()) {
            if (key != "corners_mm" && key != "from" && key != "edge" && key != "angle" && key != "depth")
                throw OpError(here + " has no setting " + py_repr_str(key) + " (corners_mm, or from, edge, angle, depth)");
        }
        if (const Json* corners = get(plane, "corners_mm"); corners != nullptr && !corners->is_null()) {
            if (plane.contains("from")) throw OpError(here + " has its corners or is torn off an earlier plane (from), not both");
            list.push_back(Json{{"corners_mm", read_corners(*corners, here)}});
            continue;
        }
        if (i == 0) throw OpError("vanishing.planes[0] needs its four corners (corners_mm)");
        const Json* from = get(plane, "from");
        if (from == nullptr || !from->is_number_integer() || from->get<std::int64_t>() < 0 || from->get<std::int64_t>() >= static_cast<std::int64_t>(i))
            throw OpError(here + ".from must be an earlier plane (0 to " + std::to_string(i - 1) + ")");
        std::string edge = "top";
        if (const Json* e = get(plane, "edge"); e != nullptr && !e->is_null()) {
            if (!e->is_string() || (*e != "top" && *e != "right" && *e != "bottom" && *e != "left"))
                throw OpError(here + ".edge must be top, right, bottom or left");
            edge = e->get<std::string>();
        }
        double angle = 90, depth = 1;
        if (const Json* a = get(plane, "angle"); a != nullptr && !a->is_null()) {
            if (!finite_number(*a)) throw OpError(here + ".angle must be a number (degrees)");
            angle = std::clamp(a->get<double>(), -360.0, 360.0);
        }
        if (const Json* d = get(plane, "depth"); d != nullptr && !d->is_null()) {
            if (!finite_number(*d)) throw OpError(here + ".depth must be a number");
            depth = std::clamp(d->get<double>(), 0.05, 20.0);
        }
        list.push_back(Json{{"from", from->get<std::int64_t>()}, {"edge", edge}, {"angle", angle}, {"depth", depth}});
    }
    out["planes"] = std::move(list);
    if (const Json* f = get(value, "focal_mm"); f != nullptr && !f->is_null()) {
        if (!finite_number(*f) || f->get<double>() <= 0 || f->get<double>() > 1e6) throw OpError("vanishing.focal_mm must be a length above 0 (mm)");
        out["focal_mm"] = f->get<double>();
    }
    return out;
}

const Json* page_vanishing(const Page& page) {
    const Json* v = page.extra.is_object() ? get(page.extra, kVanishingKey) : nullptr;
    return v != nullptr && !v->is_null() ? v : nullptr;
}

bool uses_vanishing(const Document& doc) {
    for (const smart::WrittenPage& written : smart::written_pages(doc)) {
        if (page_vanishing(*written.page) != nullptr) return true;
    }
    return false;
}

void validate_vanishing(const Document& doc) {
    for (const smart::WrittenPage& written : smart::written_pages(doc)) {
        const Json* v = page_vanishing(*written.page);
        if (v == nullptr) continue;
        try {
            (void)read_vanishing(*v);
        } catch (const OpError& error) {
            throw Error("value", std::string("a page's vanishing point planes: ") + error.what());
        }
    }
}

}  // namespace genko::core
