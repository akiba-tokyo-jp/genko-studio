// PC5 J-c (annotations@1, render/ops_annotations.hpp): the notes of a page — PhotoCraft v0.2.0's crates/engine/src/
// notes_cmds.rs (MIT, native/third_party/photocraft/NOTICE.md), in mm, with who wrote them.

#include "render/ops_annotations.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <set>
#include <vector>

#include "core/clock.hpp"
#include "core/error.hpp"
#include "core/ids.hpp"
#include "core/production_json.hpp"
#include "core/pynum.hpp"
#include "core/pyops.hpp"

namespace genko::render::annotations {

using core::Annotation;
using core::Json;
using core::OpError;

namespace {

double r4(double v) { return core::py_round(v, 4); }

std::array<double, 2> point_given(const Json& value, const char* key) {
    const auto at = [&](std::size_t i) {
        const Json& v = value[i];
        if (!v.is_number() || !std::isfinite(v.get<double>()) || std::fabs(v.get<double>()) > core::production::kCoordinateMaxMm)
            throw OpError(std::string(key) + " is [x, y] (mm)");
        return r4(v.get<double>());
    };
    if (!value.is_array() || value.size() != 2) throw OpError(std::string(key) + " is [x, y] (mm)");
    return {at(0), at(1)};
}

std::optional<std::array<double, 4>> box_given(const Json& value) {
    if (value.is_null()) return std::nullopt;
    const auto bad = [] { throw OpError("popup_mm is [x, y, w, h] (mm) or null"); };
    if (!value.is_array() || value.size() != 4) bad();
    std::array<double, 4> out{};
    for (std::size_t i = 0; i < 4; ++i) {
        const Json& v = value[i];
        if (!v.is_number() || !std::isfinite(v.get<double>()) || std::fabs(v.get<double>()) > core::production::kCoordinateMaxMm) bad();
        out[i] = r4(v.get<double>());
        if (i >= 2 && out[i] < 0) bad();
    }
    return out;
}

std::string text_given(const Json& value) {
    if (!value.is_string()) throw OpError("text is the note's text");
    std::string text = value.get<std::string>();
    if (text.size() > core::kAnnotationTextMax) throw OpError("a note's text is at most 65536 bytes");
    return text;
}

std::string author_given(const Json& value) {
    if (!value.is_string()) throw OpError("author is the note's author's name");
    std::string author = value.get<std::string>();
    if (author.size() > 1024) throw OpError("author is at most 1024 bytes");
    return author;
}

bool open_given(const Json& value) {
    if (!value.is_boolean()) throw OpError("open is true or false");
    return value.get<bool>();
}

const Json* given(const Json& op, std::string_view key) {
    const Json* v = core::get(op, key);
    return v != nullptr && !v->is_null() ? v : nullptr;
}

std::size_t index_of(const core::Page& page, const Json& id) {
    if (!id.is_string()) throw OpError("id is the note's id");
    for (std::size_t i = 0; i < page.annotations.size(); ++i)
        if (page.annotations[i].id == id.get<std::string>()) return i;
    throw OpError("no note " + id.get<std::string>() + " on the page");
}

// (an agent never changes a person's note)
void may_change(const core::OpContext& c, const Annotation& note, const char* what) {
    if (!c.actor.can_approve() && core::annotation_by_person(note.actor))
        throw OpError("a person wrote the note " + note.id + "; only a person can " + what + " it");
}

std::string fresh_id(const core::Page& page) {
    for (;;) {
        std::string id = "an_" + core::new_id();
        if (std::none_of(page.annotations.begin(), page.annotations.end(), [&](const Annotation& a) { return a.id == id; })) return id;
    }
}

void add_annotation(core::OpContext& c) {
    const std::size_t at = core::require_page(c.doc, c.op);
    const core::Page& now = c.doc.page(at);
    if (now.annotations.size() >= core::kAnnotationsMax) throw OpError("a page holds at most 1000 notes");
    const Json* where = core::get(c.op, "at_mm");
    if (where == nullptr) throw OpError("at_mm is [x, y] (mm)");
    Annotation note;
    note.id = fresh_id(now);
    note.at_mm = point_given(*where, "at_mm");
    if (const Json* v = given(c.op, "text")) note.text = text_given(*v);
    if (const Json* v = given(c.op, "author")) note.author = author_given(*v);
    if (const Json* v = given(c.op, "color")) note.color = color_given(*v);
    note.open = true;  // (a new note's box open, as Photoshop's and PhotoCraft's)
    if (const Json* v = given(c.op, "open")) note.open = open_given(*v);
    if (const Json* v = core::get(c.op, "popup_mm")) note.popup_mm = box_given(*v);
    note.modified = now();
    if (const Json* v = core::get(c.op, "modified")) {
        if (!v->is_string() || (!v->get<std::string>().empty() && !core::annotation_time(v->get<std::string>())))
            throw OpError("modified is YYYY-MM-DDThh:mm:ssZ or \"\"");
        note.modified = v->get<std::string>();
    }
    note.actor = writer_of(c.actor);
    const std::string id = note.id;
    c.doc.edit_page(at).annotations.push_back(std::move(note));
    c.report = Json{{"annotation", id}};
}

void set_annotation(core::OpContext& c) {
    const std::size_t at = core::require_page(c.doc, c.op);
    const Json* id = core::get(c.op, "id");
    if (id == nullptr) throw OpError("id is the note's id");
    const std::size_t i = index_of(c.doc.page(at), *id);
    const Annotation& was = c.doc.page(at).annotations[i];
    Annotation note = was;
    bool any = false;
    if (const Json* v = given(c.op, "text")) {
        note.text = text_given(*v);
        any = true;
    }
    if (const Json* v = given(c.op, "author")) {
        note.author = author_given(*v);
        any = true;
    }
    if (const Json* v = given(c.op, "color")) {
        note.color = color_given(*v);
        any = true;
    }
    if (const Json* v = given(c.op, "open")) {
        note.open = open_given(*v);
        any = true;
    }
    if (const Json* v = core::get(c.op, "popup_mm")) {
        note.popup_mm = box_given(*v);
        any = true;
    }
    if (const Json* v = given(c.op, "at_mm")) {
        note.at_mm = point_given(*v, "at_mm");
        any = true;
        if (note.popup_mm && !core::get(c.op, "popup_mm")) {  // (its box along with its mark)
            (*note.popup_mm)[0] = r4((*note.popup_mm)[0] + note.at_mm[0] - was.at_mm[0]);
            (*note.popup_mm)[1] = r4((*note.popup_mm)[1] + note.at_mm[1] - was.at_mm[1]);
        }
    }
    if (!any) throw OpError("give text, author, color, open, at_mm or popup_mm");
    may_change(c, was, "change");
    const bool changed = note != was;
    if (changed && (note.text != was.text || note.author != was.author || note.color != was.color)) {
        note.modified = now();  // (opening, closing and moving it do not: PhotoCraft's)
        note.actor = writer_of(c.actor);
    }
    const std::string out = note.id;
    if (changed) c.doc.edit_page(at).annotations[i] = std::move(note);
    c.report = Json{{"annotation", out}, {"changed", changed}};
}

void delete_annotations(core::OpContext& c) {
    const std::size_t at = core::require_page(c.doc, c.op);
    const core::Page& page = c.doc.page(at);
    const Json* ids = given(c.op, "ids");
    const Json* all = given(c.op, "all");
    std::set<std::string> gone;
    if (all != nullptr && ids == nullptr) {
        if (!all->is_boolean() || !all->get<bool>()) throw OpError("give ids (the notes' ids) or all: true");
        if (page.annotations.empty()) throw OpError("the page has no notes");
        for (const Annotation& a : page.annotations) gone.insert(a.id);
    } else if (ids != nullptr && all == nullptr) {
        if (!ids->is_array() || ids->empty()) throw OpError("give ids (the notes' ids) or all: true");
        for (const Json& id : *ids) gone.insert(page.annotations[index_of(page, id)].id);
    } else {
        throw OpError("give ids (the notes' ids) or all: true");
    }
    for (const Annotation& a : page.annotations)
        if (gone.contains(a.id)) may_change(c, a, "delete");
    std::vector<Annotation>& notes = c.doc.edit_page(at).annotations;
    std::erase_if(notes, [&](const Annotation& a) { return gone.contains(a.id); });
    c.report = Json{{"deleted", gone.size()}};
}

}  // namespace

std::string writer_of(const core::Actor& actor) {
    const std::string& name = actor.name();
    const auto named = [&](std::string_view prefix) { return name.starts_with(prefix) && name.size() > prefix.size() && name.size() <= 256; };
    if (named("human:") || named("ai:")) return name;
    if (actor.can_approve()) return "genko";
    return ("ai:" + (name.empty() ? std::string("unknown") : name)).substr(0, 256);
}

std::array<int, 3> color_given(const Json& value) {
    const auto bad = [] { throw OpError("color is [r, g, b] (0..255) or #rrggbb"); };
    std::array<int, 3> out{};
    if (value.is_string()) {
        const std::string s = value.get<std::string>();
        if (s.size() != 7 || s[0] != '#') bad();
        for (std::size_t k = 0; k < 3; ++k) {
            int v = 0;
            for (std::size_t j = 0; j < 2; ++j) {
                const char ch = static_cast<char>(std::tolower(static_cast<unsigned char>(s[1 + 2 * k + j])));
                const int d = ch >= '0' && ch <= '9' ? ch - '0' : ch >= 'a' && ch <= 'f' ? ch - 'a' + 10 : -1;
                if (d < 0) bad();
                v = v * 16 + d;
            }
            out[k] = v;
        }
        return out;
    }
    if (!value.is_array() || value.size() != 3) bad();
    for (std::size_t k = 0; k < 3; ++k) {
        const Json& v = value[k];
        if (!v.is_number_integer() && !(v.is_number() && std::isfinite(v.get<double>()) && v.get<double>() == std::floor(v.get<double>()))) bad();
        const double n = v.get<double>();
        if (n < 0 || n > 255) bad();
        out[k] = static_cast<int>(n);
    }
    return out;
}

std::string now() { return core::utc_time_text("%Y-%m-%dT%H:%M:%SZ"); }

Json listing(const core::Page& page) {
    Json list = core::to_json(page.annotations);
    for (std::size_t i = 0; i < list.size(); ++i) list[i]["by_person"] = core::annotation_by_person(page.annotations[i].actor);
    return Json{{"annotations", std::move(list)}};
}

std::array<double, 4> popup_of(const Annotation& note) {
    if (note.popup_mm) return *note.popup_mm;
    return {note.at_mm[0] + 6.0, note.at_mm[1] + 6.0, 60.0, 36.0};
}

}  // namespace genko::render::annotations

namespace genko::render {

void register_annotation_ops(core::OpRegistry& registry) {
    registry.add("add_annotation", annotations::add_annotation);
    registry.add("set_annotation", annotations::set_annotation);
    registry.add("delete_annotations", annotations::delete_annotations);
}

}  // namespace genko::render
