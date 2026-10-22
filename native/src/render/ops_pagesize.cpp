// PC3 節 C (C-e, SPEC.md COLOR-07「判型」; 決定 8: the whole book or each page, page-spec@1): the page size ops — Image Size
// (resize_pages), Canvas Size (set_canvas_size), Crop and the Crop tool (crop_page), Trim (trim_page), Reveal All
// (reveal_all) and Image Rotation (rotate_pages). Everything on a page goes through one map of it (core/page_geometry's
// map_page), its pixels on their own grids (render/page_pixels); the page's paper, finished size, bleed and basic frame stay
// where they were on what it holds (a page whose paper then differs from the book's keeps its own: page-spec@1; the whole
// book changed alike changes the book's paper, which needs no extension). As PhotoCraft v0.2.0's image_cmds.rs (98-330),
// mode_cmds.rs (56-140) and extra_cmds.rs (552-590), MIT (native/third_party/photocraft/NOTICE.md), on Genko's pages.

#include <algorithm>
#include <array>
#include <cmath>
#include <numbers>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "core/areas.hpp"
#include "core/covers.hpp"
#include "core/error.hpp"
#include "core/ops_util.hpp"
#include "core/page_geometry.hpp"
#include "core/pyconv.hpp"
#include "core/pyops.hpp"
#include "core/smart.hpp"
#include "render/image_ops.hpp"
#include "render/op_limits.hpp"
#include "render/ops_pc3.hpp"
#include "render/ops_smart_parts.hpp"
#include "render/page.hpp"
#include "render/page_pixels.hpp"
#include "render/raster.hpp"
#include "render/selection.hpp"

namespace genko::render {

namespace {

using core::Document;
using core::Json;
using core::Num;
using core::OpError;
using core::Page;
using core::PageSpec;
namespace geo = core::page_geometry;
using page_pixels::How;

constexpr double kEps = 1e-6;
constexpr double kPi = std::numbers::pi;
// A move of the page goes by whole pixels of the paint layers' working grid (so their pixels are exact, and the lines
// stay on them).
constexpr double kStep = 25.4 / raster::kWorkingDpi;

double r3(double v) { return core::py_round(v, 3); }
double to_step(double mm) { return std::round(mm / kStep) * kStep; }

double number_at(const Json& op, std::string_view key, std::string_view what) {
    const Json* value = core::get(op, key);
    if (value == nullptr || !value->is_number() || !std::isfinite(value->get<double>())) throw OpError(std::string(what) + " must be a finite number");
    return value->get<double>();
}

std::optional<double> optional_number(const Json& op, std::string_view key) {
    const Json* value = core::get(op, key);
    if (value == nullptr || value->is_null()) return std::nullopt;
    if (!value->is_number() || value->is_boolean() || !std::isfinite(value->get<double>())) throw OpError(std::string(key) + " must be a finite number");
    return value->get<double>();
}

bool flag_at(const Json& op, std::string_view key, bool fallback) {
    const Json* value = core::get(op, key);
    if (value == nullptr || value->is_null()) return fallback;
    if (!value->is_boolean()) throw OpError(std::string(key) + " is true or false");
    return value->get<bool>();
}

std::string text_at(const Json& op, std::string_view key, std::string fallback) {
    const Json* value = core::get(op, key);
    if (value == nullptr || value->is_null()) return fallback;
    if (!value->is_string()) throw OpError(std::string(key) + " must be text");
    return value->get<std::string>();
}

// --- the pages an op works on -------------------------------------------------------------------------------------------

// "pages": [numbers] | "all"; else the page "page" names, or (Image Size, Canvas Size) every page.
std::vector<std::size_t> target_pages(const Document& doc, const Json& op, bool all_by_default) {
    std::vector<std::size_t> all;
    for (std::size_t i = 0; i < doc.pages.size(); ++i) all.push_back(i);
    const Json* pages = core::get(op, "pages");
    if (pages != nullptr && !pages->is_null()) {
        if (pages->is_string() && pages->get<std::string>() == "all") return all;
        if (!pages->is_array() || pages->empty()) throw OpError("pages is a list of page numbers or \"all\"");
        std::set<std::size_t> out;
        for (const Json& wanted : *pages) {
            if (!wanted.is_number_integer()) throw OpError("pages is a list of page numbers or \"all\"");
            const Num index(wanted.get<std::int64_t>());
            const auto found = std::find_if(doc.pages.begin(), doc.pages.end(), [&](const core::PagePtr& p) { return p->index == index; });
            if (found == doc.pages.end()) throw OpError("no page " + index.repr());
            out.insert(static_cast<std::size_t>(found - doc.pages.begin()));
        }
        return {out.begin(), out.end()};
    }
    const Json* page = core::get(op, "page");
    if ((page == nullptr || page->is_null()) && all_by_default) return all;
    return {core::require_page(doc, op)};
}

// Each page asked as an op on it is (a page someone else holds is refused).
void check_locks(const Document& doc, const std::vector<std::size_t>& pages, const core::Actor& actor) {
    for (const std::size_t at : pages) {
        const Page& page = doc.page(at);
        const Json* owner = core::get(doc.page_locks, page.id);
        if (owner != nullptr && core::py_truthy(*owner) && !(owner->is_string() && owner->get<std::string>() == actor.name())) {
            throw OpError("page " + page.index.repr() + " locked by " + core::py_str(*owner));
        }
    }
}

const Page* numbered(const Document& doc, const Num& index) {
    for (const auto& page : doc.pages) {
        if (page->index == index) return page.get();
    }
    return nullptr;
}

// --- the paper after the op -------------------------------------------------------------------------------------------

struct Margins {
    double top = 0, bottom = 0, inner = 0, outer = 0;
};

// Two papers the same (their sizes, finished size and its place, bleed, basic frame, resolution and expression).
bool same_paper(const PageSpec& a, const PageSpec& b) {
    const auto near = [](double x, double y) { return std::fabs(x - y) < kEps; };
    const auto [aw, ah] = a.trim_size();
    const auto [bw, bh] = b.trim_size();
    const auto [ax, ay] = a.trim_origin();
    const auto [bx, by] = b.trim_origin();
    PageSpec::Margins am, bm;
    try {
        am = a.margins();
        bm = b.margins();
    } catch (const core::Error&) {
        return false;
    }
    return near(a.width_mm.value(), b.width_mm.value()) && near(a.height_mm.value(), b.height_mm.value()) && near(aw.value(), bw.value()) &&
           near(ah.value(), bh.value()) && near(ax, bx) && near(ay, by) && near(a.bleed_mm.value(), b.bleed_mm.value()) && near(am.top, bm.top) &&
           near(am.bottom, bm.bottom) && near(am.inner, bm.inner) && near(am.outer, bm.outer) && near(a.dpi.value(), b.dpi.value()) &&
           a.expression == b.expression;
}

// The margins an op gives ("margins": [top, bottom, inner, outer] or some of {"top", "bottom", "inner", "outer"}) over
// `now`.
Margins margins_given(const Json& value, Margins now) {
    if (value.is_array()) {
        if (value.size() != 4) throw OpError("margins is [top, bottom, inner, outer]");
        double* out[] = {&now.top, &now.bottom, &now.inner, &now.outer};
        for (std::size_t k = 0; k < 4; ++k) {
            if (!value[k].is_number() || !std::isfinite(value[k].get<double>())) throw OpError("margins must be finite numbers");
            *out[k] = value[k].get<double>();
        }
        return now;
    }
    if (!value.is_object()) throw OpError("margins is [top, bottom, inner, outer]");
    for (const auto& [key, v] : value.items()) {
        double* out = key == "top" ? &now.top : key == "bottom" ? &now.bottom : key == "inner" ? &now.inner : key == "outer" ? &now.outer : nullptr;
        if (out == nullptr) throw OpError("margins takes top, bottom, inner and outer");
        if (!v.is_number() || !std::isfinite(v.get<double>())) throw OpError("margins must be finite numbers");
        *out = v.get<double>();
    }
    return now;
}

// How the finished size and basic frame go: where the map takes them, or (Arbitrary) the finished size in the new
// paper's middle with the margins it had.
enum class TrimRule { Mapped, Centred };

// The page's paper after the op: `w` × `h` (mm), its finished size and basic frame by `rule`, its bleed × `bleed_scale`,
// at `dpi`; then the op's own "trim" ([w, h]: in the paper's middle), "bleed_mm" and "margins". OpError when the finished
// size and its bleed or the basic frame would not fit.
PageSpec respec(const Document& doc, const Page& page, double w, double h, const geo::Mapping& m, TrimRule rule, double bleed_scale, const Json& op,
                std::optional<double> dpi = std::nullopt) {
    if (!std::isfinite(w) || !std::isfinite(h) || w <= kEps || h <= kEps) throw OpError("the new paper must have a size");
    limits::check_sides(std::round(w / kStep), std::round(h / kStep), "the page on its new paper");
    const PageSpec& old = page.spec;
    if (core::is_cover(page) && (std::fabs(w - old.width_mm.value()) >= kEps || std::fabs(h - old.height_mm.value()) >= kEps)) {
        throw OpError("page " + page.index.repr() + " is a cover: its paper is set by its cover settings");
    }
    const std::string side = doc.start_side.value_or("");
    const bool left_bound = page.binding_edge(side) == "left";
    const core::Rect t = page.trim_rect_mm();
    const core::Rect f = page.inner_rect_mm(side);
    std::array<double, 4> trim{};
    std::array<double, 4> frame{};
    if (rule == TrimRule::Mapped) {
        trim = geo::map_box({t.x.value(), t.y.value(), t.width.value(), t.height.value()}, m);
        frame = geo::map_box({f.x.value(), f.y.value(), f.width.value(), f.height.value()}, m);
    } else {
        trim = {(w - t.width.value()) / 2, (h - t.height.value()) / 2, t.width.value(), t.height.value()};
        frame = {trim[0] + (f.x - t.x).value(), trim[1] + (f.y - t.y).value(), f.width.value(), f.height.value()};
    }
    // (the margins the frame now has in the finished size: the binding side's the inner one)
    const double left = frame[0] - trim[0], right = trim[0] + trim[2] - frame[0] - frame[2];
    Margins margins{frame[1] - trim[1], trim[1] + trim[3] - frame[1] - frame[3], left_bound ? left : right, left_bound ? right : left};
    double bleed = old.bleed_mm.value() * bleed_scale;
    // the op's own
    if (const auto b = optional_number(op, "bleed_mm")) {
        if (*b < 0) throw OpError("bleed_mm must not be negative");
        bleed = *b;
    }
    bool fit = false;
    if (const Json* given = core::get(op, "trim"); given != nullptr && !given->is_null()) {
        if (given->is_string() && given->get<std::string>() == "fit") {
            // (the paper less the bleed all round, in its middle; no bleed when the paper is too small for one)
            fit = true;
            if (2 * bleed >= std::min(w, h) - kEps) bleed = 0;
            trim = {bleed, bleed, w - 2 * bleed, h - 2 * bleed};
        } else {
            if (!given->is_array() || given->size() != 2 || !(*given)[0].is_number() || !(*given)[1].is_number()) throw OpError("trim is [width, height] or \"fit\"");
            const double tw = (*given)[0].get<double>(), th = (*given)[1].get<double>();
            if (!std::isfinite(tw) || !std::isfinite(th) || tw <= 0 || th <= 0) throw OpError("trim must be positive numbers");
            trim = {(w - tw) / 2, (h - th) / 2, tw, th};
        }
    }
    if (const Json* given = core::get(op, "margins"); given != nullptr && !given->is_null()) {
        margins = margins_given(*given, margins);
    } else if (fit) {
        // (the margins it had, made smaller where they would not leave a basic frame: 0.8 of the finished size at most)
        for (double* v : {&margins.top, &margins.bottom, &margins.inner, &margins.outer}) *v = std::max(*v, 0.0);
        const double across = margins.inner + margins.outer, down = margins.top + margins.bottom;
        if (across >= 0.8 * trim[2]) {
            const double k = across > 0 ? 0.8 * trim[2] / across : 0.0;
            margins.inner *= k;
            margins.outer *= k;
        }
        if (down >= 0.8 * trim[3]) {
            const double k = down > 0 ? 0.8 * trim[3] / down : 0.0;
            margins.top *= k;
            margins.bottom *= k;
        }
    }
    for (double* v : {&margins.top, &margins.bottom, &margins.inner, &margins.outer}) {
        if (*v < 0 && *v > -1e-3) *v = 0;  // (float noise at an edge)
    }
    if (trim[0] - bleed < -1e-3 || trim[1] - bleed < -1e-3 || trim[0] + trim[2] + bleed > w + 1e-3 || trim[1] + trim[3] + bleed > h + 1e-3) {
        throw OpError("the finished size and its bleed would not fit the new paper (give trim, bleed_mm or margins)");
    }
    if (std::min({margins.top, margins.bottom, margins.inner, margins.outer}) < 0 || margins.inner + margins.outer >= trim[2] - kEps ||
        margins.top + margins.bottom >= trim[3] - kEps) {
        throw OpError("the basic frame would not fit the finished size (give margins)");
    }
    PageSpec out = old;
    bool changed = false;
    const auto set = [&changed](Num& field, double value) {
        if (std::fabs(field.value() - value) < kEps) return;
        field = Num(r3(value));
        changed = true;
    };
    set(out.width_mm, w);
    set(out.height_mm, h);
    if (std::fabs(old.bleed_mm.value() - bleed) >= kEps) {
        out.bleed_mm = Num(r3(bleed));
        changed = true;
    }
    // (as the new paper and bleed would give it when it is not written: written when that is not it)
    const auto [ow, oh] = out.trim_size();
    if (std::fabs(ow.value() - trim[2]) >= kEps || std::fabs(oh.value() - trim[3]) >= kEps) {
        out.trim_w_mm = Num(r3(trim[2]));
        out.trim_h_mm = Num(r3(trim[3]));
        changed = true;
    }
    const PageSpec::Margins was = old.margins();
    if (std::fabs(was.top - margins.top) >= kEps || std::fabs(was.bottom - margins.bottom) >= kEps || std::fabs(was.inner - margins.inner) >= kEps ||
        std::fabs(was.outer - margins.outer) >= kEps) {
        out.margins_mm = core::NumList{Num(r3(margins.top)), Num(r3(margins.bottom)), Num(r3(margins.inner)), Num(r3(margins.outer))};
        out.inner_margin_mm = Num(r3(std::min({margins.top, margins.bottom, margins.inner, margins.outer})));
        changed = true;
    }
    // (the finished size off the paper's middle: where it is)
    const auto [tw, th] = out.trim_size();
    const double cx = (out.width_mm.value() - tw.value()) / 2, cy = (out.height_mm.value() - th.value()) / 2;
    if (std::fabs(trim[0] - cx) < 1e-3 && std::fabs(trim[1] - cy) < 1e-3) {
        out.trim_origin_mm.reset();
    } else {
        out.trim_origin_mm = std::make_pair(r3(trim[0]), r3(trim[1]));
    }
    if (old.trim_origin_mm != out.trim_origin_mm) changed = true;
    if (dpi && std::fabs(*dpi - old.dpi.value()) >= kEps) out.dpi = Num(static_cast<std::int64_t>(std::llround(*dpi)));
    if (changed) out.preset = "custom";
    return out;
}

// --- one page moved ---------------------------------------------------------------------------------------------------

struct Plan {
    std::size_t at = 0;
    PageSpec after;
    geo::Mapping m;
    How how;
    bool moves = true;  // (false: the paper alone changes — Image Size's resolution)
};

bool turns_or_mirrors(const geo::Mapping& m) { return !(m.keeps_axes() && m.m[0] > 0 && m.m[3] > 0); }

// What a page holds moved through its plan: its placed pictures, when the map turns or mirrors them, made smart objects of
// their files first (the調整役's rule: a placed picture is turned through a smart object's matrix); the page's paper
// set; then everything on it (map_page), the pixels by `how`.
void move_page(Document& doc, const Plan& plan) {
    const PageSpec before = doc.page(plan.at).spec;
    if (plan.moves && turns_or_mirrors(plan.m)) {
        const Page& seen = doc.page(plan.at);
        std::vector<std::size_t> placed;
        for (std::size_t i = 0; i < seen.layers.size(); ++i) {
            if (seen.layers[i].kind == core::LayerKind::Placed && seen.layers[i].placement_mm) placed.push_back(i);
        }
        for (const std::size_t i : placed) {
            core::Layer layer = doc.page(plan.at).layers[i];
            const std::string id = layer.id;
            core::Layer smart = smart_ops::placed_to_smart(doc, std::move(layer), id);
            doc.edit_page(plan.at).layers[i] = std::move(smart);
        }
    }
    doc.edit_page(plan.at).spec = plan.after;
    if (!plan.moves) return;
    geo::map_page(doc, plan.at, plan.m, page_pixels::hooks(doc, plan.how, before, plan.after));
}

// The pages' papers settled: the whole book done alike (`book`: the book's paper after the op, when it changed every
// page) is the book's paper; a page whose paper is not the book's keeps its own (page-spec@1). A cover keeps the paper its
// cover settings give it. The book's paper never holds a finished size off the middle (a page that has one keeps its own).
void settle(Document& doc, const std::vector<Plan>& plans, std::optional<PageSpec> book) {
    if (book) {
        book->trim_origin_mm.reset();
        doc.spec = *book;
    }
    for (const Plan& plan : plans) {
        Page& page = doc.edit_page(plan.at);
        if (const Json* cover = core::cover_of(page)) {
            page.spec = core::spec_for(doc.spec, *cover);
            page.own_spec = false;
            continue;
        }
        if (same_paper(page.spec, doc.spec)) {
            page.spec = doc.spec;
            page.own_spec = false;
        } else {
            page.own_spec = true;
        }
    }
    if (book) {  // (every page: the ones it left as they were follow the book's paper too)
        for (std::size_t i = 0; i < doc.pages.size(); ++i) {
            const Page& page = doc.page(i);
            if (!page.own_spec && core::cover_of(page) == nullptr && !same_paper(page.spec, doc.spec)) doc.edit_page(i).own_spec = true;
        }
    }
}

// A cover's paper is its cover settings' (a size cannot be given to it); a page of a spread changes size with the other.
void check_sizes(const Document& doc, const std::vector<Plan>& plans) {
    const auto sized = [&](const Plan& plan) {
        const PageSpec& was = doc.page(plan.at).spec;
        return std::fabs(was.width_mm.value() - plan.after.width_mm.value()) >= kEps || std::fabs(was.height_mm.value() - plan.after.height_mm.value()) >= kEps;
    };
    for (const Plan& plan : plans) {
        const Page& page = doc.page(plan.at);
        if (!sized(plan)) continue;
        if (core::is_cover(page)) throw OpError("page " + page.index.repr() + " is a cover: its paper is set by its cover settings");
        if (!page.spread_with || !page.spread_with->truthy()) continue;
        const Page* other = numbered(doc, *page.spread_with);
        if (other == nullptr) continue;
        const auto with = std::find_if(plans.begin(), plans.end(), [&](const Plan& p) { return doc.page(p.at).id == other->id; });
        if (with == plans.end()) {
            throw OpError("page " + page.index.repr() + " is in a spread with page " + other->index.repr() + ": change both pages");
        }
        if (std::fabs(with->after.width_mm.value() - plan.after.width_mm.value()) >= kEps ||
            std::fabs(with->after.height_mm.value() - plan.after.height_mm.value()) >= kEps) {
            throw OpError("page " + page.index.repr() + " is in a spread with page " + other->index.repr() + ": both must keep the same size");
        }
    }
}

Json report_of(const Document& doc, const std::vector<Plan>& plans) {
    Json pages = Json::array();
    for (const Plan& plan : plans) {
        const Page& page = doc.page(plan.at);
        const geo::XY at = plan.m.apply(geo::XY{0, 0});
        pages.push_back(Json{{"page", page.index.json()},
                             {"paper_mm", Json::array({page.spec.width_mm.value(), page.spec.height_mm.value()})},
                             {"offset_mm", Json::array({r3(at.x), r3(at.y)})}});
    }
    return Json{{"pages", std::move(pages)}};
}

// A stand-in page of the book's paper (to work out what the op makes of the book's paper itself).
Page book_page(const Document& doc) { return core::make_page(Num(1), doc.spec, doc.binding); }

std::string cover_refusal(const Num& index) { return "page " + index.repr() + " is a cover: its paper is set by its cover settings"; }

// The plans carried out: the sizes checked, each page moved, the papers settled, the report (with the covers left out:
// "skipped_covers", their numbers). Every page asked a cover left out is refused, as a cover named is.
void carry_out(core::OpContext& c, std::vector<Plan>& plans, std::optional<PageSpec> book, const std::vector<Num>& skipped = {}) {
    if (plans.empty() && !skipped.empty()) throw OpError(cover_refusal(skipped.front()));
    check_sizes(c.doc, plans);
    for (const Plan& plan : plans) move_page(c.doc, plan);
    settle(c.doc, plans, std::move(book));
    c.report = report_of(c.doc, plans);
    if (!skipped.empty()) {
        Json covers = Json::array();
        for (const Num& index : skipped) covers.push_back(index.json());
        (*c.report)["skipped_covers"] = std::move(covers);
    }
}

bool every_page(const Document& doc, const std::vector<std::size_t>& pages) { return pages.size() == doc.pages.size(); }

// The whole book: "pages": "all", or no page named (Image Size's and Canvas Size's default).
bool whole_book(const Json& op) {
    const Json* pages = core::get(op, "pages");
    if (pages != nullptr && !pages->is_null()) return pages->is_string() && pages->get<std::string>() == "all";
    const Json* page = core::get(op, "page");
    return page == nullptr || page->is_null();
}

// (the coordinator's decision after the PC3 review) On the whole book a cover — a front or back cover, a jacket or an
// obi, its paper set by its cover settings — is left out where the op would give it another size (`w` × `h`): the other
// pages change (each then on a paper of its own, page-spec@1, the book's paper and the covers' as they were). A cover
// named is refused (respec).
bool left_out(const Page& page, bool whole, double w, double h) {
    return whole && core::is_cover(page) &&
           (std::fabs(w - page.spec.width_mm.value()) >= kEps || std::fabs(h - page.spec.height_mm.value()) >= kEps);
}

geo::Mapping translation(double dx, double dy) { return geo::mapping({1, 0, 0, 1, dx, dy}); }

// --- resize_pages: Image Size -----------------------------------------------------------------------------------------

void resize_pages(core::OpContext& c) {
    Document& doc = c.doc;
    const std::vector<std::size_t> pages = target_pages(doc, c.op, true);
    check_locks(doc, pages, c.actor);
    const std::string method = text_at(c.op, "resample", "bicubic");
    const bool none = method == "none";
    const std::optional<resample::Filter> filter = none ? std::optional<resample::Filter>(resample::Filter::Bicubic) : resample::filter_from(method);
    if (!filter) throw OpError("resample is bicubic, bilinear, nearest, lanczos, preserve_details or none");
    const std::optional<double> dpi = optional_number(c.op, "dpi");
    if (dpi && (*dpi < 1 || *dpi > 10000 || std::fabs(*dpi - std::round(*dpi)) > kEps)) throw OpError("dpi is a whole number of 1 to 10000");
    const std::optional<double> wmm = optional_number(c.op, "width_mm"), hmm = optional_number(c.op, "height_mm");
    const std::optional<double> wpx = optional_number(c.op, "width_px"), hpx = optional_number(c.op, "height_px");
    if ((wmm || hmm) && (wpx || hpx)) throw OpError("give the size in mm or in pixels, not both");
    if ((dpi || none) && !every_page(doc, pages)) throw OpError("the resolution is the whole book's: give pages \"all\"");
    if (none && (wmm || hmm || wpx || hpx)) throw OpError("resample none keeps the pixels: give dpi alone");
    // (a page's new size: as given — mm, or pixels at the new resolution —, one side keeping its shape)
    const auto size_of = [&](const PageSpec& spec) -> std::pair<double, double> {
        const double w = spec.width_mm.value(), h = spec.height_mm.value();
        if (none) {
            const double k = dpi ? spec.dpi.value() / *dpi : 1.0;
            return {w * k, h * k};
        }
        const double at = dpi.value_or(spec.dpi.value());
        std::optional<double> nw = wmm, nh = hmm;
        if (wpx) nw = *wpx / at * 25.4;
        if (hpx) nh = *hpx / at * 25.4;
        if (nw && !nh) nh = *nw * h / w;
        if (nh && !nw) nw = *nh * w / h;
        return {nw.value_or(w), nh.value_or(h)};
    };
    const auto plan_for = [&](const Page& page) {
        const auto [w, h] = size_of(page.spec);
        Plan plan;
        const double sx = w / page.spec.width_mm.value(), sy = h / page.spec.height_mm.value();
        plan.m = geo::mapping({sx, 0, 0, sy, 0, 0});
        plan.moves = std::fabs(sx - 1) >= 1e-12 || std::fabs(sy - 1) >= 1e-12;
        plan.how.kind = none ? How::Kind::Stretch : How::Kind::Resize;
        plan.how.filter = *filter;
        plan.after = respec(doc, page, w, h, plan.m, TrimRule::Mapped, std::min(sx, sy), c.op, dpi);
        return plan;
    };
    const bool whole = whole_book(c.op);
    std::vector<Plan> plans;
    std::vector<Num> skipped;  // (the covers left out)
    for (const std::size_t at : pages) {
        const Page& page = doc.page(at);
        if (const auto [w, h] = size_of(page.spec); left_out(page, whole, w, h)) {
            skipped.push_back(page.index);
            continue;
        }
        Plan plan = plan_for(page);
        plan.at = at;
        plans.push_back(std::move(plan));
    }
    // (the resolution is the whole book's, the covers' with it: a cover left out keeps it, so a new one is refused)
    if (!skipped.empty() && (none || (dpi && std::fabs(*dpi - doc.spec.dpi.value()) >= kEps))) throw OpError(cover_refusal(skipped.front()));
    std::optional<PageSpec> book;
    if (plans.size() == doc.pages.size()) book = plan_for(book_page(doc)).after;
    carry_out(c, plans, std::move(book), skipped);
}

// --- set_canvas_size: Canvas Size ---------------------------------------------------------------------------------------

std::pair<double, double> anchor_of(const std::string& anchor) {
    static const std::vector<std::pair<std::string, std::pair<double, double>>> anchors{
        {"top_left", {0, 0}},    {"top", {0.5, 0}},      {"top_right", {1, 0}},     {"left", {0, 0.5}},   {"center", {0.5, 0.5}},
        {"right", {1, 0.5}},     {"bottom_left", {0, 1}}, {"bottom", {0.5, 1}},     {"bottom_right", {1, 1}}};
    for (const auto& [name, factors] : anchors) {
        if (name == anchor) return factors;
    }
    throw OpError("anchor is top_left, top, top_right, left, center, right, bottom_left, bottom or bottom_right");
}

// The bottom paint layer of the page (not a guide, not a reference): where Canvas Size's extension colour goes.
std::optional<std::size_t> bottom_paint_layer(const Page& page) {
    for (std::size_t i = 0; i < page.layers.size(); ++i) {
        const core::Layer& layer = page.layers[i];
        if (layer.kind != core::LayerKind::Raster || layer.reference || layer.role == core::LayerRole::Name || layer.role == core::LayerRole::Draft) continue;
        return i;
    }
    return std::nullopt;
}

// The extension colour laid on the bottom paint layer where the old paper was not (`old`: where it is now, mm).
void extend_with(core::OpContext& c, std::size_t at, const std::array<double, 4>& old, const std::vector<std::int64_t>& rgb) {
    const Page& page = c.doc.page(at);
    const std::optional<std::size_t> found = bottom_paint_layer(page);
    if (!found) throw OpError("the canvas extension colour needs a paint layer at the bottom of the page (or extend it transparent)");
    const std::string id = page.layers[*found].id;
    image_ops::Pixels p = image_ops::layer_pixels(c, at, id);
    const double w = c.doc.page(at).spec.width_mm.value(), h = c.doc.page(at).spec.height_mm.value();
    const double px = w / p.width(), py = h / p.height();
    const auto bare = [&](std::size_t i) {
        const double x = (double(i % p.width()) + 0.5) * px, y = (double(i / p.width()) + 0.5) * py;
        return x < old[0] || y < old[1] || x >= old[0] + old[2] || y >= old[1] + old[3];
    };
    const image_ops::Rgba colour{static_cast<float>(rgb[0] / 255.0), static_cast<float>(rgb[1] / 255.0), static_cast<float>(rgb[2] / 255.0), 1.0f};
    if (p.kind() == image_ops::Pixels::Kind::Inks) {
        std::vector<image_ops::Rgba> px_rgba = image_ops::rgba_of(p);
        for (std::size_t i = 0; i < p.size(); ++i) {
            if (bare(i)) px_rgba[i] = colour;
        }
        image_ops::from_rgba(p, px_rgba);
    } else {
        for (std::size_t i = 0; i < p.size(); ++i) {
            if (!bare(i)) continue;
            float* v = p.at(i);
            for (unsigned k = 0; k < p.colours() && k < 3; ++k) v[k] = colour[k];
            v[p.colours()] = 1.0f;
        }
    }
    image_ops::put_layer_pixels(c.doc, at, id, p);
}

void set_canvas_size(core::OpContext& c) {
    Document& doc = c.doc;
    const std::vector<std::size_t> pages = target_pages(doc, c.op, true);
    check_locks(doc, pages, c.actor);
    const bool relative = flag_at(c.op, "relative", false);
    const auto [ax, ay] = anchor_of(text_at(c.op, "anchor", "center"));
    const std::optional<double> wmm = optional_number(c.op, "width_mm"), hmm = optional_number(c.op, "height_mm");
    const std::optional<double> wpx = optional_number(c.op, "width_px"), hpx = optional_number(c.op, "height_px");
    if ((wmm || hmm) && (wpx || hpx)) throw OpError("give the size in mm or in pixels, not both");
    std::optional<std::vector<std::int64_t>> extension;
    if (const Json* given = core::get(c.op, "extension"); given != nullptr && !given->is_null() && !(given->is_string() && given->get<std::string>() == "transparent")) {
        if (given->is_string()) throw OpError("extension is \"transparent\" or [r, g, b]");
        extension = core::rgb3(*given, "extension");
    }
    // (a page's new canvas: as given — mm, or pixels at its resolution —, or added to its paper)
    const auto size_of = [&](const PageSpec& spec) -> std::pair<double, double> {
        const double w = spec.width_mm.value(), h = spec.height_mm.value(), dpi = spec.dpi.value();
        std::optional<double> gw = wmm, gh = hmm;
        if (wpx) gw = *wpx / dpi * 25.4;
        if (hpx) gh = *hpx / dpi * 25.4;
        return {relative ? w + gw.value_or(0) : gw.value_or(w), relative ? h + gh.value_or(0) : gh.value_or(h)};
    };
    const auto plan_for = [&](const Page& page) {
        const double w = page.spec.width_mm.value(), h = page.spec.height_mm.value();
        const auto [nw, nh] = size_of(page.spec);
        if (!(nw > kEps) || !(nh > kEps)) throw OpError("the new canvas must have a size");
        Plan plan;
        plan.m = translation(to_step((nw - w) * ax), to_step((nh - h) * ay));
        plan.moves = std::fabs(plan.m.m[4]) >= kEps || std::fabs(plan.m.m[5]) >= kEps || std::fabs(nw - w) >= kEps || std::fabs(nh - h) >= kEps;
        plan.how.kind = How::Kind::Shift;
        plan.after = respec(doc, page, nw, nh, plan.m, TrimRule::Mapped, 1.0, c.op);
        return plan;
    };
    const bool whole = whole_book(c.op);
    std::vector<Plan> plans;
    std::vector<Num> skipped;  // (the covers left out)
    for (const std::size_t at : pages) {
        const Page& page = doc.page(at);
        if (const auto [w, h] = size_of(page.spec); left_out(page, whole, w, h)) {
            skipped.push_back(page.index);
            continue;
        }
        Plan plan = plan_for(page);
        plan.at = at;
        plans.push_back(std::move(plan));
    }
    std::optional<PageSpec> book;
    if (plans.size() == doc.pages.size()) book = plan_for(book_page(doc)).after;
    std::vector<std::array<double, 4>> old;
    for (const Plan& plan : plans) {
        const PageSpec& was = doc.page(plan.at).spec;
        old.push_back({plan.m.m[4], plan.m.m[5], was.width_mm.value(), was.height_mm.value()});
    }
    carry_out(c, plans, std::move(book), skipped);
    if (extension) {
        for (std::size_t k = 0; k < plans.size(); ++k) extend_with(c, plans[k].at, old[k], *extension);
    }
}

// --- crop_page, trim_page, reveal_all: the page cut to a box (or grown to it) -----------------------------------------------

// The box of an area as an op gets it ({poly} or {mask: {box, png}}): mm; none when it is empty.
std::optional<std::array<double, 4>> area_box(const Json& area) {
    if (const Json* poly = core::get(area, "poly"); poly != nullptr && poly->is_array() && !poly->empty()) {
        double x0 = 1e300, y0 = 1e300, x1 = -1e300, y1 = -1e300;
        for (const Json& p : *poly) {
            const std::vector<double> xy = core::unpack_floats(p, 2);
            x0 = std::min(x0, xy[0]);
            y0 = std::min(y0, xy[1]);
            x1 = std::max(x1, xy[0]);
            y1 = std::max(y1, xy[1]);
        }
        if (x1 - x0 <= kEps || y1 - y0 <= kEps) return std::nullopt;
        return std::array<double, 4>{x0, y0, x1 - x0, y1 - y0};
    }
    const Json* mask = core::get(area, "mask");
    if (mask == nullptr || !mask->is_object()) throw OpError("the area is empty");
    const std::vector<double> box = core::unpack_floats(core::subscript(*mask, "box"), 4);
    const Image picture = selection::open_picture(selection::b64decode(core::subscript(*mask, "png"))).convert("L");
    const std::optional<Box> on = picture.getbbox(false);
    if (!on || picture.width() <= 0 || picture.height() <= 0) return std::nullopt;
    const double sx = box[2] / picture.width(), sy = box[3] / picture.height();
    return std::array<double, 4>{box[0] + on->x0 * sx, box[1] + on->y0 * sy, on->width() * sx, on->height() * sy};
}

// A box on the working grid (its corners to whole pixels: outward when `grow`, else to the nearest).
std::array<double, 4> on_grid(const std::array<double, 4>& box, bool grow) {
    const double x0 = box[0] / kStep, y0 = box[1] / kStep, x1 = (box[0] + box[2]) / kStep, y1 = (box[1] + box[3]) / kStep;
    const double l = grow ? std::floor(x0 + 1e-9) : std::round(x0), t = grow ? std::floor(y0 + 1e-9) : std::round(y0);
    const double r = grow ? std::ceil(x1 - 1e-9) : std::round(x1), b = grow ? std::ceil(y1 - 1e-9) : std::round(y1);
    return {l * kStep, t * kStep, std::max(r - l, 1.0) * kStep, std::max(b - t, 1.0) * kStep};
}

// The page cut to `box` (mm, on the page; past its paper too), turned by `angle` degrees clockwise about the box's middle
// first (the box is then upright).
Plan crop_plan(const Document& doc, std::size_t at, const std::array<double, 4>& box, double angle, resample::Interp interp, const Json& op) {
    const Page& page = doc.page(at);
    Plan plan;
    plan.at = at;
    const double w = box[2], h = box[3];
    if (std::fabs(std::remainder(angle, 360.0)) < 1e-9) {
        plan.m = translation(-box[0], -box[1]);
        plan.how.kind = How::Kind::Shift;
    } else {
        const double a = -angle * kPi / 180.0, s = std::sin(a), co = std::cos(a);
        const double cx = box[0] + w / 2, cy = box[1] + h / 2;
        // (the box's middle to the new paper's, the page turned back by the box's angle about it)
        plan.m = geo::mapping({co, s, -s, co, w / 2 - (co * cx - s * cy), h / 2 - (s * cx + co * cy)});
        plan.how.kind = How::Kind::Warp;
        plan.how.interp = interp;
    }
    plan.after = respec(doc, page, w, h, plan.m, TrimRule::Mapped, 1.0, op);
    return plan;
}

resample::Interp interp_at(const Json& op) {
    const std::string name = text_at(op, "interp", "bicubic");
    const std::optional<resample::Interp> interp = resample::interp_from(name);
    if (!interp) throw OpError("interp is bicubic, bilinear or nearest");
    return *interp;
}

void crop_page(core::OpContext& c) {
    Document& doc = c.doc;
    const std::vector<std::size_t> pages = target_pages(doc, c.op, false);
    check_locks(doc, pages, c.actor);
    if (!flag_at(c.op, "delete_pixels", true)) throw OpError("delete_pixels must be true: the pixels cut off are not kept (Reveal All brings back the rest)");
    std::array<double, 4> box{};
    if (const Json* rect = core::get(c.op, "rect"); rect != nullptr && !rect->is_null()) {
        const std::vector<double> r = core::unpack_floats(*rect, 4);
        for (const double v : r) {
            if (!std::isfinite(v)) throw OpError("rect must be finite numbers");
        }
        if (r[2] <= 0 || r[3] <= 0) throw OpError("rect must have a size");
        box = {r[0], r[1], r[2], r[3]};
    } else if (core::truthy_at(c.op, "area")) {
        const std::optional<std::array<double, 4>> found = area_box(core::op_area(c.op));
        if (!found) throw OpError("the area is empty");
        box = *found;
    } else {
        throw OpError("give rect [x, y, w, h] or an area to crop to");
    }
    const double angle = optional_number(c.op, "angle").value_or(0.0);
    if (std::fabs(angle) >= 360) throw OpError("angle is -359.99 to 359.99 degrees");
    const resample::Interp interp = interp_at(c.op);
    if (std::fabs(std::remainder(angle, 360.0)) < 1e-9) box = on_grid(box, false);
    std::vector<Plan> plans;
    for (const std::size_t at : pages) plans.push_back(crop_plan(doc, at, box, angle, interp, c.op));
    std::optional<PageSpec> book;
    if (every_page(doc, pages)) {
        Document stand_in = doc;
        stand_in.pages = {std::make_shared<Page>(book_page(doc))};
        book = crop_plan(stand_in, 0, box, angle, interp, c.op).after;
    }
    carry_out(c, plans, std::move(book));
}

// trim_page's box: what is drawn on the page — over nothing (transparent: drawn over a white and a black paper, where they
// differ nothing covers it), or unlike the colour at a corner (top_left, bottom_right) — at the working 200 dpi.
std::optional<std::array<double, 4>> drawn_box(const Document& doc, const Page& page, const std::string& based_on) {
    RenderOptions options;
    options.mode = "print";
    const int dpi = raster::kWorkingDpi;
    // (its nombre is the paper's, not what the page holds)
    const auto drawn = [&](const std::optional<std::array<int, 3>>& paper) {
        Page bare = page;
        bare.numero = false;
        if (!paper) return render_page(bare, dpi, options, &doc).image.convert("RGB");
        bare.fills.clear();
        bare.extra["paper_rgb"] = Json::array({(*paper)[0], (*paper)[1], (*paper)[2]});
        return render_page(bare, dpi, options, &doc).image.convert("RGB");
    };
    std::optional<Box> found;
    const auto take = [&found](int x, int y) {
        if (!found) {
            found = Box{x, y, x + 1, y + 1};
            return;
        }
        found->x0 = std::min(found->x0, x);
        found->y0 = std::min(found->y0, y);
        found->x1 = std::max(found->x1, x + 1);
        found->y1 = std::max(found->y1, y + 1);
    };
    Image picture;
    if (based_on == "transparent") {
        // (the bare paper as the page shows it in each colour — an ink page's paper is its inks' —: a pixel the same as it
        // over both papers has nothing on it)
        const auto bare_paper = [&](const std::array<int, 3>& paper) {
            Page empty = page;
            empty.numero = false;
            empty.fills.clear();
            empty.layers.clear();
            empty.frames.clear();
            empty.channels.clear();
            empty.effects = Json::array();
            empty.extra["paper_rgb"] = Json::array({paper[0], paper[1], paper[2]});
            const std::string px = render_page(empty, 10, options, nullptr).image.convert("RGB").tobytes();
            return std::array<int, 3>{static_cast<unsigned char>(px.at(0)), static_cast<unsigned char>(px.at(1)), static_cast<unsigned char>(px.at(2))};
        };
        const std::array<int, 3> white_paper = bare_paper({255, 255, 255}), black_paper = bare_paper({0, 0, 0});
        const Image white = drawn(std::array<int, 3>{255, 255, 255});
        const Image black = drawn(std::array<int, 3>{0, 0, 0});
        const std::string a = white.tobytes(), b = black.tobytes();
        const int w = white.width();
        for (std::size_t i = 0; i + 2 < a.size(); i += 3) {
            bool bare = true;
            for (std::size_t k = 0; k < 3; ++k) {
                bare = bare && std::abs(static_cast<unsigned char>(a[i + k]) - white_paper[k]) <= 1 && std::abs(static_cast<unsigned char>(b[i + k]) - black_paper[k]) <= 1;
            }
            if (!bare) take(static_cast<int>((i / 3) % std::size_t(w)), static_cast<int>((i / 3) / std::size_t(w)));
        }
        picture = white;
    } else {
        picture = drawn(std::nullopt);
        const std::string px = picture.tobytes();
        const int w = picture.width();
        const std::size_t ref = based_on == "top_left" ? 0 : px.size() - 3;
        for (std::size_t i = 0; i + 2 < px.size(); i += 3) {
            if (px[i] != px[ref] || px[i + 1] != px[ref + 1] || px[i + 2] != px[ref + 2]) {
                take(static_cast<int>((i / 3) % std::size_t(w)), static_cast<int>((i / 3) / std::size_t(w)));
            }
        }
    }
    if (!found) return std::nullopt;
    const double sx = page.spec.width_mm.value() / picture.width(), sy = page.spec.height_mm.value() / picture.height();
    return std::array<double, 4>{found->x0 * sx, found->y0 * sy, found->width() * sx, found->height() * sy};
}

void trim_page(core::OpContext& c) {
    Document& doc = c.doc;
    const std::vector<std::size_t> pages = target_pages(doc, c.op, false);
    check_locks(doc, pages, c.actor);
    const std::string based_on = text_at(c.op, "based_on", "transparent");
    if (based_on != "transparent" && based_on != "top_left" && based_on != "bottom_right") throw OpError("based_on is transparent, top_left or bottom_right");
    const bool top = flag_at(c.op, "top", true), bottom = flag_at(c.op, "bottom", true), left = flag_at(c.op, "left", true),
               right = flag_at(c.op, "right", true);
    std::vector<Plan> plans;
    for (const std::size_t at : pages) {
        const Page& page = doc.page(at);
        const std::optional<std::array<double, 4>> found = drawn_box(doc, page, based_on);
        if (!found) throw OpError("page " + page.index.repr() + " has nothing to trim to");
        const double w = page.spec.width_mm.value(), h = page.spec.height_mm.value();
        const double x0 = left ? (*found)[0] : 0, y0 = top ? (*found)[1] : 0;
        const double x1 = right ? (*found)[0] + (*found)[2] : w, y1 = bottom ? (*found)[1] + (*found)[3] : h;
        plans.push_back(crop_plan(doc, at, on_grid({x0, y0, x1 - x0, y1 - y0}, false), 0, resample::Interp::Bicubic, c.op));
    }
    carry_out(c, plans, std::nullopt);
}

// What is on the page past its paper: its lines, patches, shape parts, placed pictures and smart objects, paths, lines of
// dialogue, panels and effect lines (pixels are never past it).
std::optional<std::array<double, 4>> content_box(const Document& doc, const Page& page) {
    double x0 = 1e300, y0 = 1e300, x1 = -1e300, y1 = -1e300;
    const auto take = [&](double x, double y, double pad = 0) {
        if (!std::isfinite(x) || !std::isfinite(y)) return;
        x0 = std::min(x0, x - pad);
        y0 = std::min(y0, y - pad);
        x1 = std::max(x1, x + pad);
        y1 = std::max(y1, y + pad);
    };
    const auto take_box = [&](double x, double y, double w, double h) {
        take(x, y);
        take(x + w, y + h);
    };
    const auto take_path = [&](const core::vector::Path& path, double pad) {
        if (const auto b = path.control_bounds()) {
            take((*b)[0], (*b)[1], pad);
            take((*b)[2], (*b)[3], pad);
        }
    };
    const auto take_points = [&](const Json& points) {
        if (!points.is_array()) return;
        for (const Json& p : points) {
            if (p.is_array() && p.size() >= 2 && p[0].is_number() && p[1].is_number()) take(p[0].get<double>(), p[1].get<double>());
        }
    };
    std::vector<const core::Frame*> frames;
    for (const core::Frame& f : page.frames) frames.push_back(&f);
    while (!frames.empty()) {
        const core::Frame* f = frames.back();
        frames.pop_back();
        take_box(f->rect.x.value(), f->rect.y.value(), f->rect.width.value(), f->rect.height.value());
        for (const core::Frame& child : f->children) frames.push_back(&child);
    }
    for (const core::Layer& layer : page.layers) {
        for (const core::StrokePtr& s : layer.strokes->items) {
            for (const core::PointF& p : s->points) take(p.x, p.y, s->width_mm / 2);
        }
        for (const core::vector::ShapePart& part : layer.shapes->items) take_path(part.path, part.stroke ? part.stroke->width_mm : 0.0);
        for (const core::Patch& patch : layer.patches) {
            const Json* box = core::get(patch.attrs, "box");
            if (box == nullptr || !box->is_array() || box->size() != 4) continue;
            take_box((*box)[0].get<double>(), (*box)[1].get<double>(), (*box)[2].get<double>(), (*box)[3].get<double>());
        }
        if (layer.placement_mm) take_box(layer.placement_mm->x.value(), layer.placement_mm->y.value(), layer.placement_mm->width.value(), layer.placement_mm->height.value());
        if (layer.kind == core::LayerKind::Smart) {
            if (const core::SmartContents* contents = core::smart::contents_of(doc, layer)) {
                const auto [cw, ch] = core::smart::size_mm(*contents);
                const core::smart::Matrix m = core::smart::matrix_of(layer);
                for (const auto& [x, y] : {std::pair{0.0, 0.0}, std::pair{cw, 0.0}, std::pair{0.0, ch}, std::pair{cw, ch}}) {
                    const auto [px, py] = m.apply(x, y);
                    take(px, py);
                }
            }
        }
        if (layer.vector_mask) take_path(layer.vector_mask->path, 0);
    }
    for (const core::vector::NamedPath& p : page.paths) take_path(p.path, 0);
    if (page.work_path) take_path(*page.work_path, 0);
    for (const core::StoryLine& line : doc.story) {
        if (!(line.page_index == page.index)) continue;
        take_box(line.x_mm.value(), line.y_mm.value(), line.w_mm.value(), line.h_mm.value());
        if (line.tail) take(line.tail->x.value(), line.tail->y.value());
        for (const Json& tail : line.tails) {
            if (const Json* to = core::get(tail, "to")) take_points(Json::array({*to}));
        }
        if (line.path) {
            for (const core::Point& p : *line.path) take(p.x.value(), p.y.value());
        }
    }
    if (page.effects.is_array()) {
        for (const Json& effect : page.effects) {
            const Json* params = core::get(effect, "params");
            if (params == nullptr || !params->is_object()) continue;
            if (const Json* center = core::get(*params, "center")) take_points(Json::array({*center}));
            if (const Json* points = core::get(*params, "points")) take_points(*points);
        }
    }
    if (x1 < x0 || y1 < y0) return std::nullopt;
    return std::array<double, 4>{x0, y0, x1 - x0, y1 - y0};
}

void reveal_all(core::OpContext& c) {
    Document& doc = c.doc;
    const std::vector<std::size_t> pages = target_pages(doc, c.op, false);
    check_locks(doc, pages, c.actor);
    std::vector<Plan> plans;
    for (const std::size_t at : pages) {
        const Page& page = doc.page(at);
        const double w = page.spec.width_mm.value(), h = page.spec.height_mm.value();
        std::array<double, 4> box{0, 0, w, h};
        if (const auto found = content_box(doc, page)) {
            const double x0 = std::min(0.0, (*found)[0]), y0 = std::min(0.0, (*found)[1]);
            const double x1 = std::max(w, (*found)[0] + (*found)[2]), y1 = std::max(h, (*found)[1] + (*found)[3]);
            if (x0 < -kEps || y0 < -kEps || x1 > w + kEps || y1 > h + kEps) {
                const std::array<double, 4> grown = on_grid({x0, y0, x1 - x0, y1 - y0}, true);
                // (the paper's own edges stay where they were when nothing is past them)
                box = {x0 < -kEps ? grown[0] : 0.0, y0 < -kEps ? grown[1] : 0.0, 0, 0};
                box[2] = (x1 > w + kEps ? grown[0] + grown[2] : w) - box[0];
                box[3] = (y1 > h + kEps ? grown[1] + grown[3] : h) - box[1];
            }
        }
        Plan plan = crop_plan(doc, at, box, 0, resample::Interp::Bicubic, c.op);
        plan.moves = std::fabs(box[0]) >= kEps || std::fabs(box[1]) >= kEps || std::fabs(box[2] - w) >= kEps || std::fabs(box[3] - h) >= kEps;
        plans.push_back(std::move(plan));
    }
    carry_out(c, plans, std::nullopt);
}

// --- rotate_pages: Image Rotation -------------------------------------------------------------------------------------

void rotate_pages(core::OpContext& c) {
    Document& doc = c.doc;
    const std::vector<std::size_t> pages = target_pages(doc, c.op, false);
    check_locks(doc, pages, c.actor);
    const std::string text = text_at(c.op, "text", "upright");
    if (text != "upright") throw OpError("text is upright: the letters of the lines stay upright, never turned or mirrored");
    std::optional<geo::Turn> turn;
    std::optional<double> angle;
    if (const Json* given = core::get(c.op, "turn"); given != nullptr && !given->is_null()) {
        if (!given->is_string() || !(turn = geo::turn_from(given->get<std::string>()))) throw OpError("turn is 90cw, 90ccw, 180, flip_h or flip_v");
        if (core::get(c.op, "angle") != nullptr) throw OpError("give turn or angle, not both");
    } else {
        angle = number_at(c.op, "angle", "angle");
        if (std::fabs(*angle) >= 360) throw OpError("angle is -359.99 to 359.99 degrees");
        // (a right angle is a turn: its pixels exact)
        const double quarter = std::remainder(*angle, 360.0) / 90.0;
        if (std::fabs(quarter - std::round(quarter)) < 1e-9) {
            const int q = static_cast<int>(std::lround(quarter));
            if (q == 1) turn = geo::Turn::Cw90;
            if (q == -1) turn = geo::Turn::Ccw90;
            if (q == 2 || q == -2) turn = geo::Turn::Rotate180;
            if (q == 0) angle.reset();
            if (turn) angle.reset();
        }
    }
    const resample::Interp interp = interp_at(c.op);
    const auto plan_for = [&](const Page& page) {
        const double w = page.spec.width_mm.value(), h = page.spec.height_mm.value();
        Plan plan;
        if (turn) {
            plan.m = geo::turn_mapping(*turn, w, h);
            plan.how.kind = How::Kind::Turn;
            plan.how.turn = *resample::turn_from(*turn == geo::Turn::Cw90    ? "90cw"
                                                 : *turn == geo::Turn::Ccw90 ? "90ccw"
                                                 : *turn == geo::Turn::Rotate180 ? "180"
                                                 : *turn == geo::Turn::FlipH ? "flip_h"
                                                                             : "flip_v");
            const bool swap = *turn == geo::Turn::Cw90 || *turn == geo::Turn::Ccw90;
            // (a cover keeps its paper: a turn that keeps its size, its settings as they are)
            plan.after = core::is_cover(page) && !swap ? page.spec : respec(doc, page, swap ? h : w, swap ? w : h, plan.m, TrimRule::Mapped, 1.0, c.op);
            return plan;
        }
        if (!angle) {
            plan.moves = false;
            plan.after = page.spec;
            return plan;
        }
        const double a = *angle * kPi / 180.0, s = std::sin(a), co = std::cos(a);
        const double nw = r3(w * std::fabs(co) + h * std::fabs(s)), nh = r3(w * std::fabs(s) + h * std::fabs(co));
        // (clockwise on the page, y down: x' = c·x − s·y, y' = s·x + c·y about the papers' middles)
        plan.m = geo::mapping({co, s, -s, co, nw / 2 - (co * w / 2 - s * h / 2), nh / 2 - (s * w / 2 + co * h / 2)});
        plan.how.kind = How::Kind::Warp;
        plan.how.interp = interp;
        plan.after = respec(doc, page, nw, nh, plan.m, TrimRule::Centred, 1.0, c.op);
        return plan;
    };
    std::vector<Plan> plans;
    for (const std::size_t at : pages) {
        Plan plan = plan_for(doc.page(at));
        plan.at = at;
        plans.push_back(std::move(plan));
    }
    std::optional<PageSpec> book;
    if (every_page(doc, pages)) book = plan_for(book_page(doc)).after;
    carry_out(c, plans, std::move(book));
}

}  // namespace

void register_pagesize_ops(core::OpRegistry& registry) {
    // each takes "pages"?: [numbers] | "all" (Image Size and Canvas Size: all by default; the others: the page named), and
    // "trim"?, "bleed_mm"?, "margins"? to set the finished size, bleed and basic frame anew (by default they stay where
    // they were on what the page holds)
    registry.add("resize_pages", resize_pages);        // (width_mm, height_mm | width_px, height_px, dpi?, resample)
    registry.add("set_canvas_size", set_canvas_size);  // (width_mm, height_mm | width_px, height_px, relative?, anchor, extension?)
    registry.add("crop_page", crop_page);              // (rect | area, angle?, interp?, delete_pixels?)
    registry.add("trim_page", trim_page);              // (based_on, top, bottom, left, right)
    registry.add("reveal_all", reveal_all);
    registry.add("rotate_pages", rotate_pages);        // (turn | angle, interp, text: upright)
}

}  // namespace genko::render
