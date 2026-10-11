#include "service/human_service.hpp"

#include <algorithm>

#include "core/command_bus.hpp"
#include "core/pyconv.hpp"
#include "core/pyops.hpp"
#include "formats/checks.hpp"
#include "formats/export.hpp"
#include "service/atari.hpp"
#include "service/book.hpp"
#include "service/result.hpp"
#include "service/service_util.hpp"
#include "service/sheets.hpp"

namespace genko::service {

using core::py_get;
using core::py_truthy;

namespace {

// sorted(pages): the values as given, in Python's order (TypeError for values it cannot compare).
Json sorted_list(const Json& pages) {
    std::vector<Json> out = core::iterate(pages);
    std::stable_sort(out.begin(), out.end(), [](const Json& a, const Json& b) { return core::py_less(a, b); });
    return Json(out);
}

}  // namespace

HumanService::HumanService(fs::path project, std::string actor) : path_(std::move(project)), actor_(std::move(actor)) {
    if (actor_.rfind("human:", 0) != 0) throw core::ApplyError("承認できるのは human:<名前> だけ");
}

core::Document HumanService::apply(const Json& ops) {
    Session session(path_, actor_);
    session.apply(ops, actor_);
    session.save(actor_);
    return session.doc();
}

Json HumanService::approve_name(const Json& pages) {
    Json ops = Json::array();
    for (const Json& p : core::iterate(pages)) ops.push_back(Json::object({{"op", "approve"}, {"gate", "name"}, {"page", p}}));
    apply(ops);
    return Json::object({{"ok", true}, {"approved", sorted_list(pages)}});
}

Json HumanService::approve_art(const Json& pages) {
    Json ops = Json::array();
    for (const Json& p : core::iterate(pages)) ops.push_back(Json::object({{"op", "approve"}, {"gate", "art"}, {"page", p}}));
    apply(ops);
    return Json::object({{"ok", true}, {"approved", sorted_list(pages)}});
}

Json HumanService::approve_sheet(const Json& character_id, const Json& candidate_id, const Json& face_box01) {
    const Json op = sheet_approval_op(path_, load_episode(path_), character_id, candidate_id, face_box01);
    apply(Json::array({op}));
    return Json::object({{"ok", true}, {"approved", character_id}, {"face_asset", py_get(op, "face_asset")}});
}

Json HumanService::chat_approval(const Json& on) {
    apply(Json::array({Json::object({{"op", "allow_chat_approval"}, {"on", py_truthy(on)}})}));
    return Json::object({{"ok", true}, {"chat_approval", py_truthy(on)}, {"by", actor_}});
}

Json HumanService::revoke(const Json& gate, const Json& pages, const Json& character_id, const Json& reason) {
    Json ops = Json::array();
    if (core::py_equals(gate, Json("sheet"))) {
        ops.push_back(Json::object({{"op", "revoke"}, {"gate", "sheet"}, {"character_id", character_id}, {"reason", reason}}));
    } else {
        for (const Json& p : core::iterate(pages)) ops.push_back(Json::object({{"op", "revoke"}, {"gate", gate}, {"page", p}, {"reason", reason}}));
    }
    apply(ops);
    return Json::object({{"ok", true}, {"revoked", gate}});
}

Json HumanService::reject_proposal(const Json& proposal_id, const Json& note) {
    apply(Json::array({Json::object({{"op", "resolve_proposal"}, {"id", proposal_id}, {"status", "rejected"}, {"note", note}})}));
    return Json::object({{"ok", true}, {"rejected", proposal_id}});
}

Json HumanService::close_ticket(const Json& ticket_id, const Json& reply) {
    const core::Document doc = load_episode(path_);
    Json ticket;
    for (const Json& t : core::iterate(doc.tickets)) {
        if (core::py_equals(py_get(t, "id"), ticket_id)) {
            ticket = t;
            break;
        }
    }
    if (ticket.is_null()) return Json::object({{"ok", false}, {"error", "チケット " + core::py_str(ticket_id) + " はない"}});
    Json ops = Json::array({Json::object({{"op", "set_ticket"}, {"id", ticket_id}, {"status", "done"}})});
    if (py_truthy(reply) && core::truthy_at(ticket, "page_index")) {
        const Json frame_id = py_get(ticket, "frame_id");
        ops.push_back(Json::object({{"op", "request_fix"},
                                    {"page", ticket["page_index"]},
                                    {"frame_id", frame_id},
                                    {"instruction", reply},
                                    {"scope", py_truthy(frame_id) ? "frame" : "page"}}));
    }
    apply(ops);
    return Json::object({{"ok", true}, {"closed", ticket_id}});
}

Json HumanService::reopen_ticket(const Json& ticket_id, const Json& note) {
    try {
        apply(Json::array({Json::object({{"op", "reopen_ticket"}, {"id", ticket_id}, {"note", note}})}));
    } catch (const core::ApplyError& exc) {
        return Json::object({{"ok", false}, {"error", wording_error(exc.what())}});
    }
    return Json::object({{"ok", true}, {"reopened", ticket_id}});
}

Json HumanService::comment(const Json& page, const Json& text, const Json& frame_id) {
    const core::Document doc = apply(Json::array({Json::object({{"op", "request_fix"},
                                                                {"page", page},
                                                                {"frame_id", frame_id},
                                                                {"instruction", text},
                                                                {"scope", py_truthy(frame_id) ? "frame" : "page"}})}));
    if (!doc.tickets.is_array() || doc.tickets.empty()) core::raise_index_error();
    return Json::object({{"ok", true}, {"comment", util::item(doc.tickets.back(), "id")}});
}

Json HumanService::accept_proposal(const Json& proposal_id, bool force) {
    const core::Document episode = load_episode(path_);
    const Json all = core::py_or(py_get(episode.studio, "proposals"), Json::object());
    if (!all.is_object()) core::raise_attribute_error(all, "get");
    core::require_hashable(proposal_id);
    const Json proposal = proposal_id.is_string() ? py_get(all, proposal_id.get<std::string>()) : Json();
    if (proposal.is_null() || !core::py_equals(py_get(proposal, "status"), Json("open"))) {
        return Json::object({{"ok", false}, {"error", "開いている提案 " + core::py_str(proposal_id) + " は無い"}});
    }
    Json ops;
    try {
        ops = atari::accept_ops(episode, proposal, force);
    } catch (const core::PyValueError& exc) {
        return Json::object({{"ok", false}, {"error", exc.what()}});
    } catch (const core::ApplyError& exc) {  // (an ApplyError is a ValueError)
        return Json::object({{"ok", false}, {"error", exc.what()}});
    }
    const core::Page* page = nullptr;
    for (const auto& p : episode.pages) {
        if (core::py_equals(Json(p->id), util::item(proposal, "page_id"))) {
            page = p.get();
            break;
        }
    }
    if (page == nullptr) throw core::PyUncaught("StopIteration", "");
    ops = atari::assign_frames(ops, *page);
    try {
        apply(ops);
    } catch (const core::ApplyError& exc) {
        return Json::object({{"ok", false}, {"error", exc.what()}});
    }
    return Json::object({{"ok", true}, {"accepted", proposal_id}, {"kind", util::item(proposal, "kind")}});
}

Json HumanService::export_book(const std::string& fmt, const fs::path& out, const OfficialExport& o) {
    const core::Document episode = load_episode(path_);
    if (fmt != "pdf" && fmt != "tiff" && fmt != "png" && fmt != "webtoon" && fmt != "sns") {
        return Json::object({{"ok", false}, {"error", "format は pdf / tiff / png / webtoon / sns"}});
    }
    const bool screen = fmt == "webtoon" || fmt == "sns";
    // (screen outputs are sized in pixels, so print resolution does not apply to them)
    formats::checks::StudioCheck check;
    check.allow_fixture = o.allow_fixture;
    check.force = o.force || screen;
    if (!screen) check.color = o.color;
    const Json report = formats::checks::studio_check(episode, path_, check);
    if (!py_truthy(report["ok"])) {
        Json refused = Json::object({{"ok", false}, {"error", "preflight で止まった"}});
        for (const auto& [key, value] : report.items()) refused[key] = value;
        return refused;
    }
    std::vector<fs::path> written;
    if (fmt == "webtoon") {
        written = formats::export_webtoon(episode, out, o.width_px, o.max_height, 0, o.jpeg.value_or(false) ? "jpeg" : "png");
    } else if (fmt == "sns") {
        written = formats::export_sns(episode, out, o.long_edge, o.jpeg == std::optional<bool>(false) ? "png" : "jpeg", 92, o.spreads);
    } else {
        const std::int64_t dpi = py_truthy(o.dpi) ? core::to_int(o.dpi) : (episode.spec.dpi.truthy() ? core::py_int(episode.spec.dpi) : 600);
        written = formats::export_print(episode, out, fmt, dpi, 180, true, o.area, o.color, o.icc, o.screen);
    }
    apply(Json::array({Json::object({{"op", "approve"}, {"gate", "export"}})}));
    Json files = Json::array();
    for (const fs::path& f : written) files.push_back(util::path_text(f));
    return Json::object({{"ok", true}, {"files", std::move(files)}, {"warnings", report["warnings"]}, {"dpi", report["dpi"]}});
}

Json run_export(const core::Document& episode, const std::optional<fs::path>& project, std::string_view key, const fs::path& out,
                formats::RunOptions options) {
    if (options.official) {
        const std::string format(key);
        options.official_export = [format, out, o = options](const fs::path& folder) {
            OfficialExport official;
            official.dpi = o.dpi ? Json(*o.dpi) : Json();
            official.width_px = o.width;
            official.max_height = o.max_height;
            official.long_edge = o.long_edge;
            official.jpeg = o.jpeg;
            official.spreads = o.spreads;
            official.area = o.area;
            official.color = o.color;
            official.icc = o.icc;
            official.screen = o.screen;
            return HumanService(folder, o.actor).export_book(format, out, official);
        };
    }
    return formats::run(episode, project, key, out, options);
}

}  // namespace genko::service
