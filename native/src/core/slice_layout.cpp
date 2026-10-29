// PC5 J-b (core/slice_layout.hpp): a page's slices in pixels — PhotoCraft v0.2.0's crates/doc/src/slices.rs (MIT,
// native/third_party/photocraft/NOTICE.md) over Genko's mm.

#include "core/slice_layout.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <iterator>

#include "core/pynum.hpp"

namespace genko::core {

namespace {

double r4(double v) { return py_round(v, 4); }

std::int64_t edge(double v) {
    if (!std::isfinite(v)) return 0;
    return static_cast<std::int64_t>(std::llround(std::clamp(v, -1e15, 1e15)));
}

}  // namespace

SliceBox SliceBox::intersect(const SliceBox& other) const {
    SliceBox out{std::max(x0, other.x0), std::max(y0, other.y0), std::min(x1, other.x1), std::min(y1, other.y1)};
    if (out.empty()) return SliceBox{};
    return out;
}

SliceGrid slice_grid(double width_mm, double height_mm, std::int64_t width_px, std::int64_t height_px) {
    SliceGrid g;
    g.px_x = width_mm > 0 && width_px > 0 ? static_cast<double>(width_px) / width_mm : 1.0;
    g.px_y = height_mm > 0 && height_px > 0 ? static_cast<double>(height_px) / height_mm : 1.0;
    g.canvas = SliceBox{0, 0, std::max<std::int64_t>(0, width_px), std::max<std::int64_t>(0, height_px)};
    return g;
}

SliceBox slice_box(const std::array<double, 4>& r, const SliceGrid& g) {
    return SliceBox{edge(r[0] * g.px_x), edge(r[1] * g.px_y), edge((r[0] + r[2]) * g.px_x), edge((r[1] + r[3]) * g.px_y)};
}

std::array<double, 4> slice_rect_mm(const SliceBox& b, const SliceGrid& g) {
    const double x0 = static_cast<double>(b.x0) / g.px_x, y0 = static_cast<double>(b.y0) / g.px_y;
    const double x1 = static_cast<double>(b.x1) / g.px_x, y1 = static_cast<double>(b.y1) / g.px_y;
    return {r4(x0), r4(y0), r4(x1 - x0), r4(y1 - y0)};
}

std::vector<SliceBox> auto_slices(const SliceBox& canvas, const std::vector<SliceBox>& taken) {
    if (canvas.empty()) return {};
    std::vector<std::int64_t> xs{canvas.x0, canvas.x1}, ys{canvas.y0, canvas.y1};
    std::vector<SliceBox> inside;
    for (const SliceBox& t : taken) {
        const SliceBox r = t.intersect(canvas);
        if (r.empty()) continue;
        inside.push_back(r);
        xs.push_back(r.x0);
        xs.push_back(r.x1);
        ys.push_back(r.y0);
        ys.push_back(r.y1);
    }
    std::sort(xs.begin(), xs.end());
    xs.erase(std::unique(xs.begin(), xs.end()), xs.end());
    std::sort(ys.begin(), ys.end());
    ys.erase(std::unique(ys.begin(), ys.end()), ys.end());
    const auto covered = [&](std::int64_t x, std::int64_t y) {
        return std::any_of(inside.begin(), inside.end(), [&](const SliceBox& r) { return x >= r.x0 && x < r.x1 && y >= r.y0 && y < r.y1; });
    };
    // (horizontal runs along each band)
    std::vector<SliceBox> runs;
    for (std::size_t j = 0; j + 1 < ys.size(); ++j) {
        std::optional<std::int64_t> start;
        for (std::size_t i = 0; i + 1 < xs.size(); ++i) {
            const bool free = !covered(xs[i], ys[j]);
            if (free && !start) start = xs[i];
            if (!free && start) {
                runs.push_back(SliceBox{*start, ys[j], xs[i], ys[j + 1]});
                start.reset();
            }
        }
        if (start) runs.push_back(SliceBox{*start, ys[j], xs.back(), ys[j + 1]});
    }
    // (runs of the same columns joined down)
    std::vector<SliceBox> out;
    for (const SliceBox& r : runs) {
        const auto prev = std::find_if(out.begin(), out.end(), [&](const SliceBox& p) { return p.x0 == r.x0 && p.x1 == r.x1 && p.y1 == r.y0; });
        if (prev != out.end()) {
            prev->y1 = r.y1;
        } else {
            out.push_back(r);
        }
    }
    return out;
}

std::vector<ResolvedSlice> resolve_slices(const Slices& slices, const std::vector<std::pair<std::int64_t, SliceBox>>& stored, const SliceBox& canvas,
                                          const std::string& base) {
    std::vector<std::pair<std::optional<std::int64_t>, SliceBox>> all;
    std::vector<SliceBox> taken;
    for (const auto& [id, box] : stored) {
        const SliceBox clipped = box.intersect(canvas);
        if (clipped.empty()) continue;
        all.emplace_back(id, clipped);
        taken.push_back(clipped);
    }
    for (const SliceBox& b : auto_slices(canvas, taken)) all.emplace_back(std::nullopt, b);
    std::stable_sort(all.begin(), all.end(), [](const auto& a, const auto& b) {
        return std::pair(a.second.y0, a.second.x0) < std::pair(b.second.y0, b.second.x0);
    });
    const bool single = all.size() == 1;
    std::vector<ResolvedSlice> out;
    out.reserve(all.size());
    for (std::size_t i = 0; i < all.size(); ++i) {
        ResolvedSlice r;
        r.number = static_cast<int>(i + 1);
        r.id = all[i].first;
        r.box = all[i].second;
        const Slice* s = r.id ? find_slice(slices, *r.id) : nullptr;
        r.origin = s != nullptr ? s->origin : std::string("auto");
        r.kind = s != nullptr ? s->kind : std::string("image");
        if (s != nullptr && !s->name.empty()) {
            r.name = s->name;
        } else if (single) {
            r.name = base;
        } else {
            char digits[24];
            std::snprintf(digits, sizeof digits, "_%02d", r.number);
            r.name = base + digits;
        }
        out.push_back(std::move(r));
    }
    return out;
}

std::string slice_file_name(std::string_view name, std::string_view otherwise) {
    std::string out;
    out.reserve(name.size());
    for (std::size_t i = 0; i < name.size();) {
        const auto c = static_cast<unsigned char>(name[i]);
        if (c >= 0x80) {  // (a letter of another script: kept whole, as Rust's is_alphanumeric keeps it — but its punctuation)
            std::size_t n = c >= 0xf0 ? 4 : c >= 0xe0 ? 3 : c >= 0xc0 ? 2 : 1;
            n = std::min(n, name.size() - i);
            const std::string_view ch = name.substr(i, n);
            // (the Japanese and full-width punctuation and spaces Windows or a web server minds: '_')
            static constexpr std::string_view kMarks[] = {"　", "、", "。", "／", "＼", "：", "＊", "？", "＂",
                                                          "＜", "＞", "｜", "「", "」", "・"};
            const bool mark = std::find(std::begin(kMarks), std::end(kMarks), ch) != std::end(kMarks);
            out += mark ? std::string("_") : std::string(ch);
            i += n;
            continue;
        }
        out += (std::isalnum(c) != 0 || c == '-' || c == '_') ? static_cast<char>(c) : '_';
        ++i;
    }
    if (out.empty()) return std::string(otherwise);
    return out;
}

const Slice* find_slice(const Slices& slices, std::int64_t id) {
    for (const Slice& s : slices.list)
        if (s.id == id) return &s;
    return nullptr;
}

std::int64_t fresh_slice_id(const Slices& slices) {
    std::int64_t next = std::max<std::int64_t>(1, slices.next_id);
    for (const Slice& s : slices.list) next = std::max(next, s.id + 1);
    return next;
}

std::vector<std::array<double, 4>> divide_rect(const std::array<double, 4>& r, int rows, int columns) {
    std::vector<std::array<double, 4>> out;
    if (rows < 1 || columns < 1) return out;
    for (int j = 0; j < rows; ++j) {
        const double y0 = r[1] + r[3] * j / rows, y1 = r[1] + r[3] * (j + 1) / rows;
        for (int i = 0; i < columns; ++i) {
            const double x0 = r[0] + r[2] * i / columns, x1 = r[0] + r[2] * (i + 1) / columns;
            out.push_back({r4(x0), r4(y0), r4(r4(x1) - r4(x0)), r4(r4(y1) - r4(y0))});
        }
    }
    return out;
}

std::vector<std::array<double, 4>> guide_cells(double width_mm, double height_mm, std::vector<double> xs, std::vector<double> ys) {
    std::vector<double> across{0.0, width_mm}, down{0.0, height_mm};
    for (const double x : xs)
        if (std::isfinite(x) && x > 0 && x < width_mm) across.push_back(r4(x));
    for (const double y : ys)
        if (std::isfinite(y) && y > 0 && y < height_mm) down.push_back(r4(y));
    if (across.size() == 2 && down.size() == 2) return {};
    std::sort(across.begin(), across.end());
    across.erase(std::unique(across.begin(), across.end()), across.end());
    std::sort(down.begin(), down.end());
    down.erase(std::unique(down.begin(), down.end()), down.end());
    std::vector<std::array<double, 4>> out;
    for (std::size_t j = 0; j + 1 < down.size(); ++j) {
        for (std::size_t i = 0; i + 1 < across.size(); ++i) {
            out.push_back({across[i], down[j], r4(across[i + 1] - across[i]), r4(down[j + 1] - down[j])});
        }
    }
    return out;
}

}  // namespace genko::core
