#include "core/covers.hpp"

#include "core/pyconv.hpp"

namespace genko::core {

namespace {

// float(cover.get(key) or fallback)
double number_or(const Json& cover, const char* key, double fallback) {
    const auto it = cover.find(key);
    if (it == cover.end() || !py_truthy(*it)) return fallback;
    return py_float(*it);
}

}  // namespace

PageSpec spec_for(const PageSpec& book, const Json& cover) {
    const auto kind = cover.find("kind");
    const bool wrap = kind != cover.end() && kind->is_string() &&
                      (kind->get_ref<const std::string&>() == "jacket" || kind->get_ref<const std::string&>() == "obi");
    if (!wrap) return book;
    const auto [trim_w, trim_h] = book.trim_size();
    const double spine = number_or(cover, "spine_mm", 0);
    const double flap = number_or(cover, "flap_mm", 0);
    const Num width = Num(2) * trim_w + Num(spine) + Num(2) * Num(flap);
    const Num allowance_w = book.width_mm - trim_w;
    PageSpec spec = book;
    spec.preset = "cover";
    spec.margins_mm.reset();
    if (kind->get_ref<const std::string&>() == "obi") {  // 帯: as wide as a jacket, as tall as the band
        const double band = number_or(cover, "height_mm", 50);
        const Num allowance_h = book.height_mm - trim_h;
        spec.width_mm = py_round(width + allowance_w, 3);
        spec.height_mm = py_round(Num(band) + allowance_h, 3);
        spec.trim_w_mm = py_round(width, 3);
        spec.trim_h_mm = py_round(Num(band), 3);
        return spec;
    }
    spec.width_mm = py_round(width + allowance_w, 3);
    spec.trim_w_mm = py_round(width, 3);
    spec.trim_h_mm = trim_h;
    return spec;
}

}  // namespace genko::core
