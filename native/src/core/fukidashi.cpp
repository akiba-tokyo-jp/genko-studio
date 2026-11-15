#include "core/fukidashi.hpp"

#include <initializer_list>
#include <optional>

#include "core/pyconv.hpp"
#include "core/pynum.hpp"
#include "core/pyops.hpp"
#include "core/smart.hpp"

namespace genko::core {

namespace {

template <typename Fn>
bool any_line(const Document& doc, Fn fn) {
    for (const StoryLine& line : doc.story) {
        if (fn(line)) return true;
    }
    for (const smart::WrittenPage& written : smart::written_pages(doc)) {
        for (const Layer& layer : written.page->layers) {
            if (layer.type && fn(*layer.type)) return true;
        }
    }
    return false;
}

// style.border_mm is there and 0 (a number: 0, 0.0 or false — what the drawing reads as no width).
bool border_zero(const StoryLine& line) {
    if (!line.style.is_object()) return false;
    const auto border = line.style.find("border_mm");
    if (border == line.style.end() || border->is_null()) return false;
    return (border->is_number() || border->is_boolean()) && !py_truthy(*border);
}

// [[x, y], …] moved by (dx, dy), to nine decimals; anything that is not such a list is left as it is.
void move_points(Json& points, double dx, double dy) {
    if (!points.is_array()) return;
    for (Json& p : points) {
        if (!p.is_array() || p.size() < 2 || !p[0].is_number() || !p[1].is_number()) continue;
        p[0] = py_round(p[0].get<double>() + dx, 9);
        p[1] = py_round(p[1].get<double>() + dy, 9);
    }
}

}  // namespace

bool is_oval(const StoryLine& line) {
    return line.balloon.empty() || line.balloon == "speech" || line.balloon == "thought" || line.balloon == "whisper";
}

bool letters_in_oval(const StoryLine& line) {
    if (!is_oval(line)) return false;
    if (line.native_type && line.native_type->is_object()) {  // (type@1: a point or box text is not set in a balloon)
        const auto shape = line.native_type->find("shape");
        if (shape != line.native_type->end() && shape->is_string() && (*shape == Json("box") || *shape == Json("point"))) return false;
    }
    if (!line.style.is_object()) return true;
    if (const auto hand = line.style.find("hand"); hand != line.style.end() && !py_truthy(*hand)) return false;
    if (const auto wobble = line.style.find("wobble"); wobble != line.style.end() && py_truthy(*wobble)) return false;
    return true;
}

bool legacy_oval(const StoryLine& line) { return letters_in_oval(line) && !(line.path && !line.path->empty()); }

bool is_symbol(std::string_view kind) { return kind == "heart" || kind == "star" || kind == "flower" || kind == "diamond"; }

namespace {

// A number of a line's style (0, 0.0 and what is not a number: none).
std::optional<double> style_number(const StoryLine& line, std::string_view key) {
    if (!line.style.is_object()) return std::nullopt;
    const auto found = line.style.find(key);
    if (found == line.style.end() || !found->is_number()) return std::nullopt;
    const double value = found->is_number_integer() ? static_cast<double>(found->get<std::int64_t>()) : found->get<double>();
    if (value == 0.0 || value != value) return std::nullopt;
    return value;
}

}  // namespace

int symbol_points(const StoryLine& line) {
    const std::optional<double> points = style_number(line, "points");
    if (!points) return line.balloon == "flower" ? 6 : 5;
    return static_cast<int>(py_max(4.0, py_min(12.0, *points)));
}

double star_inner(const StoryLine& line) {
    const double depth = style_number(line, "spike_depth").value_or(0.38);
    return 1 - py_max(0.05, py_min(0.6, depth));
}

double drip_reach(const StoryLine& line) {
    if (!line.style.is_object()) return 0.0;
    const auto drip = line.style.find("drip_mm");
    if (drip == line.style.end() || !drip->is_number()) return 0.0;
    if (line.path && !line.path->empty()) return 0.0;
    for (const std::string_view kind : {"dotted_box", "flash", "picture", "sfx", "none"}) {
        if (line.balloon == kind) return 0.0;
    }
    const double h = line.h_mm.value();
    const double asked = drip->is_number_integer() ? static_cast<double>(drip->get<std::int64_t>()) : drip->get<double>();
    if (!(asked > 0) || !(h > 0) || !(line.w_mm.value() > 0)) return 0.0;
    return py_round(py_min(py_min(asked, 30.0), 0.6 * h), 9);
}

std::array<double, 4> body_box(const StoryLine& line) {
    const double x = line.x_mm.value(), y = line.y_mm.value(), w = line.w_mm.value(), h = line.h_mm.value();
    const double drip = drip_reach(line);
    if (!(drip > 0)) return {x, y, w, h};
    return {x, y, w, py_round(h - drip, 9)};
}

std::array<double, 4> letter_box(const StoryLine& line) {
    const auto [x, y, w, h] = body_box(line);
    if (!legacy_oval(line) || !(w > 0) || !(h > 0)) return {x, y, w, h};
    const double letters_w = py_round(w / kOvalGrowth, 9);
    const double letters_h = py_round(h / kOvalGrowth, 9);
    return {py_round(x + (w - letters_w) / 2, 9), py_round(y + (h - letters_h) / 2, 9), letters_w, letters_h};
}

bool migrate_legacy_line(StoryLine& line) {
    bool changed = false;
    if (border_zero(line)) {
        line.style["border_mm"] = kHairlineMm;
        changed = true;
    }
    const double w = line.w_mm.value();
    const double h = line.h_mm.value();
    if (!legacy_oval(line) || !(w > 0) || !(h > 0)) return changed;
    const double x = line.x_mm.value();
    const double y = line.y_mm.value();
    const double grown_w = py_round(w * kOvalGrowth, 9);
    const double grown_h = py_round(h * kOvalGrowth, 9);
    const double grown_x = py_round(x - (grown_w - w) / 2, 9);
    const double grown_y = py_round(y - (grown_h - h) / 2, 9);
    line.x_mm = Num(grown_x);
    line.y_mm = Num(grown_y);
    line.w_mm = Num(grown_w);
    line.h_mm = Num(grown_h);
    // (what the line holds from its box's corner stays where it was on the page)
    if (line.style.is_object()) {
        if (const auto path = line.style.find("text_path"); path != line.style.end()) move_points(*path, x - grown_x, y - grown_y);
        if (const auto cuts = line.style.find("cuts"); cuts != line.style.end() && cuts->is_array()) {
            for (Json& cut : *cuts) {
                if (cut.is_object() && cut.contains("points")) move_points(cut["points"], x - grown_x, y - grown_y);
            }
        }
    }
    return true;
}

bool uses_fukidashi(const StoryLine& line) {
    if (line.balloon == "capsule" || line.balloon == "polygon") return true;
    if (line.style.is_object()) {
        for (const std::string_view key : {"corner_mm", "squircle", "sides", "wave_mm", "spike_alt", "dash_mm"}) {
            if (line.style.contains(key)) return true;
        }
    }
    for (const Json& tail : line.tails) {
        if (!tail.is_object()) continue;
        if (tail.contains("fillet_mm")) return true;
        if (const auto kind = tail.find("kind"); kind != tail.end() && *kind == Json("inward")) return true;
    }
    return false;
}

bool uses_fukidashi(const Document& doc) {
    return any_line(doc, [](const StoryLine& line) { return uses_fukidashi(line); });
}

bool uses_fukidashi2(const StoryLine& line) {
    if (is_symbol(line.balloon)) return true;
    if (line.style.is_object()) {
        for (const std::string_view key : {"pen_swell", "pen_angle", "pen_taper", "pen_strokes", "drip_mm", "drips", "drip_width_mm", "drip_seed", "points",
                                           "fill_tone", "fill_gradient"}) {
            if (line.style.contains(key)) return true;
        }
    }
    return false;
}

bool uses_fukidashi2(const Document& doc) {
    return any_line(doc, [](const StoryLine& line) { return uses_fukidashi2(line); });
}

bool says_balloon_geometry(const Document& doc) {
    return any_line(doc, [](const StoryLine& line) { return is_oval(line) || border_zero(line); });
}

}  // namespace genko::core
