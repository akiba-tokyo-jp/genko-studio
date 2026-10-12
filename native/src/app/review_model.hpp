#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "core/json.hpp"
#include "core/model.hpp"

// What the approval box and the process bar show, and the ops their buttons send (Python's genko/app/review_model.py,
// Qt-free).
//
// People approve one request at a time after looking at it: there is no bulk button. Sending back turns into
// instructions the agent sees in `next`: a page fix for a name, panel fixes for art, a rejected sheet with a note.

namespace genko::app::review_model {

using core::Json;

struct InboxItem {
    std::string ticket_id;
    std::string kind;          // gate | help | proposal
    Json gate;                 // the gate (a proposal's kind), as the ticket has it (null: none)
    std::vector<Json> pages;   // the pages, as the ticket lists them
    Json character_id;         // null: none
    Json frame_id;             // null: none
    std::string text;
    std::string by;
    std::string name;          // the character's name for sheet requests

    // The row's words in the list (InboxItem.title).
    std::string title() const;
    // gate or "" (Python's `self.gate or ""`).
    std::string gate_text() const;
};

// The open requests and questions of the book, then its open proposals.
std::vector<InboxItem> inbox(const core::Document& doc);

// The sheet candidates still open of a sheet request, numbered as the approval box lists them (Python's
// studio_widgets.ApprovalBox._numbered_sheets): the newest first — the one just asked for —, those made before the
// style changed ("候補 n（前の絵柄）") after them.
std::vector<std::pair<std::string, Json>> numbered_sheets(const core::Document& doc, const InboxItem& item);

// Counts for the process bar, in Python's order (設定画未承認 first when the book has characters).
std::vector<std::pair<std::string, std::int64_t>> progress(const core::Document& doc);

// The ops for 承認 (export is not an op: the window runs the checked export instead). A sheet needs a candidate:
// core::PyValueError("設定画の候補を 1 つ選んでから承認します") without one; its face is cut out and stored in the book's
// assets (sheet_approval_op).
Json approve_ops(const core::Document& doc, const InboxItem& item, const std::optional<std::filesystem::path>& project = std::nullopt,
                 const Json& candidate_id = Json(), const Json& face_box01 = Json());

// The ops for 差し戻し: the request is closed as returned and the reason reaches the agent. core::PyValueError
// ("理由（返事）を書いてから送ります") for a text that is only blanks.
Json send_back_ops(const core::Document& doc, const InboxItem& item, const std::string& text,
                   const std::optional<std::vector<std::string>>& frame_ids = std::nullopt);

}  // namespace genko::app::review_model
