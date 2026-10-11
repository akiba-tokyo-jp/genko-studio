#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include "core/command_bus.hpp"
#include "core/json.hpp"
#include "core/model.hpp"

// The studio's ops (Python's genko/studio/studio_ops.py, which genko.ops.apply_ops dispatches its STUDIO_OPS to): the
// agent's state kept in the book (the bible, the script, the name plans, locations and props, the style), panel briefs
// and their regions and references, generation requests, candidates and their adoption as placed art, approvals and
// their revocation, tickets (approval requests, a person's fix instructions, questions to a person) and proposals. Each
// with Python's arguments, checks in Python's order, results and messages; the human-only rules (approve, revoke,
// reopen_ticket, reject_sheet, resolve_proposal, allow_chat_approval, a policy, an unlock, a pinned field, a person's
// region, a skip, a prompt override) as Python's (core::can_approve: "human:<name>", "human" or the unnamed "genko").
// The ops change only the book in memory (a batch is all or nothing); the only file they read is an asset's picture
// header, for place_asset and set_placement (StudioHooks). The book and page ops of the same family that Python keeps in
// ops._apply_one (set_bible, add_ticket, set_ticket) are here too.

namespace genko::core {

// studio_ops.PANEL_STATUSES, CANDIDATE_STATUSES, ORIGIN_KINDS, GATES, ADOPT_TARGETS (Python's tuples, in order).
inline constexpr std::string_view kPanelStatuses[] = {"empty", "briefed", "requested", "candidates", "fix_requested", "adopted", "skip"};
inline constexpr std::string_view kCandidateStatuses[] = {"candidate", "shortlisted", "rejected"};
inline constexpr std::string_view kOriginKinds[] = {"agent", "human", "self", "fixture", "genko"};
inline constexpr std::string_view kGates[] = {"bible", "script", "sheet", "name", "art", "export"};
inline constexpr std::string_view kAdoptTargets[] = {"art", "bg", "draft", "ink"};

// studio_ops.BRIEF_KEYS: what a panel's picture is asked to be.
inline constexpr std::string_view kBriefKeys[] = {"shot", "angle", "characters", "props", "refs", "regions", "instruction",
                                                  "location_id", "time", "action", "emotion", "gen"};

// jsonutil.content_hash: "sha256:" and the sha256 of the value's canonical JSON (keys sorted, no spaces, UTF-8).
std::string content_hash(const Json& value);

// studio_ops.brief_hash(panel): the content_hash of what the panel's picture is asked to be (BRIEF_KEYS; a character's
// 名札, "tag", left out; of the regions only those a person drew, source "user"). Python's AttributeError for a panel
// (or a region) that is not an object (PyUncaught).
std::string brief_hash(const Json& panel);

// What the studio ops need beyond core: the size of an asset's picture (Image.open(...).size of
// assets/<ab>/<sha256>.png in the book's folder `book`), nothing when there is no such file. render::ops_registry gives
// it (render::picture_size); without it place_asset, and set_placement of a placed picture that is no candidate, refuse
// that part with not_yet_ported.
struct StudioHooks {
    std::function<std::optional<std::pair<std::int64_t, std::int64_t>>(const std::filesystem::path& book, const std::string& ref)>
        picture_size;
};

// Register the studio ops (studio_ops.STUDIO_OPS) and set_bible, add_ticket and set_ticket.
void register_studio_ops(OpRegistry& registry, StudioHooks hooks = {});

}  // namespace genko::core
