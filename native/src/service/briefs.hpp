#pragma once

#include <string>
#include <utility>
#include <vector>

#include "core/json.hpp"
#include "core/model.hpp"
#include "service/result.hpp"

// What the studio tools tell an agent about a book (Python's genko/studio/service.py helpers): a page's panels and
// gutters, a panel's brief, the book's style, a candidate in a few fields, and the listings of `inspect` (materials,
// fonts, brushes); and the small rules of the writing tools (a new bible keeps the approved sheets, props written by
// name, whose approval a chat approval is).

namespace genko::service {

// Python's dict keyed by JSON values (1 == 1.0 == True are one key; a list or a dict is TypeError "unhashable"): the
// first key's place, the last value.
class PyDict {
public:
    void set(const Json& key, Json value);
    const Json* find(const Json& key) const;
    const std::vector<std::pair<Json, Json>>& items() const { return items_; }

private:
    std::vector<std::pair<Json, Json>> items_;
};

namespace briefs {

// page._find(frame_id): the frame of that id under the page's first frame (PyUncaught KeyError when there is none,
// IndexError for a page without frames).
const core::Frame& find_frame(const core::Page& page, const Json& frame_id);
// _gutter_brief(page): the gaps between panels, as move_gutter takes them.
Json gutter_brief(const core::Page& page);
// _panel_brief(episode, page_index, frame_id, full)
Json panel_brief(const core::Document& doc, const Json& page_index, const Json& frame_id, bool full);
// _brief(plan, bible): the panels in a line ("slot: shot/angle who action | …").
std::string brief(const Json& plan, const Json& bible);
// genreq.catalog(episode): the branch of the style catalog the book keeps (null: Genko's own words).
Json catalog(const core::Document& doc);
// genreq.catalog_fits(kept, expression)
bool catalog_fits(const Json& kept, const std::string& expression);
// _style_brief(episode) (null when the book keeps no catalog style).
Json style_brief(const core::Document& doc);
// _catalog_node(data)
Json catalog_node(const Json& data);
// _candidate_brief(cand)
Json candidate_brief(const Json& cand);
// _keep_approved(current, incoming): a new bible keeps approved sheets (refs, locked) and may not drop a locked
// character.
std::pair<Json, std::vector<Issue>> keep_approved(const Json& current, const Json& incoming);
// _props_by_id(plan, bible): props written by their name are read as the prop of that name (plan changed in place).
std::vector<Issue> props_by_id(Json& plan, const Json& bible);
// session.default_actor(): human:<$GENKO_USER, else the login name>.
std::string default_actor();
// _chat_person(episode, name)
std::string chat_person(const core::Document& doc, const Json& name);
// _materials_list(): every material stamp_material can place.
Json materials_list();
// fonts.system_fonts(): [{name, style, path}] of the computer's fonts that can draw Japanese (the config folder's
// fonts.json once looked for).
Json system_fonts();
// PC5 G-b (type.fonts): every font of the computer (Japanese or not) by family — [{family, styles: [{style, postscript, path,
// japanese, axes?: [{tag, name, min, default, max}], faces?, index?}]}]: each face of a collection (.ttc) with its index —
// looked for again when the font files change (their paths, sizes and times; kept in the config folder's type_fonts.json,
// {signature, families}, and for the run).
Json type_fonts();
// _fonts_list(): bundled, system (the fonts that draw Japanese) and, PC5 G-b, families (type_fonts)
Json fonts_list();
// _brushes_list(episode)
Json brushes_list(const core::Document& doc);

}  // namespace briefs
}  // namespace genko::service
