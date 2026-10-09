#pragma once

#include <optional>
#include <string>

#include "core/command_bus.hpp"
#include "core/model.hpp"

// What the book and page ops of M4 (core/ops_bookpages.cpp) need from the drawing build and the storage: a paint
// layer's pixels and a layer's mask moved onto a new basic frame (set_page_spec) and the other book read (import_pages).

namespace genko::render {

// pagespec._raster(png, new_spec, old_frame, new_frame): the picture opened as RGBA and put through an affine
// transform (bilinear) into a picture of the new paper at raster.WORKING_DPI, so what was at the old basic frame lands
// on the new one; as PNG. Pillow's errors as Python raises them (PyUncaught); OpError for a paper too large to hold.
// (`spec` is the page's own new paper, a cover's as covers::spec_for makes it: Python takes the book's for every page
// and cuts off what a jacket or a band has beyond it — the user's decision D1.)
std::string relayout_raster(const std::string& png, const core::PageSpec& spec, const core::Rect& old_frame,
                            const core::Rect& new_frame);

// A layer mask moved with what it masks (the user's decision D1: Python leaves it stretched over the new paper): its
// PNG ("L" over the page's old paper `old_paper`, white shows) made again over the page's new paper `paper` at
// ops.MASK_DPI, each pixel taking the old mask's value where the content now there came from (the old basic frame onto
// the new one as pagespec._Map moves a point), bilinear, the old mask's edge carried on past its paper; as PNG.
// Nothing when moving changes nothing: a mask of one value everywhere, or the same paper and frame; and for a mask that
// is no picture that can be read (Python never reads it there: it is left as it is). OpError for a basic frame of no
// finite size and a paper too large to hold the mask.
std::optional<std::string> relayout_mask(const std::string& png, const core::PageSpec& old_paper, const core::PageSpec& paper,
                                         const core::Rect& old_frame, const core::Rect& new_frame);

// io.load_episode(Path(from)) for import_pages: the book at `from` read whole (storage::load_document). OpError "the
// other book cannot be read (<why>)" with Python's words for a folder without project.json, text that is not JSON
// and a newer version; with this build's own for a project.json that is not a book, and for a book it would open
// read-only (missing or damaged assets, values it had to repair, features it does not know: Python reads such a book
// as it can, and its pages would come in without what is missing).
core::Document load_other_book(const std::string& from);

// Registers set_page_spec and import_pages again (core's refuse what needs these) with the two above.
void register_bookpage_ops(core::OpRegistry& registry);

}  // namespace genko::render
