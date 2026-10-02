// Python's genko/prim3d.py, expression by expression (the math module through core/pymath, Python 3.12's sum()).

#include "core/prim3d.hpp"

#include <cmath>
#include <map>
#include <unordered_map>

#include "core/error.hpp"
#include "core/mannequin.hpp"
#include "core/pyconv.hpp"
#include "core/pynum.hpp"
#include "core/pyvalue.hpp"
#include "core/stroke_geom.hpp"

namespace genko::core::prim3d {

const std::array<std::string_view, 8> kKinds{"box", "cylinder", "stairs", "floor", "scene", "sphere", "cone", "prop"};
const std::array<std::string_view, 4> kScenes{"room", "classroom", "corridor", "street"};
const std::array<std::string_view, 8> kProps{"chair", "desk", "table", "bed", "door", "window", "shelf", "car"};

namespace {

using Box6 = std::array<double, 6>;

const std::vector<std::pair<std::string_view, std::vector<Box6>>>& props() {
    static const std::vector<std::pair<std::string_view, std::vector<Box6>>> table{
        {"chair",
         {{0.5, 0.52, 0.5, 1.0, 0.08, 1.0}, {0.08, 0.78, 0.08, 0.1, 0.44, 0.1}, {0.92, 0.78, 0.08, 0.1, 0.44, 0.1},
          {0.08, 0.78, 0.92, 0.1, 0.44, 0.1}, {0.92, 0.78, 0.92, 0.1, 0.44, 0.1}, {0.5, 0.24, 0.95, 1.0, 0.48, 0.08}}},
        {"desk",
         {{0.5, 0.05, 0.5, 1.0, 0.1, 1.0}, {0.05, 0.55, 0.05, 0.08, 0.9, 0.08}, {0.95, 0.55, 0.05, 0.08, 0.9, 0.08},
          {0.05, 0.55, 0.95, 0.08, 0.9, 0.08}, {0.95, 0.55, 0.95, 0.08, 0.9, 0.08}, {0.75, 0.25, 0.5, 0.45, 0.3, 0.95}}},
        {"table",
         {{0.5, 0.05, 0.5, 1.0, 0.1, 1.0}, {0.08, 0.55, 0.08, 0.08, 0.9, 0.08}, {0.92, 0.55, 0.08, 0.08, 0.9, 0.08},
          {0.08, 0.55, 0.92, 0.08, 0.9, 0.08}, {0.92, 0.55, 0.92, 0.08, 0.9, 0.08}}},
        {"bed",
         {{0.5, 0.75, 0.5, 1.0, 0.5, 1.0}, {0.5, 0.45, 0.5, 0.96, 0.15, 0.98}, {0.5, 0.4, 0.1, 0.6, 0.12, 0.16},
          {0.5, 0.35, 0.02, 1.0, 0.7, 0.04}}},
        {"door", {{0.5, 0.5, 0.5, 1.0, 1.0, 0.08}, {0.5, 0.52, 0.45, 0.84, 0.92, 0.02}, {0.85, 0.55, 0.4, 0.06, 0.04, 0.08}}},
        {"window",
         {{0.5, 0.5, 0.5, 1.0, 1.0, 0.1}, {0.5, 0.5, 0.5, 0.9, 0.9, 0.04}, {0.5, 0.5, 0.45, 0.04, 0.9, 0.06},
          {0.5, 0.5, 0.45, 0.9, 0.04, 0.06}}},
        {"shelf",
         {{0.5, 0.5, 0.5, 1.0, 1.0, 1.0}, {0.5, 0.25, 0.5, 0.94, 0.03, 0.96}, {0.5, 0.5, 0.5, 0.94, 0.03, 0.96},
          {0.5, 0.75, 0.5, 0.94, 0.03, 0.96}}},
        {"car",
         {{0.5, 0.68, 0.5, 1.0, 0.36, 1.0}, {0.45, 0.33, 0.5, 0.55, 0.34, 0.9}, {0.18, 0.9, 0.02, 0.18, 0.2, 0.06},
          {0.82, 0.9, 0.02, 0.18, 0.2, 0.06}, {0.18, 0.9, 0.98, 0.18, 0.2, 0.06}, {0.82, 0.9, 0.98, 0.18, 0.2, 0.06}}},
    };
    return table;
}

constexpr int kEdges[12][2] = {{0, 1}, {1, 3}, {3, 2}, {2, 0}, {4, 5}, {5, 7}, {7, 6}, {6, 4}, {0, 4}, {1, 5}, {2, 6}, {3, 7}};
constexpr int kFaces[6][4] = {{0, 1, 3, 2}, {4, 6, 7, 5}, {0, 4, 5, 1}, {2, 3, 7, 6}, {0, 2, 6, 4}, {1, 5, 7, 3}};

double py_max(double a, double b) { return b > a ? b : a; }

// Python's float a // b (CPython's _float_div_mod)
double py_floordiv(double vx, double wx) {
    double mod = std::fmod(vx, wx);
    double div = (vx - mod) / wx;
    if (mod != 0.0) {
        if ((wx < 0) != (mod < 0)) div -= 1.0;
    }
    if (div != 0.0) {
        double floordiv = std::floor(div);
        if (div - floordiv > 0.5) floordiv += 1.0;
        return floordiv;
    }
    return std::copysign(0.0, vx / wx);
}

const Json* truthy(const Json* camera) { return camera != nullptr && py_truthy(*camera) ? camera : nullptr; }

std::string kind_of(const Json& prim) {
    const Json* kind = pyv::get(prim, "kind");
    return kind != nullptr && kind->is_string() ? kind->get<std::string>() : std::string();
}

bool kind_is(const Json& prim, std::initializer_list<std::string_view> names) {
    const Json* kind = pyv::get(prim, "kind");
    return kind != nullptr && pyv::is_one_of(*kind, names);
}

// prim.get("kind", "box") == "box"
bool is_box(const Json& prim) {
    const Json* kind = pyv::get(prim, "kind");
    return kind == nullptr || pyv::eq(*kind, Json("box"));
}

const Json& rot_or(const Json& prim, const Json& fallback) { return pyv::get_or(prim, "rot", fallback); }

const Json& seen_on_its_own() {
    static const Json kRot = Json::array({0.3, 0.6, 0});
    return kRot;
}

struct Place {
    Json cx;
    Json cy;
    Json cz;
};

// cx, cy, cz = (list(prim.get("pos") or [100, 150, 0]) + [0, 0, 0])[:3]
Place place_of(const Json& prim) {
    static const Json kDefault = Json::array({100, 150, 0});
    Json pos = py_list(pyv::get_or(prim, "pos", kDefault));
    for (int i = 0; i < 3; ++i) pos.push_back(0);
    return Place{pos[0], pos[1], pos[2]};
}

// float(prim.get("focal_mm", 400) or 400)
double focal_of(const Json& prim) {
    const Json* f = pyv::get(prim, "focal_mm");
    return f == nullptr || !py_truthy(*f) ? 400.0 : pyv::to_float(*f);
}

std::vector<Segment3> box_edges(double cx, double cy, double cz, double w, double h, double d) {
    std::vector<Vec3> pts;
    for (const int sx : {-1, 1}) {
        for (const int sy : {-1, 1}) {
            for (const int sz : {-1, 1}) pts.push_back(Vec3{cx + sx * w / 2, cy + sy * h / 2, cz + sz * d / 2});
        }
    }
    std::vector<Segment3> out;
    for (const auto& e : kEdges) out.emplace_back(pts[static_cast<std::size_t>(e[0])], pts[static_cast<std::size_t>(e[1])]);
    return out;
}

void rect_edges(std::vector<Segment3>& out, const std::array<Vec3, 4>& corners) {
    for (std::size_t i = 0; i < 4; ++i) out.emplace_back(corners[i], corners[(i + 1) % 4]);
}

void append(std::vector<Segment3>& out, const std::vector<Segment3>& more) { out.insert(out.end(), more.begin(), more.end()); }

std::vector<Segment3> ring_segments(const std::vector<Vec3>& ring) {
    std::vector<Segment3> out;
    for (std::size_t i = 0; i + 1 < ring.size(); ++i) out.emplace_back(ring[i], ring[i + 1]);
    return out;
}

}  // namespace

const std::vector<std::array<double, 6>>* prop_boxes(std::string_view name) {
    for (const auto& [key, boxes] : props()) {
        if (key == name) return &boxes;
    }
    return nullptr;
}

std::pair<Json, double> scene_view(std::string_view kind) {
    if (kind == "room") return {Json::array({-0.38, 0.28, 0}), -0.1};
    if (kind == "classroom") return {Json::array({-0.36, 0.2, 0}), -0.15};
    if (kind == "corridor") return {Json::array({-0.06, 0.0, 0}), -0.4};
    return {Json::array({-0.04, 0.08, 0}), -0.4};  // street
}

Json scene_size(std::string_view kind) {
    if (kind == "room") return Json::array({160, 90, 140});
    if (kind == "classroom") return Json::array({200, 90, 220});
    if (kind == "corridor") return Json::array({70, 80, 320});
    return Json::array({220, 120, 360});  // street
}

Vec3 size_of(const Json& prim) {
    static const Json kDefault = Json::array({40, 40, 40});
    Json size = pyv::get_or(prim, "size", kDefault);
    if (!size.is_array()) size = Json::array({size, size, size});
    Json items = size;
    if (items.size() < 3) {
        const Json last = pyv::at(size, -1);
        while (items.size() < 3) items.push_back(last);
    }
    Vec3 out{};
    for (std::size_t i = 0; i < 3; ++i) out[i] = py_max(0.1, pyv::to_float(items[i]));
    return out;
}

Vec3 rotate(const Vec3& p, const Json& rot) {
    Json items = py_list(rot);
    for (int i = 0; i < 3; ++i) items.push_back(0);
    double x = p[0];
    double y = p[1];
    double z = p[2];
    // turn (about the upright axis), then tip (about the across axis), then lean (in the page)
    const double turn = pyv::real(items[1]);
    const double nx = x * py_cos(turn) - z * py_sin(turn);
    const double nz = x * py_sin(turn) + z * py_cos(turn);
    x = nx;
    z = nz;
    const double tip = pyv::real(items[0]);
    const double ny = y * py_cos(tip) - z * py_sin(tip);
    const double nz2 = y * py_sin(tip) + z * py_cos(tip);
    y = ny;
    z = nz2;
    const double lean = pyv::real(items[2]);
    const double nx2 = x * py_cos(lean) - y * py_sin(lean);
    const double ny2 = x * py_sin(lean) + y * py_cos(lean);
    return Vec3{nx2, ny2, z};
}

std::vector<Vec3> corners3d(const Json& prim) {
    const Vec3 size = size_of(prim);
    const double w = size[0];
    const double h = size[1];
    const double d = size[2];
    const Json& rot = rot_or(prim, seen_on_its_own());
    std::vector<Vec3> out;
    for (const double dx : {-w / 2, w / 2}) {
        for (const double dy : {-h / 2, h / 2}) {
            for (const double dz : {-d / 2, d / 2}) out.push_back(rotate(Vec3{dx, dy, dz}, rot));
        }
    }
    return out;
}

std::vector<Point2> project(const Json& prim, const Json* camera) {
    camera = truthy(camera);
    if (camera != nullptr) {
        const Vec3 size = size_of(prim);
        const double w = size[0];
        const double h = size[1];
        const double d = size[2];
        std::vector<Vec3> local;
        for (const int sx : {-1, 1}) {
            for (const int sy : {-1, 1}) {
                for (const int sz : {-1, 1}) local.push_back(Vec3{sx * w / 2, sy * h / 2, sz * d / 2});
            }
        }
        return mesh3d::to_page(prim, local, camera).pts;
    }
    const Place at = place_of(prim);
    const double focal = focal_of(prim);
    std::vector<Point2> out;
    for (const Vec3& c : corners3d(prim)) {
        const double scale = focal / py_max(focal * 0.2, focal + pyv::to_float(at.cz) + c[2]);
        out.push_back(Point2{pyv::to_float(at.cx) + c[0] * scale, pyv::to_float(at.cy) + c[1] * scale});
    }
    return out;
}

std::vector<Segment3> scene_parts(std::string_view kind, const Vec3& size) {
    const double w = size[0];
    const double h = size[1];
    const double d = size[2];
    const double g = h / 2;  // the ground
    const double top = -h / 2;
    const double back = d / 2;
    std::vector<Segment3> out;
    const auto grid = [&](double x0, double x1, double z0, double z1, int nx, int nz) {
        for (int k = 0; k <= nx; ++k) {
            const double x = x0 + (x1 - x0) * k / nx;
            out.emplace_back(Vec3{x, g, z0}, Vec3{x, g, z1});
        }
        for (int k = 0; k <= nz; ++k) {
            const double z = z0 + (z1 - z0) * k / nz;
            out.emplace_back(Vec3{x0, g, z}, Vec3{x1, g, z});
        }
    };
    const auto wall_x = [&](double x, double z0, double z1) {
        rect_edges(out, {Vec3{x, top, z0}, Vec3{x, top, z1}, Vec3{x, g, z1}, Vec3{x, g, z0}});
    };
    const auto wall_z = [&](double z, double x0, double x1) {
        rect_edges(out, {Vec3{x0, top, z}, Vec3{x1, top, z}, Vec3{x1, g, z}, Vec3{x0, g, z}});
    };
    const auto hole_x = [&](double x, double z0, double z1, double y0, double y1) {
        rect_edges(out, {Vec3{x, y0, z0}, Vec3{x, y0, z1}, Vec3{x, y1, z1}, Vec3{x, y1, z0}});
    };
    const auto hole_z = [&](double z, double x0, double x1, double y0, double y1) {
        rect_edges(out, {Vec3{x0, y0, z}, Vec3{x1, y0, z}, Vec3{x1, y1, z}, Vec3{x0, y1, z}});
    };
    const auto seg = [&](const Vec3& a, const Vec3& b) { out.emplace_back(a, b); };
    if (kind == "room") {
        grid(-w / 2, w / 2, -d / 2, back, 6, 6);
        wall_z(back, -w / 2, w / 2);
        wall_x(-w / 2, -d / 2, back);
        wall_x(w / 2, -d / 2, back);
        hole_z(back, -w * 0.25, w * 0.15, top + h * 0.25, top + h * 0.6);   // a window
        hole_z(back, -w * 0.25, w * 0.15, top + h * 0.42, top + h * 0.43);  // its sash
        hole_x(-w / 2, back - d * 0.45, back - d * 0.15, top + h * 0.12, g);  // a door
        append(out, box_edges(w * 0.2, g - h * 0.2, back - d * 0.35, w * 0.3, h * 0.04, d * 0.22));  // a table top
        for (const int sx : {-1, 1}) {
            for (const int sz : {-1, 1}) {
                const double x = w * 0.2 + sx * w * 0.13;
                const double z = back - d * 0.35 + sz * d * 0.09;
                seg(Vec3{x, g - h * 0.18, z}, Vec3{x, g, z});
            }
        }
        append(out, box_edges(w * 0.33, g - h * 0.12, back - d * 0.12, w * 0.28, h * 0.24, d * 0.18));  // a bed / sofa
    } else if (kind == "classroom") {
        grid(-w / 2, w / 2, -d / 2, back, 8, 10);
        wall_z(back, -w / 2, w / 2);
        wall_x(-w / 2, -d / 2, back);
        wall_x(w / 2, -d / 2, back);
        hole_z(back, -w * 0.3, w * 0.3, top + h * 0.25, top + h * 0.6);  // the blackboard
        seg(Vec3{-w * 0.3, top + h * 0.62, back}, Vec3{w * 0.3, top + h * 0.62, back});  // its chalk tray
        for (int k = 0; k < 4; ++k) {  // windows along the left
            const double z0 = -d / 2 + d * (0.1 + k * 0.22);
            hole_x(-w / 2, z0, z0 + d * 0.18, top + h * 0.2, top + h * 0.65);
        }
        hole_x(w / 2, back - d * 0.25, back - d * 0.1, top + h * 0.15, g);  // the door
        for (int row = 0; row < 3; ++row) {  // desks and chairs
            for (int col = 0; col < 4; ++col) {
                const double x = -w * 0.3 + col * w * 0.2;
                const double z = back - d * 0.4 - row * d * 0.18;
                append(out, box_edges(x, g - h * 0.28, z, w * 0.11, h * 0.03, d * 0.07));
                for (const int sx : {-1, 1}) seg(Vec3{x + sx * w * 0.05, g - h * 0.27, z}, Vec3{x + sx * w * 0.05, g, z});
                append(out, box_edges(x, g - h * 0.17, z - d * 0.07, w * 0.08, h * 0.02, d * 0.05));
                seg(Vec3{x, g - h * 0.17, z - d * 0.095}, Vec3{x, g - h * 0.32, z - d * 0.095});
            }
        }
        append(out, box_edges(0, g - h * 0.15, back - d * 0.12, w * 0.24, h * 0.3, d * 0.08));  // the teacher's desk
    } else if (kind == "corridor") {
        grid(-w / 2, w / 2, -d / 2, back, 3, 16);
        wall_x(-w / 2, -d / 2, back);
        wall_x(w / 2, -d / 2, back);
        wall_z(back, -w / 2, w / 2);
        seg(Vec3{-w / 2, top, -d / 2}, Vec3{w / 2, top, -d / 2});
        for (int k = 0; k < 5; ++k) {  // doors on the right, windows on the left
            const double z0 = -d / 2 + d * (0.05 + k * 0.19);
            hole_x(w / 2, z0, z0 + d * 0.07, top + h * 0.15, g);
            hole_x(-w / 2, z0, z0 + d * 0.13, top + h * 0.2, top + h * 0.6);
        }
        for (int k = 0; k < 6; ++k) {  // ceiling lights
            const double z = -d / 2 + d * (0.08 + k * 0.16);
            append(out, box_edges(0, top + 1, z, w * 0.2, 1, d * 0.03));
        }
    } else if (kind == "street") {
        const double road = w * 0.36;
        grid(-road / 2, road / 2, -d / 2, back, 2, 12);
        for (const int side : {-1, 1}) {  // sidewalks and their kerbs
            const double x_in = side * road / 2;
            const double x_out = side * w * 0.36;
            seg(Vec3{x_in, g, -d / 2}, Vec3{x_in, g, back});
            seg(Vec3{x_in, g - 1.5, -d / 2}, Vec3{x_in, g - 1.5, back});
            seg(Vec3{x_out, g, -d / 2}, Vec3{x_out, g, back});
        }
        const double heights[] = {0.9, 0.55, 1.0, 0.7, 0.45, 0.8};
        for (const int side : {-1, 1}) {  // buildings
            double z = -d / 2;
            for (int k = 0; k < 6; ++k) {
                const double frac = side < 0 ? heights[k] : heights[5 - k];
                const double depth = d / 6;
                const double bh = h * frac;
                const double x = side * (w * 0.36 + w * 0.07);
                append(out, box_edges(x, g - bh / 2, z + depth / 2, w * 0.14, bh, depth * 0.9));
                // floors, as lines on the street side (range(1, int(bh // 18) + 1))
                const double floors = py_floordiv(bh, 18);
                if (!std::isfinite(floors)) throw Error(std::isnan(floors) ? "value" : "overflow", "cannot convert float to integer");
                const auto count = static_cast<std::int64_t>(std::trunc(std::min(floors, 1e9)));
                // (a building a few kilometres tall would have more floor lines than anyone could draw)
                if (count > 10000) throw Error("image_too_large", "the scene is too large to draw");
                for (std::int64_t floor = 1; floor <= count; ++floor) {
                    const double y = g - static_cast<double>(floor * 18);
                    const double xs = x - side * w * 0.07;
                    seg(Vec3{xs, y, z + depth * 0.1}, Vec3{xs, y, z + depth * 0.8});
                }
                z += depth;
            }
        }
        for (int k = 0; k < 4; ++k) {  // utility poles along the left
            const double z = -d / 2 + d * (0.1 + k * 0.25);
            const double x = -road / 2 - w * 0.02;
            seg(Vec3{x, g, z}, Vec3{x, g - h * 0.75, z});
            seg(Vec3{x - 6, g - h * 0.68, z}, Vec3{x + 6, g - h * 0.68, z});
        }
        seg(Vec3{-road / 2 - w * 0.02, g - h * 0.7, -d / 2}, Vec3{-road / 2 - w * 0.02, g - h * 0.7, back});  // the wire
    }
    return out;
}

std::vector<Segment3> segments3d(const Json& prim) {
    const Vec3 size = size_of(prim);
    const double w = size[0];
    const double h = size[1];
    const double d = size[2];
    const std::string kind = kind_of(prim);
    std::vector<Segment3> out;
    if (kind == "sphere") {
        const double rx = w / 2;
        const double ry = h / 2;
        const double rz = d / 2;
        const int n = 32;
        const double tau = 6.283185307179586;
        for (int k = 1; k < 6; ++k) {  // the latitudes
            const double phi = kPi * k / 6;
            std::vector<Vec3> ring;
            for (int i = 0; i <= n; ++i) {
                ring.push_back(Vec3{rx * py_sin(phi) * py_cos(tau * i / n), -ry * py_cos(phi), rz * py_sin(phi) * py_sin(tau * i / n)});
            }
            append(out, ring_segments(ring));
        }
        for (int k = 0; k < 6; ++k) {  // and the meridians
            const double th = kPi * k / 6;
            std::vector<Vec3> ring;
            for (int i = 0; i <= n; ++i) {
                ring.push_back(Vec3{rx * py_sin(tau * i / n) * py_cos(th), -ry * py_cos(tau * i / n), rz * py_sin(tau * i / n) * py_sin(th)});
            }
            append(out, ring_segments(ring));
        }
        return out;
    }
    if (kind == "cone") {
        const int n = 32;
        const double rx = w / 2;
        const double rz = d / 2;
        const double tau = 6.283185307179586;
        std::vector<Vec3> ring;
        for (int k = 0; k <= n; ++k) ring.push_back(Vec3{rx * py_cos(tau * k / n), h / 2, rz * py_sin(tau * k / n)});
        append(out, ring_segments(ring));
        const Vec3 apex{0.0, -h / 2, 0.0};
        for (int k = 0; k < 8; ++k) out.emplace_back(apex, ring[static_cast<std::size_t>(k * n / 8)]);
        return out;
    }
    if (kind == "prop") {
        const Json* prop = pyv::get(prim, "prop");
        const std::string name = prop != nullptr && py_truthy(*prop) ? py_str(*prop) : std::string("chair");
        const auto* boxes = prop_boxes(name);
        if (boxes == nullptr) boxes = prop_boxes("chair");
        for (const auto& [bx, by, bz, bw, bh, bd] : *boxes) {
            append(out, box_edges((bx - 0.5) * w, (by - 0.5) * h, (bz - 0.5) * d, bw * w, bh * h, bd * d));
        }
        return out;
    }
    if (kind == "cylinder") {
        const int n = 32;
        const double rx = w / 2;
        const double rz = d / 2;
        const double tau = 6.283185307179586;
        for (const double y : {-h / 2, h / 2}) {
            std::vector<Vec3> ring;
            for (int k = 0; k <= n; ++k) ring.push_back(Vec3{rx * py_cos(tau * k / n), y, rz * py_sin(tau * k / n)});
            append(out, ring_segments(ring));
        }
        for (int k = 0; k < 4; ++k) {
            const double a = tau * k / 4;
            out.emplace_back(Vec3{rx * py_cos(a), -h / 2, rz * py_sin(a)}, Vec3{rx * py_cos(a), h / 2, rz * py_sin(a)});
        }
    } else if (kind == "scene") {
        const Json* scene = pyv::get(prim, "scene");
        out = scene_parts(scene != nullptr && py_truthy(*scene) ? py_str(*scene) : std::string("room"), Vec3{w, h, d});
    } else if (kind == "stairs") {
        const Json* steps_value = pyv::get(prim, "steps");
        const std::int64_t steps =
            std::max<std::int64_t>(2, std::min<std::int64_t>(30, steps_value != nullptr && py_truthy(*steps_value) ? pyv::to_int(*steps_value) : 6));
        const double rise = h / static_cast<double>(steps);
        const double run = d / static_cast<double>(steps);
        std::vector<std::pair<double, double>> profile{{h / 2, -d / 2}};  // (y, z): up is −y; the stairs climb toward the back
        for (std::int64_t k = 0; k < steps; ++k) {
            const auto [y, z] = profile.back();
            profile.emplace_back(y - rise, z);
            profile.emplace_back(y - rise, z + run);
        }
        profile.emplace_back(h / 2, d / 2);
        profile.push_back(profile.front());
        for (const double x : {-w / 2, w / 2}) {
            std::vector<Vec3> pts;
            for (const auto& [y, z] : profile) pts.push_back(Vec3{x, y, z});
            append(out, ring_segments(pts));
        }
        for (std::size_t i = 0; i + 1 < profile.size(); ++i) {
            out.emplace_back(Vec3{-w / 2, profile[i].first, profile[i].second}, Vec3{w / 2, profile[i].first, profile[i].second});
        }
    } else {  // floor: a grid on the ground
        const Json* lines_value = pyv::get(prim, "lines");
        const std::int64_t n =
            std::max<std::int64_t>(2, std::min<std::int64_t>(40, lines_value != nullptr && py_truthy(*lines_value) ? pyv::to_int(*lines_value) : 8));
        for (std::int64_t k = 0; k <= n; ++k) {
            const double x = -w / 2 + w * static_cast<double>(k) / static_cast<double>(n);
            const double z = -d / 2 + d * static_cast<double>(k) / static_cast<double>(n);
            out.emplace_back(Vec3{x, 0.0, -d / 2}, Vec3{x, 0.0, d / 2});
            out.emplace_back(Vec3{-w / 2, 0.0, z}, Vec3{w / 2, 0.0, z});
        }
    }
    return out;
}

Point2 to_page(const Json& prim, const Vec3& p, const Json* camera) {
    camera = truthy(camera);
    if (camera != nullptr) return mesh3d::to_page(prim, std::span<const Vec3>(&p, 1), camera).pts[0];
    const Place at = place_of(prim);
    const double focal = focal_of(prim);
    const Vec3 r = rotate(p, rot_or(prim, seen_on_its_own()));
    const double scale = focal / py_max(focal * 0.2, focal + pyv::to_float(at.cz) + r[2]);
    return Point2{pyv::to_float(at.cx) + r[0] * scale, pyv::to_float(at.cy) + r[1] * scale};
}

std::vector<Edge> edges(const Json& prim, const Json* camera) {
    camera = truthy(camera);
    if (kind_is(prim, {"figure", "head", "hand", "mesh"})) {
        std::vector<Edge> out;
        const auto lines = mesh3d::prim_lines(prim, camera, 16.0);  // (held: the cache may let go of its copy)
        for (const Line2& line : *lines) {
            for (std::size_t i = 0; i + 1 < line.size(); ++i) out.push_back(Edge{line[i], line[i + 1], true});
        }
        return out;
    }
    if (kind_is(prim, {"cylinder", "stairs", "floor", "scene", "sphere", "cone", "prop"}) || (camera != nullptr && is_box(prim))) {
        std::vector<Edge> out;
        for (const auto& [a, b] : segments3d(prim)) out.push_back(Edge{to_page(prim, a, camera), to_page(prim, b, camera), true});
        return out;
    }
    const std::vector<Vec3> pts3 = corners3d(prim);
    const std::vector<Point2> pts = project(prim, camera);
    const double focal = focal_of(prim);
    static const Json kOrigin = Json::array({0, 0, 0});
    Json pos = py_list(pyv::get_or(prim, "pos", kOrigin));
    for (int i = 0; i < 3; ++i) pos.push_back(0);
    const double cz = pyv::to_float(pos[2]);
    bool seen_faces[6]{};
    int seen_count = 0;
    for (int f = 0; f < 6; ++f) {
        const Vec3& a = pts3[static_cast<std::size_t>(kFaces[f][0])];
        const Vec3& b = pts3[static_cast<std::size_t>(kFaces[f][1])];
        const Vec3& c = pts3[static_cast<std::size_t>(kFaces[f][2])];
        const Vec3 u{b[0] - a[0], b[1] - a[1], b[2] - a[2]};
        const Vec3 v{c[0] - a[0], c[1] - a[1], c[2] - a[2]};
        const Vec3 n{u[1] * v[2] - u[2] * v[1], u[2] * v[0] - u[0] * v[2], u[0] * v[1] - u[1] * v[0]};
        Vec3 centre{};
        for (int k = 0; k < 3; ++k) {
            const double items[4] = {pts3[static_cast<std::size_t>(kFaces[f][0])][k], pts3[static_cast<std::size_t>(kFaces[f][1])][k],
                                     pts3[static_cast<std::size_t>(kFaces[f][2])][k], pts3[static_cast<std::size_t>(kFaces[f][3])][k]};
            centre[static_cast<std::size_t>(k)] = py_float_sum(items) / 4;
        }
        centre[2] += cz + focal;  // from the camera (at -focal) to the face
        seen_faces[f] = n[0] * centre[0] + n[1] * centre[1] + n[2] * centre[2] < 0;
        seen_count += seen_faces[f] ? 1 : 0;
    }
    // the face winding decides the sign; if every face or none is "seen", flip the test
    if (seen_count > 3) {
        for (bool& s : seen_faces) s = !s;
    }
    std::vector<Edge> out;
    for (const auto& e : kEdges) {
        bool seen = false;
        for (int f = 0; f < 6 && !seen; ++f) {
            bool has_i = false;
            bool has_j = false;
            for (const int c : kFaces[f]) {
                has_i |= c == e[0];
                has_j |= c == e[1];
            }
            seen = seen_faces[f] && has_i && has_j;
        }
        out.push_back(Edge{pts[static_cast<std::size_t>(e[0])], pts[static_cast<std::size_t>(e[1])], seen});
    }
    return out;
}

namespace {

std::array<double, 4> box_of(const std::vector<Point2>& pts) {
    // min(xs), min(ys), max(xs) - min(xs), max(ys) - min(ys) with Python's min and max
    if (pts.empty()) throw Error("value", "min() arg is an empty sequence");
    double x0 = pts[0][0];
    double y0 = pts[0][1];
    double x1 = pts[0][0];
    double y1 = pts[0][1];
    for (const Point2& p : pts) {
        if (p[0] < x0) x0 = p[0];
        if (p[1] < y0) y0 = p[1];
        if (p[0] > x1) x1 = p[0];
        if (p[1] > y1) y1 = p[1];
    }
    return {x0, y0, x1 - x0, y1 - y0};
}

}  // namespace

std::array<double, 4> bbox(const Json& prim, const Json* camera) {
    if (kind_is(prim, {"figure", "head", "hand", "mesh"})) return prim_bbox(prim, camera);
    std::vector<Point2> pts;
    if (is_box(prim)) {
        pts = project(prim, camera);
    } else {
        for (const Edge& e : edges(prim, camera)) {
            pts.push_back(e.a);
            pts.push_back(e.b);
        }
    }
    return box_of(pts);
}

std::array<double, 4> prim_bbox(const Json& prim, const Json* camera) {
    camera = truthy(camera);
    if (kind_is(prim, {"figure", "head", "hand", "mesh"})) {
        const mesh3d::Seen s = mesh3d::seen(prim, camera);
        if (s.at.pts.empty()) {
            static const Json kOrigin = Json::array({0, 0});
            const Json& pos = pyv::get_or(prim, "pos", kOrigin);
            return {pyv::to_float(pyv::at(pos, 0)), pyv::to_float(pyv::at(pos, 1)), 0.0, 0.0};
        }
        // (numpy's min and max: a NaN wins)
        double x0 = s.at.pts[0][0];
        double y0 = s.at.pts[0][1];
        double x1 = x0;
        double y1 = y0;
        for (const Point2& p : s.at.pts) {
            x0 = la::np_minimum(x0, p[0]);
            y0 = la::np_minimum(y0, p[1]);
            x1 = la::np_maximum(x1, p[0]);
            y1 = la::np_maximum(y1, p[1]);
        }
        return {x0, y0, x1 - x0, y1 - y0};
    }
    if (kind_is(prim, {"mannequin"})) return mannequin::skeleton(prim).bbox;
    return bbox(prim, camera);
}

std::vector<Line2> trace(const Json& prim, const Json* camera) {
    camera = truthy(camera);
    if (kind_is(prim, {"figure", "head", "hand", "mesh"})) return *mesh3d::prim_lines(prim, camera, 40.0);
    if (kind_is(prim, {"mannequin"})) {
        const mannequin::Bone bone = mannequin::skeleton(prim);
        std::vector<Line2> out;
        for (const mannequin::Segment& s : bone.segments) out.push_back(Line2{s.a, s.b});
        Line2 head;
        for (int k = 0; k < 25; ++k) {
            head.push_back(Point2{bone.head_c[0] + bone.head_r * py_cos(k * kPi / 12), bone.head_c[1] + bone.head_r * py_sin(k * kPi / 12)});
        }
        out.push_back(std::move(head));
        return out;
    }
    std::vector<Line2> out;
    for (const Edge& e : edges(prim, camera)) {
        if (e.seen) out.push_back(Line2{e.a, e.b});
    }
    return out;
}

namespace {

struct EndKey {
    double x;
    double y;
    bool operator==(const EndKey&) const = default;
};

struct EndHash {
    std::size_t operator()(const EndKey& k) const {
        return std::hash<double>{}(k.x) * 0x9E3779B97F4A7C15ULL ^ std::hash<double>{}(k.y);
    }
};

}  // namespace

std::vector<Line2> join_lines(const std::vector<Line2>& lines, double eps) {
    // key(p) = (round(p[0] / eps), round(p[1] / eps)): Python's round to an int (half to even)
    const auto key = [eps](const Point2& p) {
        const double x = p[0] / eps;
        const double y = p[1] / eps;
        if (std::isnan(x) || std::isnan(y)) throw Error("value", "cannot convert float NaN to integer");
        if (std::isinf(x) || std::isinf(y)) throw Error("overflow", "cannot convert float infinity to integer");
        EndKey k{std::nearbyint(x), std::nearbyint(y)};
        if (k.x == 0.0) k.x = 0.0;  // (an int has no -0)
        if (k.y == 0.0) k.y = 0.0;
        return k;
    };
    std::vector<Line2> pending;
    for (const Line2& line : lines) {
        if (line.size() >= 2) pending.push_back(line);
    }
    std::unordered_map<EndKey, std::vector<std::size_t>, EndHash> ends;
    for (std::size_t i = 0; i < pending.size(); ++i) {
        ends[key(pending[i].front())].push_back(i);
        ends[key(pending[i].back())].push_back(i);
    }
    std::vector<bool> used(pending.size(), false);
    const auto take = [&](const Point2& point) -> std::optional<Line2> {
        const EndKey k = key(point);
        const auto it = ends.find(k);
        if (it == ends.end()) return std::nullopt;
        for (const std::size_t j : it->second) {
            if (!used[j]) {
                used[j] = true;
                const Line2& other = pending[j];
                if (key(other.front()) == k) return other;
                return Line2(other.rbegin(), other.rend());
            }
        }
        return std::nullopt;
    };
    std::vector<Line2> joined;
    for (std::size_t i = 0; i < pending.size(); ++i) {
        if (used[i]) continue;
        used[i] = true;
        Line2 chain = pending[i];
        while (const auto next = take(chain.back())) chain.insert(chain.end(), next->begin() + 1, next->end());
        while (const auto prev = take(chain.front())) {
            // chain[:0] = prev[::-1][:-1]
            Line2 head(prev->rbegin(), prev->rend());
            head.pop_back();
            chain.insert(chain.begin(), head.begin(), head.end());
        }
        joined.push_back(std::move(chain));
    }
    return joined;
}

}  // namespace genko::core::prim3d
