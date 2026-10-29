#pragma once

#include <string>
#include <vector>

#include "core/annotations.hpp"
#include "core/json.hpp"
#include "core/model.hpp"

// PC5 J-c (annotations@1, SPEC.md PROD-01「注釈」): ファイル → 読み込み → 注釈… (file.import.notes; PhotoCraft v0.2.0's
// engine/src/notes_cmds.rs import_notes_from, MIT): the notes of a PSD, a PSB or a .pcraft, onto a page. Not an op (an op
// reads no path): the window reads the file off its thread and applies what it gives as add_annotation ops — one Undo.

namespace genko::formats {

// The file's notes (bytes of a PSD, a PSB or a .pcraft zip), placed in mm by the file's resolution from its top left;
// their author, text, colour, box, open and date the file's. core::Error("format", …) for a file of another kind or one
// that cannot be read, core::Error("value", "the file has no notes") for one without notes.
std::vector<core::Annotation> read_annotations(std::string bytes);
// add_annotation ops putting them on the page (its number): each note's mark pulled inside the paper where it lies
// beyond it (its box along), as Photoshop does when the sizes differ. core::Error("value", "a page holds at most 1000
// notes") when the page has no room for them all.
core::Json annotation_ops(const std::vector<core::Annotation>& notes, const core::Page& page);

}  // namespace genko::formats
