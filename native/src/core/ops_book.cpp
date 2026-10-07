// The book and page ops of M1 (Python's ops._apply_one): set_note, set_meta, name_ok, set_autosave, add_page and
// delete_page. lock_page and unlock_page take effect in check_page_lock, with the actor.

#include <algorithm>
#include <cstdint>
#include <memory>

#include "core/command_bus.hpp"
#include "core/covers.hpp"
#include "core/ids.hpp"
#include "core/pyconv.hpp"
#include "core/pyops.hpp"
#include "core/ops_util.hpp"
#include <cmath>

namespace genko::core {

namespace {

// Python's _covered: a page's paper on the book's spec (a cover's is its own).
PageSpec covered(const Page& page, const PageSpec& spec) {
    const Json* cover = cover_of(page);
    return cover != nullptr ? spec_for(spec, *cover) : spec;
}

// Python's list slicing bounds: a[:n] and a[n:].
std::size_t slice_at(std::int64_t n, std::size_t size) {
    const auto length = static_cast<std::int64_t>(size);
    if (n < 0) n = std::max<std::int64_t>(0, n + length);
    return static_cast<std::size_t>(std::min(n, length));
}

void set_nombre(OpContext& c) {
    Json change = Json::object();
    for (const char* key : {"position","font","size_mm","start","hidden","hidden_size_mm","show"})
        if (const Json* value = get(c.op,key)) change[key] = *value;
    if (const auto* position = get(change,"position")) {
        const std::string s = py_str(*position);
        if (!position->is_string() || (s!="bottom_center" && s!="bottom_outside" && s!="top_outside" && s!="side_outside")) throw OpError("unknown nombre position");
    }
    for (const char* key : {"size_mm","hidden_size_mm"}) if (const auto* value=get(change,key)) {
        const double size=py_float(*value);
        const double maximum=std::string_view(key)=="size_mm"?20.0:10.0;
        if (!std::isfinite(size) || size<1 || size>maximum) throw OpError(std::string(key)+(maximum==20?" is 1 to 20":" is 1 to 10"));
    }
    if (const auto* value=get(change,"start");value && py_int(*value)<0)throw OpError("start is 0 or more");
    if (const auto* value=get(change,"font");value && py_truthy(*value)) {
        require_hashable(*value);
        const auto& keys=Json::array({"antique","gothic","mincho","maru","hand","sfx","sfx_pop"});
        if(!value->is_string() || std::find(keys.begin(),keys.end(),*value)==keys.end())throw OpError("font must be a bundled typeface");
    }
    require_finite(change);
    // The bus owns a copy-on-write transaction; validate before adopting either the page flag or settings.
    if (const auto* page=get(c.op,"page");page && !page->is_null() && c.op.contains("numero")) c.doc.edit_page(require_page(c.doc,c.op)).numero=py_truthy(c.op.at("numero"));
    if (!c.doc.nombre.is_object()) throw OpError("nombre settings must be an object");
    for (const auto& [key,value] : change.items()) c.doc.nombre[key]=value;
}

void set_note(OpContext& c) {
    const std::size_t i = require_page(c.doc, c.op);
    const Json* note = get(c.op, "note");
    c.doc.edit_page(i).note = note != nullptr ? py_str(*note) : std::string();
}

void set_meta(OpContext& c) {
    Document& doc = c.doc;
    if (const Json* title = get(c.op, "title")) doc.title = py_str(*title);
    if (const Json* episode = get(c.op, "episode")) doc.episode = Num(py_int(*episode));
    if (const Json* value = get(c.op, "binding")) {
        const auto binding = value->is_string() ? binding_from(value->get_ref<const std::string&>()) : std::nullopt;
        if (!binding) throw Error("value", py_repr(*value) + " is not a valid Binding");
        doc.binding = *binding;
        for (std::size_t i = 0; i < doc.pages.size(); ++i) {
            if (doc.pages[i]->binding != *binding) doc.edit_page(i).binding = *binding;
        }
    }
    if (const Json* side = get(c.op, "start_side")) {
        const bool valid = side->is_null() || (side->is_string() && (side->get_ref<const std::string&>() == "left" ||
                                                                     side->get_ref<const std::string&>() == "right"));
        if (!valid) throw OpError("start_side must be left, right or null");
        doc.start_side = side->is_null() ? std::nullopt : std::optional<std::string>(side->get<std::string>());
    }
    if (const Json* strict = get(c.op, "strict_gates")) doc.strict_gates = py_truthy(*strict);
    const auto respec = [&doc](const PageSpec& spec) {
        doc.spec = spec;
        for (std::size_t i = 0; i < doc.pages.size(); ++i) {
            Page& page = doc.edit_page(i);
            page.spec = covered(page, doc.spec);
        }
    };
    if (const Json* preset = get(c.op, "preset")) respec(PageSpec::publisher(py_str(*preset)));
    if (const Json* webtoon = get(c.op, "webtoon"); webtoon != nullptr && py_truthy(*webtoon)) {
        respec(PageSpec::webtoon());
    }
    if (const Json* font = get(c.op, "font_path")) doc.font_path = py_str(*font);
}

void name_ok(OpContext& c) {
    // page.name_ok = True, then pipeline.advance(page, to="ink") (which only refuses a page without name_ok)
    const auto approve = [&c](std::size_t i) {
        const Page& page = c.doc.page(i);
        if (page.name_ok && page.stage == "ink") return;
        Page& edited = c.doc.edit_page(i);
        edited.name_ok = true;
        edited.stage = "ink";
    };
    if (get(c.op, "page") == nullptr) {
        for (std::size_t i = 0; i < c.doc.pages.size(); ++i) approve(i);
    } else {
        approve(require_page(c.doc, c.op));
    }
}

void set_autosave(OpContext& c) {
    const Json* enabled = get(c.op, "enabled");
    c.doc.autosave = enabled == nullptr || py_truthy(*enabled);
}

void add_page(OpContext& c) {
    Document& doc = c.doc;
    const Json* count_value = get(c.op, "count");
    const std::int64_t count = count_value != nullptr ? py_int(*count_value) : 1;
    if (count < 1 || count > 200) throw OpError("count is 1 to 200");
    // after: the op's value (int() of it is taken where Python takes it), or the last page that is not a cover
    std::optional<Json> after_value;
    if (const Json* after = get(c.op, "after"); after != nullptr && !after->is_null()) after_value = *after;
    if (after_value) {
        bool found = false;
        for (const auto& page : doc.pages) {
            if (page->index == Num(py_int(*after_value))) {
                found = true;
                break;
            }
        }
        if (!found && py_int(*after_value) != 0) throw OpError("no page " + py_str(*after_value));
    }
    std::optional<Num> after_page;
    const bool any_cover = std::any_of(doc.pages.begin(), doc.pages.end(),
                                       [](const PagePtr& page) { return cover_of(*page) != nullptr; });
    if (!after_value && any_cover) {  // (new pages go before the covers at the end)
        Num last(0);
        bool first = true;
        for (const auto& page : doc.pages) {
            if (cover_of(*page) != nullptr) continue;
            if (first || last < page->index) last = page->index;
            first = false;
        }
        after_page = last;
    }
    const std::size_t first_new = doc.pages.size() + 1;
    for (std::int64_t n = 0; n < count; ++n) {
        const auto index = static_cast<std::int64_t>(doc.pages.size()) + 1;
        Page page = make_page(Num(index), doc.spec, doc.binding);
        Frame root;
        root.id = new_id();
        root.rect = page.inner_rect_mm();
        page.frames.push_back(std::move(root));
        doc.pages.push_back(std::make_shared<Page>(std::move(page)));
    }
    std::optional<std::int64_t> after;
    if (after_value) after = py_int(*after_value);
    if (after_page) after = py_int(*after_page);
    if (after && *after < static_cast<std::int64_t>(first_new) - 1) {
        std::vector<Num> old;
        for (const auto& page : doc.pages) old.push_back(page->index);
        const std::vector<Num> fresh(old.begin() + static_cast<std::ptrdiff_t>(first_new - 1), old.end());
        const std::vector<Num> kept(old.begin(), old.begin() + static_cast<std::ptrdiff_t>(first_new - 1));
        const std::size_t cut = slice_at(*after, kept.size());
        std::vector<Num> order(kept.begin(), kept.begin() + static_cast<std::ptrdiff_t>(cut));
        order.insert(order.end(), fresh.begin(), fresh.end());
        order.insert(order.end(), kept.begin() + static_cast<std::ptrdiff_t>(cut), kept.end());
        reorder_pages(doc, order);
    }
}

void delete_page(OpContext& c) {
    Document& doc = c.doc;
    const std::size_t i = require_page(doc, c.op);
    if (doc.pages.size() == 1) throw OpError("cannot delete the last page");
    const Num removed = doc.pages[i]->index;
    const std::string removed_id = doc.pages[i]->id;
    std::vector<PagePtr> pages;
    for (const auto& page : doc.pages) {
        if (!(page->index == removed)) pages.push_back(page);
    }
    doc.pages = std::move(pages);
    std::erase_if(doc.story, [&removed](const StoryLine& line) { return line.page_index == removed; });
    if (doc.page_locks.is_object()) doc.page_locks.erase(removed_id);
    PageMapping mapping;
    for (std::size_t n = 0; n < doc.pages.size(); ++n) mapping.set(doc.pages[n]->index, static_cast<std::int64_t>(n + 1));
    mapping.set(removed, std::nullopt);
    remap_page_refs(doc, mapping);
}

void handled_by_check(OpContext&) {}  // lock_page, unlock_page: see check_page_lock

}  // namespace

void register_book_ops(OpRegistry& registry) {
    registry.add("set_note", set_note);
    registry.add("set_meta", set_meta);
    registry.add("set_nombre", set_nombre);
    registry.add("name_ok", name_ok);
    registry.add("lock_page", handled_by_check);
    registry.add("unlock_page", handled_by_check);
    registry.add("set_autosave", set_autosave);
    registry.add("add_page", add_page);
    registry.add("delete_page", delete_page);
}

}  // namespace genko::core
