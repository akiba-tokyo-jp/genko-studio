// annotations@1 (PC5 J-0): a page's notes, their JSON and checks (core/annotations.hpp). The shape after PhotoCraft
// v0.2.0's doc/src/analysis.rs Note (MIT, native/third_party/photocraft/NOTICE.md); Genko keeps them on the page, in mm,
// with who wrote them.

#include "core/annotations.hpp"

#include <algorithm>
#include <cmath>
#include <set>

#include "core/actor.hpp"
#include "core/error.hpp"
#include "core/model.hpp"
#include "core/production_json.hpp"
#include "core/smart.hpp"

namespace genko::core {

namespace pj = production;

namespace {

bool hex(char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); }
bool digit(char c) { return c >= '0' && c <= '9'; }

// YYYY-MM-DDThh:mm:ssZ
bool utc_time(std::string_view s) {
    if (s.size() != 20) return false;
    for (std::size_t i = 0; i < s.size(); ++i) {
        const char c = s[i];
        const bool ok = i == 4 || i == 7 ? c == '-' : i == 10 ? c == 'T' : i == 13 || i == 16 ? c == ':' : i == 19 ? c == 'Z' : digit(c);
        if (!ok) return false;
    }
    return true;
}

bool actor_text(std::string_view s) {
    return s == "genko" || ((s.starts_with("human:") || s.starts_with("ai:")) && s.size() <= 256 && s.find(':') + 1 < s.size());
}

}  // namespace

bool annotation_time(std::string_view text) { return utc_time(text); }

bool annotation_by_person(std::string_view actor) { return core::can_approve(actor); }

bool annotation_id(std::string_view id) { return id.size() == 15 && id.starts_with("an_") && std::all_of(id.begin() + 3, id.end(), hex); }

Json to_json(const std::vector<Annotation>& annotations) {
    Json out = Json::array();
    for (const Annotation& a : annotations) {
        Json item = Json::object();
        item["id"] = a.id;
        item["at_mm"] = pj::to_json(a.at_mm);
        item["popup_mm"] = a.popup_mm ? pj::to_json(*a.popup_mm) : Json(nullptr);
        item["text"] = a.text;
        item["author"] = a.author;
        item["color"] = pj::to_json(a.color);
        item["open"] = a.open;
        item["modified"] = a.modified;
        item["actor"] = a.actor;
        out.push_back(std::move(item));
    }
    return out;
}

std::vector<Annotation> annotations_from_json(const Json& value) {
    const std::string where = "annotations";
    if (!value.is_array()) pj::wrong(where, "must be a list");
    std::vector<Annotation> out;
    for (std::size_t i = 0; i < value.size(); ++i) {
        const std::string at = where + "[" + std::to_string(i) + "]";
        const Json& item = value[i];
        pj::only_keys(item, at, {"id", "at_mm", "popup_mm", "text", "author", "color", "open", "modified", "actor"});
        Annotation a;
        a.id = pj::text(pj::field(item, "id", at), at + ".id", 15);
        a.at_mm = pj::point_mm(pj::field(item, "at_mm", at), at + ".at_mm");
        if (const Json& popup = pj::field(item, "popup_mm", at); !popup.is_null()) a.popup_mm = pj::rect_mm(popup, at + ".popup_mm");
        a.text = pj::text(pj::field(item, "text", at), at + ".text", kAnnotationTextMax);
        a.author = pj::text(pj::field(item, "author", at), at + ".author", 1024);
        a.color = pj::rgb(pj::field(item, "color", at), at + ".color");
        a.open = pj::boolean(pj::field(item, "open", at), at + ".open");
        a.modified = pj::text(pj::field(item, "modified", at), at + ".modified", 20);
        a.actor = pj::text(pj::field(item, "actor", at), at + ".actor", 256);
        out.push_back(std::move(a));
    }
    check_annotations(out);
    return out;
}

void check_annotations(const std::vector<Annotation>& annotations) {
    const std::string where = "annotations";
    if (annotations.size() > kAnnotationsMax) pj::wrong(where, "holds more than " + std::to_string(kAnnotationsMax) + " notes");
    std::set<std::string> ids;
    for (const Annotation& a : annotations) {
        if (!annotation_id(a.id)) pj::wrong(where, "has a note whose id is not an_ and 12 hex digits");
        if (!ids.insert(a.id).second) pj::wrong(where, "has two notes of the id " + a.id);
        if (a.text.size() > kAnnotationTextMax) pj::wrong(where, "has a note longer than " + std::to_string(kAnnotationTextMax) + " bytes");
        // (PC5 J-c, the review's P1-2: what the reader takes — a place on a page, an author of at most 1024 bytes —, so that
        // a book written is one read)
        const auto place = [](double v) { return std::isfinite(v) && std::fabs(v) <= pj::kCoordinateMaxMm; };
        if (!place(a.at_mm[0]) || !place(a.at_mm[1])) pj::wrong(where, "has a note placed past a page's places");
        if (a.popup_mm && (!std::all_of(a.popup_mm->begin(), a.popup_mm->end(), place) || (*a.popup_mm)[2] < 0 || (*a.popup_mm)[3] < 0))
            pj::wrong(where, "has a note whose box is not one");
        if (a.author.size() > 1024) pj::wrong(where, "has a note whose author is longer than 1024 bytes");
        if (!a.modified.empty() && !utc_time(a.modified)) pj::wrong(where, "has a note whose modified is not YYYY-MM-DDThh:mm:ssZ");
        if (!actor_text(a.actor)) pj::wrong(where, "has a note whose actor is not human:…, ai:… or genko");
    }
}

bool uses_annotations(const Document& doc) {
    for (const smart::WrittenPage& written : smart::written_pages(doc)) {
        if (!written.page->annotations.empty()) return true;
    }
    return false;
}

void validate_annotations(const Document& doc) {
    const auto check = [](const Page& page) { check_annotations(page.annotations); };
    for (const PagePtr& page : doc.pages)
        if (page) check(*page);
    for (const smart::WrittenPage& written : smart::written_pages(doc))
        if (!written.contents.empty()) check(*written.page);
}

}  // namespace genko::core
