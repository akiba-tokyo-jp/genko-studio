#include "service/blocking.hpp"

#include <string>
#include <string_view>
#include <utility>

#include "core/pyops.hpp"

namespace genko::service {

using core::Num;

namespace {

// Head height as a fraction of panel height, and head centre height, per shot.
const std::pair<double, double>* head_of(const Json& shot) {
    static const std::pair<std::string_view, std::pair<double, double>> table[] = {
        {"ECU", {0.80, 0.50}}, {"CU", {0.55, 0.45}}, {"MCU", {0.38, 0.36}}, {"MS", {0.26, 0.30}},
        {"FS", {0.14, 0.22}},  {"LS", {0.10, 0.35}}, {"ELS", {0.05, 0.45}},
    };
    core::require_hashable(shot);
    if (!shot.is_string()) return nullptr;
    for (const auto& [key, value] : table) {
        if (shot.get_ref<const std::string&>() == key) return &value;
    }
    return nullptr;
}

double pos_of(const Json& pos) {
    static const std::pair<std::string_view, double> table[] = {
        {"left", 0.22}, {"left_third", 0.35}, {"center", 0.5}, {"right_third", 0.65}, {"right", 0.78}};
    core::require_hashable(pos);
    if (pos.is_string()) {
        for (const auto& [key, value] : table) {
            if (pos.get_ref<const std::string&>() == key) return value;
        }
    }
    return 0.5;
}

// Python's min(a, b) and max(a, b): the first unless the second is smaller (larger), each as it is.
Num least(const Num& a, const Num& b) { return b < a ? b : a; }
Num most(const Num& a, const Num& b) { return b > a ? b : a; }

}  // namespace

Box as_box(const NumBox& box) { return {box[0].value(), box[1].value(), box[2].value(), box[3].value()}; }

Json box_json(const NumBox& box, int places) {
    Json out = Json::array();
    for (const Num& v : box) out.push_back(core::py_round(v, places).json());
    return out;
}

std::vector<Figure> figures(const Json& panel, const NumBox& rect) {
    const auto& [x, y, w, h] = rect;
    const auto* head = head_of(core::py_get(panel, "shot", Json("MS")));
    const double size_frac = head != nullptr ? head->first : 0.0;
    const double centre_frac = head != nullptr ? head->second : 0.0;
    if (size_frac == 0.0) return {};
    std::vector<Figure> out;
    for (const Json& character : core::iterate(core::py_get(panel, "characters", Json::array()))) {
        const double scale = core::py_min(1.5, core::py_max(0.5, 0.5 + core::to_float(core::py_get(character, "scale", Json(0.8))) * 0.625));
        const Num hh = least(h * Num(0.95), h * Num(size_frac) * Num(scale));
        const Num hw = hh * Num(0.85);
        const Num cx = x + w * Num(pos_of(core::py_get(character, "pos", Json("center"))));
        const Num cy = y + h * Num(centre_frac);
        const Num hx = least(most(x, cx - hw / Num(2)), x + w - hw);
        const Num hy = least(most(y, cy - hh / Num(2)), y + h - hh);
        const Num bw = least(w, hw * Num(2.4));
        const Num bx = least(most(x, cx - bw / Num(2)), x + w - bw);
        out.push_back(Figure{core::py_get(character, "id", Json("")), {hx, hy, hw, hh}, {bx, hy, bw, y + h - hy}});
    }
    return out;
}

}  // namespace genko::service
