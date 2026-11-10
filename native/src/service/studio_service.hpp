#pragma once

#include <filesystem>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "core/json.hpp"
#include "core/model.hpp"
#include "service/args.hpp"
#include "service/result.hpp"

// StudioService: the tools an agent calls (Python's genko/studio/service.py), shared by `genko studio`, MCP and HTTP.
// Writing tools default to a dry run: they check, compile and render, and save only when commit is true and nothing is
// an error. Every change goes through the ops into project.json (one save under the project lock: a failure on the way
// writes nothing). Approvals live in HumanService, which MCP never exposes.
//
// Each tool takes its arguments as Python's method takes them (`call`: positional and keyword arguments as JSON, bound
// to the method's parameters with Python's TypeError when they do not bind) and answers a ToolResult, whose to_dict()
// is the JSON every front end sends. What Python raises out of a tool is thrown as it: core::ApplyError (an unknown
// project, a refusal Python does not turn into a ToolResult, the project lock held: code "locked"), core::PyTypeError
// and core::PyValueError (TypeError / ValueError), core::PyUncaught (any other exception, by its type name),
// core::Error("not_found") for Python's FileNotFoundError and core::Error("json") for its JSONDecodeError.

namespace genko::service {

class Session;

namespace fs = std::filesystem;

// Ops an agent may send through apply_ops (Python's AGENT_OPS): gates, locks, meta (font paths), rasters from local
// paths and structural page changes are not on the list.
bool agent_op(std::string_view op);
// Python's AGENT_OPS in sorted order (`inspect target=ops`).
const std::vector<std::string>& agent_ops_sorted();
// The tools `genko studio call` takes by name (Python's AGENT_TOOLS): the MCP tool set without project management.
const std::vector<std::string>& agent_tools_sorted();
bool agent_tool(std::string_view tool);

// The page spec presets create_project takes (SPEC_PRESETS), in Python's order.
std::span<const std::pair<std::string_view, core::PageSpec (*)()>> spec_presets();

class StudioService {
public:
    explicit StudioService(fs::path root, std::string actor = "ai:agent");

    const fs::path& root() const { return root_; }
    const std::string& actor() const { return actor_; }
    // C++: Python's keyword-only confine= of import_image and import_name (whether a path must be under --root), which
    // only its command line passes (False). Not an argument of call(): MCP and HTTP cannot pass it; the command line
    // sets it on its service. True unless set.
    void set_confine(bool confine) { confine_ = confine; }
    bool confine() const { return confine_; }

    // The tool `name` with Python's call: method(*positional, **kwargs). PyTypeError for arguments that do not bind
    // (and for a tool this build does not have: AttributeError in Python, PyUncaught("AttributeError")).
    ToolResult call(std::string_view name, const Json& positional, const Json& kwargs);
    // Whether this build has the tool (every agent tool and project management).
    static bool has_tool(std::string_view name);
    // The tool's parameters as Python's signature lists them (after self).
    static std::span<const Param> parameters(std::string_view name);

    // --- projects -----------------------------------------------------------------------------------------------------
    // project_path(name, must_exist=…): the project's folder under the root (ApplyError with Python's words).
    fs::path project_path(const Json& name, bool must_exist = true) const;

    ToolResult projects(const Json& a);
    ToolResult create_project(const Json& a);
    // --- reading ------------------------------------------------------------------------------------------------------
    ToolResult status(const Json& a);
    ToolResult style_catalog(const Json& a);
    ToolResult use_style(const Json& a);
    ToolResult next(const Json& a);
    ToolResult inspect(const Json& a);
    ToolResult upscalers(const Json& a);
    // --- writing: bible, script, name ---------------------------------------------------------------------------------
    ToolResult set_bible(const Json& a);
    ToolResult set_script(const Json& a);
    ToolResult submit_name(const Json& a);
    ToolResult apply_ops(const Json& a);
    // --- reviews, requests, tickets and proposals ------------------------------------------------------------------------
    ToolResult record_review(const Json& a);
    ToolResult record_chat_approval(const Json& a);
    ToolResult request_approval(const Json& a);
    ToolResult resolve_ticket(const Json& a);
    ToolResult tickets(const Json& a);
    ToolResult request_fix(const Json& a);
    ToolResult ask_human(const Json& a);
    ToolResult proposals(const Json& a);
    ToolResult undo(const Json& a);
    // --- art: requests, imports, candidates (studio_art.cpp) ------------------------------------------------------------
    ToolResult render(const Json& a);
    ToolResult generation_request(const Json& a);
    ToolResult import_images(const Json& a);
    ToolResult candidates(const Json& a);
    ToolResult review_candidates(const Json& a);
    ToolResult adopt(const Json& a);
    ToolResult take_panel_art(const Json& a);
    ToolResult report_regions(const Json& a);
    ToolResult finish_page(const Json& a);
    ToolResult derive(const Json& a);
    ToolResult upscale(const Json& a);
    ToolResult import_image(const Json& a);
    // --- hand-drawn names (studio_atari.cpp) ----------------------------------------------------------------------------
    ToolResult import_name(const Json& a);
    ToolResult analyze_name(const Json& a);
    ToolResult propose_lines(const Json& a);
    ToolResult review_page(const Json& a);

    // --- checks and exports (studio_output.cpp) -----------------------------------------------------------------------
    ToolResult preflight(const Json& a);
    ToolResult check(const Json& a);
    // the tool "export" (a word C++ keeps for itself)
    ToolResult export_files(const Json& a);
    ToolResult export_status(const Json& a);
    ToolResult export_proof(const Json& a);

    // _page_brief(episode, page)
    Json page_brief(const core::Document& doc, const Json& page) const;

private:
    // C++: storage::NeedsMigration for a book of v1–v3 (this build writes only v4), before a tool that writes starts.
    void refuse_old_project(const Json& project) const;
    // The tool `name` called as another tool calls it (keyword arguments, the others their defaults).
    ToolResult invoke(std::string_view name, const Json& kwargs) { return call(name, Json::array(), kwargs); }
    // _export(…): the book written into <project>/exports/<time>_<format>/ (export without background).
    ToolResult export_now(const Json& a);
    // _told_once(path, request): the notes that are the same in every request the first time only.
    Json told_once(const fs::path& path, const Json& request) const;
    // _crop_inbox(path, request_id, image, crop01): the picture cut and saved beside it; the entry for the cut one.
    std::variant<Json, ToolResult> crop_inbox(const fs::path& path, const std::string& request_id, const Json& image, const Json& crop01);
    // _ops(project, ops): the ops applied and saved under the lock; {"applied", "tickets"?} or fail("apply_failed").
    ToolResult ops(const Json& project, const Json& ops);
    // _job(project, kind, work): `work` run as a background job (jobs::start) by a service of its own (the same root and
    // actor); its reply when it ends within a little while, else the job's id for export_status.
    ToolResult job(const Json& project, const std::string& kind, std::function<ToolResult(StudioService&)> work);
    // request_approval for one character (or pages).
    ToolResult request_one_approval(const Json& project, const Json& gate, const Json& pages, const Json& note,
                                    const Json& character_id);
    // The request_approval op applied in the session as this agent; the ticket it made (StopIteration without one).
    Json apply_approval_request(Session& session, const Json& gate, const Json& pages, const Json& note, const Json& character_id) const;
    // request_approval's reply for one request (how to approve it; a sheet's faces and written look) from the book saved.
    ToolResult approval_reply(const fs::path& path, const core::Document& doc, const Json& gate, const Json& pages, const Json& character_id,
                              const Json& ticket) const;

    fs::path root_;
    std::string actor_;
    bool confine_ = true;
    bool adopting_ = false;  // (upscale called by adopt, which places the enlargement next: its pixels kept for the drawing)
};

// _waiting(items): what the work waits on, by gate.
Json waiting(const Json& items);
// chat_approval_open(episode): chat approvals are taken unless a person stopped them for this book.
bool chat_approval_open(const core::Document& doc);

}  // namespace genko::service
