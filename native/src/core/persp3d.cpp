// Python's genko/persp3d.py, expression by expression (numpy through core/linalg3, the math module through
// core/pymath).

#include "core/persp3d.hpp"

#include <cmath>

#include "core/command_bus.hpp"
#include "core/error.hpp"
#include "core/linalg3.hpp"
#include "core/mesh3d.hpp"
#include "core/pyconv.hpp"
#include "core/pynum.hpp"
#include "core/pyvalue.hpp"

namespace genko::core::persp3d {

namespace {

using la::Mat3;
using la::Vec3;

const Json* truthy(const Json* camera) { return camera != nullptr && py_truthy(*camera) ? camera : nullptr; }

// x[:2] of a list or a str
Json first_two(const Json& value) {
    if (value.is_array()) return pyv::slice(value, 0, 2);
    if (value.is_string()) return pyv::slice(py_list(value), 0, 2);
    if (value.is_object()) throw Error("type", "unhashable type: 'slice'");
    throw Error("type", "'" + py_type_name(value) + "' object is not subscriptable");
}

const Json& pos_or(const Json& prim, const Json& fallback) { return pyv::get_or(prim, "pos", fallback); }

// camera.get("tip", 0), camera.get("turn", 0), camera.get("roll", 0)
Json view_of(const Json& camera) {
    Json out = Json::array();
    for (const char* key : {"tip", "turn", "roll"}) {
        const Json* v = pyv::get(camera, key);
        out.push_back(v != nullptr ? *v : Json(0));
    }
    return out;
}

struct Parts {
    Mat3 rotation;
    double cx = 0.0;
    double cy = 0.0;
    double focal = 400.0;
};

Parts parts(const Json& prim, const Json* camera) {
    camera = truthy(camera);
    const Json* rot = pyv::get(prim, "rot");
    static const Json kSeenStraight = Json::array({0, 0, 0});
    static const Json kSeenOnItsOwn = Json::array({0.3, 0.6, 0});
    const Mat3 turn = mesh3d::prim_rotation(rot != nullptr && !rot->is_null() ? *rot : (camera != nullptr ? kSeenStraight : kSeenOnItsOwn));
    Parts out;
    if (camera != nullptr) {
        const Mat3 view = mesh3d::prim_rotation(view_of(*camera));
        const Json* target_value = pyv::get(*camera, "target");
        static const Json kCentre = Json::array({100, 150});
        const Json target = target_value != nullptr && py_truthy(*target_value) ? *target_value : first_two(pos_or(prim, kCentre));
        const Json* focal_value = pyv::get(*camera, "focal_mm");
        if (focal_value != nullptr && py_truthy(*focal_value)) {
            out.focal = pyv::to_float(*focal_value);
        } else if (const Json* own = pyv::get(prim, "focal_mm"); own != nullptr && py_truthy(*own)) {
            out.focal = pyv::to_float(*own);
        }
        out.rotation = la::matmul(view, turn);
        out.cx = pyv::to_float(pyv::at(target, 0));
        out.cy = pyv::to_float(pyv::at(target, 1));
        return out;
    }
    static const Json kDefault = Json::array({100, 150, 0});
    Json pos = py_list(pos_or(prim, kDefault));
    for (int i = 0; i < 3; ++i) pos.push_back(0);
    out.rotation = turn;
    out.cx = pyv::to_float(pos[0]);
    out.cy = pyv::to_float(pos[1]);
    const Json* focal_value = pyv::get(prim, "focal_mm");
    out.focal = focal_value == nullptr || !py_truthy(*focal_value) ? 400.0 : pyv::to_float(*focal_value);
    return out;
}

// numpy's argsort of a few values (an insertion sort: ties keep their order, NaN last)
std::vector<std::size_t> argsort(const std::vector<double>& v) {
    std::vector<std::size_t> order(v.size());
    for (std::size_t i = 0; i < v.size(); ++i) order[i] = i;
    const auto less = [&](double a, double b) { return a < b || (b != b && a == a); };
    for (std::size_t i = 1; i < order.size(); ++i) {
        const std::size_t vi = order[i];
        std::size_t j = i;
        while (j > 0 && less(v[vi], v[order[j - 1]])) {
            order[j] = order[j - 1];
            --j;
        }
        order[j] = vi;
    }
    return order;
}

std::size_t argmin(const std::vector<double>& v) {
    for (std::size_t i = 0; i < v.size(); ++i) {
        if (std::isnan(v[i])) return i;
    }
    std::size_t best = 0;
    for (std::size_t i = 1; i < v.size(); ++i) {
        if (v[i] < v[best]) best = i;
    }
    return best;
}

double py_max(double a, double b) { return b > a ? b : a; }
double py_min(double a, double b) { return b < a ? b : a; }

// float(x) where Python's ValueError is reported as the op's own error (camera_for's `except ValueError`)
double value_float(const Json& v) {
    try {
        return pyv::to_float(v);
    } catch (const Error& error) {
        if (error.code() == "value") throw OpError(error.what());
        throw;
    }
}

}  // namespace

Json vanishing_points(const Json& prim, const Json* camera) {
    const Parts p = parts(prim, camera);
    Json out = Json::object();
    for (const auto& [name, u] : {std::pair<const char*, Vec3>{"x", {1.0, 0.0, 0.0}}, {"z", {0.0, 0.0, 1.0}}, {"y", {0.0, 1.0, 0.0}}}) {
        const Vec3 r = la::matvec(p.rotation, u);
        if (std::fabs(r[2]) < 1e-4) continue;
        const double x = p.cx + p.focal * r[0] / r[2];
        const double y = p.cy + p.focal * r[1] / r[2];
        if (py_hypot(x - p.cx, y - p.cy) <= kFarMm) out[name] = Json::array({py_round(x, 2), py_round(y, 2)});
    }
    return out;
}

std::optional<Json> ruler_from(const Json& prim, const Json* camera) {
    const Json vps = vanishing_points(prim, camera);
    Json points = Json::array();
    for (const char* k : {"x", "z", "y"}) {
        if (vps.contains(k)) points.push_back(vps[k]);
    }
    if (points.empty()) return std::nullopt;
    Json out = Json::object();
    out["kind"] = "perspective";
    out["points"] = pyv::slice(points, 0, 3);
    out["lock_horizon"] = points.size() >= 2;
    return out;
}

Json camera_for(const Json& ruler, const Json& prim, const Json& camera) {
    static const Json kNone = Json::array();
    std::vector<std::vector<double>> goal;
    for (const Json& p : py_list(pyv::get_or(ruler, "points", kNone))) {
        std::vector<double> xy;
        for (const Json& c : py_list(first_two(p))) xy.push_back(value_float(c));
        goal.push_back(std::move(xy));
    }
    if (goal.empty()) throw OpError("the perspective ruler has no vanishing points");
    std::vector<std::string> names;
    if (goal.size() == 1) {
        names = {"z"};
    } else {
        for (const char* n : {"x", "z", "y"}) {
            if (names.size() < goal.size()) names.emplace_back(n);
        }
    }
    const Json base = py_dict(py_truthy(camera) ? camera : Json::object());
    const Json* target_value = pyv::get(base, "target");
    static const Json kCentre = Json::array({100, 150});
    const Json target = target_value != nullptr && py_truthy(*target_value) ? *target_value : first_two(pyv::get_or(prim, "pos", kCentre));
    Json flat = prim;
    flat["rot"] = Json::array({0, 0, 0});
    const bool three = goal.size() >= 3;

    const auto cost = [&](const Vector& v) {
        const double tip = v[0];
        const double turn = v[1];
        const double roll = v[2];
        Json cam = base;
        cam["tip"] = tip;
        cam["turn"] = turn;
        cam["roll"] = three ? roll : 0.0;
        cam["focal_mm"] = la::py_exp(v[3]);
        cam["target"] = target;
        const Json vps = vanishing_points(flat, &cam);
        double total = 0.0;
        for (std::size_t i = 0; i < names.size() && i < goal.size(); ++i) {
            if (goal[i].size() != 2) {
                throw OpError(goal[i].size() > 2 ? "too many values to unpack (expected 2)"
                                                 : "not enough values to unpack (expected 2, got " + std::to_string(goal[i].size()) + ")");
            }
            if (!vps.contains(names[i])) {
                total += 1e6;
                continue;
            }
            const Json& vp = vps[names[i]];
            total += py_hypot(vp[0].get<double>() - goal[i][0], vp[1].get<double>() - goal[i][1]);
        }
        if (goal.size() == 1) total += 50 * (std::fabs(roll) + std::fabs(tip) * 0.2);  // (the across and upright axes stay square)
        return total;
    };

    std::optional<Vector> best;
    for (const double turn : {-1.2, -0.6, 0.0, 0.6, 1.2}) {
        for (const double tip : {-0.3, 0.0, 0.3}) {
            const Json* focal_value = pyv::get(base, "focal_mm");
            const double focal = focal_value != nullptr && py_truthy(*focal_value) ? value_float(*focal_value) : 400.0;
            double log_focal = 0.0;
            try {
                log_focal = la::py_log(focal);
            } catch (const Error& error) {
                throw OpError(error.what());  // (ValueError: math domain error)
            }
            const Vector found = minimize(cost, Vector{tip, turn, 0.0, log_focal});
            if (!best || cost(found) < cost(*best)) best = found;
        }
    }
    const Vector& v = *best;
    Json out = base;
    out["tip"] = py_round(v[0], 4);
    out["turn"] = py_round(v[1], 4);
    out["roll"] = py_round(three ? v[2] : 0.0, 4);
    out["focal_mm"] = py_round(py_min(5000.0, py_max(20.0, la::py_exp(v[3]))), 1);
    out["target"] = Json::array({value_float(pyv::at(target, 0)), value_float(pyv::at(target, 1))});
    return out;
}

Vector minimize(const std::function<double(const Vector&)>& f, const Vector& x0, int steps) {
    const std::size_t n = x0.size();
    std::vector<Vector> simplex{x0};
    for (std::size_t i = 0; i < n; ++i) {
        Vector x = x0;
        x[i] += 0.25;
        simplex.push_back(std::move(x));
    }
    std::vector<double> values;
    for (const Vector& x : simplex) values.push_back(f(x));
    const auto combine = [n](const Vector& a, double s, const Vector& b, const Vector& c) {  // a + s * (b - c)
        Vector out(n);
        for (std::size_t i = 0; i < n; ++i) out[i] = a[i] + s * (b[i] - c[i]);
        return out;
    };
    for (int step = 0; step < steps; ++step) {
        const std::vector<std::size_t> order = argsort(values);
        std::vector<Vector> sorted;
        std::vector<double> sorted_values;
        for (const std::size_t i : order) {
            sorted.push_back(simplex[i]);
            sorted_values.push_back(values[i]);
        }
        simplex = std::move(sorted);
        values = std::move(sorted_values);
        if (values.back() - values.front() < 1e-6) break;
        // centre = sum(simplex[:-1]) / n  (Python's sum: 0 + the first, then each in turn)
        Vector centre(n, 0.0);
        for (std::size_t k = 0; k + 1 < simplex.size(); ++k) {
            for (std::size_t i = 0; i < n; ++i) centre[i] = centre[i] + simplex[k][i];
        }
        for (double& c : centre) c = c / static_cast<double>(n);
        const Vector worst = simplex.back();
        Vector reflected(n);
        for (std::size_t i = 0; i < n; ++i) reflected[i] = centre[i] + (centre[i] - worst[i]);
        const double fr = f(reflected);
        if (fr < values[0]) {
            Vector expanded(n);
            for (std::size_t i = 0; i < n; ++i) expanded[i] = centre[i] + 2 * (centre[i] - worst[i]);
            const double fe = f(expanded);
            if (fe < fr) {
                simplex.back() = expanded;
                values.back() = fe;
            } else {
                simplex.back() = reflected;
                values.back() = fr;
            }
        } else if (fr < values[values.size() - 2]) {
            simplex.back() = reflected;
            values.back() = fr;
        } else {
            const Vector contracted = combine(centre, 0.5, worst, centre);
            const double fc = f(contracted);
            if (fc < values.back()) {
                simplex.back() = contracted;
                values.back() = fc;
            } else {
                const Vector best = simplex[0];
                for (Vector& x : simplex) x = combine(best, 0.5, x, best);
                for (std::size_t k = 0; k < simplex.size(); ++k) values[k] = f(simplex[k]);
            }
        }
    }
    return simplex[argmin(values)];
}

}  // namespace genko::core::persp3d
