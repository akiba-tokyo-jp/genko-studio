#include "core/command_bus.hpp"

#include <algorithm>
#include <cstdint>
#include <map>
#include <set>
#include <utility>
#include <vector>

#include "core/covers.hpp"
#include "core/ops_schema.hpp"
#include "core/ops_util.hpp"
#include "core/color_raster.hpp"
#include "core/pyconv.hpp"
#include "core/pyops.hpp"

namespace genko::core {

namespace {

constexpr std::string_view kSeparator = " ‖ ";  // " ‖ " (Python's _usage)

bool is_op(const Json& op, std::string_view name) {
    const Json* value = get(op, "op");
    return value != nullptr && value->is_string() && value->get_ref<const std::string&>() == name;
}

// The op's entry in the public list (`genko schema`), when it has one.
const Json* schema_of(const Json& op) {
    const Json* name = get(op, "op");
    if (name == nullptr || !name->is_string()) return nullptr;
    for (const Json& entry : ops_schema()) {
        const Json* op_name = get(entry, "op");
        if (op_name != nullptr && *op_name == *name) return &entry;
    }
    return nullptr;
}

std::string join(const std::vector<std::string>& items, std::string_view separator) {
    std::string out;
    for (std::size_t i = 0; i < items.size(); ++i) {
        if (i > 0) out += separator;
        out += items[i];
    }
    return out;
}

// Python's _usage: the keys given that the op does not take, and how the op is written.
std::string usage(const Json& op) {
    const Json* schema = schema_of(op);
    if (schema == nullptr) return {};
    std::vector<std::string> strange;
    for (const auto& [key, value] : op.items()) {
        if (!schema->contains(key) && key != "op" && key != "area" && !key.starts_with("_")) strange.push_back(key);
    }
    std::vector<std::string> keys;
    for (const auto& [key, value] : schema->items()) {
        if (key != "op" && key != "note") keys.push_back(key + ": " + py_str(value));
    }
    std::string out;
    if (!strange.empty()) out += std::string(kSeparator) + "unknown keys: " + join(strange, ", ");
    out += std::string(kSeparator) + py_str(*get(op, "op")) + " takes {" + join(keys, ", ") + "}";
    return out;
}

// Python's _unknown_keys: keys an op was given that it does not take (ignored, so said).
std::vector<std::string> unknown_key_warnings(const Json& ops) {
    std::vector<std::string> out;
    for (std::size_t i = 0; i < ops.size(); ++i) {
        const Json& op = ops[i];
        if (!op.is_object()) continue;
        const Json* schema = schema_of(op);
        if (schema == nullptr) continue;
        std::vector<std::string> strange;
        for (const auto& [key, value] : op.items()) {
            const bool known = schema->contains(key) || key == "op" || key == "area" || key == "page" || key == "id" ||
                               key == "note";
            if (!known && !key.starts_with("_")) strange.push_back(key);
        }
        if (!strange.empty()) {
            out.push_back("ops[" + std::to_string(i) + "] " + py_str(*get(op, "op")) +
                          ": unknown keys ignored: " + join(strange, ", "));
        }
    }
    return out;
}

std::size_t code_points(std::string_view text) {
    std::size_t n = 0;
    for (const char c : text) {
        if ((static_cast<unsigned char>(c) & 0xC0) != 0x80) ++n;
    }
    return n;
}

const Page* first_page(const Document& doc, const Num& index) {
    for (const auto& page : doc.pages) {
        if (page->index == index) return page.get();
    }
    return nullptr;
}

// Python's facing_problem: why two pages cannot form a spread (nothing when they face each other).
std::optional<std::string> facing_problem(const Document& doc, const Page& a, const Page& b) {
    if (!(py_abs(a.index - b.index) == Num(1))) {
        return "pages " + a.index.repr() + " and " + b.index.repr() + " are not next to each other";
    }
    const bool b_first = b.index < a.index;  // (sorted by index, a first on a tie)
    const Page& first = b_first ? b : a;
    const Page& second = b_first ? a : b;
    const std::string start = doc.binding == Binding::Right ? "right" : "left";
    const std::string start_side = doc.start_side.value_or("");
    if (first.side(start_side) != start || second.side(start_side) == start) {
        return "pages " + first.index.repr() + " and " + second.index.repr() + " are two sides of one leaf, not a spread";
    }
    return std::nullopt;
}

// Python's _op_page_index: the page an op is about (its "page", or the page of the line a line op names).
std::optional<Num> op_page_index(const Document& doc, const Json& op) {
    if (const Json* page = get(op, "page")) {
        try {
            return Num(py_int(*page));
        } catch (const Error&) {
            return std::nullopt;
        }
    }
    static const std::set<std::string, std::less<>> kLineOps{"edit_line", "move_line", "delete_line",
                                                              "set_balloon_path", "cut_balloon"};
    const Json* name = get(op, "op");
    const Json* id = get(op, "id");
    if (name != nullptr && name->is_string() && kLineOps.contains(name->get_ref<const std::string&>()) &&
        id != nullptr && py_truthy(*id)) {
        for (const StoryLine& line : doc.story) {
            if (id->is_string() && line.id == id->get_ref<const std::string&>()) return line.page_index;
        }
    }
    return std::nullopt;
}

bool json_equals_string(const Json& value, std::string_view text) {
    return value.is_string() && value.get_ref<const std::string&>() == text;
}

// An op on a page someone else has locked: refused ("page N locked by <them>").
void check_held(const Document& doc, const Page& page, const Num& index, const Actor& actor) {
    const Json* owner = get(doc.page_locks, page.id);  // (v3: locks follow the page, not its position)
    if (owner != nullptr && py_truthy(*owner) && !json_equals_string(*owner, actor.name())) {
        throw OpError("page " + index.repr() + " locked by " + py_str(*owner));
    }
}

bool one_of(std::string_view name, std::initializer_list<std::string_view> names) {
    return std::find(names.begin(), names.end(), name) != names.end();
}

// strict_gates' families of ops (Python's LAYOUT_OPS and RASTER_EDIT_OPS, with the ops of later milestones: the
// rules go by name)
bool layout_op(std::string_view name) {
    return one_of(name, {"split_frame", "merge_frame", "resize_frame", "set_layout", "cut_frame", "move_gutter",
                         "add_frame", "delete_frame"});
}

bool raster_edit_op(std::string_view name) {
    return one_of(name, {"put_raster", "import_psd", "erase_raster", "erase", "filter_raster", "flood_fill", "fill",
                         "fill_area", "fill_enclosed", "trace_edit", "gradient_fill", "transform_area", "delete_area",
                         "paste", "set_stroke_width", "reshape_stroke", "trace_prims", "effect_to_layer", "add_shape",
                         "smudge", "vector_edit", "fill_gaps", "liquify", "render_prims"});
}

// The key a drawing op reads the id of the layer it works on from (without it, the op takes the role in "layer"):
// put_raster and filter_raster read "id", flood_fill a role only, add_stroke and the other drawing ops "layer_id".
// strict_gates looks at that layer, found as the op finds it.
std::string_view layer_key(std::string_view name) {
    if (name == "put_raster" || name == "filter_raster") return "id";
    if (name == "flood_fill") return {};
    return "layer_id";
}

// bookops.NOT_PER_PAGE
bool not_per_page(std::string_view name) {
    return one_of(name, {"add_page", "delete_page", "duplicate_page", "import_pages", "reorder", "move_page",
                         "set_page_spec", "add_cover", "for_pages", "replace_text", "approve", "revoke",
                         "allow_chat_approval", "name_ok", "advance", "set_bible", "set_script", "define_brush",
                         "set_brush"});
}

bool printed(const Layer& layer) { return layer.role != LayerRole::Name && layer.role != LayerRole::Draft; }

// ops.PAGE_LOCAL_OPS, BOOK_OPS and LINE_OPS (the sets _touched_pages looks names up in)
bool page_local_op(std::string_view name) {
    return one_of(name, {"import_psd", "set_animation", "add_anim_folder", "add_cel", "set_exposure", "set_exposures",
                         "set_camera_key", "set_light_table", "split_frame", "cut_frame", "move_gutter", "merge_frame",
                         "resize_frame", "set_frame", "add_frame", "delete_frame", "add_line", "name_ok", "advance",
                         "add_stroke", "fill", "fill_area", "fill_enclosed", "transform_area", "delete_area", "paste",
                         "set_stroke_width", "reshape_stroke", "delete_stroke", "put_raster", "set_layer", "gradient_fill",
                         "duplicate_layer", "merge_down", "set_layer_mask", "paint_mask", "set_note", "select_frame",
                         "flood_fill", "add_tone", "set_tone", "delete_tone", "add_effect", "edit_effect", "delete_effect",
                         "effect_to_layer", "edit_stroke", "simplify_stroke", "set_ruler", "add_ruler", "edit_ruler",
                         "delete_ruler", "ruler_from_3d", "camera_from_ruler", "add_prim3d", "add_scene", "edit_prim",
                         "delete_prim", "trace_prims", "lt_convert", "erase_raster", "erase", "reorder_layers",
                         "stamp_material", "add_mannequin", "pose_mannequin", "set_onion", "step_onion", "set_lt",
                         "add_layer", "delete_layer", "filter_raster", "add_shape", "store_area", "forget_area", "smudge",
                         "vector_edit", "trace_edit", "fill_gaps", "merge_layers", "merge_visible", "group_layers",
                         "move_layers", "convert_layer", "set_layers", "liquify", "ruler_to_layer", "add_figure",
                         "pose_figure", "add_head", "add_hand", "import_model", "set_camera", "set_light", "render_prims"});
}

bool book_op(std::string_view name) {
    return one_of(name, {"set_brush", "define_brush", "set_autosave", "add_ticket", "set_ticket", "reorder_lines"});
}

bool line_op(std::string_view name) {
    return one_of(name, {"edit_line", "move_line", "delete_line", "set_balloon_path", "cut_balloon"});
}

// Python reads the ops once before it applies any (ops._touched_pages, to know which pages to copy): it looks each
// op's name up in sets of ops until it meets one that may change any page. A name it cannot hash stops it there with
// a TypeError, outside apply_ops' checks (Python's command line ends with a traceback).
void read_as_touched_pages_does(const Json& ops) {
    for (std::size_t i = 0; i < ops.size(); ++i) {
        const Json& op = ops[i];
        if (!op.is_object()) return;
        const Json* name = get(op, "op");
        if (name != nullptr && (name->is_array() || name->is_object())) {
            throw ApplyError("ops[" + std::to_string(i) + "] " + py_str(*name) + ": TypeError: unhashable type: '" +
                                 py_type_name(*name) + "'",
                             "python_error");
        }
        const std::string text = name != nullptr && name->is_string() ? name->get<std::string>() : std::string();
        const bool named = name != nullptr && name->is_string();
        if (named && (book_op(text) || line_op(text))) continue;
        if (!named || !page_local_op(text)) return;
        if ((text == "name_ok" || text == "advance") && !op.contains("page")) return;
        try {
            py_int(op.at("page"));
        } catch (const std::exception&) {
            return;
        }
    }
}

}  // namespace

ApplyError::ApplyError(const std::string& message, std::string code) : Error(std::move(code), message) {}

// --- OpRegistry --------------------------------------------------------------------------------------------------

void register_core_ops(OpRegistry& registry) {
    register_book_ops(registry);
    register_frame_ops(registry);
    register_page_ops(registry);
    register_stroke_ops(registry);
    register_layer_ops(registry);
    register_color_ops(registry);
    register_ruler_ops(registry);
    register_arrange_ops(registry);
}

const OpRegistry& OpRegistry::builtin() {
    static const OpRegistry registry = [] {
        OpRegistry r;
        register_core_ops(r);
        return r;
    }();
    return registry;
}

void OpRegistry::add(std::string name, OpFunction function) { ops_[std::move(name)] = std::move(function); }

void OpRegistry::set_area_resolver(AreaResolver resolver) { area_resolver_ = std::move(resolver); }

const OpFunction* OpRegistry::find(std::string_view name) const {
    const auto it = ops_.find(name);
    return it == ops_.end() ? nullptr : &it->second;
}

std::vector<std::string> OpRegistry::names() const {
    std::vector<std::string> out;
    for (const auto& [name, function] : ops_) out.push_back(name);
    return out;
}

// --- checks --------------------------------------------------------------------------------------------------------

std::size_t require_page(const Document& doc, const Json& op) {
    std::int64_t index = 0;
    const Json* page = get(op, "page");
    if (page != nullptr) {
        // (Python's int has no bound: a page number past 64 bits is only a page the book does not have)
        if (const auto big = py_big_int_text(*page)) throw OpError("no page " + *big);
    }
    try {
        if (page == nullptr) throw OpError("page (int) is required");
        index = py_int(*page);
    } catch (const Error&) {
        throw OpError("page (int) is required");
    }
    for (std::size_t i = 0; i < doc.pages.size(); ++i) {
        if (doc.pages[i]->index == Num(index)) return i;
    }
    throw OpError("no page " + std::to_string(index));
}

bool paper_on_every_page(const Json& op) {
    const Json* page = get(op, "page");
    return page == nullptr || page->is_null() || *page == Json("") || py_equals(*page, Json(0));
}

void check_page_lock(Document& doc, const Json& op, const Actor& actor) {
    const Json* name_value = get(op, "op");
    const std::string name = name_value != nullptr && name_value->is_string() ? name_value->get<std::string>() : "";
    const bool named = name_value != nullptr && name_value->is_string();
    if (named && name == "name_ok" && !actor.can_approve()) {  // (GATE_OPS)
        throw OpError(name + " needs a person (actor " + actor.name() + " cannot approve)");
    }
    if (named && (name == "undo" || name == "set_meta" || name == "set_bible" || name == "set_autosave")) return;
    if (named && name == "set_paper" && paper_on_every_page(op)) {
        // (every page it changes is asked, as an op on each one is: Python asks none of them)
        for (const auto& each : doc.pages) check_held(doc, *each, each->index, actor);
        return;
    }
    // (Python takes a lock on a page the book does not have, and lets it go, without a word: refused here)
    const bool lock_op = named && (name == "lock_page" || name == "unlock_page");
    if (const Json* page_value = get(op, "page"); lock_op && page_value != nullptr) {
        if (const auto big = py_big_int_text(*page_value)) throw OpError("no page " + *big);
    }
    const auto index = op_page_index(doc, op);
    if (!index) return;
    const Page* page = first_page(doc, *index);
    if (page == nullptr) {
        if (lock_op) throw OpError("no page " + index->repr());
        return;
    }
    const std::string key = page->id;  // (v3: locks follow the page, not its position)
    const Json* owner = get(doc.page_locks, key);
    const bool owned = owner != nullptr && py_truthy(*owner);
    if (named && name == "lock_page") {
        const Json* agent = get(op, "agent");
        const std::string wanted = agent != nullptr && py_truthy(*agent) ? py_str(*agent) : actor.name();
        if (actor.name() != kLegacyActor && wanted != actor.name()) {
            throw OpError("cannot lock page " + index->repr() + " as " + wanted + " (actor is " + actor.name() + ")");
        }
        const bool owner_is_ai = owner != nullptr && owner->is_string() &&
                                 owner->get_ref<const std::string&>().starts_with("ai:");
        if (owned && !json_equals_string(*owner, wanted) && !(actor.can_approve() && owner_is_ai)) {
            throw OpError("page " + index->repr() + " locked by " + py_str(*owner));
        }
        doc.page_locks[key] = wanted;
        return;
    }
    if (named && name == "unlock_page") {
        if (owned && !json_equals_string(*owner, actor.name()) && !actor.can_approve()) {
            throw OpError("page " + index->repr() + " locked by " + py_str(*owner) + "; " + actor.name() +
                          " cannot unlock it");
        }
        doc.page_locks.erase(key);
        return;
    }
    check_held(doc, *page, *index, actor);
}

void check_strict(const Document& doc, const Json& op_in, const Actor& actor) {
    const Json* name_value = get(op_in, "op");
    std::string name = name_value != nullptr && name_value->is_string() ? name_value->get<std::string>() : std::string();
    const bool named = name_value != nullptr && name_value->is_string();
    const auto page_of = [&doc](const Json& op) -> const Page& { return doc.page(require_page(doc, op)); };
    if (named && (layout_op(name) || (name == "set_frame" && op_in.contains("poly"))) && !actor.can_approve()) {
        const Page& page = page_of(op_in);
        if (page.name_ok) {
            throw OpError("page " + page.index.repr() +
                          ": the name is approved; a person must revoke it before the layout changes (strict_gates)");
        }
    }
    if (named && name == "advance" && json_equals_string(op_in.value("to", Json(nullptr)), "finish")) {
        const Page& page = page_of(op_in);
        if (!page.art_ok) throw OpError("page " + page.index.repr() + ": finish needs the art approved (strict_gates)");
    }
    if (named && one_of(name, {"merge_layers", "merge_visible", "set_layers", "move_layers", "group_layers"})) {
        const Page& page = page_of(op_in);
        std::vector<Json> ids;  // set(op.get("ids") or []), or every layer's id
        const bool every = truthy_at(op_in, "all") || name == "merge_visible";
        if (!every) {
            const Json* given = get(op_in, "ids");
            if (given != nullptr && py_truthy(*given)) {
                for (const Json& id : iterate(*given)) {
                    require_hashable(id);
                    ids.push_back(id);
                }
            }
        }
        const auto chosen = [&](const Layer& layer) {
            if (every) return true;
            return std::any_of(ids.begin(), ids.end(), [&](const Json& id) { return json_equals_string(id, layer.id); });
        };
        const bool any_printed =
            std::any_of(page.layers.begin(), page.layers.end(), [&](const Layer& l) { return chosen(l) && printed(l); });
        if (any_printed && !page.name_ok && (name == "merge_layers" || name == "merge_visible")) {
            throw OpError(name + " on a printed layer needs name_ok on page " + page.index.repr() + " (strict_gates)");
        }
        return;
    }
    // merge_down writes the preceding sibling, not just the named upper source.
    if (named && name == "merge_down" && op_in.contains("id")) {
        const Page& page = page_of(op_in);
        const std::string upper_id = py_str(op_in["id"]);
        for (std::size_t i = 0; i < page.layers.size(); ++i) {
            const Layer& upper = page.layers[i];
            if (upper.id != upper_id || upper.kind == LayerKind::Folder) continue;
            for (std::size_t j = i; j > 0; --j) {
                const Layer& lower = page.layers[j - 1];
                if (lower.parent_id != upper.parent_id || lower.kind == LayerKind::Folder) continue;
                if (printed(lower) && lower.exportable && !page.name_ok)
                    throw OpError("merge_down on a printed layer needs name_ok on page " +
                                  page.index.repr() + " (strict_gates)");
                break;
            }
            break;
        }
    }
    Json op = op_in;
    if (named && one_of(name, {"set_layer_mask", "paint_mask", "merge_down", "delete_layer", "duplicate_layer", "convert_layer"}) &&
        truthy_at(op_in, "id")) {
        op["layer_id"] = op_in["id"];  // (these name the layer by id: the same rule as drawing on it)
        name = "fill";
    }
    // (Python reads "layer_id" here for every op, which put_raster, filter_raster and flood_fill do not: a filter by id
    // went through, and a layer_id they ignore stood in for the layer they change)
    const std::string_view id_key = named ? layer_key(name) : std::string_view();
    if (named && (name == "add_stroke" || raster_edit_op(name)) && !id_key.empty() && truthy_at(op, id_key)) {
        const Page& page = page_of(op);
        // Match the drawing resolver's py_str conversion, including numeric ids.
        const std::string layer_id = py_str(*get(op, id_key));
        const Layer* target = nullptr;
        for (const Layer& layer : page.layers) {
            if (layer_id == layer.id) {
                target = &layer;
                break;
            }
        }
        // (a layer that is not printed — the name, a draft, or one set exportable:false — may be drawn on before the
        // name is approved: trying a line out does not change the page)
        if (target != nullptr && printed(*target) && target->exportable && !page.name_ok) {
            throw OpError(name + " on a printed layer needs name_ok on page " + page.index.repr() + " (strict_gates)");
        }
        return;
    }
    if (named && name == "set_layer" && op.contains("exportable") && op["exportable"] == Json(true)) {
        const Page& page = page_of(op);
        const Json key = truthy_at(op, "id") ? op["id"] : (op.contains("layer") ? op["layer"] : Json(nullptr));
        const Layer* target = nullptr;
        for (const Layer& layer : page.layers) {
            if (json_equals_string(key, layer.id) || json_equals_string(key, to_string(layer.role))) {
                target = &layer;
                break;
            }
        }
        const bool drawn = target != nullptr && (target->stroke_count() > 0 || !target->patches.empty() ||
                                                 (target->raster_png && !target->raster_png->empty()));
        if (drawn && printed(*target) && !target->exportable && !page.name_ok) {
            throw OpError("set_layer exportable on a drawn layer needs name_ok on page " + page.index.repr() +
                          " (strict_gates)");
        }
    }
    if (named && raster_edit_op(name)) {
        const Json* layer_value = get(op, "layer");
        const std::string layer = layer_value != nullptr && py_truthy(*layer_value) ? py_str(*layer_value)
                                  : name != "filter_raster"                        ? std::string("ink")
                                                                                   : std::string();
        if (layer != "name" && layer != "draft" && !layer.empty()) {
            const Page& page = page_of(op);
            if (!page.name_ok) {
                throw OpError(name + " on " + layer + " needs name_ok on page " + page.index.repr() + " (strict_gates)");
            }
        }
    }
    if (named && name == "add_line" && truthy_at(op, "frame_id") && !(op.contains("x_mm") && op.contains("y_mm"))) {
        throw OpError("add_line with frame_id needs explicit x_mm/y_mm (strict_gates)");
    }
}

Json expand_for_pages(const Document& doc, const Json& op) {
    const Json* ops = get(op, "ops");
    if (ops == nullptr || !ops->is_array() || ops->empty()) throw OpError("ops is the list of ops to run on each page");
    std::vector<std::string> names;
    for (const Json& item : *ops) {
        if (!item.is_object()) continue;
        const Json* name = get(item, "op");
        names.push_back(name != nullptr ? py_str(*name) : std::string("None"));
    }
    if (names.size() != ops->size()) throw OpError("ops is the list of ops to run on each page");
    for (const std::string& name : names) {
        if (not_per_page(name)) throw OpError(name + " cannot be repeated page by page");
    }
    const Json* pages_value = get(op, "pages");
    const Json wanted = pages_value != nullptr && py_truthy(*pages_value) ? *pages_value : Json("body");
    std::vector<Json> indexes;
    if (wanted == Json("all") || wanted == Json("body")) {
        for (const auto& page : doc.pages) {
            if (wanted == Json("all") || !is_cover(*page)) indexes.push_back(page->index.json());
        }
    } else {
        for (const Json& v : iterate(wanted)) {
            // (a page number past 64 bits stays its text: no page has it)
            const auto big = py_big_int_text(v);
            indexes.push_back(big ? Json(*big) : Json(to_int(v)));
        }
        for (const Json& index : indexes) {
            if (index.is_string() || first_page(doc, Num(index.get<std::int64_t>())) == nullptr) {
                throw OpError("no page " + py_str(index));
            }
        }
        // (Python also takes what it can turn into ints one by one: the text "12" is pages 1 and 2, a dict its keys,
        // 2.7 page 2; refused here)
        const bool page_numbers = wanted.is_array() && std::all_of(wanted.begin(), wanted.end(), [](const Json& v) {
            return v.is_number_integer();
        });
        if (!page_numbers) throw OpError("pages must be all, body or a list of page numbers");
    }
    Json out = Json::array();
    for (const Json& index : indexes) {
        for (const Json& item : *ops) {
            Json one = item;
            one["page"] = index;
            out.push_back(std::move(one));
        }
    }
    return out;
}

bool area_needs_resolving(const Json& area) {
    if (!area.is_object()) return false;
    for (const char* key : {"rect", "ellipse", "layer", "color", "all", "saved", "union", "intersect", "subtract", "invert",
                            "grow_mm", "feather_mm"}) {
        if (area.contains(key)) return true;
    }
    return false;
}

std::optional<Json> resolve_plain_area(const Json& area) {
    if (!area.is_object() || area.size() != 1) return std::nullopt;
    const bool rect = area.contains("rect");
    if (!rect && !area.contains("ellipse")) return std::nullopt;
    // x, y, w, h = (float(v) for v in box)
    const std::vector<double> box = unpack_floats(area.begin().value(), 4);
    const double x = box[0], y = box[1], w = box[2], h = box[3];
    Json poly = Json::array();
    if (rect) {  // selops.rect_poly
        poly = Json::array({Json::array({x, y}), Json::array({x + w, y}), Json::array({x + w, y + h}), Json::array({x, y + h})});
    } else {  // selops.ellipse_poly(box, n=72)
        constexpr double kTau = 6.283185307179586;  // math.tau
        const double cx = x + w / 2, cy = y + h / 2, rx = w / 2, ry = h / 2;
        for (int k = 0; k < 72; ++k) {
            const double angle = kTau * k / 72;
            poly.push_back(Json::array({py_round(cx + rx * py_cos(angle), 3), py_round(cy + ry * py_sin(angle), 3)}));
        }
    }
    return Json::object({{"poly", std::move(poly)}});
}

std::vector<std::string> validate_document(const Document& doc) {
    std::vector<std::string> warnings;
    std::set<std::string> ids;
    for (const auto& page : doc.pages) ids.insert(page->id);
    for (const StoryLine& line : doc.story) {
        const Page* page = first_page(doc, line.page_index);
        if (page == nullptr) {
            warnings.push_back("line " + line.id + ": page " + line.page_index.repr() + " does not exist");
            continue;
        }
        if (line.frame_id && !line.frame_id->empty() && page->find_frame(*line.frame_id) == nullptr) {
            warnings.push_back("line " + line.id + ": frame " + *line.frame_id + " is not on page " +
                               line.page_index.repr());
        }
    }
    for (const auto& page : doc.pages) {
        std::set<std::string> leaves;
        for (const Frame* frame : page->leaf_frames()) leaves.insert(frame->id);
        for (const Layer& layer : page->layers) {
            if (layer.kind == LayerKind::Placed && layer.frame_id && !layer.frame_id->empty() &&
                !leaves.contains(*layer.frame_id)) {
                warnings.push_back("page " + page->index.repr() + " layer " + layer.id + ": panel " + *layer.frame_id +
                                   " is gone (placed art will not clip)");
            }
        }
    }
    if (doc.page_locks.is_object()) {
        for (const auto& [key, owner] : doc.page_locks.items()) {
            if (!ids.contains(key)) warnings.push_back("page lock on unknown page " + key);
        }
    }
    for (const auto& page : doc.pages) {
        if (!page->spread_with || !(page->index < *page->spread_with)) continue;
        const Page* partner = first_page(doc, *page->spread_with);
        const std::optional<std::string> problem =
            partner != nullptr ? facing_problem(doc, *page, *partner)
                               : std::optional<std::string>("spread partner " + page->spread_with->repr() + " missing");
        if (problem) {
            warnings.push_back("spread " + page->index.repr() + "-" + page->spread_with->repr() + ": " + *problem);
        }
    }
    return warnings;
}

Json journal_op(const Json& op) {
    Json out = op;
    if (out.is_object()) {
        const auto it = out.find("png_base64");
        if (it != out.end() && it->is_string()) {
            *it = "<" + std::to_string(code_points(it->get_ref<const std::string&>())) + " base64 chars>";
        }
    }
    return out;
}

// --- page numbers ------------------------------------------------------------------------------------------------

void PageMapping::set(const Num& from, std::optional<std::int64_t> to) {
    for (auto& [key, value] : items_) {
        if (key == from) {  // (a dict: the key keeps its place and takes the new value)
            value = to;
            return;
        }
    }
    items_.emplace_back(from, to);
}

std::optional<std::int64_t> PageMapping::get(const Num& from) const {
    for (const auto& [key, value] : items_) {
        if (key == from) return value;
    }
    return std::nullopt;
}

std::optional<std::int64_t> PageMapping::get(const Json& from) const {
    require_hashable(from);  // (a dict lookup: a list or a dict is a TypeError)
    if (from.is_boolean()) return get(Num(from.get<bool>() ? 1 : 0));  // (Python: True == 1)
    if (const auto number = Num::from_json(from)) return get(*number);
    return std::nullopt;
}

void remap_page_refs(Document& doc, const PageMapping& mapping) {
    for (StoryLine& line : doc.story) {
        if (const auto to = mapping.get(line.page_index); to && *to != 0) line.page_index = Num(*to);
    }
    for (std::size_t i = 0; i < doc.pages.size(); ++i) {
        const auto to = mapping.get(doc.pages[i]->index);
        if (to && *to != 0 && !doc.pages[i]->index.same(Num(*to))) doc.edit_page(i).index = Num(*to);
    }
    const auto remapped = [&](const std::optional<Num>& value) -> std::optional<Num> {
        if (!value) return std::nullopt;
        const auto to = mapping.get(*value);
        return to ? std::optional<Num>(Num(*to)) : std::nullopt;
    };
    const auto same = [](const std::optional<Num>& a, const std::optional<Num>& b) {
        return a.has_value() == b.has_value() && (!a || a->same(*b));
    };
    for (std::size_t i = 0; i < doc.pages.size(); ++i) {
        const Page& page = *doc.pages[i];
        const auto spread = remapped(page.spread_with);
        const auto onion = remapped(page.onion_from);
        if (!same(spread, page.spread_with)) doc.edit_page(i).spread_with = spread;
        if (!same(onion, doc.pages[i]->onion_from)) doc.edit_page(i).onion_from = onion;
    }
    if (doc.tickets.is_array()) {
        for (Json& ticket : doc.tickets) {
            if (!ticket.is_object()) {
                throw PyUncaught("AttributeError", "'" + py_type_name(ticket) + "' object has no attribute 'get'");
            }
            const auto at = ticket.find("page_index");
            if (at == ticket.end() || at->is_null()) continue;
            if (const auto to = mapping.get(*at)) {
                *at = *to;
            } else {
                ticket["status"] = "orphaned";
            }
        }
    }
}

void reorder_pages(Document& doc, const std::vector<Num>& order) {
    std::vector<PagePtr> pages;
    pages.reserve(order.size());
    for (const Num& index : order) {
        PagePtr found;
        for (const auto& page : doc.pages) {
            if (page->index == index) found = page;  // (by_index: the last page with the number)
        }
        if (!found) throw OpKeyError(index.repr());
        pages.push_back(found);
    }
    doc.pages = std::move(pages);
    PageMapping mapping;
    for (std::size_t n = 0; n < order.size(); ++n) mapping.set(order[n], static_cast<std::int64_t>(n + 1));
    remap_page_refs(doc, mapping);
}

// --- CommandBus ----------------------------------------------------------------------------------------------------

CommandBus::CommandBus(const OpRegistry& registry) : registry_(registry) {}

namespace {

// A book whose pages are not all read yet (Document::deferred: the first page is shown while the others are read)
// takes the ops that keep to a page that is read, the book's own settings and its lines; any other op could reach a
// page whose strokes and pictures are missing here, and waits until they are read.
void refuse_unread_pages(const Document& doc, const Json& ops) {
    if (doc.deferred.empty()) return;
    for (const Json& op : ops) {
        const Json* name = op.is_object() ? get(op, "op") : nullptr;
        const std::string text = name != nullptr && name->is_string() ? name->get<std::string>() : std::string();
        if (book_op(text) || line_op(text)) continue;
        bool read = false;
        if (page_local_op(text)) {
            try {
                read = !doc.is_deferred(require_page(doc, op));
            } catch (const std::exception&) {
            }
        }
        if (!read) throw ApplyError("the book's pages are still being read: try again in a moment", "page_not_loaded");
    }
}

std::size_t color_raster_bytes(const Document& doc) {
    std::size_t bytes = 0;
    for (const PagePtr& page : doc.pages) {
        for (const Layer& layer : page->layers) bytes += layer.color_raster ? layer.color_raster->size() : 0;
    }
    return bytes;
}

// ... and an op that did change such a page anyway is refused with the whole batch; so is one that makes the
// precise colour pictures bigger, since the book's budget for them (validate_color_document) cannot count the pages
// not read yet.
void refuse_changed_unread_pages(const Document& before, const Document& after) {
    if (before.deferred.empty()) return;
    for (const PagePtr& page : before.deferred) {
        if (std::find(after.pages.begin(), after.pages.end(), page) == after.pages.end()) {
            throw ApplyError("the book's pages are still being read: try again in a moment", "page_not_loaded");
        }
    }
    if (color_raster_bytes(after) > color_raster_bytes(before)) {
        throw ApplyError("the book's pages are still being read: try again in a moment", "page_not_loaded");
    }
}

}  // namespace

ApplyResult CommandBus::apply(const Document& doc, const Json& ops_in, const Actor& actor, bool dry_run) const {
    if (!ops_in.is_array()) throw ApplyError("ops must be a JSON array");
    ApplyResult result;
    // (Python looks at ops[0].get("op") first: a lone op that is not an object stops it there)
    if (ops_in.size() == 1 && !ops_in[0].is_object()) {
        throw ApplyError("AttributeError: '" + py_type_name(ops_in[0]) + "' object has no attribute 'get'", "python_error");
    }
    if (ops_in.size() == 1 && is_op(ops_in[0], "undo")) {
        // Python undoes the session's own changes here; a book just read has none. (Saved changes are undone with
        // `genko undo`, from the journal; `genko apply` gives this op to it.)
        if (!dry_run) throw ApplyError("nothing to undo", "nothing_to_undo");
        result.doc = doc;
        result.applied.push_back("undo");
        result.has_warnings = false;
        result.journal_ops = Json::array({Json::object({{"op", "undo"}})});
        return result;
    }

    // for_pages: the same ops page by page, expanded against the book as it was given, each then checked like any other
    Json ops = ops_in;
    if (std::any_of(ops_in.begin(), ops_in.end(), [](const Json& op) { return op.is_object() && is_op(op, "for_pages"); })) {
        Json expanded = Json::array();
        for (std::size_t i = 0; i < ops_in.size(); ++i) {
            const Json& op = ops_in[i];
            if (!(op.is_object() && is_op(op, "for_pages"))) {
                expanded.push_back(op);
                continue;
            }
            const std::string prefix = "ops[" + std::to_string(i) + "] for_pages: ";
            try {
                for (Json& item : expand_for_pages(doc, op)) expanded.push_back(std::move(item));
            } catch (const OpError& error) {
                throw ApplyError(prefix + error.what());
            } catch (const PyValueError& error) {
                // (Python lets it through apply_ops; its command line prints the message alone)
                throw ApplyError(error.what(), "python_error");
            } catch (const PyUncaught& error) {
                throw ApplyError(prefix + error.type() + ": " + error.what(), "python_error");
            } catch (const Error& error) {
                // (a TypeError: Python lets it through apply_ops, and its command line stops with a traceback)
                throw ApplyError(prefix + "TypeError: " + error.what(), "python_error");
            }
        }
        ops = std::move(expanded);
    }

    read_as_touched_pages_does(ops);
    refuse_unread_pages(doc, ops);
    Document work = doc;  // (the pages stay shared until an op changes one)
    for (std::size_t i = 0; i < ops.size(); ++i) {
        Json& op = ops[i];
        if (!op.is_object()) throw ApplyError("ops[" + std::to_string(i) + "] must be an object");
        const Json* name = get(op, "op");
        const std::string name_text = name != nullptr ? py_str(*name) : std::string("None");
        const std::string prefix = "ops[" + std::to_string(i) + "] " + name_text + ": ";
        // The op as it is applied: with its area resolved it is a copy (Python's {**op, "area": …}); the journal and
        // the unknown-key warnings keep the op as it was given.
        Json resolved;
        Json* current = &op;
        std::optional<Json> report;
        try {
            if (const Json* area = get(op, "area"); area != nullptr && area_needs_resolving(*area)) {
                const std::size_t at = require_page(work, op);
                if (!registry_.area_resolver()) {
                    not_yet_ported("an area of this kind (rect, ellipse, layer, color, all, saved, union, intersect, "
                                   "subtract, invert, grow_mm, feather_mm) is resolved by the drawing ops "
                                   "(render::ops_registry)");
                }
                resolved = op;
                resolved["area"] = registry_.area_resolver()(work, at, *area);
                current = &resolved;
            }
            const Json& seen = *current;
            if (name != nullptr) require_hashable(*name);  // (Python looks the name up in sets of ops)
            check_page_lock(work, *current, actor);
            if (work.strict_gates) check_strict(work, *current, actor);
            const bool lock_op = is_op(*current, "lock_page") || is_op(*current, "unlock_page");
            if (!lock_op) {  // (lock_page and unlock_page took effect in check_page_lock)
                if (name == nullptr || !py_truthy(*name)) throw OpError("op is required");
                const OpFunction* function = name->is_string() ? registry_.find(name->get_ref<const std::string&>()) : nullptr;
                if (function == nullptr) {
                    // (undo is an op only on its own: in a batch Python does not know it)
                    if (schema_of(seen) != nullptr && !is_op(seen, "undo")) not_yet_ported(name_text + " is not in the C++ build yet");
                    throw OpError("unknown op: " + name_text);
                }
                OpContext context{work, *current, actor};
                (*function)(context);
                validate_color_document(work);
                report = std::move(context.report);
            }
        } catch (const ApplyError&) {
            throw;
        } catch (const OpError& error) {
            throw ApplyError(prefix + error.what() + usage(*current));
        } catch (const OpKeyError& error) {
            throw ApplyError(prefix + "not found: " + error.what() +
                             " (a key the op needs, or an id the book does not have)" + usage(*current));
        } catch (const PyUncaught& error) {
            // (a traceback's last line: the exception's type alone when it has no message)
            const std::string what = error.what();
            throw ApplyError(prefix + error.type() + (what.empty() ? std::string() : ": " + what), "python_error");
        } catch (const Error& error) {
            if (error.code() == "not_yet_ported") throw ApplyError(prefix + error.what(), "not_yet_ported");
            throw ApplyError(prefix + "a value of the wrong type (" + error.what() + ")" + usage(*current));
        }
        result.applied.push_back(name_text);
        // (Python: op.pop("_report") — what the op reported, or the key as it was given)
        if (const auto given = current->find("_report"); given != current->end()) {
            if (!report) report = *given;
            current->erase(given);
        }
        if (report && py_truthy(*report)) {
            if (!report->is_object()) {
                throw ApplyError(prefix + "TypeError: '" + py_type_name(*report) + "' object is not a mapping", "python_error");
            }
            Json item = Json::object();
            item["index"] = static_cast<std::int64_t>(i);
            item["op"] = name_text;
            for (const auto& [key, value] : report->items()) item[key] = value;
            result.results.push_back(std::move(item));
        }
    }
    refuse_changed_unread_pages(doc, work);
    for (auto& warning : validate_document(work)) result.warnings.push_back(std::move(warning));
    for (auto& warning : unknown_key_warnings(ops)) result.warnings.push_back(std::move(warning));
    for (const Json& op : ops) result.journal_ops.push_back(journal_op(op));
    result.doc = std::move(work);
    return result;
}

}  // namespace genko::core
