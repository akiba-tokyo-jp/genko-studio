#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

#include "core/json.hpp"
#include "core/model.hpp"
#include "formats/exporting.hpp"

// HumanService: what only a person may do (Python's genko/studio/service.py HumanService): approve and revoke, close
// and reopen tickets, comment, let chat approvals in or stop them, accept or reject proposals, and the final export.
// Never exposed over MCP. Each answers the dict Python's returns; what Python raises is thrown as it (core::ApplyError
// for a refused op or the project lock held, and as StudioService documents).

namespace genko::service {

namespace fs = std::filesystem;
using core::Json;

// HumanService.export's options (Python's keyword arguments; jpeg: None, False or True).
struct OfficialExport {
    core::Json dpi;  // null: the book's, else 600
    bool allow_fixture = false;
    bool force = false;
    int width_px = 800;
    int max_height = 1280;
    int long_edge = 2048;
    std::optional<bool> jpeg;
    bool spreads = false;
    std::string area = "bleed";
    std::string color = "auto";
    std::string icc;
    // C++: the dots of a black and white page's greys, as export_print takes them (Python passes False here: its
    // `screen` is overwritten by whether the format is for screens, so a given screen is never used; SPEC COMP-01a).
    std::optional<core::Json> screen;
};

class HumanService {
public:
    // core::ApplyError "承認できるのは human:<名前> だけ" for an actor that is not human:…
    HumanService(fs::path project, std::string actor);

    const fs::path& path() const { return path_; }
    const std::string& actor() const { return actor_; }

    Json approve_name(const Json& pages);
    Json approve_art(const Json& pages);
    // Approve a sheet and cut the face reference out of it (face_box01 in the image, 0..1; default: a square at the
    // top centre, where sheets usually put the face close-up).
    Json approve_sheet(const Json& character_id, const Json& candidate_id, const Json& face_box01 = Json());
    Json chat_approval(const Json& on);
    Json revoke(const Json& gate, const Json& pages, const Json& character_id = Json(), const Json& reason = Json(""));
    Json reject_proposal(const Json& proposal_id, const Json& note = Json(""));
    Json close_ticket(const Json& ticket_id, const Json& reply = Json(""));
    Json reopen_ticket(const Json& ticket_id, const Json& note = Json(""));
    Json comment(const Json& page, const Json& text, const Json& frame_id = Json());
    // accept_proposal(proposal_id, force): the proposal's layout or lines made real (each line given the panel its
    // centre falls in); {"ok": false, "error"} when it is not open, cannot be accepted or an op refuses it.
    Json accept_proposal(const Json& proposal_id, bool force = false);
    // export(fmt, out, …): the final export (gate ④): preflight must pass, then the pages are written into the folder
    // `out` (pdf / tiff / png at print resolution, webtoon / sns for screens) and the export approval recorded.
    // {"ok": false, "error": "preflight で止まった", **report} when the preflight stops it.
    Json export_book(const std::string& fmt, const fs::path& out, const OfficialExport& options = {});

private:
    // with ProjectLock: load, apply the ops as this person, save; the book after.
    core::Document apply(const Json& ops);

    fs::path path_;
    std::string actor_;
};

// exporting.run(episode, project, key, out, official=…, …) with the official export (Python's genko/app/exporting.py):
// formats::run, the official export handed to HumanService(project, options.actor).export after Python's own refusals.
Json run_export(const core::Document& episode, const std::optional<fs::path>& project, std::string_view key, const fs::path& out,
                formats::RunOptions options);

}  // namespace genko::service
