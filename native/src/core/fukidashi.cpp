#include "core/fukidashi.hpp"

#include <initializer_list>

#include "core/pynum.hpp"
#include "core/pyconv.hpp"
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

}  // namespace

bool is_oval(const StoryLine& line) {
    return line.balloon.empty() || line.balloon == "speech" || line.balloon == "thought" || line.balloon == "whisper";
}

bool legacy_oval(const StoryLine& line) {
    if (!is_oval(line) || (line.path && !line.path->empty())) return false;
    if (!line.style.is_object()) return true;
    if (const auto hand = line.style.find("hand"); hand != line.style.end() && !py_truthy(*hand)) return false;
    if (const auto wobble = line.style.find("wobble"); wobble != line.style.end() && py_truthy(*wobble)) return false;
    return true;
}

bool grow_legacy_oval(StoryLine& line) {
    if (!legacy_oval(line)) return false;
    const double w = line.w_mm.value();
    const double h = line.h_mm.value();
    if (!(w > 0) || !(h > 0)) return false;
    const double grown_w = py_round(w * kOvalGrowth, 3);
    const double grown_h = py_round(h * kOvalGrowth, 3);
    line.x_mm = Num(py_round(line.x_mm.value() - (grown_w - w) / 2, 3));
    line.y_mm = Num(py_round(line.y_mm.value() - (grown_h - h) / 2, 3));
    line.w_mm = Num(grown_w);
    line.h_mm = Num(grown_h);
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

bool has_ovals(const Document& doc) {
    return any_line(doc, [](const StoryLine& line) { return is_oval(line); });
}

}  // namespace genko::core
