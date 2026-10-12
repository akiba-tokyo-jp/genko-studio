#include "service/evaluate.hpp"

#include <algorithm>
#include <cerrno>
#include <system_error>
#include <vector>

#include "core/actor.hpp"
#include "core/command_bus.hpp"
#include "core/error.hpp"
#include "core/paths.hpp"
#include "core/pyconv.hpp"
#include "core/pynum.hpp"
#include "core/pyops.hpp"
#include "service/book.hpp"
#include "service/briefs.hpp"
#include "service/service_util.hpp"
#include "service/text.hpp"
#include "storage/fsutil.hpp"
#include "storage/journal.hpp"
#include "studio/files.hpp"
#include "studio/state.hpp"

namespace genko::service {

namespace fs = std::filesystem;
using core::Num;
using core::py_get;
using core::py_str;
using core::py_truthy;

namespace {

// A dict's key as json.dumps writes it (an int as its str, None as "null", True as "true").
std::string json_key(const Json& key) {
    if (key.is_boolean()) return key.get<bool>() ? "true" : "false";
    if (key.is_null()) return "null";
    if (key.is_number_float()) return core::py_float_repr(key.get<double>());
    return py_str(key);
}

// counts[key] += by, in a dict keyed as Python keys it
void count(PyDict& counts, const Json& key, const Num& by) {
    const Json* had = counts.find(key);
    const Num now = (had != nullptr ? core::to_num(*had) : Num(0)) + by;
    counts.set(key, now.json());
}

Json as_object(const PyDict& dict) {
    Json out = Json::object();
    for (const auto& [key, value] : dict.items()) out[json_key(key)] = value;
    return out;
}

}  // namespace

namespace evaluate {

Json stats(const fs::path& project) {
    const core::Document doc = load_episode(project);
    Json pages = Json::array();
    Json panels = Json::array();
    for (const auto& page_ptr : doc.pages) {
        const core::Page& page = *page_ptr;
        const Json plan = page.plan && py_truthy(*page.plan) ? *page.plan : Json::object();
        const Json submits = py_get(plan, "submits", Json(core::truthy_at(plan, "name") ? 1 : 0));
        pages.push_back(Json::object({{"page", page.index.json()},
                                      {"name_submits", core::to_int(submits)},
                                      {"name_ok", page.name_ok},
                                      {"art_ok", page.art_ok},
                                      {"stage", page.stage}}));
        for (const core::Frame* frame : page.leaf_frames()) {
            const Json panel = frame->panel && py_truthy(*frame->panel) ? *frame->panel : Json::object();
            if (!py_truthy(panel)) continue;
            const Json attempts = util::or_dict(py_get(panel, "attempts"));
            std::int64_t cast = 0;
            for (const Json& c : core::iterate(py_get(panel, "characters", Json::array()))) cast += c.is_object() ? 1 : 0;
            panels.push_back(Json::object({{"page", page.index.json()},
                                           {"frame_id", frame->id},
                                           {"slot", py_get(panel, "slot")},
                                           {"status", py_get(panel, "status")},
                                           {"characters", cast},
                                           {"requests", py_get(attempts, "requests", Json(0))},
                                           {"images", py_get(attempts, "images", Json(0))},
                                           {"fix_rounds", py_get(attempts, "fix_rounds", Json(0))}}));
        }
    }
    std::vector<Json> adopted;
    for (const Json& p : panels) {
        if (core::py_equals(p["status"], Json("adopted"))) adopted.push_back(p["images"]);
    }
    const std::vector<Json> calls = studio::read_tools(project);
    std::vector<const Json*> failed;
    for (const Json& c : calls) {
        if (!core::truthy_at(c, "ok")) failed.push_back(&c);
    }
    PyDict by_tool;
    for (const Json& c : calls) {
        const Json tool = util::item(c, "tool");
        const Json* row = by_tool.find(tool);
        Json updated = row != nullptr ? *row : Json::object({{"calls", 0}, {"failed", 0}});
        updated["calls"] = (core::to_num(updated["calls"]) + Num(1)).json();
        updated["failed"] = (core::to_num(updated["failed"]) + Num(core::truthy_at(c, "ok") ? 0 : 1)).json();
        by_tool.set(tool, std::move(updated));
    }
    PyDict codes;
    for (const Json* c : failed) {
        Json listed = py_get(*c, "codes");
        if (!py_truthy(listed)) {
            const Json error = py_get(*c, "error", Json("error"));
            if (!error.is_string()) throw core::PyTypeError("'" + core::py_type_name(error) + "' object is not subscriptable");
            listed = Json::array({text::head(error.get<std::string>(), 60)});
        }
        for (const Json& code : core::iterate(listed)) count(codes, code, Num(1));
    }
    std::vector<Json> ms;
    for (const Json& c : calls) ms.push_back(py_get(c, "ms", Json(0)));
    std::stable_sort(ms.begin(), ms.end(), [](const Json& a, const Json& b) { return core::py_less(a, b); });
    PyDict tickets;
    for (const Json& t : core::iterate(doc.tickets)) count(tickets, py_get(t, "kind", Json("?")), Num(1));
    Num submits_total(0);
    for (const Json& p : pages) submits_total = submits_total + core::to_num(p["name_submits"]);
    Num fix_total(0);
    for (const Json& p : panels) fix_total = fix_total + core::to_num(p["fix_rounds"]);
    Json median;
    Json most;
    if (!adopted.empty()) {
        std::vector<Json> sorted = adopted;
        std::stable_sort(sorted.begin(), sorted.end(), [](const Json& a, const Json& b) { return core::py_less(a, b); });
        const std::size_t n = sorted.size();
        median = n % 2 == 1 ? sorted[n / 2] : ((core::to_num(sorted[n / 2 - 1]) + core::to_num(sorted[n / 2])) / Num(2)).json();
        most = adopted.front();
        for (const Json& v : adopted) {
            if (core::py_less(v, most, ">")) most = v;
        }
    }
    Json tool_calls = Json::object({{"total", static_cast<std::int64_t>(calls.size())},
                                    {"failed", static_cast<std::int64_t>(failed.size())},
                                    {"by_tool", as_object(by_tool)},
                                    {"failure_reasons", as_object(codes)},
                                    {"ms_p50", ms.empty() ? Json() : ms[ms.size() / 2]},
                                    {"ms_p95", ms.empty() ? Json() : ms[static_cast<std::size_t>(static_cast<double>(ms.size()) * 0.95)]}});
    return Json::object({{"ok", true},
                         {"pages", std::move(pages)},
                         {"name_submits_total", submits_total.json()},
                         {"panels", panels},
                         {"images_per_adopted_panel",
                          Json::object({{"median", median}, {"max", most}, {"panels", static_cast<std::int64_t>(adopted.size())}})},
                         {"fix_rounds_total", fix_total.json()},
                         {"tool_calls", std::move(tool_calls)},
                         {"tickets", as_object(tickets)}});
}

Json audit(const fs::path& project) {
    const std::vector<Json> entries = storage::journal::audit_entries(project);
    Json violations = Json::array();
    for (const Json& e : entries) {
        if (!core::can_approve(py_str(py_get(e, "actor", Json(""))))) violations.push_back(e);
    }
    const core::Document doc = load_episode(project);
    std::vector<Json> records;
    for (const Json& r : core::iterate(py_get(doc.studio, "approvals", Json::array()))) {
        if (!core::can_approve(py_str(py_get(r, "by", Json(""))))) records.push_back(r);
    }
    PyDict by_actor;
    Num changes(0);
    for (const Json& e : entries) {
        const Num n(static_cast<std::int64_t>(core::length(py_get(e, "changes", Json::array()))));
        count(by_actor, py_get(e, "actor", Json("?")), n);
        changes = changes + n;
    }
    const bool ok = violations.empty() && records.empty();
    for (const Json& r : records) violations.push_back(Json::object({{"approval", r}}));
    Json shown = Json::array();
    for (std::size_t i = entries.size() > 100 ? entries.size() - 100 : 0; i < entries.size(); ++i) {
        const Json& e = entries[i];
        shown.push_back(Json::object({{"rev", py_get(e, "rev")},
                                      {"at", py_get(e, "at")},
                                      {"actor", py_get(e, "actor")},
                                      {"via", py_get(e, "via")},
                                      {"changes", py_get(e, "changes", Json::array())}}));
    }
    std::error_code ec;
    const bool audited = fs::is_regular_file(storage::journal::audit_file(project), ec);
    return Json::object({{"ok", ok},
                         {"changes", changes.json()},
                         {"changes_by_actor", as_object(by_actor)},
                         {"violations", std::move(violations)},
                         {"entries", std::move(shown)},
                         {"note", audited ? Json() : Json("studio/audit.jsonl が無い（M5 より前の保存だけのプロジェクト）")}});
}

}  // namespace evaluate

namespace adopt {

namespace {

constexpr const char* kBriefFromPlan[] = {"shot", "angle", "characters", "location_id", "time", "action", "emotion", "fx", "emphasis", "beat_ids"};
constexpr const char* kBriefLater[] = {"props", "sfx_at", "bleed", "slant"};

// Drafts(project): the M0 sidecar files under studio/drafts/ (jsonutil.read_json: the default when there is no file).
struct Drafts {
    fs::path root;
    Json read(const std::string& name, const Json& fallback = Json()) const { return studio::read_json(root / core::path_from_utf8(name), fallback); }
    Json bible() const { return read("bible.json"); }
    Json script() const { return read("script.json"); }
    Json name(const Json& page) const { return read("name/p" + util::zero3(page) + ".json"); }
    Json reviews() const { return read("reviews.json", Json::object()); }
    Json requests() const { return read("requests.json", Json::array()); }
};

}  // namespace

Json adopt_drafts(const fs::path& project) {
    refuse_old_book(project);  // (C++: it writes the book, which this build writes only as v4)
    const Drafts drafts{project / "studio" / "drafts"};
    std::error_code ec;
    if (!fs::is_directory(drafts.root, ec)) {
        return Json::object({{"ok", false}, {"error", util::path_text(drafts.root) + " がない（M0 の下書きはない）"}});
    }
    const fs::path adopted = drafts.root.parent_path() / "drafts.adopted";
    Json counts = Json::object({{"bible", 0}, {"script", 0}, {"names", 0}, {"reviews", 0}, {"tickets", 0}});
    const auto bump = [&counts](const char* key) { counts[key] = counts[key].get<std::int64_t>() + 1; };
    {
        Session session(project, kActor);
        Work& work = session.work();
        Json ops = Json::array();
        const Json bible = drafts.bible();
        if (!bible.is_null() && !studio::bible(work.doc)) {
            Json doc = Json::object();
            if (!bible.is_object()) core::raise_attribute_error(bible, "items");
            for (const auto& [key, value] : bible.items()) {
                if (key != "plot" && key != "characters" && key != "constraints") doc[key] = value;
            }
            ops.push_back(Json::object({{"op", "set_bible"},
                                        {"plot", py_get(bible, "plot", Json(""))},
                                        {"characters", py_get(bible, "characters", Json::array())},
                                        {"constraints", py_get(bible, "constraints", Json::array())}}));
            ops.push_back(Json::object({{"op", "set_studio"}, {"bible_doc", std::move(doc)}}));
            counts["bible"] = 1;
        }
        const Json script = drafts.script();
        if (!script.is_null() && !studio::script(work.doc)) {
            ops.push_back(Json::object({{"op", "set_script"}, {"script", script}}));
            counts["script"] = 1;
        }
        const Json reviews = drafts.reviews();
        for (const auto& page_ptr : work.doc.pages) {
            const core::Page& page = *page_ptr;
            const Json record = drafts.name(page.index.json());
            if (record.is_null() || studio::name(work.doc, page.index)) continue;
            const Json plan = core::py_or(py_get(record, "plan"), Json::object());
            const Json slots = core::py_or(py_get(record, "slot_to_frame"), Json::object());
            std::vector<std::string> leaves;
            for (const core::Frame* f : page.leaf_frames()) leaves.push_back(f->id);
            for (const Json& panel : core::iterate(py_get(plan, "panels", Json::array()))) {
                const Json slot = py_get(panel, "slot");
                core::require_hashable(slot);
                if (!slots.is_object()) core::raise_attribute_error(slots, "get");
                const Json frame_id = slot.is_string() ? py_get(slots, slot.get<std::string>()) : Json();
                core::require_hashable(frame_id);
                if (!frame_id.is_string() || std::find(leaves.begin(), leaves.end(), frame_id.get<std::string>()) == leaves.end()) continue;
                Json set = Json::object({{"slot", util::item(panel, "slot")}});
                for (const char* key : kBriefFromPlan) {
                    if (panel.contains(key)) set[key] = panel[key];
                }
                for (const char* key : kBriefLater) {
                    if (!py_get(panel, key).is_null()) set[key] = panel[key];
                }
                ops.push_back(Json::object({{"op", "set_panel"}, {"page", page.index.json()}, {"frame_id", frame_id}, {"set", std::move(set)}}));
            }
            ops.push_back(Json::object({{"op", "set_page_plan"},
                                        {"page", page.index.json()},
                                        {"plan", Json::object({{"name", plan},
                                                               {"input_hash", py_get(record, "input_hash")},
                                                               {"rev", work.doc.revision},
                                                               {"turn_role", py_get(plan, "turn_role")},
                                                               {"slot_to_frame", slots},
                                                               {"reading_order", py_get(record, "reading_order", Json::array())},
                                                               {"by", py_get(record, "by", Json(kActor))}})}}));
            bump("names");
            const Json review = py_get(reviews, py_str(page.index.json()));
            if (py_truthy(review)) {
                ops.push_back(Json::object({{"op", "record_review"},
                                            {"page", page.index.json()},
                                            {"kind", "name"},
                                            {"score", py_get(review, "score")},
                                            {"notes", py_get(review, "notes", Json(""))},
                                            {"input_hash", py_get(review, "input_hash")}}));
                bump("reviews");
            }
        }
        if (!ops.empty()) work.apply(ops, kActor);
        // tickets keep who asked: approval requests by the agent, comments by the person
        for (const Json& item : core::iterate(drafts.requests())) {
            if (!core::py_equals(py_get(item, "status"), Json("open"))) continue;
            const Json kind = py_get(item, "kind");
            if (core::py_equals(kind, Json("approval"))) {
                const std::string by = py_str(core::py_or(py_get(item, "by"), Json(kActor)));
                work.apply(Json::array({Json::object({{"op", "request_approval"},
                                                      {"gate", py_get(item, "gate", Json("name"))},
                                                      {"pages", py_get(item, "pages", Json::array())},
                                                      {"note", py_get(item, "note", Json(""))}})}),
                           by);
            } else if (core::py_equals(kind, Json("comment")) && page_at(work.doc, py_get(item, "page"))) {
                const Json page = util::item(item, "page");
                if (util::name_of(work.doc, page) &&
                    core::py_less(py_get(core::py_or(drafts.name(page), Json::object()), "seq", Json(0)), py_get(item, "seq", Json(0)), ">")) {
                    continue;  // the name was resubmitted after the comment
                }
                const std::string by = py_str(core::py_or(py_get(item, "by"), Json("human")));
                work.apply(Json::array({Json::object({{"op", "request_fix"}, {"page", page}, {"instruction", py_get(item, "text", Json(""))},
                                                      {"scope", "page"}})}),
                           by);
            } else {
                continue;
            }
            bump("tickets");
        }
        // (C++: the rename below is checked before the save — Python saves, then fails to rename)
        if (fs::exists(fs::symlink_status(adopted, ec))) {
            const bool empty_dir = fs::is_directory(fs::symlink_status(adopted, ec)) && fs::is_empty(adopted, ec);
            if (!empty_dir) {
                const int number = fs::is_directory(fs::symlink_status(adopted, ec)) ? ENOTEMPTY : ENOTDIR;
                throw core::PyUncaught("OSError", "[Errno " + std::to_string(number) + "] " + std::generic_category().message(number) + ": " +
                                                      core::py_repr_str(util::path_text(drafts.root)) + " -> " +
                                                      core::py_repr_str(util::path_text(adopted)));
            }
        }
        session.save(kActor);
        fs::rename(drafts.root, adopted, ec);
        if (ec) {
            throw core::PyUncaught("OSError", "[Errno " + std::to_string(ec.value()) + "] " + ec.message() + ": " +
                                                  core::py_repr_str(util::path_text(drafts.root)) + " -> " +
                                                  core::py_repr_str(util::path_text(adopted)));
        }
    }
    return Json::object({{"ok", true}, {"adopted", std::move(counts)}});
}

}  // namespace adopt
}  // namespace genko::service
