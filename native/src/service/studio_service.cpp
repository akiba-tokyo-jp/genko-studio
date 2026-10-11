#include "service/studio_service.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <system_error>

#include "core/command_bus.hpp"
#include "core/error.hpp"
#include "core/ops_schema.hpp"
#include "core/paths.hpp"
#include "core/pyconv.hpp"
#include "core/pyops.hpp"
#include "render/plugins.hpp"
#include "service/book.hpp"
#include "service/briefs.hpp"
#include "service/data.hpp"
#include "service/layout.hpp"
#include "service/service_util.hpp"
#include "service/stylecat.hpp"
#include "service/text.hpp"
#include "service/upscale.hpp"
#include "service/worklist.hpp"
#include "storage/asset_store.hpp"
#include "storage/reader.hpp"
#include "storage/snapshot.hpp"
#include "storage/transaction.hpp"
#include "studio/files.hpp"
#include "studio/schemas.hpp"
#include "studio/state.hpp"

namespace genko::service {

using core::py_get;
using core::py_str;
using core::py_truthy;

namespace {

constexpr std::string_view kAgentOps[] = {
    "add_anim_folder", "add_cel", "add_cover", "add_effect", "add_figure", "add_frame", "add_hand", "add_head", "add_layer",
    "add_line", "add_mannequin", "add_page", "add_prim3d", "add_region", "add_ruler", "add_scene", "add_shape", "add_stroke",
    "add_tone", "adopt_candidate", "ask_human", "attach_reference", "bind_ref", "camera_from_ruler", "close_request",
    "convert_layer", "cut_balloon", "cut_frame", "define_brush", "delete_area", "delete_effect", "delete_frame",
    "delete_layer", "delete_line", "delete_location", "delete_page", "delete_prim", "delete_prop", "delete_region",
    "delete_ruler", "delete_stroke", "delete_tone", "duplicate_layer", "duplicate_page", "edit_effect", "edit_line",
    "edit_prim", "edit_region", "edit_ruler", "edit_stroke", "effect_to_layer", "erase", "erase_raster", "fill", "fill_area",
    "fill_enclosed", "fill_gaps", "filter_raster", "flood_fill", "for_pages", "forget_area", "gradient_fill", "group_layers",
    "import_candidates", "import_model", "import_pages", "import_psd", "liquify", "merge_down", "merge_frame", "merge_layers",
    "merge_visible", "move_gutter", "move_layers", "move_line", "open_request", "paint_mask", "paste", "place_asset",
    "pose_figure", "pose_mannequin", "propose", "record_review", "register_assets", "render_prims", "reorder",
    "reorder_layers", "reorder_lines", "replace_regions", "replace_text", "request_approval", "request_fix", "reshape_stroke",
    "resize_frame", "resolve_ticket", "review_candidates", "ruler_from_3d", "ruler_to_layer", "set_animation", "set_assignee",
    "set_balloon_path", "set_camera", "set_camera_key", "set_candidate", "set_exposure", "set_exposures", "set_finish",
    "set_frame", "set_layer", "set_layer_mask", "set_layers", "set_light", "set_light_table", "set_nombre", "set_note",
    "set_onion", "set_page_plan", "set_page_spec", "set_panel", "set_paper", "set_placement", "set_ruler", "set_spread",
    "set_stroke_width", "set_studio", "set_style_catalog", "set_timelapse", "set_tone", "simplify_stroke", "smudge",
    "split_frame", "stamp_material", "step_onion", "store_area", "trace_edit", "trace_prims", "transform_area", "unadopt",
    "unbind_ref", "upsert_character", "upsert_location", "upsert_prop", "vector_edit", "withdraw_candidates"};

constexpr std::string_view kAgentTools[] = {
    "adopt", "analyze_name", "apply_ops", "ask_human", "candidates", "check", "derive", "export", "export_proof",
    "export_status", "finish_page", "generation_request", "import_image", "import_images", "import_name", "inspect", "next",
    "preflight", "proposals", "propose_lines", "record_chat_approval", "record_review", "render", "report_regions",
    "request_approval", "request_fix", "resolve_ticket", "review_candidates", "review_page", "set_bible", "set_script",
    "status", "style_catalog", "submit_name", "take_panel_art", "tickets", "undo", "upscale", "use_style"};

const std::pair<std::string_view, core::PageSpec (*)()> kSpecPresets[] = {
    {"commercial-b4", &core::PageSpec::b4_comic},
    {"doujin-b5", &core::PageSpec::b5_doujin},
    {"doujin-a5", &core::PageSpec::a5_doujin},
    {"a4-mono", &core::PageSpec::a4_mono},
    {"webtoon", &core::PageSpec::webtoon},
};

double now_seconds() {
    return std::chrono::duration<double>(std::chrono::system_clock::now().time_since_epoch()).count();
}

// Path.resolve() (os.path.realpath: the links followed as far as the path is there, the rest taken as written).
fs::path resolved(const fs::path& path) {
    std::error_code ec;
    fs::path out = fs::weakly_canonical(path, ec);
    if (ec) out = path.lexically_normal();
    return out;
}

// `root in path.parents`: root is an ancestor of path (path itself is not its own parent).
bool strictly_under(const fs::path& path, const fs::path& root) {
    auto p = path.begin();
    for (auto r = root.begin(); r != root.end(); ++r, ++p) {
        if (r->empty() && std::next(r) == root.end()) break;  // (a trailing separator)
        if (p == path.end() || *p != *r) return false;
    }
    for (; p != path.end(); ++p) {
        if (!p->empty()) return true;
    }
    return false;
}

bool is_file(const fs::path& path) {
    std::error_code ec;
    return fs::is_regular_file(path, ec);
}

// Path parts, as sorted(paths) compares them.
std::vector<std::string> parts_of(const fs::path& path) {
    std::vector<std::string> out;
    for (const fs::path& part : path) out.push_back(core::path_to_utf8(part));
    return out;
}

// x[:limit] of the items (limit an int or None; a negative one from the end).
Json head_of(const Json& items, const Json& limit) {
    if (limit.is_null()) return items;
    if (!limit.is_number_integer() && !limit.is_boolean()) {
        throw core::PyTypeError("slice indices must be integers or None or have an __index__ method");
    }
    const std::int64_t n = limit.is_boolean() ? (limit.get<bool>() ? 1 : 0) : limit.get<std::int64_t>();
    return core::py_slice(items, 0, n);
}

}  // namespace

bool agent_op(std::string_view op) {
    return std::binary_search(std::begin(kAgentOps), std::end(kAgentOps), op);
}

const std::vector<std::string>& agent_ops_sorted() {
    static const std::vector<std::string> out(std::begin(kAgentOps), std::end(kAgentOps));
    return out;
}

const std::vector<std::string>& agent_tools_sorted() {
    static const std::vector<std::string> out(std::begin(kAgentTools), std::end(kAgentTools));
    return out;
}

bool agent_tool(std::string_view tool) { return std::binary_search(std::begin(kAgentTools), std::end(kAgentTools), tool); }

std::span<const std::pair<std::string_view, core::PageSpec (*)()>> spec_presets() { return kSpecPresets; }

StudioService::StudioService(fs::path root, std::string actor) : root_(resolved(fs::absolute(root))), actor_(std::move(actor)) {}

// --- projects ------------------------------------------------------------------------------------------------------------

fs::path StudioService::project_path(const Json& name, bool must_exist) const {
    if (!name.is_string()) throw core::PyTypeError("expected str, bytes or os.PathLike object, not " + core::py_type_name(name));
    const std::string text = name.get<std::string>();
    const fs::path rel = core::path_from_utf8(text);
    bool up = false;
    for (const fs::path& part : rel) up = up || part == "..";
    if (rel.is_absolute() || up || core::py_strip(text).empty()) throw core::ApplyError("project は --root からの相対名で指定する: " + text);
    const fs::path path = resolved(root_ / rel);
    if (!strictly_under(path, root_)) throw core::ApplyError("project が --root の外を指している: " + text);
    if (must_exist && !is_file(path / "project.json")) throw core::ApplyError("project が見つからない: " + text);
    return path;
}

ToolResult StudioService::projects(const Json&) {
    std::vector<fs::path> files;
    std::error_code ec;
    for (fs::recursive_directory_iterator it(root_, fs::directory_options::skip_permission_denied, ec), end; !ec && it != end;
         it.increment(ec)) {
        if (it->path().filename() == "project.json") files.push_back(it->path());
    }
    std::sort(files.begin(), files.end(), [](const fs::path& a, const fs::path& b) { return parts_of(a) < parts_of(b); });
    Json found = Json::array();
    for (const fs::path& file : files) {
        core::Document doc;
        try {
            doc = load_episode(file.parent_path());
        } catch (const std::exception&) {
            continue;
        }
        found.push_back(Json::object({{"name", core::path_to_utf8(file.parent_path().lexically_relative(root_))},
                                      {"title", doc.title},
                                      {"pages", static_cast<std::int64_t>(doc.pages.size())}}));
    }
    return ToolResult(true, Json::object({{"projects", std::move(found)}}));
}

ToolResult StudioService::create_project(const Json& a) {
    const Json& name = a["name"];
    const fs::path path = project_path(name, false);
    std::error_code ec;
    if (fs::exists(path / "project.json", ec)) return fail("すでにある: " + py_str(name), "project_exists");
    const Json& preset = a["spec_preset"];
    core::require_hashable(preset);
    core::PageSpec (*make)() = nullptr;
    std::string names;
    for (const auto& [key, maker] : kSpecPresets) {
        names += (names.empty() ? "" : ", ") + std::string(key);
        if (preset.is_string() && preset.get<std::string>() == key) make = maker;
    }
    if (make == nullptr) return fail("判型 " + py_str(preset) + " はない（" + names + "）", "unknown_preset", "/spec_preset");
    const Json& pages = a["pages"];
    if (!(core::py_less(Json(1), pages, "<=") && core::py_less(pages, Json(400), "<="))) {
        return fail("pages は 1〜400", "pages_out_of_range", "/pages");
    }
    const core::PageSpec spec = make();
    const Json& binding_given = a["binding"];
    const auto binding = binding_given.is_string() ? core::binding_from(binding_given.get<std::string>()) : std::nullopt;
    if (!binding) throw core::PyValueError(core::py_repr(binding_given) + " is not a valid Binding");
    if (!pages.is_number_integer() && !pages.is_boolean()) {
        throw core::PyTypeError("'" + core::py_type_name(pages) + "' object cannot be interpreted as an integer");
    }
    const int count = static_cast<int>(pages.is_boolean() ? (pages.get<bool>() ? 1 : 0) : pages.get<std::int64_t>());
    const Json& title = a["title"];
    core::Document doc = core::new_episode(title.is_string() ? title.get<std::string>() : py_str(title), core::Num(1), count, spec, *binding);
    doc.strict_gates = true;  // agent projects: printed layers change only after the name is approved
    // (C++: written under the project lock, and only when no book is there by then)
    storage::ProjectLock lock(path, actor_);
    try {
        lock.try_acquire();
    } catch (const storage::LockedError& locked) {
        throw core::ApplyError(locked.what(), "locked");
    }
    if (fs::exists(path / "project.json", ec)) return fail("すでにある: " + py_str(name), "project_exists");
    storage::SaveRequest request;
    request.actor = actor_;
    request.base_revision = 0;
    request.ops = Json::array();
    storage::Saver(lock).save(doc, request);
    return ToolResult(true, Json::object({{"project", name}, {"pages", pages}}));
}

// --- reading ------------------------------------------------------------------------------------------------------------

Json waiting(const Json& items) {
    Json out = Json::array();
    const auto blocked = [](const Json& i) { return py_truthy(i["blocked_by"]); };
    for (const char* gate : {"proposal", "name", "art"}) {
        std::vector<Json> pages;
        for (const Json& i : items) {
            if (!blocked(i) || !core::py_equals(py_get(i, "gate"), Json(gate))) continue;
            const Json page = i["target"]["page"];
            bool seen = false;
            for (const Json& p : pages) seen = seen || core::py_equals(p, page);
            if (!seen) pages.push_back(page);
        }
        std::stable_sort(pages.begin(), pages.end(), [](const Json& x, const Json& y) { return core::py_less(x, y); });
        if (!pages.empty()) out.push_back(Json::object({{"gate", gate}, {"pages", Json(pages)}}));
    }
    std::vector<Json> chars;
    for (const Json& i : items) {
        if (!blocked(i) || !core::py_equals(py_get(i, "gate"), Json("sheet"))) continue;
        const Json cid = i["target"]["character_id"];
        bool seen = false;
        for (const Json& c : chars) seen = seen || core::py_equals(c, cid);
        if (!seen) chars.push_back(cid);
    }
    std::stable_sort(chars.begin(), chars.end(), [](const Json& x, const Json& y) { return core::py_less(x, y); });
    if (!chars.empty()) out.push_back(Json::object({{"gate", "sheet"}, {"characters", Json(chars)}}));
    for (const Json& i : items) {
        if (blocked(i) && core::py_equals(py_get(i, "gate"), Json("export"))) {
            out.push_back(Json::object({{"gate", "export"}}));
            break;
        }
    }
    return out;
}

bool chat_approval_open(const core::Document& doc) {
    const Json setting = py_get(doc.studio, "chat_approval");
    return !(setting.is_object() && setting.contains("on") && setting["on"].is_boolean() && !setting["on"].get<bool>());
}

ToolResult StudioService::status(const Json& a) {
    const fs::path path = project_path(a["project"]);
    const core::Document doc = load_episode(path);
    Json pages = Json::array();
    for (const auto& page_ptr : doc.pages) {
        const core::Page& page = *page_ptr;
        const auto draft = studio::name(doc, page.index);
        const auto review = studio::name_review(doc, page.index);
        const auto leaves = page.leaf_frames();
        const bool reviewed = draft && py_truthy(*draft) && review && py_truthy(*review) &&
                              core::py_equals(py_get(*review, "input_hash"), py_get(*draft, "input_hash"));
        std::int64_t adopted = 0;
        for (const core::Frame* f : leaves) {
            const Json panel = f->panel && py_truthy(*f->panel) ? *f->panel : Json::object();
            if (core::is_one_of(py_get(panel, "status"), {"adopted", "skip"})) ++adopted;
        }
        Json entry = Json::object({{"page", page.index.json()},
                                   {"named", draft.has_value()},
                                   {"reviewed", reviewed},
                                   {"name_ok", page.name_ok},
                                   {"art_ok", page.art_ok},
                                   {"panels", static_cast<std::int64_t>(leaves.size())},
                                   {"adopted", adopted},
                                   {"lines", static_cast<std::int64_t>(doc.story_for_page(page.index).size())}});
        if (core::truthy_at(page.extra, "assignee")) entry["assignee"] = page.extra["assignee"];
        if (core::truthy_at(page.extra, "cover")) entry["cover"] = util::item(page.extra["cover"], "kind");
        pages.push_back(std::move(entry));
    }
    const Json items = worklist::next_actions(doc, path);
    return ToolResult(true, Json::object({{"title", doc.title},
                                          {"revision", doc.revision},
                                          {"bible", studio::bible(doc).has_value()},
                                          {"script", studio::script(doc).has_value()},
                                          {"pages", std::move(pages)},
                                          {"style", briefs::style_brief(doc)},
                                          {"waiting_for", waiting(items)}}));
}

ToolResult StudioService::style_catalog(const Json& a) {
    Json style_id = a["style_id"];
    const Json& title = a["title"];
    const Json& project = a["project"];
    try {
        if (py_truthy(style_id)) style_id = stylecat::style_id_from(style_id);  // (a page URL a person shared works too)
        if (py_truthy(title)) {
            const Json found = stylecat::find(title);
            Json data = Json::object({{"found", found}});
            std::vector<std::string> titles;
            for (const Json& f : found) {
                const std::string t = py_str(py_get(f, "title"));
                if (std::find(titles.begin(), titles.end(), t) == titles.end()) titles.push_back(t);
            }
            if (titles.size() < found.size()) {
                data["note"] = "同じ名前の絵柄が別の分類にある。path（分類）と見本を人に見せて、id で確かめてから use_style する";
            }
            return ToolResult(true, std::move(data));
        }
        if (py_truthy(style_id)) {
            const Json data = stylecat::style(style_id);
            Json shown = briefs::catalog_node(data);
            Json same = Json::array();
            try {
                same = stylecat::namesakes(util::item(data, "id"), Json(py_str(core::py_or(py_get(data, "title"), Json("")))));
            } catch (const stylecat::CatalogError&) {
                same = Json::array();
            }
            if (!same.empty()) shown["same_title_elsewhere"] = same;
            const Json id = util::item(data, "id");
            if (!id.is_string()) throw core::PyTypeError("quote() doesn't support '" + core::py_type_name(id) + "' objects");
            return ToolResult(true, Json::object({{"style", std::move(shown)}, {"page_url", stylecat::page_url(id.get<std::string>())}}));
        }
        if (py_truthy(project)) {
            const core::Document doc = load_episode(project_path(project));
            const Json kept = briefs::catalog(doc);
            if (kept.is_null()) {
                return ToolResult(true, Json::object({{"style", Json()}, {"note", "この原稿は Genko の言葉の絵柄（カタログの絵柄を選んでいない）"}}));
            }
            return ToolResult(true, Json::object({{"style", briefs::style_brief(doc)}, {"newer", stylecat::newer(kept)}}));
        }
        Json genres = Json::array();
        for (const Json& n : stylecat::tree()) {
            if (py_get(n, "parent").is_null()) {
                genres.push_back(Json::object({{"id", py_get(n, "id")}, {"title", py_get(n, "title")}, {"summary", py_get(n, "summary")}}));
            }
        }
        return ToolResult(true, Json::object({{"genres", std::move(genres)}}));
    } catch (const stylecat::CatalogError& exc) {
        return fail(exc.what(), "style_catalog");
    }
}

ToolResult StudioService::use_style(const Json& a) {
    const fs::path path = project_path(a["project"]);
    core::Document doc = load_episode(path);
    std::vector<Issue> issues;
    const Json& style_id = a["style_id"];
    Json data;
    try {
        if (py_truthy(style_id)) data = stylecat::style(stylecat::style_id_from(style_id));
    } catch (const stylecat::CatalogError& exc) {
        return fail(exc.what(), "style_catalog", "/style_id");
    }
    if (!data.is_null() && core::py_equals(py_get(data, "expression"), Json("mono")) && doc.spec.expression == "color") {
        issues.push_back(warning("style_mono_on_color", "/style_id", "白黒の絵柄を選んだが、この原稿はカラー（カラーのページの依頼には使われない）",
                                 "白黒の原稿にするか、カラーの絵柄を人に確かめる"));
    }
    if (core::truthy_at(util::or_dict(py_get(doc.studio, "style")), "locked")) {
        return ToolResult(false, Json::object({{"committed", false}}),
                          {error("style_locked", "/style_id", "絵柄は試しのページで固定済み。変えるのは人（Genko の画面で絵柄を選ぶ）")});
    }
    Json shown = py_truthy(data) ? briefs::catalog_node(data) : Json();
    if (!data.is_null()) {
        Json same = Json::array();
        try {
            same = stylecat::namesakes(util::item(data, "id"), Json(py_str(core::py_or(py_get(data, "title"), Json("")))));
        } catch (const stylecat::CatalogError&) {
            same = Json::array();
        }
        if (!same.empty()) {
            shown["same_title_elsewhere"] = same;
            std::string where;
            for (const Json& o : same) {
                std::string path_words;
                for (const Json& step : core::iterate(core::py_or(py_get(o, "path"), Json::array()))) {
                    path_words += (path_words.empty() ? "" : " › ") + py_str(step);
                }
                where += (where.empty() ? "" : " / ") + path_words + "（" + py_str(util::item(o, "id")) + "）";
            }
            std::string chosen;
            for (const Json& step : core::iterate(core::py_or(py_get(shown, "path"), Json::array()))) {
                if (!step.is_string()) throw core::PyTypeError("sequence item: expected str instance, " + core::py_type_name(step) + " found");
                chosen += (chosen.empty() ? "" : " › ") + step.get<std::string>();
            }
            issues.push_back(warning("style_same_title", "/style_id",
                                     "「" + py_str(py_get(data, "title")) + "」という名前の絵柄はほかにもある: " + where + "。選んだのは " + chosen +
                                         "（" + py_str(util::item(data, "id")) + "）",
                                     "分類と見本を人に見せて、どちらか確かめてから commit する"));
        }
    }
    if (!py_truthy(a["commit"])) return ToolResult(true, Json::object({{"committed", false}, {"style", shown}}), issues);
    Session session(path, actor_);
    Json catalog;
    if (!data.is_null()) {
        std::optional<std::string> png;
        try {
            png = stylecat::sample_png(data);
        } catch (const stylecat::CatalogError& exc) {
            return fail(exc.what(), "style_catalog");
        }
        std::optional<std::string> asset;
        if (png) asset = storage::AssetStore(path).put_bytes(*png, ".png");
        catalog = stylecat::saved(data, asset);
    }
    try {
        session.apply(Json::array({Json::object({{"op", "set_style_catalog"}, {"catalog", catalog}})}), actor_);
    } catch (const core::ApplyError& exc) {
        return ToolResult(false, Json::object({{"committed", false}}), {error("apply_failed", "/", wording_error(exc.what()))});
    }
    session.save(actor_);
    return ToolResult(true, Json::object({{"committed", true}, {"style", briefs::style_brief(session.doc())}}), issues);
}

ToolResult StudioService::next(const Json& a) {
    const fs::path path = project_path(a["project"]);
    Json items = worklist::next_actions(load_episode(path), path);
    const double now = now_seconds();
    for (Json& entry : items) {
        if (py_truthy(entry["blocked_by"])) continue;
        const auto holder = studio::holder(path, py_str(entry["id"]), now);
        if (holder && !holder->empty() && *holder != actor_) entry["blocked_by"].push_back("claimed:" + *holder);
    }
    Json runnable = Json::array();
    for (const Json& i : items) {
        if (!py_truthy(i["blocked_by"])) runnable.push_back(i);
    }
    runnable = head_of(runnable, a["limit"]);
    if (py_truthy(a["claim"])) {
        Json kept = Json::array();
        for (const Json& i : runnable) {
            if (studio::claim(path, py_str(i["id"]), actor_, now_seconds())) kept.push_back(i);
        }
        runnable = std::move(kept);
    }
    std::int64_t blocked = 0;
    for (const Json& i : items) blocked += py_truthy(i["blocked_by"]) ? 1 : 0;
    return ToolResult(true, Json::object({{"items", std::move(runnable)}, {"blocked", blocked}, {"waiting_for", waiting(items)}}));
}

ToolResult StudioService::upscalers(const Json&) { return ToolResult(true, Json::object({{"upscalers", upscale::available()}})); }

ToolResult StudioService::inspect(const Json& a) {
    const Json& target = a["target"];
    const Json& page = a["page"];
    if (core::py_equals(target, Json("ops"))) {  // (the ops apply_ops takes: the names alone, or the ones asked for with their arguments)
        std::vector<const Json*> usable;
        for (const Json& o : core::ops_schema()) {
            if (o.contains("op") && o["op"].is_string() && agent_op(o["op"].get<std::string>())) usable.push_back(&o);
        }
        const Json& op = a["op"];
        if (!py_truthy(op)) {
            Json names = Json::array();
            for (const Json* o : usable) names.push_back((*o)["op"]);
            return ToolResult(true, Json::object({{"ops", std::move(names)},
                                                  {"note", "op に名前（カンマ区切りで複数、例 add_stroke,fill）を渡すと引数の形が返る"}}));
        }
        std::vector<std::string> wanted;
        const std::string text = py_str(op);
        std::size_t start = 0;
        for (;;) {
            const std::size_t comma = text.find(',', start);
            const std::string w = core::py_strip(text.substr(start, comma == std::string::npos ? std::string::npos : comma - start));
            if (!w.empty()) wanted.push_back(w);
            if (comma == std::string::npos) break;
            start = comma + 1;
        }
        Json found = Json::array();
        for (const Json* o : usable) {
            if (std::find(wanted.begin(), wanted.end(), (*o)["op"].get<std::string>()) != wanted.end()) found.push_back(*o);
        }
        Json unknown = Json::array();
        for (const std::string& w : wanted) {
            bool there = false;
            for (const Json& f : found) there = there || f["op"] == w;
            if (!there) unknown.push_back(w);
        }
        if (!unknown.empty() && found.empty()) {
            std::string listed;
            for (const Json& u : unknown) listed += (listed.empty() ? "" : ", ") + u.get<std::string>();
            return fail("その op は無い: " + listed + "（inspect target=ops で名前の一覧）", "no_op", "/op");
        }
        Json data = Json::object({{"ops", std::move(found)}});
        if (!unknown.empty()) data["unknown"] = std::move(unknown);
        return ToolResult(true, std::move(data));
    }
    const fs::path path = project_path(a["project"]);
    if (core::py_equals(target, Json("schemas"))) {
        return ToolResult(true, Json::object({{"schemas", Json::object({{"bible@1", studio::schema("bible@1")},
                                                                          {"script@1", studio::schema("script@1")},
                                                                          {"name_plan@1", studio::schema("name_plan@1")}})}}));
    }
    if (core::py_equals(target, Json("rules"))) return ToolResult(true, Json::object({{"rules", data::rules()}}));
    if (core::py_equals(target, Json("drawing"))) return ToolResult(true, Json::object({{"drawing", data::drawing()}}));
    if (core::py_equals(target, Json("materials"))) return ToolResult(true, Json::object({{"materials", briefs::materials_list()}}));
    if (core::py_equals(target, Json("fonts"))) return ToolResult(true, Json::object({{"fonts", briefs::fonts_list()}}));
    if (core::py_equals(target, Json("upscalers"))) return upscalers(Json::object());
    if (core::py_equals(target, Json("plugins"))) {
        Json plugins = Json::array();
        for (const render::plugins::Described& p : render::plugins::available()) {
            plugins.push_back(Json::object({{"key", p.key}, {"name", p.name}, {"params", p.params},
                                            {"kind", std::string(render::plugins::kPrefix) + p.key}}));
        }
        return ToolResult(true, Json::object({{"plugins", std::move(plugins)}, {"folder", core::path_to_utf8(render::plugins::folder())}}));
    }
    const core::Document doc = load_episode(path);
    if (core::py_equals(target, Json("brushes"))) return ToolResult(true, Json::object({{"brushes", briefs::brushes_list(doc)}}));
    if (core::py_equals(target, Json("bible"))) {
        const auto bible = studio::bible(doc);
        return ToolResult(true, Json::object({{"bible", bible ? *bible : Json()}}));
    }
    if (core::py_equals(target, Json("script"))) {
        const auto script = studio::script(doc);
        return ToolResult(true, Json::object({{"script", script ? *script : Json()}}));
    }
    if (core::py_equals(target, Json("studio"))) return ToolResult(true, Json::object({{"studio", doc.studio}}));
    if (core::py_equals(target, Json("snapshot"))) {
        Json snap = storage::snapshot(doc);
        if (!page.is_null()) {  // (that page alone: the rest of the book is not sent again)
            Json kept = Json::array();
            for (const Json& p : core::iterate(py_get(snap, "pages", Json::array()))) {
                if (core::py_equals(py_get(p, "index"), page)) kept.push_back(p);
            }
            snap["pages"] = std::move(kept);
        }
        return ToolResult(true, Json::object({{"snapshot", std::move(snap)}}));
    }
    if (core::is_one_of(target, {"page", "panel"})) {
        if (page.is_null()) return fail("page が要る", "page_required", "/page");
        if (!page_at(doc, page)) return fail(py_str(page) + " ページはない", "no_page", "/page");
        if (core::py_equals(target, Json("page"))) return ToolResult(true, page_brief(doc, page));
        return ToolResult(true, briefs::panel_brief(doc, page, a["frame_id"], py_truthy(a["full"])));
    }
    return fail("target " + py_str(target) +
                    " はない（bible / script / page / panel / studio / schemas / rules / snapshot / materials / fonts / brushes / plugins / "
                    "upscalers）",
                "unknown_target", "/target");
}

Json StudioService::page_brief(const core::Document& doc, const Json& page) const {
    const auto stored = studio::script(doc);
    const Json script = stored && py_truthy(*stored) ? *stored : Json::object({{"scenes", Json::array()}});
    Json beats = Json::array();
    const Json previous = (core::to_num(page) - core::Num(1)).json();
    const Json following = (core::to_num(page) + core::Num(1)).json();
    std::vector<Json> previous_texts;
    std::vector<Json> next_texts;
    for (const Json& scene : core::iterate(py_get(script, "scenes", Json::array()))) {
        for (const Json& beat : core::iterate(py_get(scene, "beats", Json::array()))) {
            const Json beat_page = py_get(beat, "page");
            if (core::py_equals(beat_page, page)) {
                Json entry = Json::object({{"scene_id", py_get(scene, "id")}});
                const Json fields = core::py_dict(beat);  // (held: items() of a temporary would dangle)
                for (const auto& [key, value] : fields.items()) entry[key] = value;
                beats.push_back(std::move(entry));
                continue;
            }
            core::require_hashable(beat_page);
            if (core::py_equals(beat_page, previous)) {
                previous_texts.push_back(py_get(beat, "text", Json("")));
            } else if (core::py_equals(beat_page, following)) {
                next_texts.push_back(py_get(beat, "text", Json("")));
            }
        }
    }
    const auto joined = [](const std::vector<Json>& texts) {
        std::string out;
        for (std::size_t i = 0; i < texts.size(); ++i) {
            if (!texts[i].is_string()) {
                throw core::PyTypeError("sequence item " + std::to_string(i) + ": expected str instance, " + core::py_type_name(texts[i]) + " found");
            }
            out += (i == 0 ? "" : " / ") + texts[i].get<std::string>();
        }
        return text::head(out, 400);
    };
    const auto draft = util::name_of(doc, page);
    const core::Page& target = doc.page(*page_at(doc, page));
    Json panels = Json::array();
    for (const core::Frame* f : target.leaf_frames()) {
        const Json panel = f->panel && py_truthy(*f->panel) ? *f->panel : Json::object();
        panels.push_back(Json::object({{"frame_id", f->id},
                                       {"rect_mm", Json::array({core::py_round(f->rect.x, 1).json(), core::py_round(f->rect.y, 1).json(),
                                                                core::py_round(f->rect.width, 1).json(), core::py_round(f->rect.height, 1).json()})},
                                       {"slot", py_get(panel, "slot")},
                                       {"status", py_get(panel, "status", Json("empty"))}}));
    }
    Json fixes = Json::array();
    for (const Json& t : studio::page_fixes(doc, core::to_num(page))) fixes.push_back(py_get(t, "text", Json("")));
    Json templates = Json::object();
    for (const auto& [key, value] : layout::templates().items()) templates[key] = util::item(value, "description");
    Json out = Json::object({{"page", page}});
    const Json side = worklist::page_side(page);  // (held: items() of a temporary would dangle)
    for (const auto& [key, value] : side.items()) out[key] = value;
    out["beats"] = std::move(beats);
    out["previous_page"] = joined(previous_texts);
    out["next_page"] = joined(next_texts);
    out["current_plan"] = draft ? py_get(*draft, "name") : Json();
    out["panels"] = std::move(panels);
    out["gutters"] = briefs::gutter_brief(target);
    out["fixes"] = std::move(fixes);
    out["templates"] = std::move(templates);
    return out;
}

}  // namespace genko::service
