#include "core/covers.hpp"

#include <array>
#include <map>

#include "core/pyconv.hpp"
#include "core/pyops.hpp"

namespace genko::core {

namespace {

// float(cover.get(key) or fallback)
double number_or(const Json& cover, const char* key, double fallback) {
    const auto it = cover.find(key);
    if (it == cover.end() || !py_truthy(*it)) return fallback;
    return py_float(*it);
}

// The cover kind of a page that is one (its cover_of(page)["kind"]).
std::string kind_of(const Page& page) { return cover_of(page)->at("kind").get<std::string>(); }

// {cover_of(p)["kind"]: p for p in episode.pages if is_cover(p)}: the last page of each kind
std::map<std::string, const Page*> covers_by_kind(const Document& doc) {
    std::map<std::string, const Page*> out;
    for (const auto& page : doc.pages) {
        if (is_cover(*page)) out[kind_of(*page)] = page.get();
    }
    return out;
}

}  // namespace

bool is_cover_kind(std::string_view kind) {
    return kind == "front" || kind == "back" || kind == "jacket" || kind == "obi";
}

bool is_wrap_kind(std::string_view kind) { return kind == "jacket" || kind == "obi"; }

const Json* cover_of(const Page& page) {
    if (!page.extra.is_object()) return nullptr;
    const auto it = page.extra.find("cover");
    if (it == page.extra.end() || !it->is_object()) return nullptr;
    const auto kind = it->find("kind");
    if (kind == it->end() || !kind->is_string()) return nullptr;
    return is_cover_kind(kind->get_ref<const std::string&>()) ? &*it : nullptr;
}

PageSpec spec_for(const PageSpec& book, const Json& cover) {
    const auto kind = cover.find("kind");
    const bool wrap = kind != cover.end() && kind->is_string() && is_wrap_kind(kind->get_ref<const std::string&>());
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

std::vector<Fold> folds(const Page& page, std::string_view binding) {
    const Json* cover = cover_of(page);
    if (cover == nullptr || !is_wrap_kind(cover->at("kind").get_ref<const std::string&>())) return {};
    const Rect t = page.trim_rect_mm();
    // float(cover.get("spine_mm") or 0), float(cover.get("flap_mm") or 0)
    const double spine = to_float(py_or(get_or(*cover, "spine_mm", Json()), Json(0)));
    const double flap = to_float(py_or(get_or(*cover, "flap_mm", Json()), Json(0)));
    const double face = (t.width.value() - spine - 2 * flap) / 2;
    static constexpr std::array<const char*, 5> kRight{"袖", "表紙", "背", "裏表紙", "袖"};
    static constexpr std::array<const char*, 5> kLeft{"袖", "裏表紙", "背", "表紙", "袖"};
    const auto& order = binding == "right" ? kRight : kLeft;
    const std::array<double, 5> widths{flap, face, spine, face, flap};
    std::vector<Fold> out;
    double x = t.x.value();
    for (std::size_t i = 0; i < order.size(); ++i) {
        const double w = widths[i];
        if (w > 0) out.push_back(Fold{py_round(x, 3), py_round(x + w, 3), order[i]});
        x += w;
    }
    return out;
}

std::vector<const Page*> pages_in_order(const Document& doc) {
    const auto covers = covers_by_kind(doc);
    std::vector<const Page*> out;
    if (const auto it = covers.find("jacket"); it != covers.end()) {
        out.push_back(it->second);
    } else if (const auto front = covers.find("front"); front != covers.end()) {
        out.push_back(front->second);
    }
    for (const auto& page : doc.pages) {
        if (!is_cover(*page)) out.push_back(page.get());
    }
    if (const auto it = covers.find("back"); it != covers.end()) out.push_back(it->second);  // (with a jacket too)
    return out;
}

std::vector<std::pair<const Page*, std::string>> reading_order(const Document& doc) {
    const auto kinds = covers_by_kind(doc);
    std::vector<std::pair<const Page*, std::string>> out;
    const auto jacket = kinds.find("jacket");
    if (jacket != kinds.end()) {
        out.emplace_back(jacket->second, "front");
    } else if (const auto front = kinds.find("front"); front != kinds.end()) {
        out.emplace_back(front->second, "front");
    }
    for (const auto& page : doc.pages) {
        if (!is_cover(*page)) out.emplace_back(page.get(), "page");
    }
    if (const auto back = kinds.find("back"); back != kinds.end()) {
        out.emplace_back(back->second, "back");
    } else if (jacket != kinds.end()) {
        out.emplace_back(jacket->second, "back");
    }
    return out;
}

std::string file_stem(const Page& page) {
    if (const Json* cover = cover_of(page)) return "cover_" + cover->at("kind").get<std::string>();
    if (!page.index.is_int()) throw PyValueError("Unknown format code 'd' for object of type 'float'");
    // f"p{page.index:03d}": three digits at least, the sign counted among them
    const std::int64_t index = page.index.int_value();
    std::string digits = std::to_string(index < 0 ? -static_cast<std::uint64_t>(index) : static_cast<std::uint64_t>(index));
    const std::size_t width = index < 0 ? 2 : 3;
    if (digits.size() < width) digits.insert(0, width - digits.size(), '0');
    return std::string("p") + (index < 0 ? "-" : "") + digits;
}

}  // namespace genko::core
