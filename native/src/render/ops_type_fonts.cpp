// PC5 G-d (type.resolveMissingFonts, type.replaceAllMissingFonts; SPEC.md TEXT-02「書体の置き換え」): replace_fonts — the
// fonts a book's texts name that this computer does not have, listed or replaced (PhotoCraft v0.2.0's
// engine/src/type_extra_cmds.rs:399-460, MIT: a map of missing to installed, the rest left — or, all, the default).
//
// A text names a font in its line's style (style.font), in its own character attributes (native_type's char and its ranges'),
// in its rubies' settings (ruby_spans' font) and through the book's styles (type_styles' char). A font is missing when it is
// neither one the program carries (a bundled key) nor a file there (render::text::face_exists). Lines, the pages' type layers
// and the type layers in the contents of smart objects are looked at; a locked type layer is left as it is.

#include <algorithm>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "core/command_bus.hpp"
#include "core/font_catalog.hpp"
#include "core/layer_basics.hpp"
#include "core/pyconv.hpp"
#include "core/pyops.hpp"
#include "core/smart.hpp"
#include "core/type_text.hpp"
#include "render/ops_type.hpp"
#include "render/text/fonts.hpp"
#include "render/text/lettering.hpp"
#include "render/text/opentype.hpp"
#include "render/text/shaping.hpp"

namespace genko::render::type_ops {

namespace {

using core::Document;
using core::Json;
using core::OpError;
using core::StoryLine;

// What one missing font is to become: a font (a bundled key or a file's path) with its face and names, or nothing (the
// default face: the font taken away).
struct Becomes {
    std::optional<std::string> font;
    std::int64_t face_index = 0;
    Json names = Json::object();  // (family, font_style, postscript: the new font's, when given)
};

// The fonts a character attributes' object names ({"font": …}): the missing one, with what else it says of itself.
struct Seen {
    std::int64_t texts = 0;
    Json names = Json::object();
};

// (fonts@1: one of the faces Genko comes with whose file is not on this computer is not a missing font — it comes back
// when Genko is installed whole again, and replacing it, or all the missing fonts at once, would take the face out of the
// book for good. It is said by the page's check (font_not_installed) and listed apart: not_installed, below)
bool missing(const std::string& font) { return !font.empty() && !core::fonts::added(font) && !text::face_exists(font); }
bool not_installed(const std::string& font) { return core::fonts::added(font) && !text::face_exists(font); }

// (`text`: a text names it — a style's font is listed, its texts counted where they take it)
void note(std::map<std::string, Seen>& seen, const std::string& font, const Json* chars, std::set<std::string>& in_this, bool text = true) {
    if (!missing(font)) return;
    Seen& s = seen[font];
    if (in_this.insert(font).second && text) ++s.texts;
    if (chars != nullptr) {
        for (const char* key : {"family", "font_style", "postscript"}) {
            if (chars->contains(key) && (*chars)[key].is_string() && !s.names.contains(key)) s.names[key] = (*chars)[key];
        }
    }
}

// The missing fonts a record names (its style, its own attributes, its rubies).
void look_at(const StoryLine& record, std::map<std::string, Seen>& seen) {
    std::set<std::string> in_this;
    if (record.style.is_object() && record.style.contains("font") && record.style["font"].is_string()) note(seen, record.style["font"].get<std::string>(), nullptr, in_this);
    if (record.native_type && record.native_type->is_object()) {
        const Json& nt = *record.native_type;
        const auto chars = [&](const Json& c) {
            if (c.is_object() && c.contains("font") && c["font"].is_string()) note(seen, c["font"].get<std::string>(), &c, in_this);
        };
        if (nt.contains("char")) chars(nt["char"]);
        if (nt.contains("runs") && nt["runs"].is_array()) {
            for (const Json& run : nt["runs"]) {
                if (run.is_object() && run.contains("char")) chars(run["char"]);
            }
        }
    }
    for (const core::RubySpan& span : record.ruby_spans.value_or(std::vector<core::RubySpan>{})) {
        if (span.font) note(seen, *span.font, nullptr, in_this);
    }
}

// A character attributes' object with its font replaced — true when it changed.
bool replace_in(Json& chars, const std::function<std::optional<Becomes>(const std::string&)>& pick) {
    if (!chars.is_object() || !chars.contains("font") || !chars["font"].is_string()) return false;
    const auto to = pick(chars["font"].get<std::string>());
    if (!to) return false;
    for (const char* key : {"font", "face_index", "family", "font_style", "postscript"}) chars.erase(key);
    if (to->font) {
        chars["font"] = *to->font;
        if (to->face_index > 0) chars["face_index"] = to->face_index;
        for (const auto& [key, value] : to->names.items()) chars[key] = value;
    }
    return true;
}

// The record with its missing fonts replaced — true when it changed.
bool replace_record(StoryLine& record, const std::function<std::optional<Becomes>(const std::string&)>& pick) {
    bool changed = false;
    bool style_replaced = false;
    std::int64_t face_for_style = 0;
    if (record.style.is_object() && record.style.contains("font") && record.style["font"].is_string()) {
        if (const auto to = pick(record.style["font"].get<std::string>())) {
            if (to->font) record.style["font"] = *to->font;
            else record.style.erase("font");
            face_for_style = to->font ? to->face_index : 0;
            changed = true;
            style_replaced = true;
        }
    }
    if (record.native_type && record.native_type->is_object()) {
        Json nt = *record.native_type;
        bool own = false;
        if (nt.contains("char")) own = replace_in(nt["char"], pick) || own;
        if (nt.contains("runs") && nt["runs"].is_array()) {
            for (Json& run : nt["runs"]) {
                if (run.is_object() && run.contains("char")) own = replace_in(run["char"], pick) || own;
            }
        }
        if (own) {
            set_native_type(record, nt);
            changed = true;
        }
    }
    if (face_for_style > 0) {  // (a face of a collection for the line's own font: its character attribute, the whole text's)
        Json nt = record.native_type ? *record.native_type : Json::object();
        if (!nt.contains("char") || !nt["char"].is_object()) nt["char"] = Json::object();
        if (!nt["char"].contains("font")) nt["char"]["face_index"] = face_for_style;
        set_native_type(record, nt);
    } else if (style_replaced && record.native_type && record.native_type->is_object()) {
        // (review P3-5 a: face 0 now — a face an earlier replacement gave the line's font taken back with it)
        Json nt = *record.native_type;
        if (nt.contains("char") && nt["char"].is_object() && !nt["char"].contains("font") && nt["char"].contains("face_index")) {
            nt["char"].erase("face_index");
            set_native_type(record, nt);
        }
    }
    if (record.ruby_spans) for (core::RubySpan& span : *record.ruby_spans) {
        if (!span.font) continue;
        if (const auto to = pick(*span.font)) {
            span.font = to->font;  // (none: the line's face)
            changed = true;
        }
    }
    return changed;
}

Becomes becomes_of(const Json& value, const std::string& from) {
    Becomes b;
    if (value.is_null()) return b;
    if (value.is_string()) {
        b.font = value.get<std::string>();
    } else if (value.is_object()) {
        for (const auto& [key, v] : value.items()) {
            if (key != "font" && key != "face_index" && key != "family" && key != "font_style" && key != "postscript")
                throw OpError("unknown key " + key + " for the font " + from + " is to become");
        }
        if (!value.contains("font") || !value["font"].is_string()) throw OpError("map: " + from + " must become a font (font)");
        b.font = value["font"].get<std::string>();
        if (value.contains("face_index")) {
            const Json& i = value["face_index"];
            if (!i.is_number_integer() || i.get<std::int64_t>() < 0 || i.get<std::int64_t>() > 255) throw OpError("map: face_index must be a whole number 0..255");
            b.face_index = i.get<std::int64_t>();
        }
        for (const char* key : {"family", "font_style", "postscript"}) {
            if (value.contains(key)) {
                if (!value[key].is_string() || value[key].get<std::string>().empty() || value[key].get<std::string>().size() > 256)
                    throw OpError(std::string("map: ") + key + " must be a name of 1..256 characters");
                b.names[key] = value[key];
            }
        }
    } else {
        throw OpError("map: " + from + " must become a font, or null for the default");
    }
    if (b.font && (b.font->empty() || b.font->size() > 4096)) throw OpError("map: the font " + from + " is to become must be a name of 1..4096 characters");
    if (b.font && !text::face_exists(*b.font)) throw OpError("map: the font " + *b.font + " is not on this computer");
    if (b.font) check_face(*b.font, b.face_index);  // (review P3-6)
    return b;
}

// Every record of the book (lines; the pages' and the contents' type layers but the locked ones), each given to `visit`,
// which may change it; the pages (rows) of what changed — a line's page, a type layer's, the pages showing changed
// contents — into `rows`.
void each_record(Document& doc, const std::function<bool(StoryLine&)>& visit, std::set<std::size_t>& rows) {
    for (StoryLine& line : doc.story) {
        StoryLine record = line;
        if (visit(record)) {
            fit_point_text(record, doc);
            line = std::move(record);
            for (std::size_t i = 0; i < doc.pages.size(); ++i) {
                if (doc.pages[i]->index == line.page_index) rows.insert(i);
            }
        }
    }
    const auto on_page = [&](const core::Page& page, const std::function<core::Page&()>& edit) {
        bool any = false;
        for (std::size_t j = 0; j < page.layers.size(); ++j) {
            const core::Layer& layer = page.layers[j];
            if (!core::type_text::is_type_layer(layer) || !layer.type || layer.locked || core::locked_all(page.layers, j)) continue;  // (すべてをロック too)
            StoryLine record = *layer.type;
            if (!visit(record)) continue;
            fit_point_text(record, doc);
            edit().layers[j].type = core::type_text::shared_record(std::move(record));
            any = true;
        }
        return any;
    };
    for (std::size_t i = 0; i < doc.pages.size(); ++i) {
        if (doc.is_deferred(i)) continue;
        if (on_page(doc.page(i), [&]() -> core::Page& { return doc.edit_page(i); })) rows.insert(i);
    }
    std::vector<std::string> contents;
    for (const auto& [cid, sc] : doc.smart_contents) {
        if (sc && sc->page) contents.push_back(cid);
    }
    for (const std::string& cid : contents) {
        const std::shared_ptr<core::Page> held = doc.smart_contents.at(cid)->page;
        if (on_page(*held, [&]() -> core::Page& { return doc.edit_contents_page(cid); })) {
            for (const std::size_t at : core::smart::pages_using(doc, cid)) rows.insert(at);
        }
    }
}

// The missing fonts of the book's texts and styles.
std::map<std::string, Seen> missing_fonts(const Document& doc) {
    std::map<std::string, Seen> seen;
    for (const StoryLine& line : doc.story) look_at(line, seen);
    const auto on_page = [&](const core::Page& page) {
        for (std::size_t j = 0; j < page.layers.size(); ++j) {
            const core::Layer& layer = page.layers[j];
            // (a locked one is left as it is, so not listed either: review P3-5 c)
            if (core::type_text::is_type_layer(layer) && layer.type && !layer.locked && !core::locked_all(page.layers, j)) look_at(*layer.type, seen);
        }
    };
    for (std::size_t i = 0; i < doc.pages.size(); ++i) {
        if (!doc.is_deferred(i)) on_page(doc.page(i));
    }
    for (const auto& [cid, sc] : doc.smart_contents) {
        if (sc && sc->page) on_page(*sc->page);
    }
    if (doc.type_styles && doc.type_styles->is_object()) {
        const Json& styles = *doc.type_styles;
        std::set<std::string> in_styles;
        for (const char* kind : {"character", "paragraph"}) {
            if (!styles.contains(kind) || !styles[kind].is_array()) continue;
            for (const Json& style : styles[kind]) {
                if (style.is_object() && style.contains("char") && style["char"].is_object() && style["char"].contains("font") && style["char"]["font"].is_string())
                    note(seen, style["char"]["font"].get<std::string>(), &style["char"], in_styles, false);
            }
        }
        if (styles.contains("basic") && styles["basic"].is_object() && styles["basic"].contains("char")) {
            const Json& c = styles["basic"]["char"];
            if (c.is_object() && c.contains("font") && c["font"].is_string()) note(seen, c["font"].get<std::string>(), &c, in_styles, false);
        }
    }
    return seen;
}

}  // namespace

void check_face(const std::string& font, std::int64_t face_index) {
    if (face_index <= 0 || font.find_first_of("/\\") == std::string::npos) return;  // (face 0, or a bundled font's key)
    unsigned faces = 1;
    try {
        faces = text::font_info(font, 0, false).faces;
    } catch (const std::exception&) {
        return;  // (not on this computer, or unreadable: its face is the drawing's to say)
    }
    if (face_index >= static_cast<std::int64_t>(faces))
        throw OpError("face_index " + std::to_string(face_index) + " is past the faces of " + font + " (it holds " + std::to_string(faces) + ")");
}

void check_faces(const StoryLine& record) {
    if (!record.native_type || !record.native_type->is_object()) return;
    const Json& nt = *record.native_type;
    const auto font_of = [](const Json& chars) { return chars.is_object() && chars.contains("font") && chars["font"].is_string() ? chars["font"].get<std::string>() : std::string(); };
    std::string whole = nt.contains("char") ? font_of(nt["char"]) : std::string();
    if (whole.empty() && record.style.is_object() && record.style.contains("font") && record.style["font"].is_string()) whole = record.style["font"].get<std::string>();
    const auto check_one = [&](const Json& chars) {
        if (!chars.is_object() || !chars.contains("face_index") || !chars["face_index"].is_number_integer()) return;
        const std::string own = font_of(chars);
        check_face(own.empty() ? whole : own, chars["face_index"].get<std::int64_t>());
    };
    if (nt.contains("char")) check_one(nt["char"]);
    if (nt.contains("runs") && nt["runs"].is_array()) {
        for (const Json& run : nt["runs"]) {
            if (run.is_object() && run.contains("char")) check_one(run["char"]);
        }
    }
}

// fonts@1: the added faces the book names whose files are not on this computer ({font: its key, label: its name}), in
// the catalogue's order.
Json faces_not_installed(const Document& doc) {
    Json out = Json::array();
    const std::vector<std::string> named = core::fonts::named(doc);
    for (const core::fonts::Entry& entry : core::fonts::entries()) {
        if (entry.added && std::find(named.begin(), named.end(), entry.key) != named.end() && not_installed(entry.key)) {
            out.push_back(Json{{"font", entry.key}, {"label", entry.label}});
        }
    }
    return out;
}

bool replace_fonts_asks(const Json& op) { return !op.contains("map") && !core::py_truthy(core::get_or(op, "all", Json(false))); }

void replace_fonts(core::OpContext& c) {
    const Json& op = c.op;
    for (const auto& [key, value] : op.items()) {
        if (key != "op" && key != "map" && key != "all") throw OpError("unknown key " + key + " for replace_fonts");
    }
    if (op.contains("map") && !op["map"].is_object()) throw OpError("map must be {font: the font it becomes}");
    if (op.contains("all") && !op["all"].is_boolean()) throw OpError("all must be true or false");
    const std::map<std::string, Seen> seen = missing_fonts(c.doc);
    Json listed = Json::array();
    for (const auto& [font, s] : seen) {
        Json entry{{"font", font}, {"texts", s.texts}};
        for (const auto& [key, value] : s.names.items()) entry[key] = value;
        listed.push_back(std::move(entry));
    }
    if (replace_fonts_asks(op)) {  // (a question: what is missing)
        c.report = Json{{"missing", listed}};
        // (fonts@1: the faces Genko comes with that the book names and this computer does not have, by key and name — told,
        // never replaced; the key is there only when there is one, so a book without them is answered as before)
        const Json absent = faces_not_installed(c.doc);
        if (!absent.empty()) (*c.report)["not_installed"] = absent;
        return;
    }
    std::map<std::string, Becomes> map;
    if (op.contains("map")) {
        for (const auto& [from, value] : op["map"].items()) map.emplace(from, becomes_of(value, from));
    }
    const bool all = op.value("all", Json(false)) == Json(true);
    const auto pick = [&](const std::string& font) -> std::optional<Becomes> {
        if (!seen.contains(font)) return std::nullopt;  // (one this computer has stays: upstream's)
        if (const auto it = map.find(font); it != map.end()) return it->second;
        if (all) return Becomes{};  // (the default face)
        return std::nullopt;
    };
    // the book's styles first (the texts that take them set again below)
    bool styles_changed = false;
    if (c.doc.type_styles && c.doc.type_styles->is_object()) {
        Json styles = *c.doc.type_styles;
        for (const char* kind : {"character", "paragraph"}) {
            if (!styles.contains(kind) || !styles[kind].is_array()) continue;
            for (Json& style : styles[kind]) {
                if (style.is_object() && style.contains("char")) styles_changed = replace_in(style["char"], pick) || styles_changed;
            }
        }
        if (styles.contains("basic") && styles["basic"].is_object() && styles["basic"].contains("char"))
            styles_changed = replace_in(styles["basic"]["char"], pick) || styles_changed;
        if (styles_changed) {
            const Json normal = core::type_text::check_type_styles(styles);
            if (normal.is_null()) c.doc.type_styles.reset();
            else c.doc.type_styles = normal;
        }
    }
    std::int64_t replaced = 0;
    const text::TypeStyles drawn(c.doc.type_styles ? &*c.doc.type_styles : nullptr);  // (a point text's box measured under them)
    std::set<std::size_t> rows;
    each_record(c.doc, [&](StoryLine& record) {
        if (replace_record(record, pick)) {
            ++replaced;
            return true;
        }
        // (a text that takes a style whose font changed: its point text's box set again)
        return styles_changed && text::sets_opentype(record) && text::shape_of(record) == "point";
    }, rows);
    if (styles_changed) {  // (a style's font changed: the pages of the texts typeset by the styles — each could take it)
        for (std::size_t i = 0; i < c.doc.pages.size(); ++i) {
            if (c.doc.is_deferred(i)) continue;
            for (const core::Layer& layer : c.doc.page(i).layers) {
                if (core::type_text::is_type_layer(layer) && layer.type && !layer.locked) rows.insert(i);
            }
        }
        for (const StoryLine& line : c.doc.story) {
            if (!line.native_type) continue;
            for (std::size_t i = 0; i < c.doc.pages.size(); ++i) {
                if (c.doc.pages[i]->index == line.page_index) rows.insert(i);
            }
        }
    }
    // (review P2-2: a page another has locked is not changed — the op refused, as set_type on it is)
    for (const std::size_t at : rows) core::check_page_held(c.doc, at, c.actor);
    Json names = Json::array();
    for (const auto& [font, s] : seen) names.push_back(font);
    c.report = Json{{"missing", names}, {"replaced", replaced}, {"styles", styles_changed}};
}

}  // namespace genko::render::type_ops
