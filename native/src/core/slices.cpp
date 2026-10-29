// slices@1 (PC5 J-0): a page's slices, their JSON and checks (core/slices.hpp). The shapes after PhotoCraft v0.2.0's
// doc/src/slices.rs (MIT, native/third_party/photocraft/NOTICE.md); Genko keeps them on the page, in mm.

#include "core/slices.hpp"

#include <cmath>
#include <set>

#include "core/error.hpp"
#include "core/model.hpp"
#include "core/production_json.hpp"
#include "core/smart.hpp"

namespace genko::core {

namespace pj = production;

namespace {

bool colour_text(std::string_view s) {
    if (s.size() != 7 || s[0] != '#') return false;
    for (std::size_t i = 1; i < 7; ++i) {
        const char c = s[i];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
    }
    return true;
}

}  // namespace

Json to_json(const Slices& slices) {
    Json list = Json::array();
    for (const Slice& s : slices.list) {
        Json item = Json::object();
        item["id"] = s.id;
        item["origin"] = s.origin;
        if (s.layer) item["layer"] = *s.layer;
        item["name"] = s.name;
        item["rect_mm"] = pj::to_json(s.rect_mm);
        item["kind"] = s.kind;
        item["url"] = s.url;
        item["target"] = s.target;
        item["message"] = s.message;
        item["alt"] = s.alt;
        item["cell_text"] = s.cell_text;
        item["cell_text_is_html"] = s.cell_text_is_html;
        item["align"] = Json::array({s.align[0], s.align[1]});
        item["background"] = s.background ? Json(*s.background) : Json(nullptr);
        item["outsets"] = Json::array({s.outsets[0], s.outsets[1], s.outsets[2], s.outsets[3]});
        list.push_back(std::move(item));
    }
    Json out = Json::object();
    out["group"] = slices.group;
    out["next_id"] = slices.next_id;
    out["list"] = std::move(list);
    return out;
}

Slices slices_from_json(const Json& value) {
    const std::string where = "slices";
    pj::only_keys(value, where, {"group", "next_id", "list"});
    Slices out;
    out.group = pj::text(pj::field(value, "group", where), where + ".group", 1024);
    out.next_id = pj::integer(pj::field(value, "next_id", where), where + ".next_id", 1, kSliceIdMax);
    const Json& list = pj::field(value, "list", where);
    if (!list.is_array()) pj::wrong(where + ".list", "must be a list");
    for (std::size_t i = 0; i < list.size(); ++i) {
        const std::string at = where + ".list[" + std::to_string(i) + "]";
        const Json& item = list[i];
        pj::only_keys(item, at, {"id", "origin", "layer", "name", "rect_mm", "kind", "url", "target", "message", "alt", "cell_text",
                                 "cell_text_is_html", "align", "background", "outsets"});
        Slice s;
        s.id = pj::integer(pj::field(item, "id", at), at + ".id", 1, kSliceIdMax);
        s.origin = pj::text(pj::field(item, "origin", at), at + ".origin", 16);
        if (const Json* layer = pj::optional(item, "layer")) s.layer = pj::text(*layer, at + ".layer", 256);
        s.name = pj::text(pj::field(item, "name", at), at + ".name", 1024);
        s.rect_mm = pj::rect_mm(pj::field(item, "rect_mm", at), at + ".rect_mm");
        s.kind = pj::text(pj::field(item, "kind", at), at + ".kind", 16);
        s.url = pj::text(pj::field(item, "url", at), at + ".url", kSliceTextMax);
        s.target = pj::text(pj::field(item, "target", at), at + ".target", kSliceTextMax);
        s.message = pj::text(pj::field(item, "message", at), at + ".message", kSliceTextMax);
        s.alt = pj::text(pj::field(item, "alt", at), at + ".alt", kSliceTextMax);
        s.cell_text = pj::text(pj::field(item, "cell_text", at), at + ".cell_text", kSliceTextMax);
        s.cell_text_is_html = pj::boolean(pj::field(item, "cell_text_is_html", at), at + ".cell_text_is_html");
        const Json& align = pj::field(item, "align", at);
        if (!align.is_array() || align.size() != 2) pj::wrong(at + ".align", "must be [horizontal, vertical]");
        for (std::size_t k = 0; k < 2; ++k) s.align[k] = static_cast<int>(pj::integer(align[k], at + ".align", 0, 4));
        if (const Json& bg = pj::field(item, "background", at); !bg.is_null()) s.background = pj::text(bg, at + ".background", 7);
        const Json& outsets = pj::field(item, "outsets", at);
        if (!outsets.is_array() || outsets.size() != 4) pj::wrong(at + ".outsets", "must be [top, left, bottom, right]");
        for (std::size_t k = 0; k < 4; ++k) s.outsets[k] = static_cast<int>(pj::integer(outsets[k], at + ".outsets", -kSliceOutsetMax, kSliceOutsetMax));
        out.list.push_back(std::move(s));
    }
    check_slices(out);
    return out;
}

void check_slices(const Slices& slices) {
    const std::string where = "slices";
    if (slices.list.size() > kSlicesMax) pj::wrong(where, "holds more than " + std::to_string(kSlicesMax) + " slices");
    std::set<std::int64_t> ids;
    for (const Slice& s : slices.list) {
        if (!ids.insert(s.id).second) pj::wrong(where, "has two slices of the id " + std::to_string(s.id));
        if (s.id >= slices.next_id) pj::wrong(where + ".next_id", "must be past every slice's id");
        if (s.origin != "user" && s.origin != "layer") pj::wrong(where, "has a slice of the origin " + s.origin + " (user, layer)");
        if ((s.origin == "layer") != s.layer.has_value()) pj::wrong(where, "has a slice whose layer does not go with its origin (a layer-based one names its layer)");
        if (s.kind != "image" && s.kind != "noImage" && s.kind != "table") pj::wrong(where, "has a slice of the kind " + s.kind + " (image, noImage, table)");
        if (s.background && !colour_text(*s.background)) pj::wrong(where, "has a slice whose background is not #rrggbb");
        // (PC5 J-b: the reader's bounds — a place within 1000 m of the page, a size not negative — for what the ops make)
        for (std::size_t k = 0; k < 4; ++k) {
            const double v = s.rect_mm[k];
            if (!std::isfinite(v) || std::fabs(v) > pj::kCoordinateMaxMm || (k >= 2 && v < 0)) pj::wrong(where, "has a slice whose rect_mm is out of range");
        }
    }
}

bool uses_slices(const Document& doc) {
    for (const smart::WrittenPage& written : smart::written_pages(doc)) {
        if (written.page->slices && !written.page->slices->empty()) return true;
    }
    return false;
}

void validate_slices(const Document& doc) {
    const auto check = [](const Page& page) {
        if (page.slices) check_slices(*page.slices);
    };
    for (const PagePtr& page : doc.pages)
        if (page) check(*page);
    for (const smart::WrittenPage& written : smart::written_pages(doc))
        if (!written.contents.empty()) check(*written.page);
}

}  // namespace genko::core
