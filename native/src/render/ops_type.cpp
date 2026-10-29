// PC5 節 G (文字; type@1, render/ops_type.hpp): the type ops — G-0's frames and G-a's add_type_layer, edit_type,
// type_info, line_to_type_layer and type_layer_to_line; the later groups' ops registered by name.

#include "render/ops_type.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <string>
#include <vector>

#include "core/ids.hpp"
#include "core/ops_util.hpp"
#include "core/pyconv.hpp"
#include "core/pyops.hpp"
#include "core/ruby.hpp"
#include "core/type_text.hpp"
#include "render/not_yet_ported.hpp"
#include "render/raster_ops.hpp"
#include "render/text/fonts.hpp"
#include "render/text/lettering.hpp"

namespace genko::render {

namespace {

using core::Document;
using core::Json;
using core::Layer;
using core::Num;
using core::OpContext;
using core::OpError;
using core::Page;
using core::StoryLine;

constexpr int kMeasureDpi = 300;      // (where a text's letters lie — a point text's box kept round them, type_info's lines)
constexpr std::size_t kTitleChars = 24;  // (a type layer's name from its text: its first letters, as Photoshop's)

const Json* given(const Json& op, std::string_view key) {
    const Json* v = core::get(op, key);
    return v == nullptr || v->is_null() ? nullptr : v;
}

// A text an op gives (a str, not empty).
std::string text_at(const Json& value, std::string_view what) {
    if (!value.is_string()) throw OpError(std::string(what) + " must be a string");
    if (value.get_ref<const std::string&>().empty()) throw OpError("text must not be empty");
    return value.get<std::string>();
}

double round2(double v) { return core::py_round(v, 2); }

// [x, y] (mm) or [x, y, w, h] (mm): finite numbers; a box's width and height above 0.
constexpr double kFarMm = 1000.0;  // (review P2-4: a place within a metre of the page's corner, a box at most a metre)
std::array<double, 2> point_at(const Json& v, std::string_view what) {
    if (!v.is_array() || v.size() != 2) throw OpError(std::string(what) + " [x, y] (mm) places a point text");
    const std::array<double, 2> p{core::finite_float(v[0], what), core::finite_float(v[1], what)};
    if (std::fabs(p[0]) > kFarMm || std::fabs(p[1]) > kFarMm) throw OpError(std::string(what) + " must lie within a metre of the page (x and y -1000..1000 mm)");
    return p;
}
std::array<double, 4> box_at(const Json& v, std::string_view what) {
    if (!v.is_array() || v.size() != 4) throw OpError(std::string(what) + " [x, y, w, h] (mm) places a box text");
    std::array<double, 4> b{};
    for (std::size_t i = 0; i < 4; ++i) b[i] = core::finite_float(v[i], what);
    if (!(b[2] > 0) || !(b[3] > 0)) throw OpError("the box's width and height must be above 0");
    if (std::fabs(b[0]) > kFarMm || std::fabs(b[1]) > kFarMm || b[2] > kFarMm || b[3] > kFarMm)
        throw OpError(std::string(what) + " must lie within a metre of the page (x and y -1000..1000 mm, w and h up to 1000 mm)");
    return b;
}

// A type layer's name from its text (Photoshop's: its first words).
std::string title_of(const std::string& text) {
    std::u32string words = text::u32(text).substr(0, kTitleChars);
    std::replace(words.begin(), words.end(), U'\n', U' ');
    std::replace(words.begin(), words.end(), U'\r', U' ');
    return text::utf8(words);
}

// The record's native_type as an object to change (none: empty), and back in its normal form (none when it holds nothing).
Json native_of(const StoryLine& record) { return record.native_type ? *record.native_type : Json::object(); }
void set_native(StoryLine& record, const Json& raw) {
    try {
        const Json normal = core::type_text::check_native_type(raw, record.text);
        if (normal.is_null()) record.native_type.reset();
        else record.native_type = normal;
    } catch (const core::Error& error) {
        throw OpError(error.what());
    }
}

std::optional<std::string> font_of(const Document& doc) { return std::optional<std::string>(doc.font_path); }

// Where the record's letters lie at the measuring resolution, or none when this build cannot set them (a face it cannot
// open yet: the ops leave the box as it is, as the page leaves such a text out).
std::optional<text::Placement> placed(const StoryLine& record, const Document& doc) {
    try {
        return text::placement(record, kMeasureDpi, font_of(doc));
    } catch (const NotYetPorted&) {
        return std::nullopt;
    } catch (const core::Error& error) {  // (review P2-4: a text that would set into too large a picture)
        if (error.code() == "image_too_large")
            throw OpError("the text is too large to set (a point text of very long rows, or a box text far past its box)");
        throw;
    }
}

constexpr double kPxMm = 25.4 / kMeasureDpi;
std::array<double, 4> mm_of(const Box& b) { return {b.x0 * kPxMm, b.y0 * kPxMm, b.x1 * kPxMm, b.y1 * kPxMm}; }

// A point text's box: round its letters (where it is hit, edited and found), as set now.
void fit_point(StoryLine& record, const Document& doc) {
    const std::string shape = text::shape_of(record);
    if (shape.empty() || core::truthy_at(text::style_of(record), "text_path")) return;
    const std::optional<text::Placement> p = placed(record, doc);  // (a box text too: one too large to set is refused)
    if (!p || shape != "point") return;
    const std::array<double, 4> b = mm_of(p->letters.value_or(p->picture));
    record.x_mm = Num(round2(b[0]));
    record.y_mm = Num(round2(b[1]));
    record.w_mm = Num(std::max(0.01, round2(b[2] - b[0])));
    record.h_mm = Num(std::max(0.01, round2(b[3] - b[1])));
}

// A text set in a balloon is not a point or box text (Photoshop's have none).
void without_balloon(const StoryLine& record) {
    if (record.balloon != "none")
        throw OpError("a text in a balloon is set in it: point and box text have none (balloon none first)");
}

// Its size kept as set now: a text set to fit its box takes the size it was set at.
void keep_size(StoryLine& record, const std::optional<text::Placement>& before) {
    if (core::py_truthy(core::get_or(record.style, "size_mm", Json())) || !before || before->em <= 0) return;
    Json style = record.style.is_object() ? record.style : Json::object();
    style["size_mm"] = core::py_round(static_cast<double>(before->em) * kPxMm, 2);
    record.style = std::move(style);
}

// ポイントテキストに変換: the text set unbroken from an anchor, where its letters were (its first row's or column's side:
// across, its left, middle or right by its alignment; down, its right; and its top). `anchor`: the anchor given instead.
void to_point(StoryLine& record, const Document& doc, const std::optional<std::array<double, 2>>& anchor) {
    without_balloon(record);
    const std::optional<text::Placement> before = placed(record, doc);
    keep_size(record, before);
    Json nt = native_of(record);
    nt["shape"] = "point";
    const std::array<double, 4> was = before ? mm_of(before->picture) : std::array<double, 4>{record.x_mm.value(), record.y_mm.value(),
                                                                                              (record.x_mm + record.w_mm).value(), (record.y_mm + record.h_mm).value()};
    nt["anchor_mm"] = Json::array({anchor ? (*anchor)[0] : was[0], anchor ? (*anchor)[1] : was[1]});
    set_native(record, nt);
    if (anchor || !before) return;
    const std::optional<text::Placement> after = placed(record, doc);
    if (!after) return;
    const std::array<double, 4> now = mm_of(after->picture);
    const Json align = core::get_or(text::style_of(record), "align", Json());
    const double dx = record.wrap == "vertical" ? was[2] - now[2]
                      : align == Json("center") ? (was[0] + was[2] - now[0] - now[2]) / 2
                      : align == Json("right")  ? was[2] - now[2]
                                                : was[0] - now[0];
    nt = native_of(record);
    nt["anchor_mm"] = Json::array({nt["anchor_mm"][0].get<double>() + dx, nt["anchor_mm"][1].get<double>() + was[1] - now[1]});
    set_native(record, nt);
}

// ポイントテキストに変換 of a box text: its text with a line break where its box broke a row (Photoshop's hard return at
// every soft break, as PhotoCraft's to_point), found by walking its rows as set over its text; none when there is no
// such break or the rows are not its text letter for letter (a space a column dropped: left as it is).
std::optional<std::string> with_breaks(const StoryLine& record, const Document& doc) {
    if (text::shape_of(record) != "box") return std::nullopt;
    std::vector<text::SetRow> rows;
    try {
        rows = text::set_layout(record, kMeasureDpi, font_of(doc), text::LetterParts{}).set.rows;
    } catch (const NotYetPorted&) {
        return std::nullopt;
    }
    const std::u32string source = text::u32(record.text);
    std::u32string out;
    std::size_t at = 0;
    bool broke = false;
    for (std::size_t r = 0; r < rows.size(); ++r) {
        for (const text::SetChar& ch : rows[r].chars) {
            for (const char32_t c : ch.text) {
                if (at >= source.size() || source[at] != c) return std::nullopt;
                out.push_back(c);
                ++at;
            }
        }
        if (r + 1 == rows.size()) break;
        if (at < source.size() && source[at] == U'\r' && at + 1 < source.size() && source[at + 1] == U'\n') {
            out += U"\r\n";
            at += 2;
        } else if (at < source.size() && source[at] == U'\n') {
            out.push_back(U'\n');
            ++at;
        } else {
            out.push_back(U'\n');  // (a soft break: a hard one now)
            broke = true;
        }
    }
    if (at != source.size() || !broke) return std::nullopt;
    return text::utf8(out);
}

bool to_point_in_place(const Json& op) {
    const Json* p = given(op, "point");
    return p != nullptr && *p == Json(true);
}

// 段落テキストに変換: the text set in a box, broken there — the box round its letters now (a little more, so they break
// where they did), or the box given.
void to_box(StoryLine& record, const Document& doc, const std::optional<std::array<double, 4>>& box) {
    without_balloon(record);
    const std::optional<text::Placement> before = placed(record, doc);
    keep_size(record, before);
    std::array<double, 4> b{};
    if (box) {
        b = *box;
    } else if (before) {
        const std::array<double, 4> p = mm_of(before->picture);
        b = {p[0], p[1], p[2] - p[0] + 0.5, p[3] - p[1] + 0.5};
        if (record.wrap == "vertical") b[0] -= 0.5;  // (its columns from its right: the room to the left)
    } else {
        b = {record.x_mm.value(), record.y_mm.value(), record.w_mm.value(), record.h_mm.value()};
    }
    record.x_mm = Num(round2(b[0]));
    record.y_mm = Num(round2(b[1]));
    record.w_mm = Num(std::max(0.01, round2(b[2])));
    record.h_mm = Num(std::max(0.01, round2(b[3])));
    Json nt = native_of(record);
    nt["shape"] = "box";
    nt.erase("anchor_mm");
    set_native(record, nt);
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
    // (review P3-1: what moves it — its anchor, a box or point given, its matrix — keeps to the position's lock)
    const bool moves = given(op, "anchor") != nullptr || core::has(op, "transform") || (given(op, "point") != nullptr && op["point"].is_array()) ||
                       (given(op, "box") != nullptr && op["box"].is_array());
    if (edit && moves && found.lock_position) throw OpError("the position of the layer " + found.id + " is locked");
    return t;
}

// The record's text through the edit an op gives: text (whole), replace {start, end, text} or insert {at, text} — in
// graphemes (TEXT-01), one of them at most. None: no text given.
std::optional<std::string> new_text(const std::string& before, const Json& op) {
    const Json* whole = given(op, "text");
    const Json* replace = given(op, "replace");
    const Json* insert = given(op, "insert");
    if ((whole != nullptr) + (replace != nullptr) + (insert != nullptr) > 1) throw OpError("text, replace or insert edits the text, one of them");
    const std::int64_t n = core::ruby::count(before);
    const auto place = [&](const Json& v, std::string_view what) {
        if (!v.is_number_integer()) throw OpError(std::string(what) + " must be a whole number (graphemes)");
        const std::int64_t at = v.get<std::int64_t>();
        if (at < 0 || at > n) throw OpError(std::string(what) + " must be within the text (0.." + std::to_string(n) + " graphemes)");
        return at;
    };
    if (whole != nullptr) return text_at(*whole, "text");
    if (replace != nullptr) {
        if (!replace->is_object()) throw OpError("replace is {start, end, text}");
        const std::int64_t start = place(core::get_or(*replace, "start", Json()), "replace start");
        const std::int64_t end = place(core::get_or(*replace, "end", Json()), "replace end");
        if (end < start) throw OpError("replace end must not be before its start");
        const Json words = core::get_or(*replace, "text", Json(""));
        if (!words.is_string()) throw OpError("replace text must be a string");
        std::string out = core::ruby::slice(before, 0, start) + words.get<std::string>() + core::ruby::slice(before, end, n);
        if (out.empty()) throw OpError("text must not be empty");
        return out;
    }
    if (insert != nullptr) {
        if (!insert->is_object()) throw OpError("insert is {at, text}");
        const std::int64_t at = place(core::get_or(*insert, "at", Json()), "insert at");
        const Json words = core::get_or(*insert, "text", Json());
        if (!words.is_string() || words.get_ref<const std::string&>().empty()) throw OpError("insert text must be a string, not empty");
        return core::ruby::slice(before, 0, at) + words.get<std::string>() + core::ruby::slice(before, at, n);
    }
    return std::nullopt;
}

// A type layer's record with a new text: its rubies and ranges followed (what the edit took away said in the report).
void retext(StoryLine& record, const std::string& text, std::optional<Json>& report) {
    const std::string before = record.text;
    record.text = text;
    if (before == text) return;
    if (record.ruby_spans) {
        core::ruby::Followed f = core::ruby::follow_text(before, text, *record.ruby_spans);
        record.ruby_spans = std::move(f.spans);
        if (f.removed > 0) {
            Json r = report ? *report : Json::object();
            r["ruby_removed"] = f.removed;
            report = std::move(r);
        }
    }
    if (record.native_type) {
        const Json followed = core::type_text::follow(*record.native_type, before, text, core::type_text::edits_between(before, text));
        if (const std::int64_t removed = core::type_text::removed_by(*record.native_type, followed); removed > 0) {
            Json r = report ? *report : Json::object();
            r["type_removed"] = removed;  // (ranges or paragraphs' settings the edit took away, said)
            report = std::move(r);
        }
        if (followed.is_null()) record.native_type.reset();
        else record.native_type = followed;
    }
}

// The marks a text is typed with (ruby_runs, ruby_spans, emphasis_runs, style_runs: add_line's, as the window's typing
// gives them), read as add_line reads them — on a copy of the book — into the record (its text set already).
constexpr std::array<const char*, 4> kMarks{"ruby_runs", "ruby_spans", "emphasis_runs", "style_runs"};
bool has_marks(const Json& op) {
    return std::any_of(kMarks.begin(), kMarks.end(), [&](const char* key) { return core::has(op, key); });
}
void take_marks(StoryLine& record, const Json& op, const Document& doc, const Num& page_index, const core::Actor& actor) {
    Document scratch = doc;
    Json add{{"op", "add_line"}, {"page", page_index.json()}, {"id", "_type_marks"}, {"text", record.text}, {"balloon", "none"}, {"wrap", record.wrap}};
    for (const char* key : kMarks) {
        if (core::has(op, key)) add[key] = op[key];
    }
    OpContext inner{scratch, add, actor};
    (*core::OpRegistry::builtin().find("add_line"))(inner);
    const StoryLine made = core::find_line(scratch, "_type_marks");
    record.ruby_runs = made.ruby_runs;
    record.ruby_spans = made.ruby_spans;
    record.emphasis_runs = made.emphasis_runs;
    record.style_runs = made.style_runs;
}

std::string antialias_of(const Json& v) {
    const auto methods = core::type_text::antialias_methods();
    if (!v.is_string() || std::find(methods.begin(), methods.end(), v.get_ref<const std::string&>()) == methods.end()) {
        std::string names;
        for (const std::string_view m : methods) names += (names.empty() ? "" : ", ") + std::string(m);
        throw OpError("antialias must be one of " + names);
    }
    return v.get<std::string>();
}

// The shape, anchor, anti-alias and matrix edit_type (and add_type_layer) gives, on a record (`layer`: a type layer's —
// a line stays upright and has no matrix).
void reshape(StoryLine& record, const Json& op, const Document& doc, bool layer) {
    const Json* point = given(op, "point");
    const Json* box = given(op, "box");
    if (point != nullptr && box != nullptr) throw OpError("point or box shapes the text, not both");
    if (point != nullptr) {
        if (*point == Json(true)) to_point(record, doc, std::nullopt);
        else to_point(record, doc, point_at(*point, "point"));
    }
    if (box != nullptr) {
        if (*box == Json(true)) to_box(record, doc, std::nullopt);
        else to_box(record, doc, box_at(*box, "box"));
    }
    if (const Json* anchor = given(op, "anchor")) {
        if (text::shape_of(record) != "point") throw OpError("anchor moves a point text's anchor: the text is not point text");
        const std::array<double, 2> a = point_at(*anchor, "anchor");
        Json nt = native_of(record);
        nt["anchor_mm"] = Json::array({a[0], a[1]});
        set_native(record, nt);
    }
    if (const Json* aa = given(op, "antialias")) {
        Json nt = native_of(record);
        nt["antialias"] = antialias_of(*aa);
        set_native(record, nt);
    }
    if (core::has(op, "transform")) {
        if (!layer) throw OpError("a line stays upright: turn, scale or slant its type layer (line_to_type_layer)");
        Json nt = native_of(record);
        if (op["transform"].is_null()) nt.erase("transform");
        else nt["transform"] = op["transform"];
        set_native(record, nt);
    }
}

// --- add_type_layer ------------------------------------------------------------------------------------------------------

void add_type_layer(OpContext& c) {
    Document& doc = c.doc;
    const std::size_t at = core::require_page(doc, c.op);
    const Json* text_given = given(c.op, "text");
    if (text_given == nullptr) throw OpError("text is required");
    const std::string text = text_at(*text_given, "text");
    const Json* point = given(c.op, "point");
    const Json* box = given(c.op, "box");
    if (point == nullptr && box == nullptr) throw OpError("point [x, y] or box [x, y, w, h] (mm) is required");
    if (point != nullptr && box != nullptr) throw OpError("point or box shapes the text, not both");
    StoryLine record;
    record.text = text;
    record.balloon = "none";
    record.wrap = "vertical";
    if (const Json* wrap = given(c.op, "wrap")) {
        if (*wrap != Json("vertical") && *wrap != Json("horizontal")) throw OpError("wrap must be vertical or horizontal");
        record.wrap = wrap->get<std::string>();
    }
    if (const Json* balloon = given(c.op, "balloon")) record.balloon = core::line_balloon(*balloon);  // (as add_line checks it)
    if (has_marks(c.op)) take_marks(record, c.op, doc, doc.page(at).index, c.actor);  // (rubies, emphasis, parts larger or bolder)
    core::ruby::settle(doc, record);  // (rubies by place: their order, and ruby-layout@1 — review P3-3)
    // (a line's, as add_line takes it: a picture in it checked as the drawing build checks one)
    if (const Json* style = given(c.op, "style")) record.style = core::line_style(*style, [](const std::string& bytes) { verify_style_picture(bytes); });
    if (const Json* pt = given(c.op, "size_pt")) {  // (Photoshop's size: points, the record's mm)
        if (core::py_truthy(core::get_or(record.style, "size_mm", Json()))) throw OpError("size_pt or style.size_mm sizes the text, not both");
        const double size = core::finite_float(*pt, "size_pt");
        if (!(size > 0) || size > 1296) throw OpError("size_pt must be above 0 and at most 1296");
        Json style = record.style.is_object() ? record.style : Json::object();
        style["size_mm"] = core::py_round(size * 25.4 / 72.0, 4);
        record.style = core::line_style(style, [](const std::string& bytes) { verify_style_picture(bytes); });
    }
    Json nt = Json::object();
    if (const Json* native = given(c.op, "native_type")) {
        if (!native->is_object()) throw OpError("native_type must be an object");
        nt = *native;
    }
    for (const char* key : {"char", "para", "char_style", "para_style"}) {
        if (const Json* v = given(c.op, key)) nt[key] = *v;
    }
    if (const Json* aa = given(c.op, "antialias")) nt["antialias"] = antialias_of(*aa);
    if (point != nullptr) {
        const std::array<double, 2> p = point_at(*point, "point");
        without_balloon(record);
        nt["shape"] = "point";
        nt["anchor_mm"] = Json::array({p[0], p[1]});
        record.x_mm = Num(p[0]);
        record.y_mm = Num(p[1]);
        record.w_mm = Num(1);
        record.h_mm = Num(1);
    } else {
        // (段落テキスト; with a balloon, the text set in it as a line's — fitted to it, in its box)
        const std::array<double, 4> b = box_at(*box, "box");
        // (letters on a path — style.text_path — follow it, unboxed: no shape — review P3-12)
        if (record.balloon == "none" && !core::truthy_at(record.style, "text_path")) nt["shape"] = "box";
        record.x_mm = Num(b[0]);
        record.y_mm = Num(b[1]);
        record.w_mm = Num(b[2]);
        record.h_mm = Num(b[3]);
    }
    set_native(record, nt);
    Page& page = doc.edit_page(at);
    Layer layer;
    layer.id = core::truthy_at(c.op, "new_id") ? core::py_str(c.op["new_id"]) : core::new_id();
    for (const Layer& l : page.layers) {
        if (l.id == layer.id) throw OpError("layer " + layer.id + " exists");
    }
    layer.role = core::LayerRole::Text;
    layer.kind = core::LayerKind::Type;
    layer.panel_clip = false;  // (as a line: not cut by the panels)
    if (const Json* parent = given(c.op, "parent")) {  // (in a folder of the page)
        const std::string folder = core::py_str(*parent);
        const auto it = std::find_if(page.layers.begin(), page.layers.end(), [&](const Layer& l) { return l.id == folder; });
        if (it == page.layers.end() || it->kind != core::LayerKind::Folder) throw OpError("parent must be a folder of the page");
        layer.parent_id = Json(folder);
    }
    std::size_t index = page.layers.size();
    if (const Json* above = given(c.op, "above")) {  // (just above that layer; none: on top)
        const std::string id = core::py_str(*above);
        const auto it = std::find_if(page.layers.begin(), page.layers.end(), [&](const Layer& l) { return l.id == id; });
        if (it == page.layers.end()) throw OpError("no layer " + id);
        index = static_cast<std::size_t>(it - page.layers.begin()) + 1;
        if (!given(c.op, "parent")) layer.parent_id = it->parent_id;  // (in the folder it is put in)
    }
    layer.title = given(c.op, "title") != nullptr ? core::py_str(c.op["title"]) : title_of(text);
    fit_point(record, doc);
    layer.type = core::type_text::shared_record(std::move(record));
    const std::string id = layer.id;
    page.layers.insert(page.layers.begin() + static_cast<std::ptrdiff_t>(index), std::move(layer));
    core::type_text::add_feature(doc);
    c.report = Json{{"layer_id", id}};
}

// --- edit_type -----------------------------------------------------------------------------------------------------------

void edit_type(OpContext& c) {
    Document& doc = c.doc;
    const Target t = target_of(doc, c.op, true);
    static constexpr std::array<const char*, 11> kChanges{"text", "replace", "insert", "point", "box", "anchor", "wrap", "antialias", "transform", "title", "warp"};
    if (std::none_of(kChanges.begin(), kChanges.end(), [&](const char* key) { return core::has(c.op, key); }) && !has_marks(c.op))
        throw OpError("edit_type changes text, replace, insert, point, box, anchor, wrap, antialias, transform, title or warp");
    if (const Json* wrap = given(c.op, "wrap"); wrap != nullptr && *wrap != Json("vertical") && *wrap != Json("horizontal"))
        throw OpError("wrap must be vertical or horizontal");
    if (t.line != nullptr) {
        if (given(c.op, "title") != nullptr) throw OpError("title names a type layer: a line has none");
        const std::string id = t.line->id;
        std::optional<std::string> text = new_text(t.line->text, c.op);
        if (!text && to_point_in_place(c.op)) text = with_breaks(*t.line, doc);  // (a box text's soft breaks kept as hard ones)
        const Json* wrap = given(c.op, "wrap");
        if (text || wrap != nullptr || has_marks(c.op) || core::has(c.op, "warp")) {  // (a line's text: edit_line's — its rubies and ranges followed, its report)
            const core::OpFunction* edit_line = core::OpRegistry::builtin().find("edit_line");
            Json op{{"op", "edit_line"}, {"id", id}};
            if (text) op["text"] = *text;
            if (wrap != nullptr) op["wrap"] = *wrap;
            if (core::has(c.op, "warp")) op["style"] = Json{{"native_warp", c.op["warp"]}};  // (ワープテキスト: the line's style.native_warp)
            for (const char* key : kMarks) {
                if (core::has(c.op, key)) op[key] = c.op[key];
            }
            OpContext inner{c.doc, op, c.actor};
            (*edit_line)(inner);
            c.report = inner.report;
        }
        StoryLine& line = core::find_line(doc, id);
        StoryLine record = line;
        reshape(record, c.op, doc, false);
        fit_point(record, doc);
        line.native_type = record.native_type;
        line.style = record.style;
        line.x_mm = record.x_mm;
        line.y_mm = record.y_mm;
        line.w_mm = record.w_mm;
        line.h_mm = record.h_mm;
        core::type_text::add_feature(doc);
        return;
    }
    Page& page = doc.edit_page(t.page);
    Layer& layer = page.layers[t.layer];
    StoryLine record = *layer.type;
    const std::string before = record.text;
    std::optional<std::string> text = new_text(before, c.op);
    if (!text && to_point_in_place(c.op)) text = with_breaks(record, doc);  // (a box text's soft breaks kept as hard ones)
    if (text) retext(record, *text, c.report);
    if (const Json* wrap = given(c.op, "wrap")) record.wrap = wrap->get<std::string>();
    if (core::has(c.op, "warp")) {  // (ワープテキスト — review P2-6: the record's style.native_warp, checked as a line's; null: none)
        Json style = record.style.is_object() ? record.style : Json::object();
        if (c.op["warp"].is_null()) style.erase("native_warp");
        else style["native_warp"] = c.op["warp"];
        record.style = style.empty() ? Json::object() : core::line_style(style, [](const std::string& bytes) { verify_style_picture(bytes); });
    }
    if (has_marks(c.op)) take_marks(record, c.op, doc, page.index, c.actor);  // (the marks typed: as add_line reads them)
    core::ruby::settle(doc, record);
    reshape(record, c.op, doc, true);
    fit_point(record, doc);
    if (const Json* title = given(c.op, "title")) {
        layer.title = core::py_str(*title);
    } else if (record.text != before && layer.title == title_of(before)) {
        layer.title = title_of(record.text);  // (named by its text, as Photoshop's: the name follows it until it is given one)
    }
    layer.type = core::type_text::shared_record(std::move(record));
    core::type_text::add_feature(doc);
}

// --- type_info -----------------------------------------------------------------------------------------------------------

// A page point (px at the measuring resolution) in mm, through the record's matrix.
std::array<double, 2> on_page(double x_px, double y_px, const std::optional<std::array<double, 6>>& t) {
    const double x = x_px * kPxMm, y = y_px * kPxMm;
    if (!t) return {x, y};
    const auto& m = *t;
    return {m[0] * x + m[2] * y + m[4], m[1] * x + m[3] * y + m[5]};
}

Json mm_json(double v) { return Json(core::py_round(v, 2)); }

// Where its letters are set (G-a): its outline and rows (across: each row's baseline from its left to its right; down:
// each column's middle from its top to its foot), on the page (mm, through its matrix), whether a box text runs past
// its box, its size; none of it for a text this build cannot set (a face it cannot open yet) or one on a path.
void set_info(Json& out, const StoryLine& record, const Document& doc) {
    const std::optional<std::array<double, 6>> t = core::type_text::transform_of(record);
    try {
        const text::Placement p = text::placement(record, kMeasureDpi, font_of(doc));
        out["size_mm"] = mm_json(static_cast<double>(p.em) * kPxMm);
        if (p.letters) {
            const Box& b = *p.letters;
            double x0 = 1e300, y0 = 1e300, x1 = -1e300, y1 = -1e300;
            for (const auto& [x, y] : {std::pair{b.x0, b.y0}, {b.x1, b.y0}, {b.x0, b.y1}, {b.x1, b.y1}}) {
                const std::array<double, 2> q = on_page(x, y, t);
                x0 = std::min(x0, q[0]);
                y0 = std::min(y0, q[1]);
                x1 = std::max(x1, q[0]);
                y1 = std::max(y1, q[1]);
            }
            out["outline"] = Json::array({mm_json(x0), mm_json(y0), mm_json(x1), mm_json(y1)});
        } else {
            out["outline"] = nullptr;
        }
        out["overflow"] = p.overflow;
        const text::SetLayout set = text::set_layout(record, kMeasureDpi, font_of(doc), text::LetterParts{});
        Json rows = Json::array();
        if (std::find(set.set.effects.begin(), set.set.effects.end(), "text_path") == set.set.effects.end()) {
            const Point at = text::lettering_at(record, kMeasureDpi, set.layout.corner_x, set.layout.corner_y);
            const auto& m = set.set.to_image;
            const auto image = [&](double x, double y) {
                return on_page(at.x + m[0] * x + m[2] * y + m[4], at.y + m[1] * x + m[3] * y + m[5], t);
            };
            for (const text::SetRow& row : set.set.rows) {
                std::string letters;
                for (const text::SetChar& ch : row.chars) letters += text::utf8(ch.text);
                const std::array<double, 2> a = set.set.vertical ? image(row.axis, row.start) : image(row.start, row.axis);
                const std::array<double, 2> b = set.set.vertical ? image(row.axis, row.end) : image(row.end, row.axis);
                rows.push_back(Json{{"text", letters}, {"from", Json::array({mm_json(a[0]), mm_json(a[1])})}, {"to", Json::array({mm_json(b[0]), mm_json(b[1])})}});
            }
        }
        out["lines"] = std::move(rows);
    } catch (const NotYetPorted&) {
        out["outline"] = nullptr;
        out["lines"] = Json::array();
    }
}

Json info_of(const StoryLine& record, const Document& doc) {
    const Json* styles = doc.type_styles ? &*doc.type_styles : nullptr;
    Json out = Json::object();
    out["text"] = record.text;
    out["graphemes"] = core::ruby::count(record.text);
    out["wrap"] = record.wrap;
    out["box"] = Json::array({record.x_mm.json(), record.y_mm.json(), record.w_mm.json(), record.h_mm.json()});
    out["balloon"] = record.balloon;
    const Json* shape = record.native_type ? core::get(*record.native_type, "shape") : nullptr;
    out["shape"] = shape != nullptr ? *shape : Json("line");  // (none: set as a line, in its box fitted to its balloon)
    if (const Json* anchor = record.native_type ? core::get(*record.native_type, "anchor_mm") : nullptr) out["anchor"] = *anchor;
    const Json* antialias = record.native_type ? core::get(*record.native_type, "antialias") : nullptr;
    out["antialias"] = antialias != nullptr ? *antialias : Json("smooth");
    if (const Json* transform = record.native_type ? core::get(*record.native_type, "transform") : nullptr) out["transform"] = *transform;
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
    set_info(out, record, doc);
    return out;
}

void type_info(OpContext& c) {
    const Target t = target_of(c.doc, c.op, false);
    if (t.line != nullptr) {
        Json out = info_of(*t.line, c.doc);
        out["line"] = t.line->id;
        c.report = std::move(out);
        return;
    }
    const Layer& layer = c.doc.page(t.page).layers[t.layer];
    Json out = info_of(*layer.type, c.doc);
    out["layer_id"] = layer.id;
    out["title"] = layer.title;
    c.report = std::move(out);
}

// --- line_to_type_layer, type_layer_to_line ------------------------------------------------------------------------------

// 台詞をテキストレイヤーに: a line's record as a type layer on its page, on top (or just above a layer) — its speaker,
// panel and place in the reading order are the story's and stay with the line (kept: `keep`) or go with it. The type
// layer is drawn among the layers (under the panels' borders); a line under a layer (below_layer) is that by its place.
void line_to_type_layer(OpContext& c) {
    Document& doc = c.doc;
    const Json* line_id = given(c.op, "line");
    if (line_id == nullptr) throw OpError("line (a line's id) names the line");
    const std::string id = core::py_str(*line_id);
    const StoryLine& line = core::find_line(doc, id);
    const auto page_at = std::find_if(doc.pages.begin(), doc.pages.end(), [&](const auto& p) { return p->index == line.page_index; });
    if (page_at == doc.pages.end()) throw OpError("the line " + id + " is on no page");
    const std::size_t at = static_cast<std::size_t>(page_at - doc.pages.begin());
    StoryLine record = core::type_text::record_of(line);
    // (review P3-2: what stays the story's said — in the report, and as a note — so type_layer_to_line can give it back)
    Json dropped = Json::object();
    if (!line.speaker.empty()) dropped["speaker"] = line.speaker;
    if (line.frame_id && !line.frame_id->empty()) dropped["frame_id"] = *line.frame_id;
    {
        const auto page_lines = doc.story_for_page(line.page_index);
        const auto it = std::find_if(page_lines.begin(), page_lines.end(), [&](const StoryLine* l) { return l->id == id; });
        if (it != page_lines.begin() && it != page_lines.end()) dropped["after"] = (*std::prev(it))->id;  // (its place in the reading order)
    }
    if (record.style.is_object() && record.style.contains("below_layer")) dropped["below_layer"] = record.style["below_layer"];
    if (record.style.is_object()) record.style.erase("below_layer");  // (its place among the layers is the layer's own)
    Page& page = doc.edit_page(at);
    Layer layer;
    layer.id = core::truthy_at(c.op, "new_id") ? core::py_str(c.op["new_id"]) : core::new_id();
    for (const Layer& l : page.layers) {
        if (l.id == layer.id) throw OpError("layer " + layer.id + " exists");
    }
    layer.role = core::LayerRole::Text;
    layer.kind = core::LayerKind::Type;
    layer.panel_clip = false;
    layer.title = given(c.op, "title") != nullptr ? core::py_str(c.op["title"]) : title_of(record.text);
    std::size_t index = page.layers.size();
    if (const Json* above = given(c.op, "above")) {
        const std::string other = core::py_str(*above);
        const auto it = std::find_if(page.layers.begin(), page.layers.end(), [&](const Layer& l) { return l.id == other; });
        if (it == page.layers.end()) throw OpError("no layer " + other);
        index = static_cast<std::size_t>(it - page.layers.begin()) + 1;
        layer.parent_id = it->parent_id;
    }
    layer.type = core::type_text::shared_record(std::move(record));
    const std::string made = layer.id;
    page.layers.insert(page.layers.begin() + static_cast<std::ptrdiff_t>(index), std::move(layer));
    if (!core::truthy_at(c.op, "keep")) {
        std::erase_if(doc.story, [&](const StoryLine& l) { return l.id == id; });
    }
    core::type_text::add_feature(doc);
    c.report = Json{{"layer_id", made}};
    if (!dropped.empty()) {
        (*c.report)["dropped"] = dropped;
        c.notes.push_back("the line's speaker, panel, place in the reading order and below_layer are the story's: the type layer " + made +
                          " has none (" + dropped.dump() + "; type_layer_to_line takes speaker, frame_id and after)");
    }
}

// テキストレイヤーを台詞に: a type layer's record as a line of its page, last in the reading order, with no speaker —
// over every layer and the panels' borders, as a line is. A type layer its matrix turns is not a line (a line stays
// upright). The layer goes, or stays (`keep`).
void type_layer_to_line(OpContext& c) {
    Document& doc = c.doc;
    const Target t = target_of(doc, c.op, !core::truthy_at(c.op, "keep"));
    if (t.line != nullptr) throw OpError("page and layer_id name the type layer");
    const Layer& layer = doc.page(t.page).layers[t.layer];
    if (core::type_text::transform_of(*layer.type))
        throw OpError("a type layer turned, scaled or slanted stays a type layer: a line stays upright (clear its transform first)");
    const StoryLine record = *layer.type;
    const std::string layer_id = layer.id;
    const Num index = doc.page(t.page).index;
    std::string id = core::truthy_at(c.op, "new_id") ? core::py_str(c.op["new_id"]) : std::string();
    if (!id.empty()) {
        for (const StoryLine& l : doc.story) {
            if (l.id == id) throw OpError("line " + id + " already exists");
        }
    }
    StoryLine& line = doc.add_line(index, record.text, "", std::nullopt, "", record.x_mm, record.y_mm, record.w_mm, record.h_mm, record.balloon, record.tail);
    if (id.empty()) id = line.id;
    line = record;
    line.id = id;
    line.page_index = index;
    line.speaker.clear();
    line.frame_id.reset();
    // (review P3-2: its speaker, panel and place in the reading order given back — line_to_type_layer's `dropped`)
    if (const Json* speaker = given(c.op, "speaker")) line.speaker = core::py_str(*speaker);
    if (const Json* frame = given(c.op, "frame_id")) {
        const std::string frame_id = core::py_str(*frame);
        if (doc.page(t.page).find_frame(frame_id) == nullptr) throw OpError("no panel " + frame_id + " on the page");
        line.frame_id = frame_id;
    }
    if (const Json* after = given(c.op, "after")) {
        const std::string other = core::py_str(*after);
        const auto from = std::find_if(doc.story.begin(), doc.story.end(), [&](const StoryLine& l) { return l.id == id; });
        const auto to = std::find_if(doc.story.begin(), doc.story.end(), [&](const StoryLine& l) { return l.id == other; });
        if (to == doc.story.end() || !(to->page_index == index)) throw OpError("after names a line of the page");
        StoryLine moved_line = std::move(*from);
        const std::string after_id = to->id;
        doc.story.erase(from);
        const auto put = std::find_if(doc.story.begin(), doc.story.end(), [&](const StoryLine& l) { return l.id == after_id; });
        doc.story.insert(std::next(put), std::move(moved_line));
    }
    StoryLine& made = core::find_line(doc, id);
    core::ruby::settle(doc, made);    if (!core::truthy_at(c.op, "keep")) {
        Page& page = doc.edit_page(t.page);
        std::erase_if(page.layers, [&](const Layer& l) { return l.id == layer_id; });
    }
    core::type_text::add_feature(doc);
    c.report = Json{{"id", id}};
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
    registry.add("line_to_type_layer", line_to_type_layer);
    registry.add("type_layer_to_line", type_layer_to_line);
    registry.add("replace_fonts", later("replace_fonts", "G-d"));
}

}  // namespace genko::render
