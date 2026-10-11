#include "service/atari.hpp"

#include <algorithm>
#include <cmath>

#include "core/error.hpp"
#include "core/paths.hpp"
#include "core/pyconv.hpp"
#include "core/pynum.hpp"
#include "core/pyops.hpp"
#include "formats/pillow_save.hpp"
#include "render/draw.hpp"
#include "render/page.hpp"
#include "render/text/fonts.hpp"
#include "service/letter.hpp"
#include "service/pyfmt.hpp"
#include "service/service_util.hpp"
#include "service/sheets.hpp"
#include "service/text.hpp"
#include "storage/asset_store.hpp"
#include "storage/fsutil.hpp"
#include "studio/files.hpp"

namespace genko::service::xycut {

namespace {

using core::py_round_int;

struct Node {
    Box box{};
    std::string axis;  // "" for a leaf
    std::vector<Node> children;
    double gutter = 1.0;
    double border = 0.0;
};

int px(double v) { return static_cast<int>(py_round_int(v)); }

render::Image binarize(const render::Image& image, double dpi, const Params& params) {
    const render::Image gray = image.convert("L");
    const int t = std::min(otsu(gray), 200);
    render::Image ink = gray.point([t](int v) { return v <= t ? 255 : 0; });
    const int grow = std::max(1, px(params.get("thicken_mm") / 25.4 * dpi));
    if (grow > 1) ink = ink.filter(render::Filter::max_filter(grow % 2 != 0 ? grow : grow + 1));
    return ink;
}

render::Image drop_specks(const render::Image& ink, int max_px) {
    const int w = ink.width();
    const int h = ink.height();
    std::string data = ink.tobytes();
    std::vector<unsigned char> seen(static_cast<std::size_t>(w) * h, 0);
    const std::int64_t n = static_cast<std::int64_t>(w) * h;
    std::vector<std::int64_t> stack;
    std::vector<std::int64_t> blob;
    for (std::int64_t start = 0; start < n; ++start) {
        if (data[static_cast<std::size_t>(start)] == 0 || seen[static_cast<std::size_t>(start)] != 0) continue;
        stack.assign(1, start);
        seen[static_cast<std::size_t>(start)] = 1;
        blob.clear();
        std::int64_t x0 = start % w, x1 = start % w, y0 = start / w, y1 = start / w;
        while (!stack.empty()) {
            const std::int64_t i = stack.back();
            stack.pop_back();
            blob.push_back(i);
            const std::int64_t y = i / w;
            const std::int64_t x = i % w;
            x0 = std::min(x0, x), x1 = std::max(x1, x), y0 = std::min(y0, y), y1 = std::max(y1, y);
            for (const std::int64_t j : {i - w - 1, i - w, i - w + 1, i - 1, i + 1, i + w - 1, i + w, i + w + 1}) {
                if (0 <= j && j < n && data[static_cast<std::size_t>(j)] != 0 && seen[static_cast<std::size_t>(j)] == 0 && std::abs(j % w - x) <= 1) {
                    seen[static_cast<std::size_t>(j)] = 1;
                    stack.push_back(j);
                }
            }
        }
        if (x1 - x0 < max_px && y1 - y0 < max_px) {
            for (const std::int64_t i : blob) data[static_cast<std::size_t>(i)] = 0;
        }
    }
    return render::Image::frombytes("L", render::Size{w, h}, data);
}

std::vector<int> profile(const render::Image& ink, const Box& box, bool horizontal) {
    const render::Image region = ink.crop(render::Box{box[0], box[1], box[2], box[3]});
    const int w = region.width();
    const int h = region.height();
    std::vector<int> out;
    if (horizontal) {
        const std::string rows = region.resize(render::Size{1, h}, render::Resample::Box).tobytes();
        for (unsigned char v : rows) out.push_back(px(static_cast<double>(v) * w / 255));
    } else {
        const std::string cols = region.resize(render::Size{w, 1}, render::Resample::Box).tobytes();
        for (unsigned char v : cols) out.push_back(px(static_cast<double>(v) * h / 255));
    }
    return out;
}

std::vector<std::pair<int, int>> gaps(const std::vector<int>& prof, double limit, int min_run) {
    std::vector<std::pair<int, int>> out;
    std::optional<int> start;
    const int n = static_cast<int>(prof.size());
    for (int i = 0; i <= n; ++i) {
        const double value = i < n ? prof[static_cast<std::size_t>(i)] : limit + 1;
        const bool empty = value <= limit;
        if (empty && !start) {
            start = i;
        } else if (!empty && start) {
            if (*start > 0 && i < n && i - *start >= min_run) out.emplace_back(*start, i);
            start.reset();
        }
    }
    return out;
}

bool flanked(const render::Image& ink, const Box& box, bool horizontal, int a, int b, double px_per_mm, double cover = 0.5) {
    const auto [x0, y0, x1, y1] = box;
    const int band = std::max(2, px(3.0 * px_per_mm));
    std::array<Box, 2> sides{};
    if (horizontal) {  // a horizontal gutter at rows a..b: long horizontal lines above and below
        sides = {Box{x0, y0 + std::max(0, a - band), x1, y0 + a}, Box{x0, y0 + b, x1, y0 + std::min(y1 - y0, b + band)}};
    } else {
        sides = {Box{x0 + std::max(0, a - band), y0, x0 + a, y1}, Box{x0 + b, y0, x0 + std::min(x1 - x0, b + band), y1}};
    }
    for (const Box& side : sides) {
        if (side[2] <= side[0] || side[3] <= side[1]) return false;
        const render::Image region = ink.crop(render::Box{side[0], side[1], side[2], side[3]});
        const render::Image lines = horizontal ? region.transpose(render::Transpose::Transpose) : region;
        const int w = lines.width();
        const int h = lines.height();
        double best = 0.0;
        for (int i = 0; i < w; ++i) {
            const std::string column = lines.crop(render::Box{i, 0, i + 1, h}).tobytes();
            const auto inked = std::count_if(column.begin(), column.end(), [](char v) { return v != 0; });
            best = core::py_max(best, static_cast<double>(inked) / std::max(1, h));
        }
        if (best < cover) return false;
    }
    return true;
}

std::optional<Box> trim(const render::Image& ink, const Box& box) {
    const auto bbox = ink.crop(render::Box{box[0], box[1], box[2], box[3]}).getbbox();
    if (!bbox) return std::nullopt;
    return Box{box[0] + bbox->x0, box[1] + bbox->y0, box[0] + bbox->x1, box[1] + bbox->y1};
}

double border(const render::Image& ink, const Box& box, int band) {
    const auto [x0, y0, x1, y1] = box;
    const std::array<std::pair<Box, bool>, 4> strips{
        std::pair<Box, bool>{Box{x0, y0, x1, std::min(y1, y0 + band)}, false}, std::pair<Box, bool>{Box{x0, std::max(y0, y1 - band), x1, y1}, false},
        std::pair<Box, bool>{Box{x0, y0, std::min(x1, x0 + band), y1}, true}, std::pair<Box, bool>{Box{std::max(x0, x1 - band), y0, x1, y1}, true}};
    std::vector<double> scores;
    for (const auto& [strip, horizontal] : strips) {
        if (strip[2] <= strip[0] || strip[3] <= strip[1]) continue;
        const std::vector<int> prof = profile(ink, strip, horizontal);
        const auto inked = std::count_if(prof.begin(), prof.end(), [](int v) { return v > 0; });
        scores.push_back(static_cast<double>(inked) / std::max<std::size_t>(1, prof.size()));
    }
    if (scores.empty()) return 0.0;
    return core::py_round(core::py_float_sum(scores) / static_cast<double>(scores.size()), 3);
}

std::optional<Node> cut(const render::Image& ink, const Box& box, double px_per_mm, const Params& params, int depth = 0,
                        const std::string& prefer = "horizontal") {
    const auto trimmed = trim(ink, box);
    if (!trimmed) return std::nullopt;
    const auto [x0, y0, x1, y1] = *trimmed;
    const double min_panel = params.get("min_panel_mm") * px_per_mm;
    if ((x1 - x0) < min_panel || (y1 - y0) < min_panel) return std::nullopt;
    const int min_gap = std::max(2, px(params.get("min_gutter_mm") * px_per_mm));
    struct Candidate {
        bool horizontal;
        int a;
        int b;
        double clean;
    };
    std::vector<Candidate> candidates;
    const int ends = px(4.0 * px_per_mm);  // corners overshoot into gutters: the region's two ends are not looked at
    for (const bool horizontal : {true, false}) {
        Box inner = *trimmed;
        if (horizontal && x1 - x0 > 3 * ends) inner = Box{x0 + ends, y0, x1 - ends, y1};
        if (!horizontal && y1 - y0 > 3 * ends) inner = Box{x0, y0 + ends, x1, y1 - ends};
        const std::vector<int> prof = profile(ink, inner, horizontal);
        for (const auto& [a, b] : gaps(prof, params.get("gutter_px"), min_gap)) {
            if (!flanked(ink, *trimmed, horizontal, a, b, px_per_mm)) continue;  // gaps in the pen line, not a gutter
            std::int64_t stray_sum = 0;
            for (int k = a; k < b; ++k) stray_sum += prof[static_cast<std::size_t>(k)];
            const double stray = static_cast<double>(stray_sum) / std::max(1, b - a);
            candidates.push_back(Candidate{horizontal, a, b, core::py_round(core::py_max(0.0, 1.0 - stray / (params.get("gutter_px") + 1)), 3)});
        }
    }
    if (!candidates.empty() && depth < 8) {
        const bool want_horizontal = prefer == "horizontal";
        const bool has_preferred = std::any_of(candidates.begin(), candidates.end(), [&](const Candidate& c) { return c.horizontal == want_horizontal; });
        const bool horizontal = has_preferred ? want_horizontal : !want_horizontal;
        std::vector<Candidate> cuts;
        for (const Candidate& c : candidates) {
            if (c.horizontal == horizontal) cuts.push_back(c);
        }
        std::stable_sort(cuts.begin(), cuts.end(), [](const Candidate& p, const Candidate& q) { return p.a < q.a; });
        std::vector<std::pair<int, int>> pieces;
        int start = 0;
        const int span = horizontal ? (y1 - y0) : (x1 - x0);
        for (const Candidate& c : cuts) {
            pieces.emplace_back(start, c.a);
            start = c.b;
        }
        pieces.emplace_back(start, span);
        std::vector<Node> children;
        for (const auto& [a, b] : pieces) {
            const Box sub = horizontal ? Box{x0, y0 + a, x1, y0 + b} : Box{x0 + a, y0, x0 + b, y1};
            if (auto node = cut(ink, sub, px_per_mm, params, depth + 1, horizontal ? "vertical" : "horizontal")) children.push_back(std::move(*node));
        }
        if (children.size() >= 2) {
            double gutter = cuts.front().clean;
            for (const Candidate& c : cuts) gutter = core::py_min(gutter, c.clean);
            return Node{*trimmed, horizontal ? "horizontal" : "vertical", std::move(children), core::py_round(gutter, 3), 0.0};
        }
        if (children.size() == 1) return std::move(children.front());
    }
    const int band = std::max(2, px(1.2 * px_per_mm));
    Node leaf;
    leaf.box = *trimmed;
    leaf.border = border(ink, *trimmed, band);
    return leaf;
}

Json to_mm(const Box& box, const std::array<double, 4>& placement, std::array<int, 2> size) {
    const auto [px_, py, pw, ph] = placement;
    const double sx = pw / size[0];
    const double sy = ph / size[1];
    return Json::array({core::py_round(px_ + box[0] * sx, 2), core::py_round(py + box[1] * sy, 2), core::py_round((box[2] - box[0]) * sx, 2),
                        core::py_round((box[3] - box[1]) * sy, 2)});
}

void reading_order(const Node& node, bool right_to_left, std::vector<const Node*>& out) {
    if (node.children.empty()) {
        out.push_back(&node);
        return;
    }
    std::vector<const Node*> kids;
    for (const Node& c : node.children) kids.push_back(&c);
    if (node.axis == "vertical" && right_to_left) std::reverse(kids.begin(), kids.end());
    for (const Node* kid : kids) reading_order(*kid, right_to_left, out);
}

Json tree_dict(const Node& node, const std::array<double, 4>& placement, std::array<int, 2> size) {
    Json out = Json::object({{"rect_mm", to_mm(node.box, placement, size)}});
    if (!node.children.empty()) {
        out["axis"] = node.axis;
        Json kids = Json::array();
        for (const Node& c : node.children) kids.push_back(tree_dict(c, placement, size));
        out["children"] = std::move(kids);
    }
    return out;
}

Json tiers_of(const Node& root, bool right_to_left) {
    std::vector<const Node*> tiers_nodes;
    if (root.axis == "horizontal") {
        for (const Node& c : root.children) tiers_nodes.push_back(&c);
    } else {
        tiers_nodes.push_back(&root);
    }
    double total_h = 0;
    for (const Node* n : tiers_nodes) total_h += n->box[3] - n->box[1];
    Json tiers = Json::array();
    int slot = 0;
    for (const Node* tier : tiers_nodes) {
        std::vector<const Node*> cols_nodes;
        if (tier->axis == "vertical") {
            for (const Node& c : tier->children) cols_nodes.push_back(&c);
        } else {
            cols_nodes.push_back(tier);
        }
        if (right_to_left) std::reverse(cols_nodes.begin(), cols_nodes.end());
        double total_w = 0;
        for (const Node* n : cols_nodes) total_w += n->box[2] - n->box[0];
        Json cols = Json::array();
        for (const Node* col : cols_nodes) {
            Json rows;
            if (!col->children.empty()) {
                if (col->axis != "horizontal") return Json();
                for (const Node& r : col->children) {
                    if (!r.children.empty()) return Json();
                }
                double total_r = 0;
                for (const Node& r : col->children) total_r += r.box[3] - r.box[1];
                rows = Json::array();
                for (const Node& r : col->children) {
                    ++slot;
                    rows.push_back(Json::object({{"slot", "p" + std::to_string(slot)}, {"h", core::py_round((r.box[3] - r.box[1]) / total_r, 3)}}));
                }
            } else {
                ++slot;
            }
            cols.push_back(Json::object({{"slot", core::py_truthy(rows) ? Json() : Json("p" + std::to_string(slot))},
                                         {"w", core::py_round((col->box[2] - col->box[0]) / total_w, 3)},
                                         {"rows", rows}}));
        }
        tiers.push_back(Json::object({{"h", core::py_round((tier->box[3] - tier->box[1]) / total_h, 3)}, {"cols", std::move(cols)}}));
    }
    for (Json& tier : tiers) {
        for (Json& col : tier["cols"]) {
            if (col["slot"].is_null()) col["slot"] = col["rows"][0]["slot"];
        }
    }
    return tiers;
}

}  // namespace

double Params::get(const char* name) const { return core::to_float(fields.at(name)); }

Params Params::from(const Json& given) {
    Params p;
    if (given.is_null()) return p;
    const Json dict = util::or_dict(given, "items");  // (held: items() of a temporary would dangle)
    for (const auto& [key, value] : dict.items()) {
        if (p.fields.contains(key)) p.fields[key] = value;
    }
    return p;
}

int otsu(const render::Image& gray) {
    const std::vector<long> hist = gray.histogram();
    std::int64_t total = 0;
    std::int64_t sum_all = 0;
    for (std::size_t i = 0; i < hist.size(); ++i) {
        total += hist[i];
        sum_all += static_cast<std::int64_t>(i) * hist[i];
    }
    double best = 0.0;
    int best_t = 128;
    std::int64_t weight_b = 0;
    double sum_b = 0.0;
    for (int t = 0; t < 256; ++t) {
        weight_b += hist[static_cast<std::size_t>(t)];
        if (weight_b == 0) continue;
        const std::int64_t weight_f = total - weight_b;
        if (weight_f == 0) break;
        sum_b += static_cast<double>(static_cast<std::int64_t>(t) * hist[static_cast<std::size_t>(t)]);
        const double mean_b = sum_b / static_cast<double>(weight_b);
        const double mean_f = (static_cast<double>(sum_all) - sum_b) / static_cast<double>(weight_f);
        const double d = mean_b - mean_f;
        const double between = static_cast<double>(weight_b * weight_f) * (d * d);
        if (between > best) {
            best = between;
            best_t = t;
        }
    }
    return best_t;
}

Detection detect(const render::Image& image, const std::array<double, 4>& placement_mm, const Params& params, bool right_to_left) {
    const render::Image gray = image.convert("L");
    const int threshold = otsu(gray);
    // work at a fixed resolution: fast, and the thresholds mean the same at any scan dpi
    const render::Size work{std::max(1, px(placement_mm[2] * params.get("work_px_per_mm"))), std::max(1, px(placement_mm[3] * params.get("work_px_per_mm")))};
    const render::Image small = image.size() != work ? gray.resize(work, render::Resample::Box) : gray;
    const std::array<int, 2> size{small.width(), small.height()};
    const double px_per_mm = size[0] / placement_mm[2];
    render::Image ink = binarize(small, px_per_mm * 25.4, params);
    ink = drop_specks(ink, std::max(2, px(params.get("speck_mm") * px_per_mm)));
    const auto root = cut(ink, Box{0, 0, size[0], size[1]}, px_per_mm, params);
    Detection out;
    out.threshold = threshold;
    if (!root) return out;
    std::vector<const Node*> leaves;
    reading_order(*root, right_to_left, leaves);
    std::vector<double> confidences;
    for (const Node* leaf : leaves) {
        const double factor = !root->children.empty() ? 0.5 + 0.5 * root->gutter : 1.0;
        const double confidence = core::py_round(core::py_min(1.0, leaf->border * 1.1) * factor, 3);
        confidences.push_back(confidence);
        out.leaves_mm.push_back(Json::object({{"rect_mm", to_mm(leaf->box, placement_mm, size)}, {"confidence", confidence}, {"border", leaf->border}}));
    }
    out.confidence = core::py_round(core::py_float_sum(confidences) / static_cast<double>(confidences.size()), 3);
    out.tree = tree_dict(*root, placement_mm, size);
    out.tiers = tiers_of(*root, right_to_left);
    return out;
}

std::optional<Box> content_box(const render::Image& image, double dpi_guess) {
    Params params;
    params.fields["thicken_mm"] = 0.3;
    const render::Image ink = binarize(image, dpi_guess, params);
    const int w = ink.width();
    const int h = ink.height();
    const std::vector<int> rows = profile(ink, Box{0, 0, w, h}, true);
    const std::vector<int> cols = profile(ink, Box{0, 0, w, h}, false);
    const int row_min = std::max(2, px(w * 0.02));
    const int col_min = std::max(2, px(h * 0.02));
    std::vector<int> ys;
    std::vector<int> xs;
    for (std::size_t i = 0; i < rows.size(); ++i) {
        if (rows[i] >= row_min) ys.push_back(static_cast<int>(i));
    }
    for (std::size_t i = 0; i < cols.size(); ++i) {
        if (cols[i] >= col_min) xs.push_back(static_cast<int>(i));
    }
    if (xs.empty() || ys.empty()) return std::nullopt;
    return Box{xs.front(), ys.front(), xs.back() + 1, ys.back() + 1};
}

}  // namespace genko::service::xycut

namespace genko::service::atari {

namespace fs = std::filesystem;
using core::Num;
using core::py_get;
using core::py_str;
using core::py_truthy;

std::pair<Json, std::string> placement_for(const render::Image& image, const core::Page& page, const std::string& align) {
    const Num W = page.spec.width_mm;
    const Num H = page.spec.height_mm;
    const Json whole = Json::array({0.0, 0.0, W.json(), H.json()});
    if (align == "page") return {whole, "page"};
    const auto box = xycut::content_box(image, image.width() / (W / Num(25.4)).value());
    if (!box || (static_cast<double>((*box)[2] - (*box)[0]) * ((*box)[3] - (*box)[1]) < 0.4 * image.width() * image.height() && align == "auto")) {
        return {whole, "page"};
    }
    const core::Rect live = page.inner_rect_mm();
    const int bw = (*box)[2] - (*box)[0];
    const int bh = (*box)[3] - (*box)[1];
    const double sx = live.width.value() / bw;
    const double sy = live.height.value() / bh;
    if (align == "auto" && std::abs(sx / sy - 1) > 0.08) return {whole, "page"};  // the drawing does not have the live area's shape
    const double s = (sx + sy) / 2;
    const double x = live.x.value() + live.width.value() / 2 - ((*box)[0] + bw / 2.0) * s;
    const double y = live.y.value() + live.height.value() / 2 - ((*box)[1] + bh / 2.0) * s;
    return {Json::array({core::py_round(x, 2), core::py_round(y, 2), core::py_round(image.width() * s, 2), core::py_round(image.height() * s, 2)}), "live"};
}

Json atari_of(const core::Page& page) {
    const Json plan = page.plan && py_truthy(*page.plan) ? *page.plan : Json::object();
    return py_get(plan, "atari");
}

Json analyze(const fs::path& project, const core::Document& doc, const core::Page& page, const Json& params) {
    const Json found_atari = atari_of(page);
    if (!py_truthy(found_atari)) throw core::PyValueError(page.index.repr() + " ページにアタリが無い（先に import_name）");
    const std::string asset = py_str(util::item(found_atari, "asset"));
    const auto data = storage::AssetStore(project).get_bytes(asset, ".png");
    if (!data) throw core::PyValueError("アタリの画像 " + asset.substr(0, 19) + "… が assets/ に無い");
    const render::Image image = open_picture(*data);
    const xycut::Params settings = xycut::Params::from(core::py_or(params, Json::object()));
    const std::vector<double> placement = core::unpack_floats(util::item(found_atari, "placement_mm"), 4);
    const xycut::Detection found = xycut::detect(image, {placement[0], placement[1], placement[2], placement[3]}, settings,
                                                 doc.binding == core::Binding::Right);
    Json record = Json::object({{"asset", found_atari["asset"]},
                                {"placement_mm", found_atari["placement_mm"]},
                                {"align", py_get(found_atari, "align")},
                                {"params", settings.fields},
                                {"threshold", found.threshold},
                                {"confidence", found.confidence},
                                {"panels", found.leaves_mm},
                                {"tree", found.tree},
                                {"tiers", found.tiers}});
    const std::string stem = asset.substr(7, 12) + "_" + studio::content_hash(settings.fields).substr(7, 6);
    const fs::path folder = project / "studio" / "analysis" / core::path_from_utf8(page.id);
    fs::create_directories(folder);
    storage::write_atomic(folder / core::path_from_utf8(stem + ".json"), studio::text_mode(core::dump_python_indent2(record) + "\n"));
    const render::Image overlay = overlay_image(doc, page, 72, Json::object({{"kind", "layout"}, {"panels", found.leaves_mm}}));
    util::write_file(folder / core::path_from_utf8(stem + ".png"), formats::png_bytes(overlay));
    return Json::object({{"id", "pr_" + studio::content_hash(Json::object({{"page", page.id}, {"layout", found.tree}})).substr(7, 10)},
                         {"kind", "layout"},
                         {"page", page.index.json()},
                         {"source", "genko:xycut"},
                         {"analysis", "studio/analysis/" + page.id + "/" + stem + ".json"},
                         {"confidence", found.confidence},
                         {"panels", found.leaves_mm},
                         {"tree", found.tree},
                         {"tiers", found.tiers}});
}

Json accept_ops(const core::Document& doc, const Json& proposal, bool force) {
    const core::Page* page = nullptr;
    for (const auto& p : doc.pages) {
        if (core::py_equals(Json(p->id), py_get(proposal, "page_id"))) {
            page = p.get();
            break;
        }
    }
    if (page == nullptr) throw core::PyValueError("提案のページがもう無い");
    Json ops = Json::array();
    if (core::py_equals(util::item(proposal, "kind"), Json("layout"))) {
        if (!core::truthy_at(proposal, "tree")) throw core::PyValueError("コマが検出されていない提案は確定できない");
        ops.push_back(Json::object({{"op", "set_layout"}, {"page", page->index.json()}, {"tree", proposal["tree"]}, {"force", force}}));
    } else {
        for (const Json& line : core::iterate(py_get(proposal, "lines", Json::array()))) {
            Json op = Json::object({{"op", "add_line"},
                                    {"page", page->index.json()},
                                    {"text", util::item(line, "text")},
                                    {"speaker", core::py_or(py_get(line, "speaker"), Json(""))},
                                    {"balloon", core::py_or(py_get(line, "balloon"), Json("speech"))},
                                    {"wrap", core::py_or(py_get(line, "wrap"), Json("vertical"))},
                                    {"x_mm", util::item(line, "x_mm")},
                                    {"y_mm", util::item(line, "y_mm")},
                                    {"w_mm", util::item(line, "w_mm")},
                                    {"h_mm", util::item(line, "h_mm")}});
            if (core::truthy_at(line, "tail")) op["tail"] = line["tail"];
            ops.push_back(std::move(op));
        }
    }
    ops.push_back(Json::object({{"op", "resolve_proposal"}, {"id", util::item(proposal, "id")}, {"status", "accepted"}}));
    return ops;
}

Json assign_frames(const Json& ops, const core::Page& page) {
    Json out = Json::array();
    for (const Json& op : ops) {
        if (core::py_equals(py_get(op, "op"), Json("add_line")) && !core::truthy_at(op, "frame_id")) {
            const Num x = core::to_num(op["x_mm"]) + core::to_num(op["w_mm"]) / Num(2);
            const Num y = core::to_num(op["y_mm"]) + core::to_num(op["h_mm"]) / Num(2);
            const core::Frame* frame = page.frame_at(x, y);
            if (frame != nullptr && frame->children.empty()) {
                Json with = op;
                with["frame_id"] = frame->id;
                out.push_back(std::move(with));
                continue;
            }
        }
        out.push_back(op);
    }
    return out;
}

Json line_from_input(const Json& item, const core::Page& page, const Json& atari) {
    const std::string text = core::py_strip(py_str(core::py_or(py_get(item, "text"), Json(""))));
    if (text.empty()) throw core::PyValueError("text が空");
    const std::string balloon = py_str(core::py_or(py_get(item, "balloon"), Json("speech")));
    double x = 0, y = 0, w = 0, h = 0;
    if (core::truthy_at(item, "box01")) {
        if (!py_truthy(atari)) throw core::PyValueError("box01 はアタリの画像の中の位置。アタリが無いページでは x_mm, y_mm を使う");
        const std::vector<double> b = core::unpack_floats(item["box01"], 4);
        const std::vector<double> p = core::unpack_floats(util::item(atari, "placement_mm"), 4);
        x = p[0] + b[0] * p[2], y = p[1] + b[1] * p[3], w = b[2] * p[2], h = b[3] * p[3];
    } else if (!py_get(item, "x_mm").is_null() && !py_get(item, "y_mm").is_null()) {
        x = core::to_float(item["x_mm"]);
        y = core::to_float(item["y_mm"]);
        const Json given_w = py_get(item, "w_mm");
        const Json given_h = py_get(item, "h_mm");
        if (!py_truthy(given_w) || !py_truthy(given_h)) {
            std::vector<std::string> breaks;
            std::size_t start = 0;
            for (;;) {
                const std::size_t nl = text.find('\n', start);
                breaks.push_back(text.substr(start, nl == std::string::npos ? std::string::npos : nl - start));
                if (nl == std::string::npos) break;
                start = nl + 1;
            }
            const auto measured = letter::measure(breaks, balloon);
            w = measured[0], h = measured[1];
        } else {
            w = core::to_float(given_w), h = core::to_float(given_h);
        }
    } else {
        throw core::PyValueError("位置（x_mm, y_mm か box01）が要る");
    }
    if (!(0 <= x && x < page.spec.width_mm.value() && 0 <= y && y < page.spec.height_mm.value())) throw core::PyValueError("位置がページの外");
    Json line = Json::object({{"text", text},
                              {"balloon", balloon},
                              {"x_mm", core::py_round(x, 2)},
                              {"y_mm", core::py_round(y, 2)},
                              {"w_mm", core::py_round(w, 2)},
                              {"h_mm", core::py_round(h, 2)},
                              {"speaker", py_get(item, "speaker")},
                              {"wrap", core::py_or(py_get(item, "wrap"), Json("vertical"))}});
    if (core::truthy_at(item, "tail")) {
        Json tail = Json::array();
        for (const Json& v : core::iterate(item["tail"])) tail.push_back(core::to_float(v));
        line["tail"] = std::move(tail);
    }
    return line;
}

render::Image overlay_image(const core::Document& doc, const core::Page& page, int dpi, const Json& extra) {
    render::RenderOptions options;
    options.mode = "name";
    render::Image image = render::render_page(page, dpi, options, &doc).image.convert("RGB");
    render::Draw draw(image);
    render::TrueTypeFonts fonts;
    const render::TrueTypeFont& font = fonts.truetype("sfx", std::max(8, std::max(10, render::mm_to_px(3.5, dpi))));
    std::vector<Json> proposals;
    const Json all = util::or_dict(py_get(doc.studio, "proposals"), "values");
    for (const auto& [id, p] : all.items()) {
        (void)id;
        if (core::py_equals(py_get(p, "page_id"), Json(page.id)) && core::py_equals(py_get(p, "status"), Json("open"))) proposals.push_back(p);
    }
    if (py_truthy(extra)) proposals.push_back(extra);
    const int width = std::max(2, render::mm_to_px(0.6, dpi));
    const auto mm = [dpi](const Json& v) { return render::mm_to_px(core::to_float(v), dpi); };
    for (const Json& proposal : proposals) {
        if (core::py_equals(py_get(proposal, "kind"), Json("layout"))) {
            int i = 0;
            for (const Json& panel : core::iterate(py_get(proposal, "panels", Json::array()))) {
                ++i;
                const std::vector<Json> r = core::unpack_values(util::item(panel, "rect_mm"), 4);
                const std::array<int, 4> box{mm(r[0]), mm(r[1]), render::mm_to_px((core::to_num(r[0]) + core::to_num(r[2])).value(), dpi),
                                             render::mm_to_px((core::to_num(r[1]) + core::to_num(r[3])).value(), dpi)};
                draw.rectangle(render::BoxF{double(box[0]), double(box[1]), double(box[2]), double(box[3])}, std::nullopt, render::Ink{220, 30, 30}, width);
                const std::string label = std::to_string(i) + " (" + fixed(core::to_float(py_get(panel, "confidence", Json(0))), 2) + ")";
                draw.text(render::PointD{double(box[0] + 4), double(box[1] + 4)}, render::text::u32(label), font, render::Ink{220, 30, 30});
            }
        } else {
            for (const Json& line : core::iterate(py_get(proposal, "lines", Json::array()))) {
                const Num x = core::to_num(util::item(line, "x_mm"));
                const Num y = core::to_num(util::item(line, "y_mm"));
                const std::array<int, 4> box{render::mm_to_px(x.value(), dpi), render::mm_to_px(y.value(), dpi),
                                             render::mm_to_px((x + core::to_num(util::item(line, "w_mm"))).value(), dpi),
                                             render::mm_to_px((y + core::to_num(util::item(line, "h_mm"))).value(), dpi)};
                draw.rectangle(render::BoxF{double(box[0]), double(box[1]), double(box[2]), double(box[3])}, std::nullopt, render::Ink{30, 90, 220}, width);
                std::string shown = py_str(util::item(line, "text"));
                for (std::size_t at = shown.find('\n'); at != std::string::npos; at = shown.find('\n', at)) shown.replace(at, 1, " ");
                draw.text(render::PointD{double(box[0] + 2), double(box[3] + 2)}, render::text::u32(text::head(shown, 16)), font, render::Ink{30, 90, 220});
            }
        }
    }
    return image;
}

}  // namespace genko::service::atari
