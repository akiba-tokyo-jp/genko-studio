// PC5 J-d2 (render/measure_features.hpp): 計測値を記録's values and the measurement log's columns and CSV — PhotoCraft
// v0.2.0's crates/engine/src/analysis_cmds.rs (MIT, native/third_party/photocraft/NOTICE.md).

#include "render/measure_features.hpp"

#include <QDateTime>

#include <algorithm>
#include <cmath>
#include <limits>
#include <numbers>

#include "core/channels.hpp"
#include "core/color_raster.hpp"
#include "core/error.hpp"
#include "core/measure.hpp"
#include "core/pynum.hpp"
#include "core/pyops.hpp"
#include "render/op_limits.hpp"
#include "render/page.hpp"
#include "render/raster_ops.hpp"
#include "render/selection.hpp"

namespace genko::render::measure {

using core::Json;
using core::OpError;

namespace {

constexpr std::uint32_t kNone = std::numeric_limits<std::uint32_t>::max();

double r4(double v) { return core::py_round(v, 4); }

// Union-find root with path halving.
std::uint32_t root(std::vector<std::uint32_t>& parent, std::uint32_t i) {
    while (parent[i] != i) {
        parent[i] = parent[parent[i]];
        i = parent[i];
    }
    return i;
}

constexpr std::string_view kCommon[] = {"label", "dateTime", "document", "source", "scale", "scaleUnits", "scaleFactor"};
constexpr std::string_view kSelectionOnly[] = {"count", "area", "perimeter", "circularity", "height", "width", "grayMin", "grayMax",
                                               "grayMean", "grayMedian", "integratedDensity", "histogram"};
constexpr std::string_view kRulerOnly[] = {"length", "angle"};
constexpr std::string_view kCountOnly[] = {"count"};

// (Photoshop's 計測ログ columns as its Japanese menus name them)
constexpr Column kColumns[] = {
    {"label", "ラベル"},
    {"dateTime", "日時"},
    {"document", "ドキュメント"},
    {"source", "ソース"},
    {"scale", "スケール"},
    {"scaleUnits", "スケール単位"},
    {"scaleFactor", "スケール係数"},
    {"count", "カウント"},
    {"area", "面積"},
    {"perimeter", "周囲の長さ"},
    {"circularity", "円形度"},
    {"height", "高さ"},
    {"width", "幅"},
    {"grayMin", "グレー値（最小）"},
    {"grayMax", "グレー値（最大）"},
    {"grayMean", "グレー値（平均）"},
    {"grayMedian", "グレー値（中央値）"},
    {"integratedDensity", "積分濃度"},
    {"histogram", "ヒストグラム"},
    {"length", "長さ"},
    {"angle", "角度"},
};

std::size_t column_index(std::string_view key) {
    for (std::size_t i = 0; i < std::size(kColumns); ++i)
        if (kColumns[i].key == key) return i;
    return std::size(kColumns);
}

void check_source(std::string_view source) {
    if (std::find(kSources.begin(), kSources.end(), source) == kSources.end()) throw OpError("source is selection, ruler or count");
}

}  // namespace

double Feature::circularity() const { return perimeter > 0 ? std::min(4.0 * std::numbers::pi * area / (perimeter * perimeter), 1.0) : 0.0; }

std::vector<Feature> measure_features(std::span<const std::uint8_t> mask, std::span<const float> gray, std::size_t w, std::size_t h,
                                      std::array<std::int64_t, 2> origin, double range) {
    // Two-pass connected components (8 directions).
    std::vector<std::uint32_t> label(w * h, kNone);
    std::vector<std::uint32_t> parent;
    for (std::size_t y = 0; y < h; ++y) {
        for (std::size_t x = 0; x < w; ++x) {
            const std::size_t i = y * w + x;
            if (mask[i] == 0) continue;
            std::array<std::uint32_t, 4> nbrs{kNone, kNone, kNone, kNone};
            if (x > 0) nbrs[0] = label[i - 1];
            if (y > 0) {
                nbrs[1] = label[i - w];
                if (x > 0) nbrs[2] = label[i - w - 1];
                if (x + 1 < w) nbrs[3] = label[i - w + 1];
            }
            std::uint32_t best = kNone;
            for (const std::uint32_t n : nbrs)
                if (n != kNone) best = std::min(best, root(parent, n));
            if (best == kNone) {
                best = static_cast<std::uint32_t>(parent.size());
                parent.push_back(best);
            } else {
                for (const std::uint32_t n : nbrs) {
                    if (n == kNone) continue;
                    const std::uint32_t r = root(parent, n);
                    if (r != best) {
                        const std::uint32_t a = std::min(r, best), b = std::max(r, best);
                        parent[b] = a;
                        best = a;
                    }
                }
            }
            label[i] = best;
        }
    }
    // Compact roots → feature indices in the order they first appear.
    std::vector<std::uint32_t> index(parent.size(), kNone);
    std::uint32_t n = 0;
    for (std::uint32_t& l : label) {
        if (l == kNone) continue;
        const std::uint32_t r = root(parent, l);
        if (index[r] == kNone) index[r] = n++;
        l = index[r];
    }
    std::vector<double> perimeter(n, 0.0);
    // Perimeter: marching squares over the pixel-centre grid (padded with unselected pixels), each cell's length given
    // to the feature of a pixel in it.
    const auto at = [&](std::ptrdiff_t x, std::ptrdiff_t y) -> std::uint32_t {
        if (x < 0 || y < 0 || x >= static_cast<std::ptrdiff_t>(w) || y >= static_cast<std::ptrdiff_t>(h)) return kNone;
        return label[static_cast<std::size_t>(y) * w + static_cast<std::size_t>(x)];
    };
    constexpr double kHalfDiagonal = std::numbers::sqrt2 / 2.0;
    for (std::ptrdiff_t y = -1; y < static_cast<std::ptrdiff_t>(h); ++y) {
        for (std::ptrdiff_t x = -1; x < static_cast<std::ptrdiff_t>(w); ++x) {
            const std::array<std::uint32_t, 4> c{at(x, y), at(x + 1, y), at(x + 1, y + 1), at(x, y + 1)};
            const int inside = static_cast<int>(std::count_if(c.begin(), c.end(), [](std::uint32_t v) { return v != kNone; }));
            double length = 0;
            if (inside == 1 || inside == 3) {
                length = kHalfDiagonal;
            } else if (inside == 2) {
                length = (c[0] != kNone) == (c[2] != kNone) ? 2.0 * kHalfDiagonal : 1.0;
            }
            if (length > 0) {
                const auto f = std::find_if(c.begin(), c.end(), [](std::uint32_t v) { return v != kNone; });
                perimeter[*f] += length;
            }
        }
    }
    // Area, bounds and gray values.
    std::vector<Feature> features(n);
    for (Feature& f : features) {
        f.gray_min = std::numeric_limits<double>::max();
        f.gray_max = std::numeric_limits<double>::lowest();
        f.histogram.assign(256, 0);
    }
    std::vector<double> sums(n, 0.0);
    std::vector<std::vector<std::uint16_t>> values(n);
    for (std::size_t y = 0; y < h; ++y) {
        for (std::size_t x = 0; x < w; ++x) {
            const std::uint32_t l = label[y * w + x];
            if (l == kNone) continue;
            Feature& f = features[l];
            f.area += 1.0;
            const std::int64_t px = static_cast<std::int64_t>(x) + origin[0], py = static_cast<std::int64_t>(y) + origin[1];
            if (!f.bounded) {
                f.x0 = px, f.y0 = py, f.x1 = px + 1, f.y1 = py + 1;
                f.bounded = true;
            } else {
                f.x0 = std::min(f.x0, px), f.y0 = std::min(f.y0, py), f.x1 = std::max(f.x1, px + 1), f.y1 = std::max(f.y1, py + 1);
            }
            const double g = static_cast<double>(gray[y * w + x]);
            f.gray_min = std::min(f.gray_min, g);
            f.gray_max = std::max(f.gray_max, g);
            sums[l] += g;
            const double clamped = std::clamp(g, 0.0, 1.0);
            f.histogram[static_cast<std::size_t>(std::round(clamped * 255.0))] += 1;
            values[l].push_back(static_cast<std::uint16_t>(std::round(clamped * 65535.0)));
        }
    }
    for (std::size_t i = 0; i < features.size(); ++i) {
        Feature& f = features[i];
        f.perimeter = perimeter[i];
        f.gray_mean = sums[i] / std::max(f.area, 1.0) * range;
        f.gray_min *= range;
        f.gray_max *= range;
        f.gray_median = median(values[i]) * range;
    }
    return features;
}

double median(std::vector<std::uint16_t>& values) {
    if (values.empty()) return 0.0;
    const std::size_t mid = values.size() / 2;
    std::nth_element(values.begin(), values.begin() + static_cast<std::ptrdiff_t>(mid), values.end());
    double m = values[mid];
    if (values.size() % 2 == 0) m = (m + *std::max_element(values.begin(), values.begin() + static_cast<std::ptrdiff_t>(mid))) / 2.0;
    return m / 65535.0;
}

Feature summary(const std::vector<Feature>& features, double median_of_all) {
    Feature out;
    out.histogram.assign(256, 0);
    out.gray_min = std::numeric_limits<double>::max();
    out.gray_max = std::numeric_limits<double>::lowest();
    double weighted = 0;
    for (const Feature& f : features) {
        out.area += f.area;
        out.perimeter += f.perimeter;
        for (std::size_t i = 0; i < out.histogram.size() && i < f.histogram.size(); ++i) out.histogram[i] += f.histogram[i];
        if (f.bounded) {
            if (!out.bounded) {
                out.x0 = f.x0, out.y0 = f.y0, out.x1 = f.x1, out.y1 = f.y1;
                out.bounded = true;
            } else {
                out.x0 = std::min(out.x0, f.x0), out.y0 = std::min(out.y0, f.y0), out.x1 = std::max(out.x1, f.x1), out.y1 = std::max(out.y1, f.y1);
            }
        }
        out.gray_min = std::min(out.gray_min, f.gray_min);
        out.gray_max = std::max(out.gray_max, f.gray_max);
        weighted += f.gray_mean * f.area;
    }
    out.gray_mean = weighted / std::max(out.area, 1.0);
    out.gray_median = median_of_all;
    return out;
}

// --- the data points -----------------------------------------------------------------------------------------------------

std::span<const Column> columns() { return kColumns; }

std::string column_name(std::string_view key) {
    const std::size_t i = column_index(key);
    return std::string(i < std::size(kColumns) ? kColumns[i].name : key);
}

std::vector<std::string> available(std::string_view source) {
    std::vector<std::string> out(std::begin(kCommon), std::end(kCommon));
    const std::span<const std::string_view> own = source == "selection" ? std::span<const std::string_view>(kSelectionOnly)
                                                  : source == "ruler"   ? std::span<const std::string_view>(kRulerOnly)
                                                                        : std::span<const std::string_view>(kCountOnly);
    out.insert(out.end(), own.begin(), own.end());
    return out;
}

const std::vector<std::string>& DataPoints::of(std::string_view source) const {
    return source == "selection" ? selection : source == "ruler" ? ruler : count;
}

std::vector<std::string>& DataPoints::of(std::string_view source) {
    return source == "selection" ? selection : source == "ruler" ? ruler : count;
}

DataPoints choose_data_points(const DataPoints& now, const Json& given) {
    if (!given.is_object()) throw OpError("the data points are {selection?, ruler?, count?, reset?}");
    for (const auto& [key, value] : given.items()) {
        if (key != "reset" && std::find(kSources.begin(), kSources.end(), key) == kSources.end())
            throw OpError("unknown data point source " + key + " (selection, ruler, count, reset)");
        (void)value;
    }
    const Json* reset = core::get(given, "reset");
    if (reset != nullptr && !reset->is_boolean()) throw OpError("reset is true or false");
    DataPoints next = reset != nullptr && reset->get<bool>() ? DataPoints{} : now;
    for (const std::string_view source : kSources) {
        const Json* v = core::get(given, source);
        if (v == nullptr) continue;
        const std::vector<std::string> can = available(source);
        std::string joined;
        for (const std::string& k : can) joined += (joined.empty() ? "" : "|") + k;
        const auto check = [&](const std::string& k) {
            if (std::find(can.begin(), can.end(), k) == can.end())
                throw OpError(k + " is not a " + std::string(source) + " data point (" + joined + ")");
        };
        std::vector<std::string>& list = next.of(source);
        if (v->is_array()) {  // (the whole list…)
            std::vector<std::string> keys;
            for (const Json& k : *v) {
                if (!k.is_string()) throw OpError(std::string(source) + "'s entries are the data points' keys");
                check(k.get<std::string>());
                if (std::find(keys.begin(), keys.end(), k.get<std::string>()) == keys.end()) keys.push_back(k.get<std::string>());
            }
            list = std::move(keys);
        } else if (v->is_object()) {  // (…or {key: on} changes)
            for (const auto& [k, on] : v->items()) {
                check(k);
                if (!on.is_boolean()) throw OpError(std::string(source) + "." + k + " is true or false");
                std::erase(list, k);
                if (on.get<bool>()) list.push_back(k);
            }
        } else {
            throw OpError(std::string(source) + " is a list of keys or {key: true | false}");
        }
        std::stable_sort(list.begin(), list.end(), [](const std::string& a, const std::string& b) { return column_index(a) < column_index(b); });
    }
    return next;
}

Json to_json(const DataPoints& points) {
    return Json{{"selection", points.selection}, {"ruler", points.ruler}, {"count", points.count}};
}

DataPoints data_points_from_json(const Json& value) {
    DataPoints out;
    if (!value.is_object()) return out;
    for (const std::string_view source : kSources) {
        const Json* v = core::get(value, source);
        if (v == nullptr || !v->is_array()) continue;
        try {
            out = choose_data_points(out, Json{{std::string(source), *v}});
        } catch (const OpError&) {  // (a list this build cannot read: the default kept)
        }
    }
    return out;
}

// --- 計測値を記録 --------------------------------------------------------------------------------------------------------

namespace {

// The gray (0..1) of each pixel of `region` (on the page at `dpi`) and what a gray of 1 reads as: the page's look in its
// depth's range. Drawn in bands of rows so a large precise page is never drawn whole at once.
struct Gray {
    std::vector<float> values;
    double range = 255.0;
};

Gray gray_of(const core::Document& doc, const core::Page& page, int dpi, const RenderRegion& region, std::vector<std::string>& notes) {
    Gray out;
    const int depth = core::page_depth(page);
    out.range = depth == 16 ? 32768.0 : depth == 32 ? 1.0 : 255.0;
    const bool grey_inks = page.color && page.color->space == core::InkSpace::Gray;
    const bool precise = !page.color && page.rgb && page.rgb->depth > 8;
    out.values.assign(static_cast<std::size_t>(region.w) * static_cast<std::size_t>(region.h), 0.0f);
    const int band = std::max(1, static_cast<int>(8'000'000 / std::max(1, region.w)));
    std::vector<std::string> omitted;
    for (int top = 0; top < region.h; top += band) {
        RenderOptions options = proof_options();
        options.skip_unported = true;
        options.inks = grey_inks;
        options.precise = precise;
        options.region = RenderRegion{region.x, region.y + top, region.w, std::min(band, region.h - top)};
        const RenderResult made = render_page(page, dpi, options, &doc);
        for (const std::string& o : made.omitted)
            if (std::find(omitted.begin(), omitted.end(), o) == omitted.end()) omitted.push_back(o);
        const std::size_t count = static_cast<std::size_t>(region.w) * static_cast<std::size_t>(options.region->h);
        float* into = out.values.data() + static_cast<std::size_t>(top) * static_cast<std::size_t>(region.w);
        if (grey_inks && made.inks && made.inks->pixel_count() == count) {
            for (std::size_t i = 0; i < count; ++i) into[i] = static_cast<float>(1.0 - made.inks->value(i, 0));  // (ink → lightness)
            continue;
        }
        if (precise && made.precise && made.precise->size() >= 16) {
            const core::ColorRasterView view(*made.precise);
            if (static_cast<std::size_t>(view.width()) * view.height() == count) {
                for (std::size_t i = 0; i < count; ++i) {
                    const auto p = view.pixel(i);
                    into[i] = static_cast<float>(0.30 * p[0] + 0.59 * p[1] + 0.11 * p[2]);
                }
                continue;
            }
        }
        const std::string rgb = made.image.convert("RGB").tobytes();
        for (std::size_t i = 0; i < count && i * 3 + 2 < rgb.size(); ++i) {
            const auto c = [&](std::size_t k) { return static_cast<unsigned char>(rgb[i * 3 + k]) / 255.0; };
            into[i] = static_cast<float>(0.30 * c(0) + 0.59 * c(1) + 0.11 * c(2));
        }
    }
    for (const std::string& o : omitted) notes.push_back("the gray values leave out what this build does not draw yet: " + o);
    return out;
}

std::string document_name(const core::Document& doc, const core::Page& page) {
    const std::string number = page.index.json().dump();
    return doc.title.empty() ? number + " ページ" : doc.title + "（" + number + " ページ）";
}

Json base_row(const core::Document& doc, const core::Page& page, std::string_view source, const std::string& label, const std::string& now) {
    const std::optional<core::MeasurementScale>& scale = page.measure ? page.measure->scale : std::nullopt;
    const core::MeasurementScale s = scale.value_or(core::MeasurementScale{});
    return Json{{"label", label},
                {"dateTime", now},
                {"document", document_name(doc, page)},
                {"source", source == "selection" ? "選択範囲" : source == "ruler" ? "ものさしツール" : "カウントツール"},
                {"scale", core::describe(s)},
                {"scaleUnits", s.units},
                {"scaleFactor", core::scale_factor(scale)}};
}

// A feature's values: `per` page pixels a counted pixel, `factor` logical units a page pixel.
void feature_values(Json& row, const Feature& f, std::size_t count, double per, double factor) {
    const double side = per * factor;
    row["count"] = count;
    row["area"] = r4(f.area * side * side);
    row["perimeter"] = r4(f.perimeter * side);
    row["circularity"] = r4(f.circularity());
    row["height"] = r4(static_cast<double>(f.height()) * side);
    row["width"] = r4(static_cast<double>(f.width()) * side);
    row["grayMin"] = r4(f.gray_min);
    row["grayMax"] = r4(f.gray_max);
    row["grayMean"] = r4(f.gray_mean);
    row["grayMedian"] = r4(f.gray_median);
    row["integratedDensity"] = r4(f.area * per * per * f.gray_mean);  // (PhotoCraft's: the pixels — the page's — × the mean)
    row["histogram"] = f.histogram;
}

}  // namespace

Recorded record(const core::Document& doc, const core::Page& page, std::string_view source, const std::optional<Json>& area,
                std::size_t number, const std::string& now) {
    static const core::Measure none;
    const core::Measure& m = page.measure ? *page.measure : none;
    Recorded out;
    if (source == "auto") {
        out.source = area ? "selection" : m.ruler ? "ruler" : core::count_total(m) > 0 ? "count" : "selection";
    } else {
        check_source(source);
        out.source = std::string(source);
    }
    const std::string label = "計測 " + std::to_string(number);
    const double factor = core::scale_factor(m.scale);
    if (out.source == "ruler") {
        if (!m.ruler) throw OpError("the page has no ruler line to measure (draw one with the ruler tool: set_ruler_line)");
        const core::RulerReadout readout = core::ruler_readout(*m.ruler, m.scale, core::page_px_per_mm(page));
        Json row = base_row(doc, page, out.source, label, now);
        row["length"] = readout.length;
        row["angle"] = readout.angle;
        out.rows.push_back(std::move(row));
        return out;
    }
    if (out.source == "count") {
        Json row = base_row(doc, page, out.source, label, now);
        row["count"] = core::count_total(m);
        out.rows.push_back(std::move(row));
        return out;
    }
    // the selection (none: the whole page), counted on the page's selection grid
    use_brushes_of(doc);  // (as the ops that draw see the book)
    const int dpi = selection::grid_dpi(page);
    out.grid_dpi = dpi;
    const int pw = mm_to_px(page.spec.width_mm.value(), dpi), ph = mm_to_px(page.spec.height_mm.value(), dpi);
    Box box{0, 0, pw, ph};
    std::vector<std::uint8_t> mask;
    if (area) {
        const Image drawn = selection::to_mask(*area, page, &doc, dpi);
        const std::optional<Box> held = drawn.getbbox();
        if (!held) throw OpError("the selection is empty");
        box = Box{std::max(0, held->x0), std::max(0, held->y0), std::min(pw, held->x1), std::min(ph, held->y1)};
        if (box.width() <= 0 || box.height() <= 0) throw OpError("the selection is empty");
        limits::check_picture(box.width(), box.height(), "the selection measured");
        const std::string bytes = drawn.crop(box).tobytes();
        mask.resize(bytes.size());
        for (std::size_t i = 0; i < bytes.size(); ++i) mask[i] = static_cast<unsigned char>(bytes[i]) >= 128 ? 1 : 0;  // (coverage ≥ 50 %)
    } else {
        limits::check_picture(pw, ph, "the page measured");
        mask.assign(static_cast<std::size_t>(pw) * static_cast<std::size_t>(ph), 1);
    }
    const std::size_t w = static_cast<std::size_t>(box.width()), h = static_cast<std::size_t>(box.height());
    const Gray gray = gray_of(doc, page, dpi, RenderRegion{box.x0, box.y0, box.width(), box.height()}, out.notes);
    const std::vector<Feature> features = measure_features(mask, gray.values, w, h, {box.x0, box.y0}, gray.range);
    if (features.empty()) throw OpError("the selection is empty");
    // the median of every value selected (as one feature's)
    std::vector<std::uint64_t> counts(65536, 0);
    std::uint64_t total = 0;
    for (std::size_t i = 0; i < mask.size(); ++i) {
        if (mask[i] == 0) continue;
        counts[static_cast<std::size_t>(std::round(std::clamp(static_cast<double>(gray.values[i]), 0.0, 1.0) * 65535.0))] += 1;
        ++total;
    }
    const auto nth = [&](std::uint64_t k) {  // (the k-th value, 0 the least)
        std::uint64_t seen = 0;
        for (std::size_t v = 0; v < counts.size(); ++v) {
            seen += counts[v];
            if (seen > k) return static_cast<double>(v);
        }
        return 0.0;
    };
    double middle = nth(total / 2);
    if (total % 2 == 0) middle = (middle + nth(total / 2 - 1)) / 2.0;
    const double per = core::page_px_per_mm(page) / (dpi / 25.4);  // (page pixels a counted pixel)
    Json row = base_row(doc, page, out.source, label, now);
    feature_values(row, summary(features, middle / 65535.0 * gray.range), features.size(), per, factor);
    out.rows.push_back(std::move(row));
    if (features.size() > 1) {
        for (std::size_t i = 0; i < features.size(); ++i) {
            Json one = base_row(doc, page, out.source, "フィーチャ " + std::to_string(i + 1), now);
            feature_values(one, features[i], 1, per, factor);
            out.rows.push_back(std::move(one));
        }
    }
    return out;
}

std::string now_text() {
    const QDateTime now = QDateTime::currentDateTime();
    return now.toOffsetFromUtc(now.offsetFromUtc()).toString(Qt::ISODate).toStdString();  // (the offset written: +09:00)
}

double nice_marker_length(const core::Page& page) {
    const std::optional<core::MeasurementScale>& scale = page.measure ? page.measure->scale : std::nullopt;
    const double target = page.trim_rect_mm().width.value() * core::page_px_per_mm(page) / 5.0 * core::scale_factor(scale);
    const double e = std::pow(10.0, std::floor(std::log10(target)));
    for (const double k : {5.0, 2.0, 1.0})
        if (k * e <= target) return k * e;
    return e;
}

Json kept(const Json& row, const std::vector<std::string>& points) {
    Json out = Json::object();
    for (const Column& c : kColumns) {
        const std::string key(c.key);
        if (row.contains(key) && std::find(points.begin(), points.end(), key) != points.end()) out[key] = row[key];
    }
    return out;
}

// --- the log's CSV -------------------------------------------------------------------------------------------------------

std::vector<std::string> log_columns(std::span<const Json> rows) {
    std::vector<std::string> out;
    for (const Column& c : kColumns) {
        const std::string key(c.key);
        if (std::any_of(rows.begin(), rows.end(), [&](const Json& r) { return r.is_object() && r.contains(key); })) out.push_back(key);
    }
    return out;
}

std::string csv_field(const Json& value) {
    std::string s;
    if (value.is_string()) {
        s = value.get<std::string>();
    } else if (value.is_array()) {
        for (const Json& v : value) s += (s.empty() ? "" : " ") + v.dump();
    } else if (!value.is_null()) {
        s = value.dump();
    }
    if (s.find_first_of(",\"\n\r") == std::string::npos) return s;
    std::string quoted = "\"";
    for (const char ch : s) {
        if (ch == '"') quoted += '"';
        quoted += ch;
    }
    return quoted + "\"";
}

std::string csv(std::span<const Json> rows) {
    const std::vector<std::string> cols = log_columns(rows);
    std::string out;
    for (std::size_t i = 0; i < cols.size(); ++i) out += (i ? "," : "") + csv_field(Json(column_name(cols[i])));
    out += "\n";
    for (const Json& row : rows) {
        for (std::size_t i = 0; i < cols.size(); ++i) {
            if (i) out += ",";
            if (row.is_object() && row.contains(cols[i])) out += csv_field(row[cols[i]]);
        }
        out += "\n";
    }
    return out;
}

}  // namespace genko::render::measure
