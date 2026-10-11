#pragma once

#include <array>
#include <string>
#include <utility>
#include <vector>

#include "core/command_bus.hpp"
#include "core/json.hpp"
#include "core/model.hpp"
#include "core/pynum.hpp"
#include "service/book.hpp"
#include "studio/schemas.hpp"

// The tier DSL of a name plan → panels, through the split_frame op (Python's genko/studio/layout.py). Tiers run top
// to bottom; the cols inside a tier are listed right to left (Japanese reading order), and a col may stack rows.
// split_frame cannot name its children, so each split is applied on its own and the new leaves are read back from the
// frame tree.

namespace genko::service::layout {

using core::Json;

inline constexpr double kTierGutterMm = 7.0;
inline constexpr double kColGutterMm = 3.0;
inline constexpr double kSumTolerance = 0.02;
inline constexpr std::size_t kMaxPanels = 8;

// layout.LayoutError (an ApplyError): "<path>: <message>", with the two kept apart.
class LayoutError : public core::ApplyError {
public:
    LayoutError(std::string path, std::string message);
    const std::string& where() const { return path_; }
    const std::string& message() const { return message_; }

private:
    std::string path_;
    std::string message_;
};

using RectMm = std::array<core::Num, 4>;  // x, y, w, h as round(…, 3) keeps them (an int stays an int)

// layout.CompiledLayout (its dicts in the order Python fills them).
struct CompiledLayout {
    std::vector<std::pair<std::string, std::string>> slot_to_frame;
    std::vector<std::pair<std::string, RectMm>> leaf_rects_mm;
    std::vector<std::string> reading_order;
    std::vector<std::pair<std::size_t, std::string>> source_map;
    Json ops = Json::array();

    const std::string* frame_of(const std::string& slot) const;
    const RectMm* rect_of(const std::string& slot) const;
};

// layout.templates(): the page layout templates (layouts.json).
const Json& templates();
// layout.resolve_tiers(plan): plan["tiers"] when given, else the template's; LayoutError otherwise.
Json resolve_tiers(const Json& plan);
// layout.slots_in_order(tiers)
std::vector<std::string> slots_in_order(const Json& tiers);
// layout.check_tiers(plan): the plan's tiers and panels as a page can hold them (ratios, slots, panels).
std::vector<studio::Issue> check_tiers(const Json& plan);
// layout.is_blank(episode, page): one panel and no lines.
bool is_blank(const core::Document& doc, const core::Page& page);
// layout.clear_page(episode, page, agent=…): the lines deleted and the panels merged back (or the drawn panels set back
// to the basic frame), applied to `work`; the ops.
Json clear_page(Work& work, std::size_t page, const std::string& agent);
// layout.apply_layout(episode, page_index, plan, agent=…): split a blank page into the plan's panels (applied to
// `work`). LayoutError for what the plan or the page does not allow; an op's ApplyError as Python lets it through.
CompiledLayout apply_layout(Work& work, const Json& page_index, const Json& plan, const std::string& agent);

}  // namespace genko::service::layout
