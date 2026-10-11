#include "studio/state.hpp"

#include <algorithm>
#include <utility>

#include "core/error.hpp"
#include "core/pyconv.hpp"
#include "core/pyops.hpp"

namespace genko::studio {

using core::Json;
using core::Num;

namespace {

const core::Page* page_at(const core::Document& doc, const Num& index) {
    for (const auto& page : doc.pages) {
        if (page->index == index) return page.get();
    }
    return nullptr;
}

// `"name" in plan` for a plan Python keeps as it was read (a dict's key; a list's item; a str's part)
bool has_name(const Json& plan) {
    if (plan.is_object()) return plan.contains("name");
    if (plan.is_array()) return std::any_of(plan.begin(), plan.end(), [](const Json& item) { return item == Json("name"); });
    if (plan.is_string()) return plan.get_ref<const std::string&>().find("name") != std::string::npos;
    throw core::PyTypeError("argument of type '" + core::py_type_name(plan) + "' is not iterable");
}

std::string join(const std::vector<std::string>& parts, const std::string& separator) {
    std::string out;
    for (std::size_t i = 0; i < parts.size(); ++i) out += (i > 0 ? separator : "") + parts[i];
    return out;
}

}  // namespace

std::optional<Json> bible(const core::Document& doc) {
    const Json* bible_doc = core::get(doc.studio, "bible_doc");
    if (bible_doc == nullptr || bible_doc->is_null()) return std::nullopt;
    if (!bible_doc->is_object()) throw core::PyTypeError("'" + core::py_type_name(*bible_doc) + "' object is not a mapping");
    Json out = *bible_doc;
    out["plot"] = doc.bible.plot;
    out["characters"] = doc.bible.characters;
    out["constraints"] = doc.bible.constraints;
    return out;
}

std::optional<Json> script(const core::Document& doc) {
    const Json* script = core::get(doc.studio, "script");
    if (script == nullptr || script->is_null()) return std::nullopt;
    return *script;
}

std::optional<Json> name(const core::Document& doc, const Num& index) {
    const core::Page* page = page_at(doc, index);
    if (page == nullptr || !page->plan || !core::py_truthy(*page->plan) || !has_name(*page->plan)) return std::nullopt;
    return *page->plan;
}

std::vector<std::pair<Num, Json>> names(const core::Document& doc) {
    std::vector<std::pair<Num, Json>> out;
    for (const auto& page : doc.pages) {
        if (!page->plan || !core::py_truthy(*page->plan) || !has_name(*page->plan)) continue;
        auto it = std::find_if(out.begin(), out.end(), [&page](const auto& item) { return item.first == page->index; });
        if (it != out.end()) {
            it->second = *page->plan;
        } else {
            out.emplace_back(page->index, *page->plan);
        }
    }
    return out;
}

std::optional<Json> name_review(const core::Document& doc, const Num& index) {
    const auto record = name(doc, index);
    if (!record) return std::nullopt;
    const Json reviews = core::py_get(*record, "reviews", Json::object());
    const Json review = core::py_get(reviews, "name", Json());
    if (review.is_null()) return std::nullopt;
    return review;
}

std::vector<Json> open_tickets(const core::Document& doc, const std::optional<std::string>& kind) {
    std::vector<Json> out;
    for (const Json& ticket : core::iterate(doc.tickets)) {
        if (!(core::py_get(ticket, "status") == Json("open"))) continue;
        if (kind && !(core::py_get(ticket, "kind") == Json(*kind))) continue;
        out.push_back(ticket);
    }
    return out;
}

std::vector<Json> page_fixes(const core::Document& doc, const Num& index) {
    const core::Page* page = page_at(doc, index);
    const Json page_id = page != nullptr ? Json(page->id) : Json();
    std::vector<Json> out;
    for (const Json& ticket : open_tickets(doc, "fix")) {
        const bool here = core::py_equals(core::py_get(ticket, "page_id"), page_id) || core::py_equals(core::py_get(ticket, "page_index"), index.json());
        if (here && !core::py_truthy(core::py_get(ticket, "frame_id")) && core::py_get(ticket, "assignee") == Json("agent")) out.push_back(ticket);
    }
    return out;
}

std::string written_look(const Json& character) {
    const Json look = core::py_or(core::py_get(character, "look"), Json::object());
    std::vector<std::string> parts;
    for (const auto& [key, label] : {std::pair{"hair", "髪"}, std::pair{"eyes", "目"}, std::pair{"build", "体格"}, std::pair{"silhouette", "シルエット"}}) {
        const Json value = core::py_get(look, key);
        if (core::py_truthy(value)) parts.push_back(std::string(label) + ": " + core::py_str(value));
    }
    if (const Json height = core::py_get(look, "height_cm"); core::py_truthy(height)) parts.push_back("身長: " + core::py_str(height) + " cm");
    std::vector<std::string> outfits;
    for (const Json& outfit : core::iterate(core::py_or(core::py_get(look, "outfits"), Json::array()))) {
        if (!outfit.is_object()) continue;
        const Json desc = core::py_get(outfit, "desc");
        if (core::py_truthy(desc)) outfits.push_back(core::py_str(desc));
    }
    if (!outfits.empty()) parts.push_back("服: " + join(outfits, " ／ "));
    if (const Json marks = core::py_get(look, "marks"); core::py_truthy(marks)) {
        std::vector<std::string> items;
        for (const Json& mark : core::iterate(marks)) items.push_back(core::py_str(mark));
        parts.push_back("特徴: " + join(items, "、"));
    }
    return join(parts, "。");
}

}  // namespace genko::studio
