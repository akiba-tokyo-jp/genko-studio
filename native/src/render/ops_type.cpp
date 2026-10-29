// PC5 節 G (文字; type@1, render/ops_type.hpp): the type ops — G-0's add_type_layer, edit_type (its text) and type_info, and
// the frames of the later groups' ops.

#include "render/ops_type.hpp"

#include <algorithm>
#include <string>
#include <vector>

#include "core/ids.hpp"
#include "core/ops_util.hpp"
#include "core/pyconv.hpp"
#include "core/pyops.hpp"
#include "core/ruby.hpp"
#include "core/type_text.hpp"
#include "render/raster_ops.hpp"
#include "render/text/fonts.hpp"

namespace genko::render {

namespace {

using core::Document;
using core::Json;
using core::Layer;
using core::OpContext;
using core::OpError;
using core::Page;
using core::StoryLine;

const Json* given(const Json& op, std::string_view key) {
    const Json* v = core::get(op, key);
    return v == nullptr || v->is_null() ? nullptr : v;
}

// The args this group does not take yet: refused by name, for the group that writes them.
void refuse_later(const Json& op, std::string_view name, std::initializer_list<std::string_view> keys, std::string_view group) {
    for (const std::string_view key : keys) {
        if (given(op, key) != nullptr)
            core::not_yet_ported(std::string(name) + " is not in the C++ build yet with " + std::string(key) + " (PC5 " + std::string(group) + ")");
    }
}

// The text an op gives (a str, not empty).
std::string text_of(const Json& op) {
    const Json* text = given(op, "text");
    if (text == nullptr || !text->is_string()) throw OpError("text is required");
    if (text->get_ref<const std::string&>().empty()) throw OpError("text must not be empty");
    return text->get<std::string>();
}

// What an op names: a line of the story ("line": its id) or a type layer of a page ("page" and "layer_id").
struct Target {
    StoryLine* line = nullptr;
    std::size_t page = 0;
    std::size_t layer = 0;
};

Target target_of(Document& doc, const Json& op, bool edit) {
    const Json* line = given(op, "line");
    const Json* layer_id = given(op, "layer_id");
    if (line != nullptr && layer_id != nullptr) throw OpError("line or layer_id names the text, not both");
    if (line != nullptr) return Target{&core::find_line(doc, core::py_str(*line))};
    if (layer_id == nullptr) throw OpError("line (a line's id) or page and layer_id (a type layer) name the text");
    Target t;
    t.page = core::require_page(doc, op);
    t.layer = core::layer_by_id(doc.page(t.page), core::py_str(*layer_id));
    const Layer& found = doc.page(t.page).layers[t.layer];
    if (!core::type_text::is_type_layer(found)) throw OpError("layer " + found.id + " is not a type layer");
    if (!found.type) throw OpError("the type layer " + found.id + " holds no text");
    if (edit && found.locked) throw OpError("the layer is locked");
    return t;
}

// --- add_type_layer ------------------------------------------------------------------------------------------------------

void add_type_layer(OpContext& c) {
    refuse_later(c.op, "add_type_layer", {"point", "char", "para", "char_style", "para_style", "antialias", "size_pt", "parent", "above"}, "G-a");
    Document& doc = c.doc;
    const std::size_t at = core::require_page(doc, c.op);
    const std::string text = text_of(c.op);
    const Json* box = given(c.op, "box");
    if (box == nullptr || !box->is_array() || box->size() != 4) throw OpError("box [x, y, w, h] (mm) is required");
    std::array<double, 4> b{};
    for (std::size_t i = 0; i < 4; ++i) b[i] = core::finite_float((*box)[i], "box");
    if (!(b[2] > 0) || !(b[3] > 0)) throw OpError("the box's width and height must be above 0");
    StoryLine record;
    record.text = text;
    record.x_mm = core::Num(b[0]);
    record.y_mm = core::Num(b[1]);
    record.w_mm = core::Num(b[2]);
    record.h_mm = core::Num(b[3]);
    record.balloon = "none";
    record.wrap = "vertical";
    if (const Json* wrap = given(c.op, "wrap")) {
        if (*wrap != Json("vertical") && *wrap != Json("horizontal")) throw OpError("wrap must be vertical or horizontal");
        record.wrap = wrap->get<std::string>();
    }
    if (const Json* balloon = given(c.op, "balloon")) record.balloon = core::line_balloon(*balloon);  // (as add_line checks it)
    Page& page = doc.edit_page(at);
    // (a line's, as add_line takes it: a picture in it checked as the drawing build checks one)
    if (const Json* style = given(c.op, "style")) record.style = core::line_style(*style, [](const std::string& bytes) { verify_style_picture(bytes); });
    if (const Json* native = given(c.op, "native_type")) {
        try {
            const Json normal = core::type_text::check_native_type(*native, text);
            if (!normal.is_null()) record.native_type = normal;
        } catch (const core::Error& error) {
            throw OpError(error.what());
        }
    }
    Layer layer;
    layer.id = core::truthy_at(c.op, "new_id") ? core::py_str(c.op["new_id"]) : core::new_id();
    for (const Layer& l : page.layers) {
        if (l.id == layer.id) throw OpError("layer " + layer.id + " exists");
    }
    layer.role = core::LayerRole::Text;
    layer.kind = core::LayerKind::Type;
    layer.panel_clip = false;  // (as a line: not cut by the panels)
    if (const Json* title = given(c.op, "title")) {
        layer.title = core::py_str(*title);
    } else {  // (Photoshop's: the text's first words)
        std::u32string words = text::u32(text).substr(0, 24);
        std::replace(words.begin(), words.end(), U'\n', U' ');
        layer.title = text::utf8(words);
    }
    layer.type = core::type_text::shared_record(std::move(record));
    const std::string id = layer.id;
    page.layers.push_back(std::move(layer));
    core::type_text::add_feature(doc);
    c.report = Json{{"layer_id", id}};
}

// --- edit_type -----------------------------------------------------------------------------------------------------------

void edit_type(OpContext& c) {
    refuse_later(c.op, "edit_type", {"replace", "insert", "point", "box", "anchor", "transform", "antialias", "title"}, "G-a");
    Document& doc = c.doc;
    const Target t = target_of(doc, c.op, true);
    const std::string text = text_of(c.op);
    if (t.line != nullptr) {  // (a line: edit_line's text — its rubies and ranges followed, its report)
        const core::OpFunction* edit_line = core::OpRegistry::builtin().find("edit_line");
        const Json op{{"op", "edit_line"}, {"id", t.line->id}, {"text", text}};
        OpContext inner{c.doc, op, c.actor};
        (*edit_line)(inner);
        c.report = inner.report;
        return;
    }
    Page& page = doc.edit_page(t.page);
    Layer& layer = page.layers[t.layer];
    StoryLine record = *layer.type;
    const std::string before = record.text;
    record.text = text;
    if (before != text) {
        if (record.ruby_spans) {
            core::ruby::Followed f = core::ruby::follow_text(before, text, *record.ruby_spans);
            record.ruby_spans = std::move(f.spans);
            if (f.removed > 0) c.report = Json{{"ruby_removed", f.removed}};
        }
        if (record.native_type) {
            const Json followed = core::type_text::follow(*record.native_type, before, text, core::type_text::edits_between(before, text));
            if (const std::int64_t removed = core::type_text::removed_by(*record.native_type, followed); removed > 0) {
                Json report = c.report ? *c.report : Json::object();
                report["type_removed"] = removed;  // (ranges or paragraphs' settings the edit took away, said)
                c.report = std::move(report);
            }
            if (followed.is_null()) record.native_type.reset();
            else record.native_type = followed;
        }
    }
    core::ruby::settle(doc, record);
    layer.type = core::type_text::shared_record(std::move(record));
}

// --- type_info -----------------------------------------------------------------------------------------------------------

Json info_of(const StoryLine& record, const Json* styles) {
    Json out = Json::object();
    out["text"] = record.text;
    out["graphemes"] = core::ruby::count(record.text);
    out["wrap"] = record.wrap;
    out["box"] = Json::array({record.x_mm.json(), record.y_mm.json(), record.w_mm.json(), record.h_mm.json()});
    out["balloon"] = record.balloon;
    const Json* shape = record.native_type ? core::get(*record.native_type, "shape") : nullptr;
    out["shape"] = shape != nullptr ? *shape : Json("line");  // (none: set as a line, in its box fitted to its balloon)
    const Json* antialias = record.native_type ? core::get(*record.native_type, "antialias") : nullptr;
    out["antialias"] = antialias != nullptr ? *antialias : Json("smooth");
    if (record.native_type) out["native_type"] = *record.native_type;
    Json paragraphs = Json::array();
    const std::int64_t count = core::type_text::paragraph_count(record.text);
    const std::int64_t n = core::ruby::count(record.text);
    for (std::int64_t p = 0; p < count; ++p) {
        const std::int64_t start = core::type_text::paragraph_start(record.text, p);
        const std::int64_t end = p + 1 < count ? core::type_text::paragraph_start(record.text, p + 1) : n;
        paragraphs.push_back(Json{{"index", p}, {"start", start}, {"end", end}, {"para", core::type_text::resolve_para(record, styles, p)}});
    }
    out["paragraphs"] = std::move(paragraphs);
    Json runs = Json::array();
    for (const core::type_text::CharRun& run : core::type_text::resolve_chars(record, styles))
        runs.push_back(Json{{"start", run.start}, {"end", run.end}, {"char", run.attrs}});
    out["runs"] = std::move(runs);
    return out;
}

void type_info(OpContext& c) {
    const Target t = target_of(c.doc, c.op, false);
    const Json* styles = c.doc.type_styles ? &*c.doc.type_styles : nullptr;
    if (t.line != nullptr) {
        Json out = info_of(*t.line, styles);
        out["line"] = t.line->id;
        c.report = std::move(out);
        return;
    }
    const Layer& layer = c.doc.page(t.page).layers[t.layer];
    Json out = info_of(*layer.type, styles);
    out["layer_id"] = layer.id;
    out["title"] = layer.title;
    c.report = std::move(out);
}

// --- the later groups' ops (registered: their names known, refused until they are written) --------------------------------

core::OpFunction later(std::string name, std::string group) {
    return [name, group](OpContext&) { core::not_yet_ported(name + " is not in the C++ build yet (PC5 " + group + ")"); };
}

}  // namespace

void register_type_ops(core::OpRegistry& registry) {
    registry.add("add_type_layer", add_type_layer);
    registry.add("edit_type", edit_type);
    registry.add_query("type_info", type_info);  // (a question: asked alone, applied as a dry run whoever asks)
    registry.add("set_type", later("set_type", "G-b"));
    registry.add("type_style", later("type_style", "G-c"));
    registry.add("line_to_type_layer", later("line_to_type_layer", "G-a"));
    registry.add("type_layer_to_line", later("type_layer_to_line", "G-a"));
    registry.add("replace_fonts", later("replace_fonts", "G-d"));
}

}  // namespace genko::render
