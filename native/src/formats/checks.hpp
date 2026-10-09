#pragma once

#include <filesystem>
#include <optional>

#include "core/json.hpp"
#include "core/model.hpp"

// 入稿前の点検 (Python's genko/checks.py): every problem in the book with the place to show it, as Python finds them —
// lines not placed, outside the finished size or the basic frame, lettered too small, with a tail buried in the balloon,
// overlapping; faces and people the panel's edge cuts; pictures pasted below 350 dpi, paint layers kept at the working
// resolution; art beyond the bleed; faces reported on art since replaced; tones that beat into a moiré; spreads whose
// pages do not face; pages whose art is only on layers that do not print, and empty pages. A book made with agents
// (strict_gates or studio) with a folder also gets the studio's preflight (genko/studio/preflight.check: approvals,
// adopted art, missing or low-resolution pictures, test images, provenance, character sheets).
//
// Each issue: {"level": "error" | "warning", "code", "page", "message" (Japanese), "box": [x, y, w, h] (mm) or null,
// "target": {"kind": "line" | "layer" | "frame" | "page", "id"}}, its keys in Python's order, its numbers as Python's.
//
// Python catches every exception of a line's lettering (no size) and of the preflight (none of its issues); here what
// this build cannot do yet (render::NotYetPorted: a line's letters or a tone's picture it does not draw yet) is thrown
// instead of being left out (ARCHITECTURE.md §4a). Python's other errors (a box that is not four numbers, a picture that
// does not open) are thrown as core::Error where Python raises them.

namespace genko::formats::checks {

inline constexpr double kMinTextMm = 2.4;  // smaller dialogue is hard to read in print
inline constexpr int kMinDpi = 350;

// checks.page_issues(episode, page): the issues of one page, in Python's order.
core::Json page_issues(const core::Document& episode, const core::Page& page);

// checks.book(episode, project): {"ok", "issues", "errors", "warnings"} — every page's issues, then the studio
// preflight's errors and warnings when the book is a studio project and `project` (its folder) is given.
// core::Error("page_not_loaded") for a book whose pages are still being read (Document::deferred).
core::Json book(const core::Document& episode, const std::optional<std::filesystem::path>& project = std::nullopt);

// genko/studio/preflight.check(episode, project) with its defaults: {"errors": [...], "warnings": [...]}, each
// {"code", "severity", "path", "message", "hint"} (Issue.to_dict). Python's exceptions as core::Error (book() takes
// them as Python's `except Exception` does: no preflight issues).
core::Json preflight(const core::Document& episode, const std::filesystem::path& project);

}  // namespace genko::formats::checks
