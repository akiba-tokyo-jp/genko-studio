#include "app/templates.hpp"

#include <QFile>

#include <algorithm>
#include <cmath>
#include <set>

#include "app/config.hpp"
#include "app/frame_tools.hpp"
#include "app/icons.hpp"
#include "core/actor.hpp"
#include "core/command_bus.hpp"
#include "core/error.hpp"
#include "core/frames.hpp"
#include "core/ids.hpp"
#include "core/paths.hpp"
#include "core/pyconv.hpp"
#include "core/pynum.hpp"
#include "render/ops_registry.hpp"
#include "storage/fsutil.hpp"

namespace genko::app::templates {

namespace {

using core::Json;

constexpr double kTierGutter = 7.0;  // (layout.TIER_GUTTER_MM)
constexpr double kColGutter = 3.0;   // (layout.COL_GUTTER_MM)

std::filesystem::path mine_file() { return config_dir() / "panel_templates.json"; }

Json read_mine() {
    try {
        const Json data = core::parse_python_json(storage::read_file(mine_file()));
        return data.is_array() ? data : Json::array();
    } catch (const std::exception&) {
        return Json::array();
    }
}

void write_mine(const Json& items) {
    core::DumpOptions options;
    options.indent = 1;
    options.item_separator = ",";
    options.key_separator = ": ";
    storage::make_dirs_durable(config_dir());
    storage::write_atomic(mine_file(), core::dump(items, options));
}

double f(const Json& value) { return core::py_float(value); }

// The cuts and looks, made one by one on a copy of the book (each op applied as it is made, so the next one can name
// the panels it made), with every id new_id() gave on the way.
class Builder {
public:
    Builder(const core::Document& doc, std::size_t index, std::string actor)
        : doc_(doc), index_(index), actor_(std::move(actor)), number_(doc.page(index).index.json()) {}

    void apply(Json op) {
        const core::CommandBus bus(render::ops_registry());
        core::ScopedIdScript script;
        core::ApplyResult result = bus.apply(doc_, Json::array({op}), core::Actor(actor_));
        doc_ = std::move(result.doc);
        out_.ops.push_back(std::move(op));
        out_.ids.insert(out_.ids.end(), script.taken().begin(), script.taken().end());
    }

    const core::Frame& find(const std::string& id) const {
        const core::Frame* frame = doc_.page(index_).find_frame(id);
        if (frame == nullptr) throw core::Error("key", "no frame " + id);
        return *frame;
    }

    const core::Page& page() const { return doc_.page(index_); }
    const Json& number() const { return number_; }
    Plan done() { return std::move(out_); }

    // layout._split_sequence: split a panel into len(ratios) parts; their ids in reading order (top to bottom;
    // columns right to left).
    std::vector<std::string> split_sequence(const std::string& frame_id, const std::vector<double>& ratios, const std::string& axis,
                                            double gutter, const std::vector<double>& tilts) {
        if (ratios.size() == 1) return {frame_id};
        const core::Rect& rect = find(frame_id).rect;
        double total = 0.0;
        for (const double r : ratios) total += r;
        const double extent = axis == "horizontal" ? rect.height.value() : rect.width.value();
        const double usable = extent - gutter * static_cast<double>(ratios.size() - 1);
        std::vector<std::string> parts;
        std::string remaining = frame_id;
        for (std::size_t i = 0; i + 1 < ratios.size(); ++i) {
            const double span = usable * ratios[i] / total;
            const core::Rect& r = find(remaining).rect;
            const double rest = (axis == "horizontal" ? r.height.value() : r.width.value()) - gutter;
            const double ratio = axis == "horizontal" ? span / rest : (rest - span) / rest;
            Json op = Json::object({{"op", "split_frame"}, {"page", number_}, {"frame_id", remaining}, {"axis", axis},
                                    {"ratio", ratio}, {"gutter_mm", gutter}});
            if (i < tilts.size() && std::abs(tilts[i]) > 1e-6) op["tilt_mm"] = core::py_round(tilts[i], 2);  // (斜め)
            apply(op);
            const core::Frame& node = find(remaining);
            if (node.children.size() != 2) throw core::Error("value", "the split did not make two panels");
            if (axis == "horizontal") {
                parts.push_back(node.children[0].id);
                remaining = node.children[1].id;
            } else {
                parts.push_back(node.children[1].id);
                remaining = node.children[0].id;
            }
        }
        parts.push_back(remaining);
        return parts;
    }

private:
    core::Document doc_;
    std::size_t index_;
    std::string actor_;
    Json number_;
    Plan out_;
};

// layout.apply_layout for a built-in template (no panel briefs: the template's own bleeds and slants).
void lay_out(Builder& b, const Json& spec) {
    const Json& tiers = spec.at("tiers");
    std::set<std::string> bleeds;
    if (spec.contains("bleed") && spec["bleed"].is_array()) {
        for (const Json& slot : spec["bleed"]) bleeds.insert(core::py_str(slot));
    }
    std::map<std::string, double> slants;
    if (spec.contains("slant") && spec["slant"].is_object()) {
        for (const auto& [slot, value] : spec["slant"].items()) slants[slot] = f(value);
    }
    const auto slant_of = [&](const std::optional<std::string>& slot) {
        if (!slot) return 0.0;
        const auto it = slants.find(*slot);
        return it == slants.end() ? 0.0 : it->second;
    };
    // the slot of a tier or column that is one panel (its border to the next is that panel's)
    const auto leaf = [](const auto& self, const Json& part) -> std::optional<std::string> {
        if (part.contains("cols") && !part["cols"].is_null()) {
            const Json& cols = part["cols"];
            return cols.size() == 1 ? self(self, cols[0]) : std::nullopt;
        }
        if (part.contains("rows") && core::py_truthy(part["rows"])) return std::nullopt;
        if (part.contains("slot")) return core::py_str(part["slot"]);
        return std::nullopt;
    };
    std::map<std::string, std::string> slot_to_frame;
    std::vector<double> heights;
    std::vector<double> tier_tilts;
    for (const Json& tier : tiers) {
        heights.push_back(f(tier["h"]));
        tier_tilts.push_back(slant_of(leaf(leaf, tier)));
    }
    const std::vector<std::string> tier_ids =
        b.split_sequence(b.page().frames.at(0).id, heights, "horizontal", kTierGutter, tier_tilts);
    for (std::size_t ti = 0; ti < tiers.size(); ++ti) {
        const Json& cols = tiers[ti]["cols"];
        std::vector<double> widths;
        std::vector<double> col_tilts;
        for (const Json& col : cols) {
            widths.push_back(f(col["w"]));
            col_tilts.push_back(slant_of(leaf(leaf, col)));
        }
        const std::vector<std::string> col_ids = b.split_sequence(tier_ids[ti], widths, "vertical", kColGutter, col_tilts);
        for (std::size_t ci = 0; ci < cols.size(); ++ci) {
            const Json& col = cols[ci];
            if (col.contains("rows") && core::py_truthy(col["rows"])) {
                std::vector<double> row_heights;
                std::vector<double> row_tilts;
                for (const Json& row : col["rows"]) {
                    row_heights.push_back(f(row["h"]));
                    row_tilts.push_back(slant_of(row.contains("slot") ? std::optional<std::string>(core::py_str(row["slot"])) : std::nullopt));
                }
                const std::vector<std::string> row_ids = b.split_sequence(col_ids[ci], row_heights, "horizontal", kTierGutter, row_tilts);
                for (std::size_t ri = 0; ri < row_ids.size(); ++ri) slot_to_frame[core::py_str(col["rows"][ri]["slot"])] = row_ids[ri];
            } else {
                slot_to_frame[core::py_str(col["slot"])] = col_ids[ci];
            }
        }
    }
    for (const std::string& slot : bleeds) {  // (sorted, as Python's sorted(bleeds))
        const auto it = slot_to_frame.find(slot);
        if (it == slot_to_frame.end()) continue;
        b.apply(Json::object({{"op", "set_frame"}, {"page", b.number()}, {"frame_id", it->second}, {"bleed", true}}));
    }
}

core::Rect rect_of(const Json& node) {
    const Json& r = node.at("rect_mm");
    return core::Rect{core::Num(f(r[0])), core::Num(f(r[1])), core::Num(f(r[2])), core::Num(f(r[3]))};
}

// A person's template: the tree's cuts and looks, made with the panel ops.
void build(Builder& b, const Json& node, const std::string& frame_id) {
    const bool has_children = node.contains("children") && node["children"].is_array() && !node["children"].empty();
    const std::string axis = node.contains("axis") && node["axis"].is_string() ? node["axis"].get<std::string>() : std::string();
    if (has_children) {
        const Json& kids = node["children"];
        if (axis == "free") {
            throw core::Error("value", "描いたコマのテンプレートは、この版ではまだ割れません（割ったコマのテンプレートは使えます）");
        }
        if (node.contains("split") && node["split"].is_object() && node["split"].contains("a") && kids.size() == 2) {
            // the cut as it was kept (in the panel's own box: it fits this panel's size)
            core::Frame cut = b.find(frame_id);
            cut.split = node["split"];
            cut.split_axis = axis.empty() ? std::nullopt : std::optional<std::string>(axis);
            const auto line = core::cut_line(cut);
            if (!line) throw core::Error("value", "テンプレートの割り方が読めません");
            b.apply(Json::object({{"op", "cut_frame"},
                                  {"page", b.number()},
                                  {"frame_id", frame_id},
                                  {"p0", Json::array({core::py_round(line->p0.x.value(), 3), core::py_round(line->p0.y.value(), 3)})},
                                  {"p1", Json::array({core::py_round(line->p1.x.value(), 3), core::py_round(line->p1.y.value(), 3)})},
                                  {"gutter_mm", line->gutter}}));
            const core::Frame& parent = b.find(frame_id);
            const std::string first = parent.children.at(0).id;
            const std::string second = parent.children.at(1).id;
            build(b, kids[0], first);
            build(b, kids[1], second);
        } else {
            const bool horizontal = axis == "horizontal";
            std::vector<const Json*> sorted;
            for (const Json& kid : kids) sorted.push_back(&kid);
            std::stable_sort(sorted.begin(), sorted.end(), [&](const Json* a, const Json* c) {
                return horizontal ? rect_of(*a).y.value() < rect_of(*c).y.value() : rect_of(*a).x.value() < rect_of(*c).x.value();
            });
            std::vector<double> ratios;
            double gaps = 0.0;
            for (std::size_t i = 0; i < sorted.size(); ++i) {
                const core::Rect r = rect_of(*sorted[i]);
                ratios.push_back(horizontal ? r.height.value() : r.width.value());
                if (i > 0) {
                    const core::Rect before = rect_of(*sorted[i - 1]);
                    gaps += horizontal ? r.y.value() - (before.y + before.height).value() : r.x.value() - (before.x + before.width).value();
                }
            }
            const double gutter = sorted.size() > 1 ? std::max(0.0, gaps / static_cast<double>(sorted.size() - 1)) : 0.0;
            std::vector<std::string> parts = b.split_sequence(frame_id, ratios, horizontal ? "horizontal" : "vertical", gutter, {});
            if (!horizontal) std::reverse(parts.begin(), parts.end());  // (split_sequence lists columns right to left)
            for (std::size_t i = 0; i < sorted.size(); ++i) build(b, *sorted[i], parts[i]);
        }
        return;
    }
    // a panel's look
    Json change = Json::object();
    if (node.contains("poly") && core::py_truthy(node["poly"])) change["poly"] = node["poly"];
    if (node.contains("bleed") && core::py_truthy(node["bleed"])) change["bleed"] = true;
    if (node.contains("clip") && node["clip"].is_boolean() && !node["clip"].get<bool>()) change["clip"] = false;
    if (node.contains("border_mm")) change["border_mm"] = node["border_mm"];
    if (node.contains("curves") && core::py_truthy(node["curves"])) change["curves"] = node["curves"];
    if (node.contains("line") && core::py_truthy(node["line"])) change["line"] = node["line"];
    if (node.contains("corner_mm") && core::py_truthy(node["corner_mm"])) change["corner_mm"] = node["corner_mm"];
    if (change.empty()) return;
    Json op = Json::object({{"op", "set_frame"}, {"page", b.number()}, {"frame_id", frame_id}});
    for (auto& [key, value] : change.items()) op[key] = value;
    b.apply(op);
}

// layout.scaled_tree: the same tree on paper sx × wider and sy × taller.
Json scaled(const Json& node, double sx, double sy) {
    Json out = node;
    const Json& r = node.at("rect_mm");
    out["rect_mm"] = Json::array({core::py_round(f(r[0]) * sx, 3), core::py_round(f(r[1]) * sy, 3), core::py_round(f(r[2]) * sx, 3),
                                  core::py_round(f(r[3]) * sy, 3)});
    if (node.contains("poly") && core::py_truthy(node["poly"])) {
        Json poly = Json::array();
        for (const Json& p : node["poly"]) poly.push_back(Json::array({core::py_round(f(p[0]) * sx, 3), core::py_round(f(p[1]) * sy, 3)}));
        out["poly"] = poly;
    }
    if (node.contains("children") && core::py_truthy(node["children"])) {
        Json children = Json::array();
        for (const Json& child : node["children"]) children.push_back(scaled(child, sx, sy));
        out["children"] = children;
    }
    return out;
}

}  // namespace

std::vector<Template> builtin() {
    icons::init_resources();
    QFile file(QStringLiteral(":/genko/templates/layouts.json"));
    std::vector<Template> out;
    if (!file.open(QIODevice::ReadOnly)) return out;
    const QByteArray bytes = file.readAll();
    const Json all = core::parse_python_json(std::string_view(bytes.constData(), static_cast<std::size_t>(bytes.size())));
    for (const auto& [key, spec] : all.items()) {
        Template t;
        t.key = key;
        const std::string description = spec.contains("description") ? core::py_str(spec["description"]) : std::string();
        t.label = QString::fromStdString(description.empty() ? key : description);
        t.spec = spec;
        out.push_back(std::move(t));
    }
    return out;
}

std::vector<Template> mine() {
    std::vector<Template> out;
    for (const Json& item : read_mine()) {
        if (!item.is_object() || !item.contains("tree") || !item.contains("size") || !core::py_truthy(item["tree"])) continue;
        Template t;
        t.key = item.contains("name") ? core::py_str(item["name"]) : std::string();
        t.label = QStringLiteral("自分: %1").arg(QString::fromStdString(t.key));
        t.mine = true;
        t.spec = item;
        out.push_back(std::move(t));
    }
    return out;
}

void save_mine(const QString& name, const core::Page& page) {
    Json kept = Json::array();
    for (const Json& item : read_mine()) {
        if (item.is_object() && item.contains("name") && core::py_str(item["name"]) == name.toStdString()) continue;
        kept.push_back(item);
    }
    Json entry = Json::object();
    entry["name"] = name.toStdString();
    entry["size"] = Json::array({page.spec.width_mm.json(), page.spec.height_mm.json()});
    entry["tree"] = frame_tree(page.frames.at(0));
    kept.push_back(entry);
    write_mine(kept);
}

void remove_mine(const QString& name) {
    Json kept = Json::array();
    for (const Json& item : read_mine()) {
        if (item.is_object() && item.contains("name") && core::py_str(item["name"]) == name.toStdString()) continue;
        kept.push_back(item);
    }
    write_mine(kept);
}

bool is_blank(const core::Document& doc, const core::Page& page) {
    return page.leaf_frames().size() == 1 && doc.story_for_page(page.index).empty();
}

Plan plan(const core::Document& doc, std::size_t page_index, const Template& chosen, const std::string& actor) {
    const core::Page& page = doc.page(page_index);
    if (!doc.story_for_page(page.index).empty()) {
        throw core::Error("value", "台詞のあるページは、この版ではまだテンプレートで割り直せません（台詞の操作は次の段階で入ります）");
    }
    if (page.frames.empty()) throw core::Error("value", "このページにはコマがありません");
    if (core::is_free(page.frames[0])) {
        throw core::Error("value", "描いたコマのページは、この版ではまだテンプレートで割り直せません（コマを基本枠に戻す操作は次の段階で入ります）");
    }
    Builder b(doc, page_index, actor);
    // merge_frame collapses the root's cuts: the page is back to its basic frame
    if (!page.frames[0].children.empty()) {
        b.apply(Json::object({{"op", "merge_frame"}, {"page", b.number()}, {"frame_id", page.frames[0].children[0].id}}));
    }
    if (chosen.mine) {
        const Json& size = chosen.spec.at("size");
        const double w = f(size[0]);
        const double h = f(size[1]);
        const Json tree = scaled(chosen.spec.at("tree"), page.spec.width_mm.value() / (w != 0.0 ? w : 1.0),
                                 page.spec.height_mm.value() / (h != 0.0 ? h : 1.0));
        build(b, tree, b.page().frames.at(0).id);
    } else {
        lay_out(b, chosen.spec);
    }
    return b.done();
}

}  // namespace genko::app::templates
