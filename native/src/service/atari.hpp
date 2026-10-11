#pragma once

#include <array>
#include <filesystem>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "core/json.hpp"
#include "core/model.hpp"
#include "render/image.hpp"

// Hand-drawn names (アタリ, Python's genko/studio/atari.py and xycut.py): the scan imported as an asset on the page's
// DRAFT layer, aligned in mm; its panels found by recursive XY-cut (every result written to studio/analysis/<page id>/
// with an overlay) and proposed as a layout; lines read from the handwriting proposed too. Nothing on the page changes
// until a person accepts a proposal.

namespace genko::service::xycut {

using core::Json;
using Box = std::array<int, 4>;  // x0, y0, x1, y1 in pixels (x1, y1 exclusive)

// xycut.Params, as the dataclass holds what it was given (an int stays an int in its __dict__).
struct Params {
    Json fields = Json::object({{"min_gutter_mm", 0.6}, {"min_panel_mm", 12.0}, {"gutter_px", 1}, {"speck_mm", 3.0}, {"thicken_mm", 0.3},
                                {"work_px_per_mm", 4.0}});
    double get(const char* name) const;
    // Params(**{k: v for k, v in params.items() if k in fields})
    static Params from(const Json& given);
};

struct Detection {
    Json leaves_mm = Json::array();  // [{rect_mm, confidence, border}] in reading order
    Json tree = Json::object();      // {rect_mm, axis?, children?} in page mm
    Json tiers;                      // the tiers DSL when the tree fits, else null
    int threshold = 0;
    double confidence = 0.0;
};

int otsu(const render::Image& gray);
// xycut.detect(image, placement_mm, params, right_to_left)
Detection detect(const render::Image& image, const std::array<double, 4>& placement_mm, const Params& params, bool right_to_left);
// xycut.content_box(image, dpi_guess): the box of the drawing on a scan (ignoring specks).
std::optional<Box> content_box(const render::Image& image, double dpi_guess = 150.0);

}  // namespace genko::service::xycut

namespace genko::service::atari {

using core::Json;

// atari.placement_for(image, page, align): where the scan sits on the page (x, y, w, h in mm) and the alignment used.
std::pair<Json, std::string> placement_for(const render::Image& image, const core::Page& page, const std::string& align = "auto");
// atari.atari_of(page): page.plan["atari"] (null without).
Json atari_of(const core::Page& page);
// atari.analyze(project, episode, page, params): the analysis files written; the layout proposal. core::PyValueError
// with Python's words for a page without a scan.
Json analyze(const std::filesystem::path& project, const core::Document& doc, const core::Page& page, const Json& params = Json());
// atari.accept_ops(episode, proposal, force): the ops a person's acceptance applies (PyValueError with Python's words).
Json accept_ops(const core::Document& doc, const Json& proposal, bool force);
// atari.assign_frames(ops, page): each proposed line given the panel its centre falls in.
Json assign_frames(const Json& ops, const core::Page& page);
// atari.line_from_input(item, page, atari): one proposed line checked (PyValueError / PyTypeError with Python's words).
Json line_from_input(const Json& item, const core::Page& page, const Json& atari);
// atari.overlay_image(episode, page, dpi, extra): the page with its scan and the open proposals drawn over it.
render::Image overlay_image(const core::Document& doc, const core::Page& page, int dpi, const Json& extra = Json());

}  // namespace genko::service::atari
