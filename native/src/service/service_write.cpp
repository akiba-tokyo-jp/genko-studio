// StudioService's writing tools (Python's genko/studio/service.py: set_bible … tickets): each checks what the agent sent,
// and changes the book only through the ops, saved once under the project lock.

#include <QByteArray>
#include <QtGlobal>

#include <algorithm>
#include <filesystem>
#include <functional>
#include <optional>

#if !defined(_WIN32)
#include <pwd.h>
#include <unistd.h>
#endif

#include "core/command_bus.hpp"
#include "core/error.hpp"
#include "core/ids.hpp"
#include "core/paths.hpp"
#include "core/pyconv.hpp"
#include "core/pyops.hpp"
#include "service/book.hpp"
#include "service/briefs.hpp"
#include "service/layout.hpp"
#include "service/letter.hpp"
#include "service/lint.hpp"
#include "service/preview.hpp"
#include "service/service_util.hpp"
#include "service/sheets.hpp"
#include "service/studio_service.hpp"
#include "service/worklist.hpp"
#include "storage/asset_store.hpp"
#include "storage/undo.hpp"
#include "studio/schemas.hpp"
#include "studio/state.hpp"

namespace genko::service {

using core::py_get;
using core::py_str;
using core::py_truthy;

namespace {

constexpr const char* kBriefFromPlan[] = {"shot", "angle", "characters", "location_id", "time", "action", "emotion", "fx", "emphasis", "beat_ids"};
constexpr const char* kBriefLater[] = {"props", "sfx_at", "bleed", "slant"};  // (plan fields added later: copied when given)

void append(std::vector<Issue>& to, const std::vector<Issue>& more) { to.insert(to.end(), more.begin(), more.end()); }

std::vector<Issue> joined(std::vector<Issue> a, const std::vector<Issue>& b) {
    append(a, b);
    return a;
}

// Path(text).expanduser()
fs::path expanduser(const std::string& text) {
    if (text.empty() || text[0] != '~') return core::path_from_utf8(text);
    const std::size_t slash = text.find('/');
    const std::string user = text.substr(1, slash == std::string::npos ? std::string::npos : slash - 1);
    std::string home;
    if (user.empty()) {
        home = qgetenv("HOME").toStdString();
#if !defined(_WIN32)
        if (home.empty()) {
            if (const passwd* entry = getpwuid(getuid()); entry != nullptr && entry->pw_dir != nullptr) home = entry->pw_dir;
        }
#endif
    } else {
#if !defined(_WIN32)
        if (const passwd* entry = getpwnam(user.c_str()); entry != nullptr && entry->pw_dir != nullptr) home = entry->pw_dir;
#endif
        if (home.empty()) return core::path_from_utf8(text);  // (an unknown user: left as it is)
    }
    while (home.size() > 1 && home.back() == '/') home.pop_back();
    return core::path_from_utf8(home + (slash == std::string::npos ? std::string() : text.substr(slash)));
}

fs::path resolved(const fs::path& path) {
    std::error_code ec;
    fs::path out = fs::weakly_canonical(path, ec);
    if (ec) out = path.lexically_normal();
    return out;
}

bool strictly_under(const fs::path& path, const fs::path& root) {
    auto p = path.begin();
    for (auto r = root.begin(); r != root.end(); ++r, ++p) {
        if (r->empty() && std::next(r) == root.end()) break;
        if (p == path.end() || *p != *r) return false;
    }
    for (; p != path.end(); ++p) {
        if (!p->empty()) return true;
    }
    return false;
}

// {k: v for k, v in counts.items()} of a dict whose keys are ints (json.dumps writes them as their str; True as "true").
std::string json_key(const Json& key) {
    if (key.is_boolean()) return key.get<bool>() ? "true" : "false";
    return py_str(key);
}

// _compile_name(episode, page_index, plan, bible, script, replace): the plan's panels and balloons applied to `work`;
// nothing (and the issues) when it cannot be.
std::pair<std::optional<layout::CompiledLayout>, std::vector<Issue>> compile_name(Work& work, const Json& page_index, const Json& plan,
                                                                                const Json& bible, const Json& script, bool replace,
                                                                                const std::string& actor) {
    const auto at = page_at(work.doc, page_index);
    if (!at) throw core::PyUncaught("StopIteration", "");
    if (work.doc.page(*at).name_ok) {
        return {std::nullopt, {error("name_approved", "/page", py_str(page_index) + " ページのネームは承認済み。変えるには人間が承認を取り消す")}};
    }
    if (!layout::is_blank(work.doc, work.doc.page(*at))) {
        if (!replace) {
            return {std::nullopt, {error("page_not_blank", "/page", py_str(page_index) + " ページにはすでにコマか台詞がある", "作り直すなら replace:true")}};
        }
        layout::clear_page(work, *at, actor);
    }
    layout::CompiledLayout compiled;
    try {
        compiled = layout::apply_layout(work, page_index, plan, actor);
    } catch (const layout::LayoutError& exc) {
        return {std::nullopt, {error("layout_failed", exc.where(), exc.message())}};
    }
    Json speakers = Json::object();
    const Json index = lint::script_index(script);  // (held: items() of a temporary would dangle)
    for (const auto& [bid, info] : index.items()) speakers[bid] = py_get(util::item(info, "beat"), "speaker_id");
    auto [placements, issues] = letter::place_page(plan, compiled, bible, speakers);
    append(issues, lint::lint_name_geometry(plan, compiled, placements));
    if (studio::has_errors(issues)) return {std::nullopt, issues};
    Json ops = letter::placements_to_ops(page_index, placements, compiled);
    ops.push_back(Json::object({{"op", "set_note"}, {"page", page_index}, {"note", briefs::brief(plan, bible)}}));
    try {
        work.apply(ops, actor);
    } catch (const core::ApplyError& exc) {
        return {std::nullopt, {error("apply_failed", "/", exc.what())}};
    }
    return {std::move(compiled), std::move(issues)};
}

}  // namespace

// --- writing: bible, script, name ----------------------------------------------------------------------------------------

ToolResult StudioService::set_bible(const Json& a) {
    const fs::path path = project_path(a["project"]);
    const Json& schema = studio::schema("bible@1");
    const Json bible = studio::fill_nulls(a["bible"], schema);
    std::vector<Issue> issues = studio::validate(bible, schema);
    if (!studio::has_errors(issues)) append(issues, lint::lint_bible(bible));
    if (studio::has_errors(issues) || !py_truthy(a["commit"])) {
        const auto count = static_cast<std::int64_t>(core::length(py_get(bible, "characters", Json::array())));
        return ToolResult(!studio::has_errors(issues), Json::object({{"committed", false}, {"characters", count}}), issues);
    }
    Session session(path, actor_);
    auto [characters, more] = briefs::keep_approved(session.doc().bible.characters, util::item(bible, "characters"));
    if (studio::has_errors(more)) return ToolResult(false, Json::object({{"committed", false}}), joined(issues, more));
    Json doc = Json::object();
    for (const auto& [key, value] : bible.items()) {
        if (key != "plot" && key != "characters" && key != "constraints") doc[key] = value;
    }
    Json ops = Json::array({Json::object({{"op", "set_bible"},
                                          {"plot", util::item(bible, "plot")},
                                          {"characters", characters},
                                          {"constraints", util::item(bible, "constraints")}}),
                            Json::object({{"op", "set_studio"}, {"bible_doc", doc}})});
    if (core::truthy_at(bible, "title")) ops.push_back(Json::object({{"op", "set_meta"}, {"title", bible["title"]}}));
    PyDict known;
    for (const Json& p : core::iterate(py_get(session.doc().studio, "props", Json::array()))) known.set(py_get(p, "id"), p);
    for (const Json& prop : core::iterate(core::py_or(py_get(bible, "props"), Json::array()))) {  // (the props join the book's list)
        const Json* had = known.find(util::item(prop, "id"));
        ops.push_back(Json::object({{"op", "upsert_prop"}, {"prop", util::merged(had != nullptr ? *had : Json::object(), prop)}}));
    }
    session.apply(ops, actor_);
    session.save(actor_);
    return ToolResult(true, Json::object({{"committed", true}}), issues);
}

ToolResult StudioService::set_script(const Json& a) {
    const fs::path path = project_path(a["project"]);
    const core::Document doc = load_episode(path);
    const auto bible = studio::bible(doc);
    if (!bible) return fail("先に set_bible で企画書を保存する", "bible_missing");
    const Json& script = a["script"];
    std::vector<Issue> issues = studio::validate(script, studio::schema("script@1"));
    if (!studio::has_errors(issues)) append(issues, lint::lint_script(script, *bible, static_cast<std::int64_t>(doc.pages.size())));
    PyDict per_page;
    if (script.is_object()) {
        for (const Json& scene : core::iterate(py_get(script, "scenes", Json::array()))) {
            for (const Json& beat : core::iterate(py_get(scene, "beats", Json::array()))) {
                if (!beat.is_object()) continue;
                const Json page = py_get(beat, "page");
                if (!page.is_number_integer() && !page.is_boolean()) continue;
                const Json* had = per_page.find(page);
                per_page.set(page, Json((had != nullptr ? had->get<std::int64_t>() : 0) + 1));
            }
        }
    }
    Json counts = Json::object();
    for (const auto& [key, value] : per_page.items()) counts[json_key(key)] = value;
    Json data = Json::object({{"committed", false}, {"beats_per_page", std::move(counts)}});
    if (studio::has_errors(issues) || !py_truthy(a["commit"])) return ToolResult(!studio::has_errors(issues), std::move(data), issues);
    Session session(path, actor_);
    session.apply(Json::array({Json::object({{"op", "set_script"}, {"script", script}})}), actor_);
    session.save(actor_);
    data["committed"] = true;
    return ToolResult(true, std::move(data), issues);
}

ToolResult StudioService::submit_name(const Json& a) {
    const fs::path path = project_path(a["project"]);
    const core::Document doc = load_episode(path);
    const auto bible = studio::bible(doc);
    const auto script = studio::script(doc);
    if (!bible || !script) return fail("先に企画書（set_bible）と脚本（set_script）を保存する", "script_missing");
    const Json& schema = studio::schema("name_plan@1");
    Json plan = studio::fill_nulls(a["plan"], schema);
    std::vector<Issue> issues = studio::validate(plan, schema);
    if (!plan.is_object()) core::raise_attribute_error(plan, "get");
    append(issues, briefs::props_by_id(plan, *bible));
    if (studio::has_errors(issues)) return ToolResult(false, Json::object({{"committed", false}}), issues);
    const bool tall = doc.spec.height_mm > core::Num(2) * doc.spec.width_mm;
    append(issues, lint::lint_name_plan(plan, *script, *bible, static_cast<std::int64_t>(doc.pages.size()), tall));
    if (studio::has_errors(issues)) return ToolResult(false, Json::object({{"committed", false}}), issues);
    const Json page_index = util::item(plan, "page");
    const bool replace = py_truthy(a["replace"]);
    Work work{doc, Json::array()};
    auto [compiled, more] = compile_name(work, page_index, plan, *bible, *script, replace, actor_);
    append(issues, more);
    if (!compiled || studio::has_errors(issues)) return ToolResult(false, Json::object({{"committed", false}}), issues);
    Json panels = Json::object();
    for (const auto& [slot, rect] : compiled->leaf_rects_mm) {
        panels[slot] = Json::array({core::py_round(rect[0], 1).json(), core::py_round(rect[1], 1).json(), core::py_round(rect[2], 1).json(),
                                    core::py_round(rect[3], 1).json()});
    }
    Json reading = Json::array();
    for (const std::string& slot : compiled->reading_order) reading.push_back(slot);
    Json data = Json::object({{"committed", false}, {"page", page_index}, {"reading_order", reading}, {"panels", std::move(panels)}});
    const std::string png = preview(work.doc, page_index, "name", a["max_px"], plan);
    const fs::path out = path / "studio" / "reviews" / ("p" + util::zero3(page_index) + "_name.png");
    util::write_file(out, png);
    if (!py_truthy(a["commit"])) return ToolResult(true, std::move(data), issues, {png}, {util::path_text(out)});
    {
        Session session(path, actor_);
        auto [again, problems] = compile_name(session.work(), page_index, plan, *bible, *script, replace, actor_);
        if (!again || studio::has_errors(problems)) return ToolResult(false, Json::object({{"committed", false}}), problems);
        Json ops = Json::array();
        for (const Json& panel : core::iterate(util::item(plan, "panels"))) {
            const Json slot = util::item(panel, "slot");
            const std::string* frame = slot.is_string() ? again->frame_of(slot.get<std::string>()) : nullptr;
            if (frame == nullptr) continue;
            Json set = Json::object({{"slot", slot}});
            for (const char* key : kBriefFromPlan) {
                if (panel.contains(key)) set[key] = panel[key];
            }
            for (const char* key : kBriefLater) {
                if (!py_get(panel, key).is_null()) set[key] = panel[key];
            }
            ops.push_back(Json::object({{"op", "set_panel"}, {"page", page_index}, {"frame_id", *frame}, {"set", std::move(set)}}));
        }
        const core::Document& now = session.doc();
        const auto found = page_at(now, page_index);  // (compile_name found it in this session)
        if (!found) return fail(py_str(page_index) + " ページはない", "no_page", "/page");
        const core::Page& target = now.page(*found);
        const Json stored = target.plan && py_truthy(*target.plan) ? *target.plan : Json::object();
        const std::int64_t submits = core::to_int(py_get(stored, "submits", Json(0))) + 1;
        Json slot_to_frame = Json::object();
        for (const auto& [slot, frame] : again->slot_to_frame) slot_to_frame[slot] = frame;
        Json order = Json::array();
        for (const std::string& slot : again->reading_order) order.push_back(slot);
        ops.push_back(Json::object({{"op", "set_page_plan"},
                                    {"page", page_index},
                                    {"plan", Json::object({{"submits", submits},
                                                           {"name", plan},
                                                           {"input_hash", worklist::plan_hash(plan)},
                                                           {"rev", now.revision},
                                                           {"turn_role", py_get(plan, "turn_role")},
                                                           {"slot_to_frame", std::move(slot_to_frame)},
                                                           {"reading_order", std::move(order)},
                                                           {"by", actor_}})}}));
        for (const Json& t : studio::page_fixes(now, core::to_num(page_index))) {
            ops.push_back(Json::object({{"op", "set_ticket"}, {"id", util::item(t, "id")}, {"status", "done"}}));
        }
        try {
            session.apply(ops, actor_);
        } catch (const core::ApplyError& exc) {
            return ToolResult(false, Json::object({{"committed", false}}), {error("apply_failed", "/", exc.what())});
        }
        session.save(actor_);
        data["committed"] = true;
        const auto saved_at = page_at(session.doc(), page_index);
        if (saved_at) data["gutters"] = briefs::gutter_brief(session.doc().page(*saved_at));
    }
    return ToolResult(true, std::move(data), issues, {png}, {util::path_text(out)});
}

ToolResult StudioService::apply_ops(const Json& a) {
    const fs::path path = project_path(a["project"]);
    const Json& given = a["ops"];
    if (!given.is_array() || given.size() > 500) return fail("ops は 500 個までの配列", "ops_invalid", "/ops");
    for (std::size_t i = 0; i < given.size(); ++i) {
        const Json name = given[i].is_object() ? py_get(given[i], "op") : Json();
        core::require_hashable(name);
        if (!name.is_string() || !agent_op(name.get<std::string>())) {
            return fail("op " + py_str(name) + " はこの道具では使えない", "op_not_allowed", "/ops/" + std::to_string(i) + "/op");
        }
    }
    // (C++: the ops for_pages repeats page by page are an agent's ops as well, checked as those given — Python looks only
    // at the top, so any op, put_raster reading any file among them, went through inside for_pages: SPEC COMP-01a)
    std::function<std::optional<ToolResult>(const Json&, const std::string&)> repeated_allowed;
    repeated_allowed = [&repeated_allowed](const Json& list, const std::string& at) -> std::optional<ToolResult> {
        for (std::size_t j = 0; j < list.size(); ++j) {
            const Json& op = list[j];
            const Json name = op.is_object() ? py_get(op, "op") : Json();
            if (!name.is_string()) continue;  // (for_pages refuses what is not an op, apply_ops an op it does not know)
            const std::string where = at + "/" + std::to_string(j);
            if (!agent_op(name.get<std::string>())) return fail("op " + py_str(name) + " はこの道具では使えない", "op_not_allowed", where + "/op");
            if (name == Json("for_pages") && op.contains("ops") && op["ops"].is_array()) {
                if (auto refused = repeated_allowed(op["ops"], where + "/ops")) return refused;
            }
        }
        return std::nullopt;
    };
    for (std::size_t i = 0; i < given.size(); ++i) {
        const Json& op = given[i];
        if (core::py_equals(py_get(op, "op"), Json("for_pages")) && op.contains("ops") && op["ops"].is_array()) {
            if (auto refused = repeated_allowed(op["ops"], "/ops/" + std::to_string(i) + "/ops")) return std::move(*refused);
        }
    }
    Json ops = given;  // (each op copied: [dict(op) for op in ops])
    const bool commit = py_truthy(a["commit"]);
    // import_psd's path, read like the other tools' (in the book or under --root): made absolute, or refused
    const auto confined_psd = [this, &path](Json& op, const std::string& where) -> std::optional<ToolResult> {
        if (!core::py_equals(py_get(op, "op"), Json("import_psd")) || !core::truthy_at(op, "path")) return std::nullopt;
        const fs::path raw = expanduser(py_str(op["path"]));
        const fs::path target = resolved(raw.is_absolute() ? raw : path / raw);
        if (!strictly_under(target, root_)) {
            return fail("path は原稿のフォルダからの相対パスか、--root の中のパス: " + py_str(op["path"]), "path_outside_root", where + "/path");
        }
        op["path"] = util::path_text(target);
        return std::nullopt;
    };
    // (C++: and the ones for_pages repeats)
    std::function<std::optional<ToolResult>(Json&, const std::string&)> repeated_confined;
    repeated_confined = [&repeated_confined, &confined_psd](Json& list, const std::string& at) -> std::optional<ToolResult> {
        for (std::size_t j = 0; j < list.size(); ++j) {
            Json& op = list[j];
            if (!op.is_object()) continue;
            const std::string where = at + "/" + std::to_string(j);
            if (auto refused = confined_psd(op, where)) return refused;
            if (core::py_equals(py_get(op, "op"), Json("for_pages")) && op.contains("ops") && op["ops"].is_array()) {
                if (auto refused = repeated_confined(op["ops"], where + "/ops")) return refused;
            }
        }
        return std::nullopt;
    };
    for (std::size_t i = 0; i < ops.size(); ++i) {
        Json& op = ops[i];
        if (auto refused = confined_psd(op, "/ops/" + std::to_string(i))) return std::move(*refused);
        if (core::py_equals(py_get(op, "op"), Json("for_pages")) && op.is_object() && op.contains("ops") && op["ops"].is_array()) {
            if (auto refused = repeated_confined(op["ops"], "/ops/" + std::to_string(i) + "/ops")) return std::move(*refused);
        }
        if (core::py_equals(py_get(op, "op"), Json("import_pages"))) {  // (作品の結合: another book under --root; its asset files come along)
            const Json from = py_get(op, "from");
            const fs::path raw = expanduser(py_str(core::py_or(from, Json(""))));
            const fs::path target = resolved(raw.is_absolute() ? raw : path / raw);
            std::error_code ec;
            if (!strictly_under(target, root_) || !fs::is_regular_file(target / "project.json", ec)) {
                return fail("from は --root の中の別の原稿（.genko のフォルダ）: " + py_str(from), "path_outside_root",
                            "/ops/" + std::to_string(i) + "/from");
            }
            op["from"] = util::path_text(target);
            if (commit) storage::copy_assets(target, path);
        }
    }
    core::ApplyResult result;
    try {
        result = service::apply_ops(load_episode(path), ops, actor_, true);
        if (commit) {
            Session session(path, actor_);
            result = session.apply(ops, actor_);
            session.save(actor_);
        }
    } catch (const core::ApplyError& exc) {
        return fail(exc.what(), "apply_failed", "/ops");
    }
    Json data = Json::object({{"committed", a["commit"]}, {"applied", result.applied}, {"warnings", result.has_warnings ? result.warnings : Json::array()}});
    if (!result.results.empty()) data["results"] = result.results;
    return ToolResult(true, std::move(data));
}

// --- reviews and requests ------------------------------------------------------------------------------------------------

ToolResult StudioService::ops(const Json& project, const Json& ops) {
    const fs::path path = project_path(project);
    Json applied;
    Json fresh = Json::array();
    try {
        Session session(path, actor_);
        std::vector<Json> before;
        for (const Json& t : core::iterate(session.doc().tickets)) before.push_back(util::item(t, "id"));
        const core::ApplyResult result = session.apply(ops, actor_);
        session.save(actor_);
        applied = result.applied;
        for (const Json& t : core::iterate(session.doc().tickets)) {
            const Json id = util::item(t, "id");
            if (std::none_of(before.begin(), before.end(), [&id](const Json& b) { return core::py_equals(b, id); })) fresh.push_back(id);
        }
    } catch (const core::ApplyError& exc) {
        return fail(exc.what(), "apply_failed", "/");
    }
    Json data = Json::object({{"applied", std::move(applied)}});
    if (!fresh.empty()) data["tickets"] = std::move(fresh);
    return ToolResult(true, std::move(data));
}

ToolResult StudioService::record_review(const Json& a) {
    const fs::path path = project_path(a["project"]);
    const Json& page = a["page"];
    {
        Session session(path, actor_);
        const auto draft = util::name_of(session.doc(), page);
        if (!draft) return fail(py_str(page) + " ページのネームがまだない", "name_missing", "/page");
        session.apply(Json::array({Json::object({{"op", "record_review"},
                                                 {"page", page},
                                                 {"kind", "name"},
                                                 {"score", a["score"]},
                                                 {"notes", a["notes"]},
                                                 {"input_hash", util::item(*draft, "input_hash")}})}),
                      actor_);
        session.save(actor_);
    }
    return ToolResult(true, Json::object({{"page", page}}));
}

ToolResult StudioService::record_chat_approval(const Json& a) {
    const fs::path path = project_path(a["project"]);
    const Json& gate = a["gate"];
    if (!core::is_one_of(gate, {"name", "art", "sheet"})) {
        return fail("チャットで記録できる承認は name / art / sheet（正式な書き出しは人が Genko で行う）", "gate_not_available", "/gate");
    }
    const Json& message = a["message"];
    if (core::py_strip(py_str(core::py_or(message, Json("")))).empty()) {
        return fail("message に、人がチャットで送った承認の言葉をそのまま入れる", "message_required", "/message");
    }
    const core::Document doc = load_episode(path);
    if (!chat_approval_open(doc)) {
        return fail("この原稿は、人がチャットでの承認を止めている。承認は人が Genko の承認箱で行う", "chat_approval_off", "/project");
    }
    const std::string person = briefs::chat_person(doc, a["person"]);
    const Json extra = Json::object({{"via", actor_}, {"message", core::py_strip(py_str(message))}});
    const Json& pages = a["pages"];
    const Json& character_id = a["character_id"];
    Json ops = Json::array();
    if (core::py_equals(gate, Json("sheet"))) {
        if (!py_truthy(character_id) || !py_truthy(a["candidate_id"])) {
            return fail("sheet には character_id と candidate_id が要る", "character_required", "/character_id");
        }
        ops.push_back(util::merged(sheet_approval_op(path, doc, character_id, a["candidate_id"], a["face_box01"]), extra));
    } else {
        if (!py_truthy(pages)) return fail("pages に承認されたページを入れる", "pages_required", "/pages");
        for (const Json& p : core::iterate(pages)) {
            ops.push_back(util::merged(Json::object({{"op", "approve"}, {"gate", gate}, {"page", core::to_int(p)}}), extra));
        }
    }
    try {
        Session session(path, person);
        // (C++: looked at again under the lock — a person may have stopped chat approvals since: Python looks once,
        // before it takes the lock, and records the approval all the same)
        if (!chat_approval_open(session.doc())) {
            return fail("この原稿は、人がチャットでの承認を止めている。承認は人が Genko の承認箱で行う", "chat_approval_off", "/project");
        }
        session.apply(ops, person);
        session.save(person);
    } catch (const core::ApplyError& exc) {
        return fail(exc.what(), "apply_failed", "/gate");
    }
    std::vector<std::int64_t> numbers;
    for (const Json& p : core::iterate(core::py_or(pages, Json::array()))) numbers.push_back(core::to_int(p));
    std::sort(numbers.begin(), numbers.end());
    Json shown = Json();
    if (!numbers.empty()) {
        shown = Json::array();
        for (std::int64_t n : numbers) shown.push_back(n);
    }
    return ToolResult(true, Json::object({{"approved", gate}, {"pages", shown}, {"character_id", character_id}, {"by", person}, {"via", actor_}}));
}

namespace {

// dict.fromkeys(values): each once, in order (TypeError for an unhashable one).
std::vector<Json> unique_keys(const Json& values) {
    std::vector<Json> out;
    for (const Json& v : core::iterate(values)) {
        core::require_hashable(v);
        if (std::none_of(out.begin(), out.end(), [&v](const Json& o) { return core::py_equals(o, v); })) out.push_back(v);
    }
    return out;
}

}  // namespace

ToolResult StudioService::request_approval(const Json& a) {
    const Json& project = a["project"];
    const Json& gate = a["gate"];
    const Json& character_ids = a["character_ids"];
    if (core::py_equals(gate, Json("sheet")) && py_truthy(character_ids)) {
        // (C++: every request made in one save, after all of them are checked — Python saves each in turn and keeps the
        // ones before a request it refuses: SPEC COMP-01a. The ids each takes are Python's: see below)
        const std::vector<Json> ids = unique_keys(character_ids);
        const fs::path path = project_path(project);
        for (const Json& cid : ids) {
            if (!py_truthy(cid)) return fail("sheet の承認依頼には character_id が要る", "character_required", "/character_id");
        }
        std::vector<Json> tickets;
        core::Document doc;
        {
            Session session(path, actor_);
            for (std::size_t i = 0; i < ids.size(); ++i) {
                // (Python reads the book again for each character, taking ids for its pages' default layers: the same
                // read here, under the same lock, so the ids the requests take after it are Python's)
                if (i > 0) (void)load_episode(path);
                tickets.push_back(apply_approval_request(session, gate, a["pages"], a["note"], ids[i]));
            }
            session.save(actor_);
            doc = session.doc();
        }
        Json requests = Json::array();
        std::vector<std::string> images;
        for (std::size_t i = 0; i < ids.size(); ++i) {
            const ToolResult done = approval_reply(path, doc, gate, a["pages"], ids[i], tickets[i]);
            Json entry = Json::object({{"character_id", ids[i]}});
            for (const auto& [key, value] : done.data.items()) entry[key] = value;
            requests.push_back(std::move(entry));
            images.insert(images.end(), done.images.begin(), done.images.end());
        }
        return ToolResult(true, Json::object({{"requests", std::move(requests)}}), {}, std::move(images));
    }
    return request_one_approval(project, gate, a["pages"], a["note"], a["character_id"]);
}

Json StudioService::apply_approval_request(Session& session, const Json& gate, const Json& pages, const Json& note, const Json& character_id) const {
    std::vector<Json> before;
    for (const Json& t : core::iterate(session.doc().tickets)) before.push_back(util::item(t, "id"));
    session.apply(Json::array({Json::object({{"op", "request_approval"}, {"gate", gate}, {"pages", pages}, {"note", note},
                                             {"character_id", character_id}})}),
                  actor_);
    for (const Json& t : core::iterate(session.doc().tickets)) {
        const Json id = util::item(t, "id");
        if (std::none_of(before.begin(), before.end(), [&id](const Json& b) { return core::py_equals(b, id); })) return t;
    }
    throw core::PyUncaught("StopIteration", "");
}

ToolResult StudioService::request_one_approval(const Json& project, const Json& gate, const Json& pages, const Json& note,
                                               const Json& character_id) {
    const fs::path path = project_path(project);
    if (!core::is_one_of(gate, {"name", "art", "sheet", "export"})) {
        return fail("依頼できる承認は name / art / sheet / export", "gate_not_available", "/gate");
    }
    const bool sheet = core::py_equals(gate, Json("sheet"));
    if (sheet && !py_truthy(character_id)) return fail("sheet の承認依頼には character_id が要る", "character_required", "/character_id");
    Json ticket;
    core::Document doc;
    {
        Session session(path, actor_);
        ticket = apply_approval_request(session, gate, pages, note, character_id);
        session.save(actor_);
        doc = session.doc();
    }
    return approval_reply(path, doc, gate, pages, character_id, ticket);
}

ToolResult StudioService::approval_reply(const fs::path& path, const core::Document& doc, const Json& gate, const Json& pages,
                                         const Json& character_id, const Json& ticket) const {
    const bool sheet = core::py_equals(gate, Json("sheet"));
    std::string pages_arg;
    for (const Json& p : util::sorted_unique(pages)) pages_arg += (pages_arg.empty() ? "" : ",") + py_str(p);
    std::string written;
    std::vector<std::string> faces;
    Json face_note;
    if (sheet) {  // (the face each candidate would give, to check before approving)
        storage::AssetStore store(path);
        const Json all = util::or_dict(py_get(doc.studio, "character_candidates"));
        core::require_hashable(character_id);
        const auto it = character_id.is_string() ? all.find(character_id.get<std::string>()) : all.end();
        const Json candidates = it != all.end() ? core::py_slice(*it, 0, 6) : Json::array();
        for (const Json& cand : core::iterate(candidates)) {
            const auto data = store.get_bytes(py_str(util::item(cand, "asset")), ".png");
            if (!data) continue;
            render::Image face = face_crop(open_picture(*data), py_get(cand, "face_box01"));
            face.thumbnail(render::Size{256, 256});
            faces.push_back(png(face));
        }
        face_note = "images は候補ごとの顔の切り出し（承認すると以後の作画の参照になる）。ずれていれば、取り込むときに"
                    "face_box01 を付け直すか、承認のときに --face x,y,w,h で位置を指定する";
        Json character = Json::object();
        for (const Json& c : core::iterate(doc.bible.characters)) {
            if (core::py_equals(py_get(c, "id"), character_id)) {
                character = c;
                break;
            }
        }
        written = studio::written_look(character);
    }
    const std::string where = util::path_text(path);
    std::string how;
    if (sheet) {
        how = "genko studio approve " + where + " sheet --character " + py_str(character_id) + " --candidate <候補id>";
    } else if (core::py_equals(gate, Json("export"))) {
        how = "genko studio export " + where + " --format pdf --out <出力先>";
    } else {
        how = "genko studio approve " + where + " " + py_str(gate) + " --pages " + pages_arg;
    }
    Json data = Json::object({{"request", util::item(ticket, "id")}, {"how_to_approve", how}});
    if (!face_note.is_null()) data["faces"] = face_note;
    if (sheet && !written.empty()) {
        data["written"] = written;
        data["check"] = "人に承認を頼むとき、written（企画書の見た目の文）も一緒に送り、候補と食い違いが無いか見てもらう。"
                        "Genko は絵と文を比べられない";
    }
    return ToolResult(true, std::move(data), {}, std::move(faces));
}

ToolResult StudioService::resolve_ticket(const Json& a) {
    return ops(a["project"], Json::array({Json::object({{"op", "resolve_ticket"}, {"id", a["ticket_id"]}, {"note", a["note"]}})}));
}

ToolResult StudioService::tickets(const Json& a) {
    const fs::path path = project_path(a["project"]);
    const Json& status = a["status"];
    Json items = Json::array();
    for (const Json& t : core::iterate(load_episode(path).tickets)) {
        if (core::py_equals(status, Json("all")) || core::py_equals(py_get(t, "status"), status)) items.push_back(t);
    }
    return ToolResult(true, Json::object({{"tickets", std::move(items)}}));
}

ToolResult StudioService::request_fix(const Json& a) {
    return ops(a["project"], Json::array({Json::object({{"op", "request_fix"},
                                                        {"page", a["page"]},
                                                        {"frame_id", a["frame_id"]},
                                                        {"candidate_id", a["candidate_id"]},
                                                        {"instruction", a["instruction"]},
                                                        {"scope", a["scope"]}})}));
}

ToolResult StudioService::ask_human(const Json& a) {
    Json op = Json::object({{"op", "ask_human"}, {"text", a["text"]}, {"item", a["item"]}});
    if (!a["page"].is_null()) op["page"] = a["page"];
    if (py_truthy(a["frame_id"])) op["frame_id"] = a["frame_id"];
    return ops(a["project"], Json::array({std::move(op)}));
}

ToolResult StudioService::proposals(const Json& a) {
    const fs::path path = project_path(a["project"]);
    const core::Document doc = load_episode(path);
    const Json all = util::or_dict(py_get(doc.studio, "proposals"), "values");
    const Json& status = a["status"];
    Json brief = Json::array();
    for (const auto& [id, p] : all.items()) {
        (void)id;
        if (!core::py_equals(status, Json("all")) && !core::py_equals(py_get(p, "status"), status)) continue;
        Json entry = Json::object();
        for (const char* key : {"id", "kind", "page", "status", "source", "by", "confidence"}) entry[key] = py_get(p, key);
        entry["count"] = static_cast<std::int64_t>(
            core::length(core::py_or(core::py_or(py_get(p, "panels"), py_get(p, "lines")), Json::array())));
        brief.push_back(std::move(entry));
    }
    return ToolResult(true, Json::object({{"proposals", std::move(brief)}}));
}

ToolResult StudioService::undo(const Json& a) {
    const fs::path path = project_path(a["project"]);
    Json rev;
    try {
        storage::ProjectLock lock(path, actor_);
        try {
            lock.try_acquire();
        } catch (const storage::LockedError& locked) {
            throw core::ApplyError(locked.what(), "locked");
        }
        rev = storage::restore(lock, actor_, false, false).rev;
    } catch (const core::ApplyError& exc) {
        return fail(exc.what(), "undo_refused", "/");
    } catch (const core::Error& exc) {
        // (journal.restore's refusals are Python's ApplyError: nothing to undo, another's change, an approval, …)
        static const char* const kRefusals[] = {"nothing_to_undo", "nothing_to_redo", "external_change", "other_actor",
                                                "needs_person",    "no_before",       "missing_snapshot", "history_missing"};
        if (std::find(std::begin(kRefusals), std::end(kRefusals), exc.code()) == std::end(kRefusals)) throw;
        return fail(exc.what(), "undo_refused", "/");
    }
    return ToolResult(true, Json::object({{"undone_revision", rev}}));
}

}  // namespace genko::service
