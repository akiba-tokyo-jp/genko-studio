#pragma once

#include <string>

#include "core/json.hpp"
#include "core/model.hpp"

// import_psd's work for what opens a PSD or PSB as a book (PC4 F-d; formats/open_file, SPEC.md FILE-01「PSD・PSB」): the
// file's bytes `data` read onto the page `op` names ("page", and the op's own "fit", "id", "name", "embedded_profile", …;
// not "path" or "psd") as import_psd reads them, the book changed in place. `exact`: the page is made of the file — its
// layers the file's pixels whatever their size (not kept within 12000 pixels a side) and their masks at those pixels.
// Its report, as import_psd's; core::OpError and the op's other errors as import_psd throws them (the book then as it was
// is the caller's: it changes `doc` only when the read succeeds).

namespace genko::render {

core::Json import_psd_data(core::Document& doc, const core::Json& op, std::string data, bool exact = false);

}  // namespace genko::render
