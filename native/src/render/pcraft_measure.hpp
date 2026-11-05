#pragma once

#include <optional>

#include "core/json.hpp"
#include "core/measure.hpp"
#include "core/model.hpp"
#include "render/pcraft_production.hpp"

// PC5 J-d1 (measure@1, SPEC.md PROD-01「計測」, FILE-01「.pcraft」): a .pcraft's measurement (the document's "measurement":
// PhotoCraft v0.2.0's doc/src/analysis.rs Measurement — scale {pixel_length, logical_length, units}, count_groups [{name,
// color, marker_size, label_size, visible, points [[x, y]…]}], active_count_group, ruler {start, end, protractor} | null;
// the places in the document's pixels, a field left out its default as PhotoCraft's serde reads it; MIT,
// native/third_party/photocraft/NOTICE.md) as the page's, and back. The scale's pixel_length is the document's pixels in
// the file and the page's in the book. Read: the places in mm, the sizes kept to their ranges, a group without a name
// カウントグループ n; a value not of that shape leaves it out, said ("計測（カウント・ものさし）は読めない形のため外しました").
// Written: the page's measuring (none — all the defaults —: the key not written).

namespace genko::render::pcraft_measure {

// The measuring of a "measurement" value (mm a pixel `mm`, the page's pixels a mm `page_px_per_mm`; none: all the
// defaults). core::Error("value") for one not of the shape.
std::optional<core::Measure> measure_of(const core::Json& value, double mm, double page_px_per_mm);

void read(const core::Json& value, pcraft_production::ReadContext& context);
std::optional<core::Json> write(const core::Page& page, pcraft_production::WriteContext& context);

}  // namespace genko::render::pcraft_measure
