#pragma once

#include <cstddef>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "core/json.hpp"
#include "render/psd.hpp"
#include "render/psd_descriptor.hpp"
#include "render/psd_smart.hpp"

// PC4 D7 (SPEC.md COLOR-05「PSD のスマートオブジェクト」・COLOR-08「フィルター」, design 2.9 and the decision 2 (b)):
// Photoshop's smart filters of a placed layer — its SoLd descriptor's filterFX: filterFXList, each filter's descriptor
// (Fltr, or its filterID alone), its blending options (Md, Opct), whether it shows (enab), the stack's enab and
// filterMaskEnable — read as Genko's smart filters through the table (core::from_upstream: the kinds of フィルター). A
// stack of which one filter is not in the table is not read at all (the others would lay over another picture: the
// order's meaning changed): the smart object shows Photoshop's picture as before, and the import says which. Its filter
// mask is not read: a stack whose mask hides something — the file's FEid/FXid filter effects say so for its layer, or
// cannot say — is not read either. Upstream PhotoCraft reads none of it (io/src/psd_import.rs:236); the descriptors'
// classes and keys are Photoshop's as the public descriptions of the PSD format and psd-tools read them. Writing them is
// not done (a PSD written keeps the block it was read with: formats/psd_export).

namespace genko::render::psd_filters {

// Where the file's pixels are on the page: the file's (x, y) at the page's pixel (at_x + x·scale_x, at_y + y·scale_y)
// of a page of page_w × page_h pixels and width_mm × height_mm.
struct Placement {
    double scale_x = 1, scale_y = 1;
    double at_x = 0, at_y = 0;
    double page_w = 1, page_h = 1;
    double width_mm = 0, height_mm = 0;
    double file_w = 1, file_h = 1;  // the file's canvas in its pixels
};

// What the file's filter effects (its global FEid and FXid blocks) say of the smart filters' masks: by the id of each
// layer it holds one for, whether the mask hides something (or cannot be read); and whether one of an id no layer has
// does.
struct Masks {
    std::map<std::string, bool> hides;
    bool unread = false;  // (a block or an entry it could not read: any stack's mask may hide something)
};
// At most this many entries a file (the rest: unread).
inline constexpr std::size_t kMaskEntriesMax = 4096;
Masks read_masks(const std::vector<psd::File::TaggedBlock>& globals);

// One import's descriptor budget, all its placed layers' filterFX together (render/psd_descriptor's kUnpackedBudget).
struct Budget {
    std::size_t left = psd::descriptor::kUnpackedBudget;
};

// At most this many filters' names a stack not read says.
inline constexpr std::size_t kNamesMax = 8;

struct Read {
    std::optional<core::Json> filters;  // Genko's smart filters, the bottom one first (none: not read)
    bool on = true;                     // the stack shown (filterFX's enab)
    // Not read: why — "unknown" (filters not in the table: `names`, at most kNamesMax, `more` past them), "mask" (its
    // filter mask hides something), "too_many" (more than core::smart::kFiltersMax), "unreadable" (its descriptor) —;
    // empty when there is nothing to read.
    std::string why;
    std::vector<std::string> names;
    std::size_t more = 0;
};
// `sold`: the layer's SoLd (or SoLE) payload, `placed` as read_placed read it.
Read read(std::string_view sold, const psd::Placed& placed, const Placement& where, const Masks& masks, Budget& budget);

}  // namespace genko::render::psd_filters
