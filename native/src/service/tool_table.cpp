// The tools by name, with the parameters of Python's methods (StudioService.call: what MCP, HTTP and `genko studio call`
// hand a tool).

#include <vector>

#include "core/error.hpp"
#include "service/book.hpp"
#include "service/studio_service.hpp"

namespace genko::service {

namespace {

using Method = ToolResult (StudioService::*)(const Json&);

struct Tool {
    std::string_view name;
    std::vector<Param> params;
    Method method = nullptr;
    bool writes = false;  // (it changes the book, its assets, requests or previews: a book of v1–v3 is refused first)
};

// The tools that write the book or what is kept beside it for the book (its assets, requests, inbox, previews of a
// change), even when they only try a change (commit false): C++ refuses a book of v1–v3 before they start (this build
// writes only v4: `genko migrate` makes a v4 copy). The others read (render's and review_page's pictures, the exports,
// a work item's lease are not the book), as Python's.
bool writing_tool(std::string_view name) {
    for (const char* w : {"use_style", "set_bible", "set_script", "submit_name", "apply_ops", "record_review", "record_chat_approval",
                          "request_approval", "resolve_ticket", "request_fix", "ask_human", "undo", "generation_request", "import_images",
                          "review_candidates", "adopt", "take_panel_art", "report_regions", "finish_page", "derive", "upscale", "import_name",
                          "analyze_name", "propose_lines", "import_image"}) {
        if (name == w) return true;
    }
    return false;
}

Param required(const char* name) { return Param{name, std::nullopt, false}; }
Param optional(const char* name, Json fallback) { return Param{name, std::move(fallback), false}; }

const std::vector<Tool>& tools() {
    static const std::vector<Tool> table = [] {
        const Json none;
        std::vector<Tool> t;
        t.push_back({"projects", {}, &StudioService::projects});
        t.push_back({"create_project",
                     {required("name"), required("title"), required("pages"), optional("spec_preset", "commercial-b4"), optional("binding", "right")},
                     &StudioService::create_project});
        t.push_back({"status", {required("project")}, &StudioService::status});
        t.push_back({"style_catalog", {optional("project", none), optional("style_id", none), optional("title", none)}, &StudioService::style_catalog});
        t.push_back({"use_style", {required("project"), required("style_id"), optional("commit", false)}, &StudioService::use_style});
        t.push_back({"next", {required("project"), optional("limit", 5), optional("claim", false)}, &StudioService::next});
        t.push_back({"inspect",
                     {required("project"), required("target"), optional("page", none), optional("frame_id", none), optional("op", none),
                      optional("full", false)},
                     &StudioService::inspect});
        t.push_back({"render",
                     {required("project"), required("page"), optional("mode", "name"), optional("max_px", 1024), optional("frame_id", none),
                      optional("kind", none), optional("candidate_id", none), optional("layer_id", none)},
                     &StudioService::render});
        t.push_back({"generation_request",
                     {required("project"), optional("page", none), optional("frame_id", none), optional("character_id", none),
                      optional("location_id", none), optional("purpose", none), optional("mode", "new"), optional("parent", none),
                      optional("tool", none), optional("instruction", none), optional("regions", none), optional("focus_character", none)},
                     &StudioService::generation_request});
        t.push_back({"import_images", {required("project"), required("request_id"), required("images"), optional("preview", true)},
                     &StudioService::import_images});
        t.push_back({"candidates",
                     {required("project"), optional("page", none), optional("frame_id", none), optional("character_id", none),
                      optional("location_id", none)},
                     &StudioService::candidates});
        t.push_back({"review_candidates", {required("project"), required("page"), required("frame_id"), required("reviews")},
                     &StudioService::review_candidates});
        t.push_back({"adopt",
                     {required("project"), required("candidate_id"), optional("page", none), optional("frame_id", none), optional("to", "art"),
                      optional("fit", none), optional("offset_mm", none), optional("scale", none), optional("location_id", none),
                      optional("regions", none), optional("upscale", true), optional("method", "genko"), optional("preview", true)},
                     &StudioService::adopt});
        t.push_back({"take_panel_art",
                     {required("project"), required("request_id"), required("image"), optional("regions", none), optional("upscale", true),
                      optional("method", "genko"), optional("crop01", none)},
                     &StudioService::take_panel_art});
        t.push_back({"report_regions", {required("project"), required("page"), required("frame_id"), required("regions")},
                     &StudioService::report_regions});
        t.push_back({"finish_page", {required("project"), required("page"), optional("commit", false)}, &StudioService::finish_page});
        t.push_back({"preflight", {required("project"), optional("allow_fixture", false), optional("force", false)}, &StudioService::preflight});
        t.push_back({"check", {required("project"), optional("pages", none)}, &StudioService::check});
        t.push_back({"export",
                     {required("project"), optional("format", "png"), optional("pages", none), optional("dpi", none), optional("area", "bleed"),
                      optional("width", 800), optional("max_height", 1280), optional("long_edge", none), optional("jpeg", false),
                      optional("spreads", false), optional("color", "auto"), optional("icc", none), optional("fps", 12), optional("seconds", none),
                      optional("movie", "webp"), optional("background", false), optional("dots", false), optional("screen", none)},
                     &StudioService::export_files});
        t.push_back({"export_status", {required("project"), required("job"), optional("wait_s", 0)}, &StudioService::export_status});
        t.push_back({"export_proof", {required("project"), optional("format", "png"), optional("background", false)}, &StudioService::export_proof});
        t.push_back({"derive",
                     {required("project"), required("page"), required("frame_id"), optional("kind", "lineart"), optional("candidate_id", none),
                      optional("params", none)},
                     &StudioService::derive});
        t.push_back({"upscale",
                     {required("project"), required("page"), required("frame_id"), optional("candidate_id", none), optional("scale", none),
                      optional("method", "genko"), optional("background", false)},
                     &StudioService::upscale});
        t.push_back({"upscalers", {}, &StudioService::upscalers});
        t.push_back({"import_name",
                     {required("project"), required("files"), optional("start_page", 1), optional("align", "auto")},
                     &StudioService::import_name});
        t.push_back({"analyze_name", {required("project"), required("page"), optional("params", none)}, &StudioService::analyze_name});
        t.push_back({"propose_lines", {required("project"), required("page"), required("lines")}, &StudioService::propose_lines});
        t.push_back({"review_page", {required("project")}, &StudioService::review_page});
        // (Python's keyword-only confine= is not an argument here: set_confine — MCP and HTTP cannot pass it)
        t.push_back({"import_image", {required("project"), optional("path", none), optional("png_base64", none)},
                     &StudioService::import_image});
        t.push_back({"set_bible", {required("project"), required("bible"), optional("commit", false)}, &StudioService::set_bible});
        t.push_back({"set_script", {required("project"), required("script"), optional("commit", false)}, &StudioService::set_script});
        t.push_back({"submit_name",
                     {required("project"), required("plan"), optional("commit", false), optional("replace", false), optional("max_px", 1024)},
                     &StudioService::submit_name});
        t.push_back({"apply_ops", {required("project"), required("ops"), optional("commit", false)}, &StudioService::apply_ops});
        t.push_back({"record_review", {required("project"), required("page"), required("score"), required("notes")}, &StudioService::record_review});
        t.push_back({"record_chat_approval",
                     {required("project"), required("gate"), required("message"), optional("pages", none), optional("character_id", none),
                      optional("candidate_id", none), optional("face_box01", none), optional("person", none)},
                     &StudioService::record_chat_approval});
        t.push_back({"request_approval",
                     {required("project"), required("gate"), required("pages"), optional("note", ""), optional("character_id", none),
                      optional("character_ids", none)},
                     &StudioService::request_approval});
        t.push_back({"resolve_ticket", {required("project"), required("ticket_id"), required("note")}, &StudioService::resolve_ticket});
        t.push_back({"tickets", {required("project"), optional("status", "open")}, &StudioService::tickets});
        t.push_back({"request_fix",
                     {required("project"), required("page"), required("instruction"), optional("frame_id", none), optional("candidate_id", none),
                      optional("scope", "frame")},
                     &StudioService::request_fix});
        t.push_back({"ask_human",
                     {required("project"), required("text"), optional("page", none), optional("frame_id", none), optional("item", none)},
                     &StudioService::ask_human});
        t.push_back({"proposals", {required("project"), optional("status", "open")}, &StudioService::proposals});
        t.push_back({"undo", {required("project")}, &StudioService::undo});
        for (Tool& tool : t) tool.writes = writing_tool(tool.name);
        return t;
    }();
    return table;
}

const Tool* find(std::string_view name) {
    for (const Tool& tool : tools()) {
        if (tool.name == name) return &tool;
    }
    return nullptr;
}

}  // namespace

bool StudioService::has_tool(std::string_view name) { return find(name) != nullptr; }

void StudioService::refuse_old_project(const Json& project) const { refuse_old_book(project_path(project)); }

std::span<const Param> StudioService::parameters(std::string_view name) {
    const Tool* tool = find(name);
    if (tool == nullptr) return {};
    return tool->params;
}

ToolResult StudioService::call(std::string_view name, const Json& positional, const Json& kwargs) {
    const Tool* tool = find(name);
    if (tool == nullptr) {
        if (agent_tool(name)) throw core::Error("not_yet_ported", "the tool " + std::string(name) + " is not in this build yet");
        throw core::PyUncaught("AttributeError", "'StudioService' object has no attribute '" + std::string(name) + "'");
    }
    const Json bound = bind_arguments("StudioService." + std::string(name), tool->params, positional, kwargs);
    if (tool->writes) refuse_old_project(bound["project"]);
    return (this->*(tool->method))(bound);
}

}  // namespace genko::service
