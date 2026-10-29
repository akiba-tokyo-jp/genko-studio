// PC5 J-c (render/pcraft_annotations.hpp): a .pcraft's notes as the page's, and back — PhotoCraft v0.2.0's
// doc/src/analysis.rs Note and format/src/manifest.rs (MIT, native/third_party/photocraft/NOTICE.md).

#include "render/pcraft_annotations.hpp"

#include <algorithm>
#include <cmath>
#include <set>
#include <stdexcept>

#include "core/error.hpp"
#include "core/ids.hpp"
#include "core/production_json.hpp"
#include "core/pynum.hpp"
#include "render/image_ops.hpp"
#include "render/ops_annotations.hpp"
#include "render/pcraft_manifest.hpp"
#include "render/psd_annotations.hpp"

namespace genko::render::pcraft_annotations {

using core::Json;

namespace {

[[noreturn]] void bad(const std::string& what) { throw core::Error("value", "notes: " + what); }

double number(const Json& v, const char* what) {
    if (!v.is_number() || !std::isfinite(v.get<double>())) bad(std::string(what) + " is not a number");
    return v.get<double>();
}

int unit(double v) { return static_cast<int>(std::lround(std::clamp(std::isfinite(v) ? v : 0.0, 0.0, 1.0) * 255.0)); }

// A document colour as RGB 0..255 (PhotoCraft's Color::to_rgb).
std::array<int, 3> rgb_of(const pcraft::Color& c) {
    switch (c.mode) {
    case pcraft::Mode::Grayscale:
    case pcraft::Mode::Bitmap:
    case pcraft::Mode::Duotone:
        return {unit(c.c[0]), unit(c.c[0]), unit(c.c[0])};
    case pcraft::Mode::Cmyk: {
        const double k = 1 - std::clamp(c.c[3], 0.0, 1.0);
        return {unit((1 - c.c[0]) * k), unit((1 - c.c[1]) * k), unit((1 - c.c[2]) * k)};
    }
    case pcraft::Mode::Lab: {
        const auto rgb = image_ops::lab_to_srgb({static_cast<float>(c.c[0] * 100.0), static_cast<float>(c.c[1] * 255.0 - 128.0),
                                                 static_cast<float>(c.c[2] * 255.0 - 128.0)});
        return {unit(rgb[0]), unit(rgb[1]), unit(rgb[2])};
    }
    default:
        return {unit(c.c[0]), unit(c.c[1]), unit(c.c[2])};
    }
}

std::string text(const Json& note, const char* key) {
    const Json* v = pcraft::optional_field(note, key);
    if (v == nullptr) return {};
    if (!v->is_string()) bad(std::string(key) + " is not text");
    return v->get<std::string>();
}

}  // namespace

std::vector<core::Annotation> notes_of(const Json& value, double mm) {
    if (!value.is_array()) bad("not a list");
    std::set<std::string> ids;
    std::vector<core::Annotation> out;
    for (const Json& n : value) {
        if (!n.is_object()) bad("a note is not an object");
        core::Annotation a;
        do {
            a.id = "an_" + core::new_id();
        } while (!ids.insert(a.id).second);
        a.author = annotations::cut_utf8(text(n, "author"), 1024);
        a.text = text(n, "text");
        if (const Json* c = pcraft::optional_field(n, "color")) {
            try {
                a.color = rgb_of(pcraft::color_from(*c));
            } catch (const pcraft::Unreadable&) {
                bad("a colour is not one");
            }
        } else {
            a.color = {255, 255, 130};  // (PhotoCraft's default: rgb(1, 1, 0.51))
        }
        if (const Json* p = pcraft::optional_field(n, "position")) {
            if (!p->is_array() || p->size() != 2) bad("position is not [x, y]");
            a.at_mm = {core::py_round(number((*p)[0], "position") * mm, 4), core::py_round(number((*p)[1], "position") * mm, 4)};
        }
        if (const Json* p = pcraft::optional_field(n, "popup")) {
            if (!p->is_array() || p->size() != 4) bad("popup is not [left, top, right, bottom]");
            const double l = number((*p)[0], "popup"), t = number((*p)[1], "popup"), r = number((*p)[2], "popup"), b = number((*p)[3], "popup");
            if (l != 0 || t != 0 || r != 0 || b != 0) {
                a.popup_mm = std::array<double, 4>{core::py_round(std::min(l, r) * mm, 4), core::py_round(std::min(t, b) * mm, 4),
                                                   core::py_round(std::fabs(r - l) * mm, 4), core::py_round(std::fabs(b - t) * mm, 4)};
            }
        }
        if (const Json* o = pcraft::optional_field(n, "open")) {
            if (!o->is_boolean()) bad("open is not true or false");
            a.open = o->get<bool>();
        }
        a.modified = psd_annotations::utc_of_pdf_date(text(n, "modified"));
        a.actor = annotations::ReadingActor::now();  // (the reader: the op's actor, else a person's — the file's)
        // (the review's P1-2: places the book's reader takes; the page's paper pulls it in further — read())
        annotations::pull_inside(a, core::production::kCoordinateMaxMm, core::production::kCoordinateMaxMm);
        out.push_back(std::move(a));
    }
    core::check_annotations(out);
    return out;
}

void read(const Json& value, pcraft_production::ReadContext& context) {
    if (context.page == nullptr) return;
    std::vector<core::Annotation> notes;
    try {
        notes = notes_of(value, context.mm);
    } catch (const core::Error&) {
        if (context.say) context.say("注釈は読めない形のため外しました");
        return;
    }
    if (notes.empty()) return;
    for (core::Annotation& a : notes) annotations::pull_inside(a, context.page->spec.width_mm.value(), context.page->spec.height_mm.value());
    context.page->annotations = std::move(notes);
}

std::optional<Json> write(const core::Page& page, pcraft_production::WriteContext& context) {
    if (page.annotations.empty()) return std::nullopt;
    const auto px = [&](double mm) { return core::py_round(mm * context.px, 3); };
    Json list = Json::array();
    for (const core::Annotation& a : page.annotations) {
        Json note = Json::object();
        note["author"] = a.author;
        note["color"] = pcraft::to_json(pcraft::rgb_color(a.color[0] / 255.0, a.color[1] / 255.0, a.color[2] / 255.0));
        note["text"] = a.text;
        note["position"] = Json::array({px(a.at_mm[0]), px(a.at_mm[1])});
        if (a.popup_mm) {
            const auto& b = *a.popup_mm;
            note["popup"] = Json::array({px(b[0]), px(b[1]), px(b[0] + b[2]), px(b[1] + b[3])});
        } else {
            note["popup"] = Json::array({0.0, 0.0, 0.0, 0.0});
        }
        note["open"] = a.open;
        note["modified"] = psd_annotations::pdf_date_of(a.modified);
        list.push_back(std::move(note));
    }
    return list;
}

}  // namespace genko::render::pcraft_annotations
