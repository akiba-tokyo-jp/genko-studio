// StudioService's checks and exports (Python's genko/studio/service.py: preflight, check, export, _export,
// export_status, export_proof, _job): what stops the final export, the pre-press check a person runs in the app, the
// book written in any format into <project>/exports/ (not the official export, which is a person's), watermarked
// proofs, and long work as background jobs.

#include <algorithm>
#include <limits>
#include <chrono>
#include <thread>

#include "core/anim.hpp"
#include "core/clock.hpp"
#include "core/error.hpp"
#include "core/paths.hpp"
#include "core/pyconv.hpp"
#include "core/pynum.hpp"
#include "core/pyops.hpp"
#include "formats/checks.hpp"
#include "formats/exporting.hpp"
#include "render/anim.hpp"
#include "render/timelapse.hpp"
#include "service/book.hpp"
#include "service/jobs.hpp"
#include "service/preflight.hpp"
#include "service/service_util.hpp"
#include "service/studio_service.hpp"

namespace genko::service {

using core::py_get;
using core::py_str;
using core::py_truthy;

namespace {

// A size in pixels or dots an exporter takes as an int (width, max_height, long_edge, an animation's dpi): nothing for
// one an int cannot hold (C++: Python passes it on and makes a picture until its memory runs out; it is refused here,
// not cut to its low bits).
std::optional<int> int_size(std::int64_t value) {
    if (value < std::numeric_limits<int>::min() || value > std::numeric_limits<int>::max()) return std::nullopt;
    return static_cast<int>(value);
}

ToolResult too_large(const char* name, const Json& value) {
    return fail(std::string(name) + " が大きすぎる: " + core::py_str(value), "bad_size", std::string("/") + name);
}

// _for_agents(found): pre-press findings as an agent needs them (the same kind on many pages said once, with where;
// what only a person decides said so, not as a switch the agent does not have).
Json for_agents(const Json& found) {
    Json out = Json::array();
    std::vector<std::pair<std::string, std::size_t>> grouped;  // code → its place in out
    for (const Json& item : found) {
        Json entry = item;
        if (py_str(core::py_or(py_get(entry, "hint"), Json(""))).find("--") != std::string::npos) {
            entry["hint"] = "通すかどうかは人が決める（人が書き出すときに選ぶ）。あなたは直せるものを直すか、人に伝える";
        }
        const Json code = py_get(entry, "code");
        if (core::is_one_of(code, {"fixture_image", "upscaled", "provenance_missing"})) {
            const std::string key = code.get<std::string>();
            const auto first = std::find_if(grouped.begin(), grouped.end(), [&key](const auto& g) { return g.first == key; });
            if (first != grouped.end()) {
                Json& kept = out[first->second];
                if (!kept.contains("where")) kept["where"] = Json::array({kept["path"]});
                kept["where"].push_back(util::item(entry, "path"));
                continue;
            }
            grouped.emplace_back(key, out.size());
        }
        out.push_back(std::move(entry));
    }
    for (const auto& [code, at] : grouped) {
        (void)code;
        Json& entry = out[at];
        if (py_truthy(py_get(entry, "where"))) {
            entry["message"] = py_str(entry["message"]) + "（ほか " + std::to_string(entry["where"].size() - 1) + " か所: where）";
        }
    }
    return out;
}

// int(x) of each page number, each once, sorted.
std::vector<std::int64_t> page_numbers(const Json& pages) {
    std::vector<std::int64_t> out;
    for (const Json& p : core::iterate(pages)) {
        const std::int64_t n = core::to_int(p);
        if (std::find(out.begin(), out.end(), n) == out.end()) out.push_back(n);
    }
    std::sort(out.begin(), out.end());
    return out;
}

std::vector<std::string> texts(const std::vector<fs::path>& paths) {
    std::vector<std::string> out;
    for (const fs::path& p : paths) out.push_back(util::path_text(p));
    return out;
}

Json json_list(const std::vector<std::string>& items) {
    Json out = Json::array();
    for (const std::string& s : items) out.push_back(s);
    return out;
}

}  // namespace

ToolResult StudioService::preflight(const Json& a) {
    const fs::path path = project_path(a["project"]);
    const core::Document episode = load_episode(path);
    formats::checks::StudioCheck options;
    options.allow_fixture = py_truthy(a["allow_fixture"]);
    options.force = py_truthy(a["force"]);
    const Json report = formats::checks::studio_check(episode, path, options);
    const Json found = formats::checks::book(episode, path);  // (and what a person sees in 入稿前の点検: counted here, listed by check)
    return ToolResult(true, Json::object({{"ready", report["ok"]},
                                          {"errors", for_agents(report["errors"])},
                                          {"warnings", for_agents(report["warnings"])},
                                          {"dpi", report["dpi"]},
                                          {"min_dpi", report["min_dpi"]},
                                          {"checks", Json::object({{"errors", found["errors"]},
                                                                   {"warnings", found["warnings"]},
                                                                   {"note", "点検の中身は check で読む（pages でページを絞れる）"}})}}));
}

ToolResult StudioService::check(const Json& a) {
    const fs::path path = project_path(a["project"]);
    const Json report = formats::checks::book(load_episode(path), path);
    Json found = report["issues"];
    const Json& pages = a["pages"];
    std::vector<std::int64_t> wanted;
    if (py_truthy(pages)) {
        wanted = page_numbers(pages);
        Json kept = Json::array();
        for (const Json& issue : found) {
            const Json page = py_get(issue, "page");
            core::require_hashable(page);
            if (page.is_null() || std::any_of(wanted.begin(), wanted.end(), [&page](std::int64_t n) { return core::py_equals(page, Json(n)); })) {
                kept.push_back(issue);
            }
        }
        found = std::move(kept);
    }
    // (the list goes in "checks", as preflight's does: "issues" is the reply's own list of tool problems)
    std::int64_t errors = 0;
    std::int64_t warnings = 0;
    for (const Json& issue : found) {
        errors += core::py_equals(py_get(issue, "level"), Json("error")) ? 1 : 0;
        warnings += core::py_equals(py_get(issue, "level"), Json("warning")) ? 1 : 0;
    }
    Json data = Json::object({{"errors", errors}, {"warnings", warnings}, {"checks", std::move(found)}});
    if (py_truthy(pages)) {
        Json listed = Json::array();
        for (const std::int64_t n : wanted) listed.push_back(n);
        data["pages"] = std::move(listed);
    }
    return ToolResult(true, std::move(data));
}

ToolResult StudioService::export_files(const Json& a) {
    if (py_truthy(a["background"])) {
        return job(a["project"], "export", [a](StudioService& service) { return service.export_now(a); });
    }
    return export_now(a);
}

ToolResult StudioService::export_now(const Json& a) {
    const fs::path path = project_path(a["project"]);
    const Json& format = a["format"];
    const Json& pages = a["pages"];
    const Json long_edge = py_truthy(a["long_edge"]) ? a["long_edge"] : Json(core::py_equals(format, Json("kindle")) ? 2560 : 2048);
    const std::int64_t long_edge_px = core::to_int(long_edge);  // (each format's own default)
    const std::string stamp = core::local_time_text("%Y%m%d-%H%M%S");
    const std::string movie = py_str(a["movie"]);
    if (core::py_equals(format, Json("timelapse"))) {
        if (!pages.is_null() && core::length(pages) != 1) return fail("timelapse の pages は 1 ページだけ（省略で全ページ）", "bad_pages", "/pages");
        const fs::path out = path / "exports" / core::path_from_utf8(stamp + "_timelapse") / core::path_from_utf8("timelapse." + movie);
        const std::optional<Json> page = py_truthy(pages) ? std::optional<Json>(Json(core::to_int(core::subscript(pages, 0)))) : std::nullopt;
        int frames = -1;
        fs::path written;
        try {
            const std::optional<double> seconds = a["seconds"].is_null() ? std::nullopt : std::optional<double>(core::to_float(a["seconds"]));
            written = render::timelapse::export_timelapse(path, out, page, core::to_float(a["fps"]), seconds, movie, 2.0, &frames);
        } catch (const core::PyValueError& exc) {
            return fail(exc.what(), "export_failed", "/");
        }
        const std::int64_t recorded = static_cast<std::int64_t>(render::timelapse::frames(path, page).size());
        const std::string file = util::path_text(written);
        return ToolResult(true, Json::object({{"folder", util::path_text(written.parent_path())}, {"files", Json::array({file})},
                                              {"frames", frames >= 0 ? std::int64_t{frames} : recorded}, {"recorded", recorded}}),
                          {}, {}, {file});
    }
    if (core::py_equals(format, Json("animation"))) {
        const core::Document episode = load_episode(path);
        if (!py_truthy(pages) || core::length(pages) != 1) return fail("animation の pages はアニメーションのページ 1 つ", "bad_pages", "/pages");
        const Json first = core::subscript(pages, 0);
        const auto at = page_at(episode, Json(core::to_int(first)));
        if (!at || !core::anim::is_animation(episode.page(*at))) {
            return fail(py_str(first) + " ページはアニメーションではない（set_animation）", "bad_pages", "/pages");
        }
        const core::Page& page = episode.page(*at);
        const fs::path out = path / "exports" / core::path_from_utf8(stamp + "_animation") /
                             core::path_from_utf8("p" + util::zero3(page.index.json()) + "." + movie);
        std::vector<fs::path> written;
        try {
            const std::optional<int> dpi = py_truthy(a["dpi"]) ? int_size(core::to_int(a["dpi"])) : std::optional<int>(100);
            if (!dpi) return too_large("dpi", a["dpi"]);
            std::optional<int> width;
            if (!core::py_equals(a["width"], Json(800))) {
                width = int_size(core::to_int(a["width"]));
                if (!width) return too_large("width", a["width"]);
            }
            written = render::anim::export_animation(page, out, &episode, *dpi, movie, true, "trim", width);
        } catch (const core::PyValueError& exc) {
            return fail(exc.what(), "export_failed", "/");
        }
        const std::vector<std::string> files = texts(written);
        return ToolResult(true, Json::object({{"folder", util::path_text(out.parent_path())}, {"files", json_list(files)},
                                              {"frames", core::anim::frames_of(page)}}),
                          {}, {}, files);
    }
    core::require_hashable(format);
    if (!format.is_string() || formats::format(format.get<std::string>()) == nullptr) {
        std::string keys;
        for (const formats::Format& f : formats::formats()) keys += std::string(f.key) + " / ";
        return fail("format は " + keys + "timelapse / animation", "bad_format", "/format");
    }
    const std::string key = format.get<std::string>();
    const core::Document episode = load_episode(path);
    if (!pages.is_null()) {
        const std::int64_t count = static_cast<std::int64_t>(episode.pages.size());
        bool bad = false;
        for (const Json& p : core::iterate(pages)) {
            const std::int64_t n = core::to_int(p);
            bad = bad || !(1 <= n && n <= count);
        }
        if (bad || !py_truthy(pages)) return fail("pages は 1〜" + std::to_string(count) + " のページ番号", "bad_pages", "/pages");
    }
    const fs::path out = path / "exports" / core::path_from_utf8(stamp + "_" + key);
    formats::RunOptions options;
    options.actor = actor_;
    if (py_truthy(a["dpi"])) options.dpi = core::to_int(a["dpi"]);
    const std::optional<int> width = int_size(core::to_int(a["width"]));
    if (!width) return too_large("width", a["width"]);
    const std::optional<int> max_height = int_size(core::to_int(a["max_height"]));
    if (!max_height) return too_large("max_height", a["max_height"]);
    const std::optional<int> edge = int_size(long_edge_px);
    if (!edge) return too_large("long_edge", long_edge);
    options.width = *width;
    options.max_height = *max_height;
    options.long_edge = *edge;
    options.jpeg = py_truthy(a["jpeg"]);
    options.spreads = py_truthy(a["spreads"]);
    options.area = py_str(a["area"]);
    options.color = py_str(a["color"]);
    options.icc = py_truthy(a["icc"]) ? py_str(a["icc"]) : std::string();
    if (py_truthy(pages)) options.pages = page_numbers(pages);
    options.dots = py_truthy(a["dots"]);
    if (!a["screen"].is_null()) options.screen = a["screen"];
    const Json result = formats::run(episode, std::nullopt, key, out, options);
    if (!py_truthy(py_get(result, "ok"))) {
        const Json error = py_get(result, "error");
        return fail(py_truthy(error) ? py_str(error) : std::string("書き出せなかった"), "export_failed", "/");
    }
    std::vector<std::string> files;
    for (const Json& f : result["files"]) files.push_back(py_str(f));
    return ToolResult(true, Json::object({{"folder", util::path_text(out)}, {"files", result["files"]}}), {}, {}, std::move(files));
}

ToolResult StudioService::export_status(const Json& a) {
    const double given = py_truthy(a["wait_s"]) ? core::to_float(a["wait_s"]) : 0.0;
    const double capped = given < 60.0 ? given : 60.0;  // (min(60.0, …), max(0.0, …) as Python takes them)
    const double wait = capped > 0.0 ? capped : 0.0;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::duration_cast<std::chrono::steady_clock::duration>(std::chrono::duration<double>(wait));
    Json record = jobs::status(project_path(a["project"]), a["job"]);
    while (py_truthy(py_get(record, "ok")) && core::py_equals(py_get(record, "status"), Json("running")) && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        record = jobs::status(project_path(a["project"]), a["job"]);
    }
    if (!py_truthy(py_get(record, "ok"))) return fail(py_str(util::item(record, "error")), "no_job", "/job");
    std::vector<std::string> files;
    for (const Json& f : core::iterate(core::py_or(py_get(core::py_or(py_get(record, "result"), Json::object()), "files"), Json::array()))) {
        files.push_back(py_str(f));
    }
    Json data = Json::object();
    for (const auto& [key, value] : record.items()) {
        if (key != "ok") data[key] = value;
    }
    return ToolResult(true, std::move(data), {}, {}, std::move(files));
}

ToolResult StudioService::export_proof(const Json& a) {
    if (py_truthy(a["background"])) {
        return job(a["project"], "export_proof", [a](StudioService& service) {
            Json now = a;
            now["background"] = false;
            return service.export_proof(now);
        });
    }
    const fs::path path = project_path(a["project"]);
    const core::Document episode = load_episode(path);
    if (!core::is_one_of(a["format"], {"pdf", "png"})) return fail("format は pdf か png", "bad_format", "/format");
    const std::vector<std::string> files = texts(preflight::export_proof(episode, path, a["format"].get<std::string>()));
    return ToolResult(true, Json::object({{"files", json_list(files)}, {"dpi", preflight::kProofDpi}, {"watermark", true}}), {}, {}, files);
}

ToolResult StudioService::job(const Json& project, const std::string& kind, std::function<ToolResult(StudioService&)> work) {
    const fs::path path = project_path(project);
    const Json reply = jobs::start(
        path, kind,
        [root = root_, actor = actor_, work = std::move(work)] {
            StudioService service(root, actor);
            return work(service).to_dict();
        },
        actor_);
    Json data = Json::object();
    for (const auto& [key, value] : reply.items()) {
        if (key != "ok" && key != "issues" && key != "files") data[key] = value;
    }
    if (!py_truthy(py_get(reply, "ok"))) return ToolResult(false, std::move(data));
    std::vector<std::string> files;
    for (const Json& f : core::iterate(core::py_or(py_get(reply, "files"), Json::array()))) files.push_back(py_str(f));
    return ToolResult(true, std::move(data), {}, {}, std::move(files));
}

}  // namespace genko::service
