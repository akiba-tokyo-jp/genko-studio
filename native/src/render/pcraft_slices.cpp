// PC5 J-b (render/pcraft_slices.hpp): a .pcraft's slices as the page's, and back — PhotoCraft v0.2.0's doc/src/slices.rs
// (MIT, native/third_party/photocraft/NOTICE.md).

#include "render/pcraft_slices.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <set>
#include <string>

#include "core/error.hpp"
#include "core/production_json.hpp"
#include "core/pynum.hpp"
#include "core/slices.hpp"
#include "render/ops_slices.hpp"
#include "render/pcraft_manifest.hpp"

namespace genko::render::pcraft_slices {

using core::Json;

namespace {

struct Bad {};

const Json* field(const Json& object, const char* key) {
    if (!object.is_object()) throw Bad{};
    return pcraft::optional_field(object, key);
}

double number(const Json& v) {
    if (!v.is_number() || v.is_boolean() || !std::isfinite(v.get<double>())) throw Bad{};
    return v.get<double>();
}

std::int64_t whole(const Json& v) {
    const double n = number(v);
    if (n != std::floor(n)) throw Bad{};
    return static_cast<std::int64_t>(std::clamp(n, -1e15, 1e15));
}

std::string text(const Json& object, const char* key) {
    const Json* v = field(object, key);
    if (v == nullptr) return {};
    if (!v->is_string()) throw Bad{};
    return v->get<std::string>();
}

}  // namespace

void read(const Json& value, pcraft_production::ReadContext& context) {
    // (none, or an empty list — serde reads the struct from a sequence too: its defaults —: no slices, nothing said)
    if (context.page == nullptr || value.is_null() || (value.is_array() && value.empty())) return;
    core::Slices out;
    try {
        out.group = text(value, "groupName");
        if (out.group.size() > 1024) out.group.clear();  // (PC5 J-b review P2-1: too long a name none, as the PSD's)
        const Json* list = field(value, "list");
        if (list != nullptr) {
            if (!list->is_array() || list->size() > core::kSlicesMax) throw Bad{};
            std::set<std::int64_t> ids;
            for (const Json& item : *list) {
                if (!item.is_object()) throw Bad{};
                const std::string origin = text(item, "origin");
                if (origin == "auto") continue;  // (never kept: made again)
                core::Slice s;
                const Json* id = field(item, "id");
                s.id = id != nullptr ? whole(*id) : 0;
                if (const Json* layer = field(item, "layer"); layer != nullptr && origin == "layer") {
                    const auto found = context.layers.find(std::to_string(whole(*layer)));
                    if (found != context.layers.end()) {
                        s.origin = "layer";
                        s.layer = found->second;
                    }
                }
                s.name = text(item, "name");
                if (s.name.size() > 1024) s.name.clear();  // (review P2-1: as the PSD's reader — the made name then)
                const std::string kind = text(item, "kind");
                s.kind = kind == "noImage" || kind == "table" ? kind : std::string("image");
                s.url = text(item, "url");
                s.target = text(item, "target");
                s.message = text(item, "message");
                s.alt = text(item, "alt");
                s.cell_text = text(item, "cellText");
                // (review P2-1: a text past the reader's bound — the slices unreadable: left out and said, as a PSD's are kept out)
                for (const std::string* t : {&s.url, &s.target, &s.message, &s.alt, &s.cell_text})
                    if (t->size() > core::kSliceTextMax) throw Bad{};
                if (const Json* v = field(item, "cellTextIsHtml")) {
                    if (!v->is_boolean()) throw Bad{};
                    s.cell_text_is_html = v->get<bool>();
                }
                if (const Json* v = field(item, "horizontalAlign")) s.align[0] = static_cast<int>(std::clamp<std::int64_t>(whole(*v), 0, 4));
                if (const Json* v = field(item, "verticalAlign")) s.align[1] = static_cast<int>(std::clamp<std::int64_t>(whole(*v), 0, 4));
                if (const Json* v = field(item, "background")) {
                    if (!v->is_array() || v->size() != 4) throw Bad{};
                    std::array<int, 4> c{};
                    for (std::size_t k = 0; k < 4; ++k) c[k] = static_cast<int>(std::clamp<std::int64_t>(whole((*v)[k]), 0, 255));
                    char hex[8];
                    std::snprintf(hex, sizeof hex, "#%02x%02x%02x", c[1], c[2], c[3]);
                    s.background = std::string(hex);
                }
                if (const Json* v = field(item, "outsets"); v != nullptr && s.origin == "layer") {
                    if (!v->is_array() || v->size() != 4) throw Bad{};
                    for (std::size_t k = 0; k < 4; ++k)
                        s.outsets[k] = static_cast<int>(std::clamp<std::int64_t>(whole((*v)[k]), -core::kSliceOutsetMax, core::kSliceOutsetMax));
                }
                const Json* rect = field(item, "rect");
                if (rect == nullptr || !rect->is_object()) throw Bad{};
                const double x0 = number(rect->value("x0", Json(0))), y0 = number(rect->value("y0", Json(0)));
                const double x1 = number(rect->value("x1", Json(0))), y1 = number(rect->value("y1", Json(0)));
                const double mm = context.mm;
                s.rect_mm = {core::py_round(std::min(x0, x1) * mm, 4), core::py_round(std::min(y0, y1) * mm, 4), core::py_round(std::fabs(x1 - x0) * mm, 4),
                             core::py_round(std::fabs(y1 - y0) * mm, 4)};
                out.list.push_back(std::move(s));
            }
            // (an id of 0, out of range or twice: one after the largest)
            std::vector<core::Slice*> fresh;
            for (core::Slice& s : out.list) {
                if (s.id < 1 || s.id >= core::kSliceIdMax || !ids.insert(s.id).second) fresh.push_back(&s);
            }
            std::int64_t next = ids.empty() ? 1 : *ids.rbegin() + 1;
            for (core::Slice* s : fresh) s->id = next++;
            if (next > core::kSliceIdMax) throw Bad{};  // (review P2-1: ids past the reader's: unreadable, said)
            out.next_id = next;
        }
        if (out.list.empty()) return;  // (a group's name alone: nothing to keep)
        core::check_slices(out);
    } catch (const Bad&) {
        if (context.say) context.say("スライスは読めない形のため外しました");
        return;
    } catch (const core::Error&) {
        if (context.say) context.say("スライスは読めない形のため外しました");
        return;
    } catch (const pcraft::Unreadable&) {
        if (context.say) context.say("スライスは読めない形のため外しました");
        return;
    }
    context.page->slices = std::move(out);
}

std::optional<Json> write(const core::Page& page, pcraft_production::WriteContext& context) {
    if (!page.slices || page.slices->list.empty()) return std::nullopt;
    Json list = Json::array();
    const auto px = [&](double mm) { return static_cast<std::int64_t>(std::llround(mm * context.px)); };
    for (const core::Slice& s : page.slices->list) {
        std::array<double, 4> r = s.rect_mm;
        Json layer = nullptr;
        std::string origin = "user";
        if (s.origin == "layer" && s.layer) {
            if (context.book != nullptr) {
                if (const auto now = slices::layer_rect_mm(*context.book, page, s)) r = *now;
            }
            if (const auto id = context.layers.find(*s.layer); id != context.layers.end()) {
                origin = "layer";
                layer = std::stoull(id->second);
            }
        }
        Json background = nullptr;
        if (s.background && s.background->size() == 7) {
            const auto hex = [&](std::size_t at) { return std::stoi(s.background->substr(at, 2), nullptr, 16); };
            background = Json::array({255, hex(1), hex(3), hex(5)});
        }
        list.push_back(Json{{"id", s.id},
                            {"groupId", 0},
                            {"origin", origin},
                            {"layer", layer},
                            {"name", s.name},
                            {"rect", Json{{"x0", px(r[0])}, {"y0", px(r[1])}, {"x1", px(r[0] + r[2])}, {"y1", px(r[1] + r[3])}}},
                            {"kind", s.kind},
                            {"url", s.url},
                            {"target", s.target},
                            {"message", s.message},
                            {"alt", s.alt},
                            {"cellTextIsHtml", s.cell_text_is_html},
                            {"cellText", s.cell_text},
                            {"horizontalAlign", s.align[0]},
                            {"verticalAlign", s.align[1]},
                            {"background", background},
                            {"outsets", origin == "layer" ? Json::array({s.outsets[0], s.outsets[1], s.outsets[2], s.outsets[3]}) : Json::array({0, 0, 0, 0})}});
    }
    return Json{{"groupName", page.slices->group}, {"list", std::move(list)}};
}

}  // namespace genko::render::pcraft_slices
