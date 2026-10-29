#include "service/briefs.hpp"

#include <QByteArray>
#include <QtGlobal>

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <set>
#include <sstream>

#if !defined(_WIN32)
#include <pwd.h>
#include <unistd.h>
#endif

#include "core/brushes.hpp"
#include "core/frames.hpp"
#include "core/paths.hpp"
#include "core/poses.hpp"
#include "core/pyconv.hpp"
#include "core/pyops.hpp"
#include "core/pyre.hpp"
#include "render/brushes.hpp"
#include "render/draw.hpp"
#include "render/materials.hpp"
#include "render/text/fonts.hpp"
#include "render/text/shaping.hpp"
#include "service/blocking.hpp"
#include "service/lint.hpp"
#include "service/stylecat.hpp"
#include "storage/fsutil.hpp"

namespace genko::service {

namespace fs = std::filesystem;
using core::Num;
using core::py_get;
using core::py_str;
using core::py_truthy;

void PyDict::set(const Json& key, Json value) {
    core::require_hashable(key);
    for (auto& [k, v] : items_) {
        if (core::py_equals(k, key)) {
            v = std::move(value);
            return;
        }
    }
    items_.emplace_back(key, std::move(value));
}

const Json* PyDict::find(const Json& key) const {
    core::require_hashable(key);
    for (const auto& [k, v] : items_) {
        if (core::py_equals(k, key)) return &v;
    }
    return nullptr;
}

namespace briefs {

namespace {

Json rounded(const Num& v, int places) { return core::py_round(v, places).json(); }

// `x or {}` of a value Python then calls .get on (AttributeError for a truthy value that is not a dict).
Json or_dict(const Json& value) {
    if (!py_truthy(value)) return Json::object();
    if (!value.is_object()) core::raise_attribute_error(value, "get");
    return value;
}

Json point_json(const core::Point& p, int places) { return Json::array({rounded(p.x, places), rounded(p.y, places)}); }

// frames._extend(rect, p0, p1)
std::pair<core::Point, core::Point> extend(const core::Rect& rect, const core::Point& p0, const core::Point& p1) {
    const Num dx = p1.x - p0.x;
    const Num dy = p1.y - p0.y;
    if (core::py_abs(dx) >= core::py_abs(dy)) {
        const Num k = dy / (dx.truthy() ? dx : Num(1e-6));
        const Num x0 = rect.x;
        const Num x1 = rect.x + rect.width;
        return {core::Point{x0, p0.y + (x0 - p0.x) * k}, core::Point{x1, p0.y + (x1 - p0.x) * k}};
    }
    const Num k = dx / (dy.truthy() ? dy : Num(1e-6));
    const Num y0 = rect.y;
    const Num y1 = rect.y + rect.height;
    return {core::Point{p0.x + (y0 - p0.y) * k, y0}, core::Point{p0.x + (y1 - p0.y) * k, y1}};
}

// min(a, b) / max(a, b) as Python picks them (the first unless the second is smaller / larger)
Num least(const Num& a, const Num& b) { return b < a ? b : a; }
Num most(const Num& a, const Num& b) { return b > a ? b : a; }

Json gutter(const std::string& node, std::size_t index, const core::Point& p0, const core::Point& p1, const Num& width,
            bool horizontal) {
    return Json::object({{"frame_id", node},
                         {"index", static_cast<std::int64_t>(index)},
                         {"width_mm", rounded(width, 2)},
                         {"direction", horizontal ? "horizontal" : "vertical"},
                         {"from_mm", point_json(p0, 1)},
                         {"to_mm", point_json(p1, 1)}});
}

// frames.gutters(root), each as _gutter_brief gives it
void walk(const core::Frame& node, Json& out) {
    if (node.children.empty()) return;
    if (core::is_free(node)) {
        for (const core::Frame& child : node.children) walk(child, out);
        return;
    }
    const auto line = core::cut_line(node);
    if (line && node.children.size() == 2) {
        const std::vector<core::Point> pts = core::shape(node);
        const auto [a, b] = extend(core::bbox(pts), line->p0, line->p1);
        const bool horizontal = core::py_abs(line->p1.x - line->p0.x) >= core::py_abs(line->p1.y - line->p0.y);
        out.push_back(gutter(node.id, 0, a, b, Num(line->gutter), horizontal));
    } else {
        const bool horizontal = node.split_axis && *node.split_axis == "horizontal";
        std::vector<const core::Frame*> kids;
        for (const core::Frame& child : node.children) kids.push_back(&child);
        std::stable_sort(kids.begin(), kids.end(), [&](const core::Frame* a, const core::Frame* b) {
            return horizontal ? a->rect.y < b->rect.y : a->rect.x < b->rect.x;
        });
        for (std::size_t i = 0; i + 1 < kids.size(); ++i) {
            const core::Rect& ra = kids[i]->rect;
            const core::Rect& rb = kids[i + 1]->rect;
            if (horizontal) {
                const Num g = rb.y - (ra.y + ra.height);
                const Num y = ra.y + ra.height + g / Num(2);
                const Num x0 = least(ra.x, rb.x);
                const Num x1 = most(ra.x + ra.width, rb.x + rb.width);
                out.push_back(gutter(node.id, i, core::Point{x0, y}, core::Point{x1, y}, g, true));
            } else {
                const Num g = rb.x - (ra.x + ra.width);
                const Num x = ra.x + ra.width + g / Num(2);
                const Num y0 = least(ra.y, rb.y);
                const Num y1 = most(ra.y + ra.height, rb.y + rb.height);
                out.push_back(gutter(node.id, i, core::Point{x, y0}, core::Point{x, y1}, g, false));
            }
        }
    }
    for (const core::Frame& child : node.children) walk(child, out);
}

const core::Frame* find_under(const core::Frame& node, const Json& frame_id) {
    if (core::py_equals(Json(node.id), frame_id)) return &node;
    for (const core::Frame& child : node.children) {
        if (const core::Frame* found = find_under(child, frame_id)) return found;
    }
    return nullptr;
}

Json rect_json(const core::Rect& r, int places) {
    return Json::array({rounded(r.x, places), rounded(r.y, places), rounded(r.width, places), rounded(r.height, places)});
}

}  // namespace

const core::Frame& find_frame(const core::Page& page, const Json& frame_id) {
    if (page.frames.empty()) core::raise_index_error();
    if (const core::Frame* found = find_under(page.frames.front(), frame_id)) return *found;
    throw core::PyUncaught("KeyError", core::py_repr(frame_id));
}

Json gutter_brief(const core::Page& page) {
    Json out = Json::array();
    if (page.frames.empty()) return out;
    walk(page.frames.front(), out);
    return out;
}

Json candidate_brief(const Json& cand) {
    const Json review = or_dict(py_get(cand, "review"));
    const Json upscaled = or_dict(py_get(cand, "upscaled"));
    const Json metrics = or_dict(py_get(cand, "metrics"));
    const Json origin = or_dict(py_get(cand, "origin"));
    const std::pair<const char*, Json> fields[] = {
        {"id", py_get(cand, "id")},
        {"status", py_get(cand, "status")},
        {"px", py_get(cand, "px")},
        {"parent", py_get(cand, "parent")},
        {"mode", py_get(cand, "mode")},
        {"upscaled", py_get(upscaled, "scale")},
        {"rank", py_get(metrics, "rank")},
        {"score", py_get(review, "score")},
        {"origin", core::py_or(py_get(origin, "tool_id"), py_get(or_dict(py_get(cand, "origin")), "kind"))},
    };
    Json out = Json::object();
    for (const auto& [key, value] : fields) {
        if (!value.is_null()) out[key] = value;
    }
    return out;
}

Json panel_brief(const core::Document& doc, const Json& page_index, const Json& frame_id, bool full) {
    const auto at = page_at(doc, page_index);
    if (!at) throw core::PyUncaught("StopIteration", "");
    const core::Page& page = doc.page(*at);
    std::vector<const core::Frame*> frames;
    if (py_truthy(frame_id)) {
        frames.push_back(&find_frame(page, frame_id));
    } else {
        frames = page.leaf_frames();
    }
    PyDict chars;
    for (const Json& c : core::iterate(doc.bible.characters)) chars.set(py_get(c, "id"), c);
    Json out = Json::array();
    for (const core::Frame* frame : frames) {
        const Json panel = frame->panel ? *frame->panel : Json::object();
        Json cast = Json::array();
        for (const Json& c : core::iterate(py_get(panel, "characters", Json::array()))) {
            if (c.is_object()) cast.push_back(py_get(c, "id"));
        }
        Json shown;
        if (full) {
            shown = panel;
        } else {
            shown = panel;
            Json briefs = Json::array();
            for (const Json& c : core::iterate(py_get(panel, "candidates", Json::array()))) briefs.push_back(candidate_brief(c));
            shown["candidates"] = std::move(briefs);
        }
        Json figures = Json::array();
        const NumBox rect{frame->rect.x, frame->rect.y, frame->rect.width, frame->rect.height};
        for (const Figure& f : service::figures(panel, rect)) {
            figures.push_back(Json::object({{"char", f.char_id}, {"head_mm", box_json(f.head, 2)}, {"body_mm", box_json(f.body, 2)}}));
        }
        Json refs = Json::object();
        PyDict shown_refs;
        for (const Json& cid : cast) {
            if (const Json* c = chars.find(cid)) shown_refs.set(cid, py_get(*c, "refs", Json::array()));
        }
        for (const auto& [cid, value] : shown_refs.items()) refs[py_str(cid)] = value;  // (json.dumps writes the keys as str)
        Json lines = Json::array();
        for (const core::StoryLine* line : doc.story_for_page(page.index)) {
            if (line->frame_id && *line->frame_id == frame->id) lines.push_back(line->text);
        }
        out.push_back(Json::object({{"frame_id", frame->id},
                                    {"rect_mm", rect_json(frame->rect, 2)},
                                    {"bleed", frame->bleed},
                                    {"panel", std::move(shown)},
                                    {"figures", std::move(figures)},
                                    {"character_refs", std::move(refs)},
                                    {"lines", std::move(lines)}}));
    }
    return Json::object({{"page", page_index}, {"name_ok", page.name_ok}, {"art_ok", page.art_ok}, {"panels", std::move(out)}});
}

std::string brief(const Json& plan, const Json& bible) {
    PyDict names;
    for (const Json& c : core::iterate(py_get(bible, "characters", Json::array()))) names.set(py_get(c, "id"), py_get(c, "name", Json("")));
    std::string out;
    bool first = true;
    for (const Json& panel : core::iterate(py_get(plan, "panels", Json::array()))) {
        std::string who;
        bool first_who = true;
        for (const Json& c : core::iterate(py_get(panel, "characters", Json::array()))) {
            const Json id = py_get(c, "id");
            const Json* name = names.find(id);
            const Json shown = name != nullptr ? *name : py_get(c, "id", Json(""));
            if (!shown.is_string()) {
                throw core::PyTypeError("sequence item " + std::to_string(first_who ? 0 : 1) + ": expected str instance, " +
                                        core::py_type_name(shown) + " found");
            }
            who += (first_who ? "" : "・") + shown.get<std::string>();
            first_who = false;
        }
        const std::string part = py_str(py_get(panel, "slot")) + ": " + py_str(py_get(panel, "shot")) + "/" +
                                 py_str(py_get(panel, "angle")) + " " + who + " " + py_str(py_get(panel, "action", Json("")));
        out += (first ? "" : " | ") + core::py_strip(part);
        first = false;
    }
    return out;
}

Json catalog(const core::Document& doc) {
    const Json style = or_dict(py_get(doc.studio, "style"));
    const Json kept = py_get(style, "catalog");
    if (kept.is_object() && py_truthy(py_get(or_dict(py_get(kept, "prompt")), "ja"))) return kept;
    return Json();
}

bool catalog_fits(const Json& kept, const std::string& expression) {
    return !(core::py_equals(py_get(kept, "expression", Json("mono")), Json("mono")) && expression == "color");
}

Json style_brief(const core::Document& doc) {
    const Json kept = catalog(doc);
    if (kept.is_null()) return Json();
    return Json::object({{"id", py_get(kept, "id")},
                         {"title", py_get(kept, "title")},
                         {"path", py_get(kept, "path")},
                         {"version", py_get(kept, "version")},
                         {"expression", py_get(kept, "expression")},
                         {"has_sample", py_truthy(py_get(kept, "sample"))},
                         {"fits", catalog_fits(kept, doc.spec.expression)}});
}

Json catalog_node(const Json& data) {
    Json children = Json::array();
    for (const Json& c : core::iterate(core::py_or(py_get(data, "children"), Json::array()))) {
        if (c.is_object()) children.push_back(Json::object({{"id", py_get(c, "id")}, {"title", py_get(c, "title")}, {"summary", py_get(c, "summary")}}));
    }
    return Json::object({{"id", py_get(data, "id")},
                         {"title", py_get(data, "title")},
                         {"summary", py_get(data, "summary")},
                         {"level", py_get(data, "level")},
                         {"path", stylecat::full_path(data)},
                         {"expression", py_get(data, "expression")},
                         {"version", py_get(data, "version")},
                         {"prompt_ja", py_get(or_dict(py_get(data, "prompt")), "ja")},
                         {"children", std::move(children)}});
}

std::pair<Json, std::vector<Issue>> keep_approved(const Json& current, const Json& incoming) {
    PyDict by_id;
    for (const Json& c : core::iterate(current)) by_id.set(py_get(c, "id"), c);
    Json out = Json::array();
    for (const Json& ch : core::iterate(incoming)) {
        const Json* old = by_id.find(py_get(ch, "id"));
        Json merged = core::py_dict(ch);
        if (old != nullptr && py_truthy(*old)) {
            for (const char* key : {"refs", "locked"}) {
                if (old->contains(key)) merged[key] = (*old)[key];
            }
        }
        out.push_back(std::move(merged));
    }
    std::vector<Json> kept;
    for (const Json& c : core::iterate(incoming)) {
        const Json id = py_get(c, "id");
        core::require_hashable(id);
        kept.push_back(id);
    }
    std::vector<Issue> issues;
    for (const auto& [cid, old] : by_id.items()) {
        if (!core::truthy_at(old, "locked")) continue;
        bool there = false;
        for (const Json& k : kept) there = there || core::py_equals(k, cid);
        if (!there) issues.push_back(error("locked_character", "/characters", py_str(cid) + " の設定画は承認済み。消すには人間が承認を取り消す"));
    }
    return {std::move(out), std::move(issues)};
}

std::vector<Issue> props_by_id(Json& plan, const Json& bible) {
    PyDict by_name;
    std::vector<Json> ids;
    const Json props = core::py_or(py_get(bible, "props"), Json::array());
    for (const Json& p : core::iterate(props)) {
        if (p.is_object() && core::truthy_at(p, "name")) by_name.set(py_str(p["name"]), py_get(p, "id"));
    }
    for (const Json& p : core::iterate(props)) {
        if (!p.is_object()) continue;
        const Json id = py_get(p, "id");
        core::require_hashable(id);
        ids.push_back(id);
    }
    std::vector<Issue> out;
    if (!plan.is_object()) core::raise_attribute_error(plan, "get");
    if (!plan.contains("panels") || !py_truthy(plan["panels"])) return out;
    Json& panels = plan["panels"];
    if (!panels.is_array()) {
        (void)core::iterate(panels);  // (Python iterates what it was given: TypeError for a value that is not iterable)
        return out;
    }
    for (std::size_t index = 0; index < panels.size(); ++index) {
        Json& panel = panels[index];
        if (!panel.is_object() || !panel.contains("props") || !panel["props"].is_array()) continue;
        Json& items = panel["props"];
        for (std::size_t n = 0; n < items.size(); ++n) {
            const Json item = items[n];
            core::require_hashable(item);
            bool known = false;
            for (const Json& id : ids) known = known || core::py_equals(id, item);
            if (known) continue;
            const Json* found = by_name.find(Json(py_str(item)));
            if (found == nullptr) continue;
            const Json id = *found;
            items[n] = id;
            out.push_back(warning("prop_by_name", "/panels/" + std::to_string(index) + "/props/" + std::to_string(n),
                                  "小物「" + py_str(item) + "」を id " + py_str(id) + " として読んだ", "props は企画書の id で書く"));
        }
    }
    return out;
}

namespace {

std::string word_safe(const std::string& name) {
    static const core::PyPattern other("[^\\w.-]", 0);
    core::PyReBudget budget;
    return other.subn("_", name, nullptr, budget).first;
}

std::string login_name() {
    for (const char* key : {"LOGNAME", "USER", "LNAME", "USERNAME"}) {  // (getpass.getuser's order)
        const QByteArray value = qgetenv(key);
        if (!value.isEmpty()) return value.toStdString();
    }
#if !defined(_WIN32)
    if (const passwd* entry = getpwuid(getuid()); entry != nullptr && entry->pw_name != nullptr) return entry->pw_name;
#endif
    return "user";
}

}  // namespace

std::string default_actor() {
    std::string name = qgetenv("GENKO_USER").toStdString();
    if (name.empty()) name = login_name();
    name = word_safe(name);
    if (name.empty()) name = "user";
    return name.rfind("human:", 0) == 0 ? name : "human:" + name;
}

std::string chat_person(const core::Document& doc, const Json& name) {
    if (py_truthy(name) && !core::py_strip(py_str(name)).empty()) {
        std::string text = core::py_strip(py_str(name));
        if (text.rfind("human:", 0) == 0) text = text.substr(6);
        std::string clean = word_safe(text);
        if (clean.empty()) clean = "user";
        return "human:" + clean;
    }
    const Json setting = py_get(doc.studio, "chat_approval");
    if (setting.is_object() && py_str(py_get(setting, "by", Json(""))).rfind("human:", 0) == 0) return py_str(setting["by"]);
    const std::vector<Json> records = core::iterate(core::py_or(py_get(doc.studio, "approvals"), Json::array()));
    for (auto it = records.rbegin(); it != records.rend(); ++it) {
        if (py_str(core::py_get(*it, "by", Json(""))).rfind("human:", 0) == 0) return py_str(core::subscript(*it, "by"));
    }
    return default_actor();
}

Json materials_list() {
    Json out = Json::array();
    for (const Json& item : render::materials::all_materials(core::poses::config_dir())) {
        Json entry = Json::object({{"id", py_get(item, "id")},
                                   {"name", py_get(item, "name")},
                                   {"kind", py_get(item, "kind")},
                                   {"folder", py_get(item, "folder", Json(""))}});
        if (core::truthy_at(item, "tone")) entry["tone"] = item["tone"];
        if (core::truthy_at(item, "effect")) entry["effect"] = item["effect"];
        if (core::truthy_at(item, "tags")) entry["tags"] = core::py_list(item["tags"]);
        if (core::py_equals(py_get(item, "kind"), Json("lettering"))) entry["text"] = py_get(item, "text");
        out.push_back(std::move(entry));
    }
    return out;
}

namespace {

// Path parts as Python's sorted(Path, …) compares them (part by part).
std::vector<std::string> parts_of(const fs::path& path) {
    std::vector<std::string> out;
    for (const fs::path& part : path) out.push_back(core::path_to_utf8(part));
    return out;
}

std::vector<fs::path> font_dirs() {
    std::vector<fs::path> dirs;
#if defined(_WIN32)
    const QByteArray windir = qgetenv("WINDIR");
    dirs.push_back(fs::path(windir.isEmpty() ? std::string("C:\\Windows") : windir.toStdString()) / "Fonts");
    dirs.push_back(fs::path(qgetenv("LOCALAPPDATA").toStdString()) / "Microsoft" / "Windows" / "Fonts");
#elif defined(__APPLE__)
    const fs::path home = qgetenv("HOME").toStdString();
    dirs = {"/System/Library/Fonts", "/Library/Fonts", home / "Library" / "Fonts"};
#else
    const fs::path home = qgetenv("HOME").toStdString();
    dirs = {"/usr/share/fonts", "/usr/local/share/fonts", home / ".fonts", home / ".local" / "share" / "fonts"};
#endif
    std::vector<fs::path> out;
    std::error_code ec;
    for (const fs::path& d : dirs) {
        if (fs::is_directory(d, ec)) out.push_back(d);
    }
    return out;
}

std::string lower_ascii(std::string text) {
    for (char& c : text) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return text;
}

}  // namespace

Json system_fonts() {
    const fs::path cache = core::poses::config_dir() / "fonts.json";
    try {
        std::ifstream in(cache, std::ios::binary);
        if (in) {
            std::ostringstream text;
            text << in.rdbuf();
            core::ParseOptions options;
            options.universal_newlines = true;
            return core::parse_python_json(text.str(), nullptr, options);
        }
    } catch (const core::Error&) {  // (not read: looked for again)
    }
    Json found = Json::array();
    for (const fs::path& folder : font_dirs()) {
        std::vector<fs::path> paths;
        std::error_code ec;
        for (fs::recursive_directory_iterator it(folder, fs::directory_options::skip_permission_denied, ec), end; it != end && !ec;
             it.increment(ec)) {
            paths.push_back(it->path());
        }
        std::sort(paths.begin(), paths.end(), [](const fs::path& a, const fs::path& b) { return parts_of(a) < parts_of(b); });
        for (const fs::path& path : paths) {
            const std::string suffix = lower_ascii(path.extension().string());
            if (suffix != ".ttf" && suffix != ".otf" && suffix != ".ttc") continue;
            try {
                render::TrueTypeFonts fonts;  // (one font at a time: its bytes are let go before the next)
                const render::TrueTypeFont& font = fonts.truetype(core::path_to_utf8(path), 24);
                if (!font.getmask2(U"あ漢").first.getbbox()) continue;
                const auto [name, style] = font.getname();
                if ((name && core::utf8_error(*name)) || (style && core::utf8_error(*style))) continue;
                found.push_back(Json::object({{"name", name ? Json(*name) : Json()},
                                              {"style", style ? Json(*style) : Json()},
                                              {"path", core::path_to_utf8(path)}}));
            } catch (const std::exception&) {
                continue;
            }
        }
    }
    try {
        fs::create_directories(cache.parent_path());
        std::ofstream out(cache, std::ios::binary | std::ios::trunc);
        core::DumpOptions options;
        options.indent = 1;
        options.item_separator = ",";
        out << core::dump(found, options);
    } catch (const std::exception&) {
    }
    return found;
}

namespace {

// The font files of the computer's font folders (.ttf, .otf, .ttc), in the order of their names, and their signature:
// each one's path, size and time written — a font put in, taken away or replaced changes it (review P3-6).
struct FontFiles {
    std::vector<fs::path> paths;
    std::string signature;
};
FontFiles font_files() {
    FontFiles out;
    for (const fs::path& folder : font_dirs()) {
        std::vector<fs::path> paths;
        std::error_code ec;
        for (fs::recursive_directory_iterator it(folder, fs::directory_options::skip_permission_denied, ec), end; it != end && !ec;
             it.increment(ec)) {
            const std::string suffix = lower_ascii(it->path().extension().string());
            if (suffix == ".ttf" || suffix == ".otf" || suffix == ".ttc") paths.push_back(it->path());
        }
        std::sort(paths.begin(), paths.end(), [](const fs::path& a, const fs::path& b) { return parts_of(a) < parts_of(b); });
        out.paths.insert(out.paths.end(), paths.begin(), paths.end());
    }
    std::uint64_t hash = 1469598103934665603ULL;  // (FNV-1a over the files' paths, sizes and times)
    const auto mix = [&](std::string_view bytes) {
        for (const char c : bytes) {
            hash ^= static_cast<unsigned char>(c);
            hash *= 1099511628211ULL;
        }
    };
    for (const fs::path& path : out.paths) {
        std::error_code size_ec, time_ec;
        const auto size = fs::file_size(path, size_ec);
        const auto when = fs::last_write_time(path, time_ec);
        mix(core::path_to_utf8(path));
        mix("|" + std::to_string(size_ec ? 0 : size) + "|" + std::to_string(time_ec ? 0 : when.time_since_epoch().count()) + "\n");
    }
    char hex[17] = {};
    std::snprintf(hex, sizeof hex, "%016llx", static_cast<unsigned long long>(hash));
    out.signature = std::to_string(out.paths.size()) + ":" + hex;
    return out;
}

// A list of families as type_fonts keeps it: [{family: str, styles: [{path: str, …}]}] (anything else: looked for again).
bool families_shape(const Json& v) {
    if (!v.is_array()) return false;
    for (const Json& family : v) {
        if (!family.is_object() || !py_get(family, "family").is_string() || !py_get(family, "styles").is_array()) return false;
        for (const Json& face : family["styles"]) {
            if (!face.is_object() || !py_get(face, "path").is_string()) return false;
        }
    }
    return true;
}

}  // namespace

Json type_fonts() {
    const fs::path cache = core::poses::config_dir() / "type_fonts.json";
    const FontFiles files = font_files();
    const std::string key = core::path_to_utf8(cache) + "\n" + files.signature;
    // (the list made in this run, while the fonts and the config folder are the same: a config folder that cannot be
    // written is not looked through again and again — review P3-6)
    static std::mutex memo_mutex;
    static std::string memo_signature;
    static Json memo;
    {
        const std::lock_guard<std::mutex> lock(memo_mutex);
        if (!memo_signature.empty() && memo_signature == key) return memo;
    }
    try {
        std::ifstream in(cache, std::ios::binary);
        if (in) {
            std::ostringstream text;
            text << in.rdbuf();
            core::ParseOptions options;
            options.universal_newlines = true;
            const Json kept = core::parse_python_json(text.str(), nullptr, options);
            // (the list kept with the fonts' signature, in its shape: else looked for again)
            if (kept.is_object() && py_get(kept, "signature") == Json(files.signature) && families_shape(py_get(kept, "families"))) {
                const std::lock_guard<std::mutex> lock(memo_mutex);
                memo_signature = key;
                memo = kept["families"];
                return memo;
            }
        }
    } catch (const core::Error&) {  // (not read: looked for again)
    }
    std::set<std::string> japanese;
    try {
        for (const Json& f : core::iterate(system_fonts())) {
            if (py_get(f, "path").is_string()) japanese.insert(py_get(f, "path").get<std::string>());
        }
    } catch (const std::exception&) {
    }
    std::map<std::string, Json> families;  // (by family, in the order of their names)
    for (const fs::path& path : files.paths) {
        const std::string file = core::path_to_utf8(path);
        // (each face of a collection — a .ttc's second and later too, with its index: review P3-8 — read for the list
        // alone, not kept with the faces shaping holds: review P3-7)
        unsigned faces = 1;
        for (unsigned index = 0; index < faces && index < 64; ++index) {
            try {
                const render::text::FontInfo info = render::text::font_info(file, index, false);
                if (index == 0) faces = info.faces;
                if (info.family.empty() || core::utf8_error(info.family) || core::utf8_error(info.style) || core::utf8_error(info.postscript)) continue;
                Json face = Json::object({{"style", info.style.empty() ? Json() : Json(info.style)},
                                          {"postscript", info.postscript.empty() ? Json() : Json(info.postscript)},
                                          {"path", file},
                                          {"japanese", japanese.contains(file)}});
                if (!info.axes.empty()) {
                    Json axes = Json::array();
                    for (const render::text::FontAxis& a : info.axes) {
                        axes.push_back(Json::object({{"tag", a.tag}, {"name", core::utf8_error(a.name) ? Json() : Json(a.name)}, {"min", a.min},
                                                     {"default", a.value}, {"max", a.max}}));
                    }
                    face["axes"] = std::move(axes);
                }
                if (info.faces > 1) {
                    face["faces"] = info.faces;
                    face["index"] = index;
                }
                Json& family = families[info.family];
                if (family.is_null()) family = Json::object({{"family", info.family}, {"styles", Json::array()}});
                family["styles"].push_back(std::move(face));
            } catch (const std::exception&) {
                if (index == 0) break;
            }
        }
    }
    Json found = Json::array();
    for (auto& [name, family] : families) found.push_back(std::move(family));
    try {  // (written whole or not at all: a temporary file renamed over the last — review P3-6)
        fs::create_directories(cache.parent_path());
        core::DumpOptions options;
        options.indent = 1;
        options.item_separator = ",";
        storage::write_atomic(cache, core::dump(Json::object({{"signature", files.signature}, {"families", found}}), options));
    } catch (const std::exception&) {
    }
    const std::lock_guard<std::mutex> lock(memo_mutex);
    memo_signature = key;
    memo = found;
    return found;
}

Json fonts_list() {
    Json bundled = Json::array();
    for (const auto& [key, label] : lint::bundled_fonts()) bundled.push_back(Json::object({{"key", key}, {"label", label}}));
    Json system = Json::array();
    try {
        for (const Json& f : core::iterate(system_fonts())) {
            system.push_back(Json::object({{"name", py_get(f, "name")}, {"style", py_get(f, "style")}, {"path", py_get(f, "path")}}));
        }
    } catch (const std::exception&) {  // (an unreadable fonts folder is not the agent's problem)
        system = Json::array();
    }
    Json families = Json::array();  // (PC5 G-b, type.fonts: every font of the computer by family — what the 文字 panel lists)
    try {
        families = type_fonts();
    } catch (const std::exception&) {
        families = Json::array();
    }
    return Json::object({{"bundled", std::move(bundled)},
                         {"system", std::move(system)},
                         {"families", std::move(families)},
                         {"note", "style.font takes a bundled key or a system font's path; style.weight normal|bold|heavy thickens any face"}});
}

Json brushes_list(const core::Document& doc) {
    Json builtin = Json::array();
    for (const core::Brush& b : core::builtin_brushes()) {
        builtin.push_back(Json::object({{"key", b.key},
                                        {"label", b.label},
                                        {"width_mm", b.width_mm},
                                        {"texture", b.texture.empty() ? Json() : Json(b.texture)},
                                        {"fixed_width", b.fixed_width},
                                        {"opacity", b.opacity}}));
    }
    const auto own = [](const Json& brushes) {
        Json out = Json::array();
        for (const auto& [key, d] : brushes.items()) {
            Json entry = Json::object({{"key", key}});
            if (d.is_object()) {
                for (const auto& [kk, v] : d.items()) {
                    // (PC4 E1: a dab brush's engine too — only those have one)
                    // (PC4 E2, review P3-16: the library's group too — a book's brushes have none)
                    if (kk == "label" || kk == "base" || kk == "width_mm" || kk == "texture" || kk == "engine" || kk == "group") entry[kk] = v;
                }
            }
            out.push_back(std::move(entry));
        }
        return out;
    };
    Json library = Json::object();
    try {
        library = render::brushes::load_library(core::poses::config_dir());
    } catch (const std::exception&) {
        library = Json::object();
    }
    return Json::object({{"builtin", std::move(builtin)},
                         {"book", own(doc.brush_custom.is_object() ? doc.brush_custom : Json::object())},
                         {"library", own(library.is_object() ? library : Json::object())}});
}

}  // namespace briefs
}  // namespace genko::service
