#pragma once

#include <filesystem>
#include <string>
#include <utility>

#include "core/json.hpp"
#include "core/model.hpp"
#include "render/image.hpp"

// The previews the studio tools give an agent (Python's service._preview and _png, and review.annotate): a page as
// render_page draws it, or one panel as render_frame cuts it, with the name's non-printing badges, as PNG bytes the
// way Pillow's Image.save(format="PNG") writes them.

namespace genko::service {

using core::Json;

// _png(image): Image.save(buf, format="PNG").
std::string png(const render::Image& image);

// review.annotate(image, page, dpi, plan): the reading order, slot and a short brief per panel, and where each person
// stands (non-printing badges). `plan`: the name plan or null.
render::Image annotate(const render::Image& image, const core::Page& page, int dpi, const Json& plan);

// _preview(episode, page_index, mode, max_px, plan, frame_id): the page (or one panel) as PNG. core::ApplyError
// "<page> ページはない" / "<page> ページにコマ <id> はない（このページのコマ: …）".
std::string preview(const core::Document& doc, const Json& page_index, const std::string& mode, const Json& max_px,
                    const Json& plan, const Json& frame_id = Json());

// _render_kind(episode, project, page_index, frame_id, kind, candidate_id, max_px): a panel's comparison with a
// candidate (kind "compare") or one of its guides ("guide:composition" / "guide:pose" / "guide:keepout") as PNG, and
// its label (studio_art.cpp; the app's panel view shows the comparison too).
std::pair<std::string, std::string> render_kind(const core::Document& doc, const std::filesystem::path& project, const Json& page_index,
                                                const Json& frame_id, const Json& kind, const Json& candidate_id, const Json& max_px);

}  // namespace genko::service
