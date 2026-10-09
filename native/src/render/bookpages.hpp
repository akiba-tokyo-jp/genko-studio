#pragma once

#include <string>

#include "core/command_bus.hpp"
#include "core/model.hpp"

// What the book and page ops of M4 (core/ops_bookpages.cpp) need from the drawing build and the storage: a paint
// layer's pixels moved onto a new basic frame (set_page_spec) and the other book read (import_pages).

namespace genko::render {

// pagespec._raster(png, new_spec, old_frame, new_frame): the picture opened as RGBA and put through an affine
// transform (bilinear) into a picture of the new paper at raster.WORKING_DPI, so what was at the old basic frame lands
// on the new one; as PNG. Pillow's errors as Python raises them (PyUncaught); OpError for a paper too large to hold.
std::string relayout_raster(const std::string& png, const core::PageSpec& spec, const core::Rect& old_frame,
                            const core::Rect& new_frame);

// io.load_episode(Path(from)) for import_pages: the book at `from` read whole (storage::load_document). OpError "the
// other book cannot be read (<why>)" with Python's words for a folder without project.json, text that is not JSON
// and a newer version; with this build's own for a project.json that is not a book, and for a book it would open
// read-only (missing or damaged assets, values it had to repair, features it does not know: Python reads such a book
// as it can, and its pages would come in without what is missing).
core::Document load_other_book(const std::string& from);

// Registers set_page_spec and import_pages again (core's refuse what needs these) with the two above.
void register_bookpage_ops(core::OpRegistry& registry);

}  // namespace genko::render
