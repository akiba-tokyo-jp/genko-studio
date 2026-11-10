#pragma once

#include <QString>

#include <string>
#include <vector>

#include "core/json.hpp"
#include "core/model.hpp"

// Panel layout templates (テンプレートでコマを割る, Python's genko/studio/layout.py and dialogs.TemplateDialog): the
// built-in ones (layouts.json: tiers top to bottom, columns right to left, a column may stack rows; some panels bleed
// or have slanted borders) and a person's own (今のコマ割りをテンプレートに残す: <config>/panel_templates.json, the
// panel tree as frame_tree writes it, with the paper's size).
//
// Python sets the finished tree with the studio's set_layout op. This build applies the same cuts with the panel ops
// it has (merge_frame, split_frame, cut_frame, set_frame) as one change, so one Undo takes it back: the cuts are made
// on a copy of the book first, and the ids they gave are given again when the change is applied (Session::apply's
// ids), so every op of the change names the panels the ones before it made.

namespace genko::app::templates {

struct Template {
    std::string key;     // a built-in template's key, or a person's template's name
    QString label;       // what the list shows
    bool mine = false;   // a person's own
    core::Json spec;     // built-in: {"description", "tiers", "bleed"?, "slant"?}; mine: {"name", "size", "tree"}
};

std::vector<Template> builtin();
std::vector<Template> mine();
// Keep this page's panels as a template (replacing one of the same name).
void save_mine(const QString& name, const core::Page& page);
void remove_mine(const QString& name);

// The page has one panel and no lines (Python's layout.is_blank).
bool is_blank(const core::Document& doc, const core::Page& page);

struct Plan {
    core::Json ops = core::Json::array();
    std::vector<std::string> ids;  // the ids the ops give, in order
};

// The ops that make the page's panels the template's (the page's panels merged back first). Throws core::Error with
// the reason (in Japanese) when this build cannot do it for this page.
Plan plan(const core::Document& doc, std::size_t page_index, const Template& chosen, const std::string& actor);

}  // namespace genko::app::templates
