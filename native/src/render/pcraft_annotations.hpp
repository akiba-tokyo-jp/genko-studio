#pragma once

#include <optional>
#include <vector>

#include "core/annotations.hpp"
#include "core/json.hpp"
#include "core/model.hpp"
#include "render/pcraft_production.hpp"

// PC5 J-c (annotations@1, SPEC.md PROD-01「注釈」, FILE-01「.pcraft」): a .pcraft's notes (the document's "notes": PhotoCraft
// v0.2.0's doc/src/analysis.rs Note — author, color, text, position [x, y] and popup [left, top, right, bottom] in the
// document's pixels, open, modified as a PDF date; MIT, native/third_party/photocraft/NOTICE.md) as the page's, and back.
// Read: each note's place in mm, its date as UTC (render/psd_annotations), its author "genko" (a person's: the file's); a
// value not of that shape leaves the notes out, said. Written: the page's notes (none: the key not written).

namespace genko::render::pcraft_annotations {

// The notes of a "notes" value (mm a pixel `mm`; their ids new). core::Error("value") for one not of the shape.
std::vector<core::Annotation> notes_of(const core::Json& value, double mm);

void read(const core::Json& value, pcraft_production::ReadContext& context);
std::optional<core::Json> write(const core::Page& page, pcraft_production::WriteContext& context);

}  // namespace genko::render::pcraft_annotations
