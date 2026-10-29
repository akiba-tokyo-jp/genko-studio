// PC5 J-b (core/slice_layout.hpp): a page's slices in pixels — PhotoCraft v0.2.0's crates/doc/src/slices.rs (MIT,
// native/third_party/photocraft/NOTICE.md) over Genko's mm.

#include "core/slice_layout.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <functional>
#include <iterator>

#include "core/error.hpp"
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

namespace {

// The distinct edges (sorted) and where a value lies among them.
std::size_t index_of(const std::vector<std::int64_t>& edges, std::int64_t v) {
    return static_cast<std::size_t>(std::lower_bound(edges.begin(), edges.end(), v) - edges.begin());
}

// A box's cells among the edges: columns [c0, c1), rows [r0, r1).
struct Span {
    std::size_t c0 = 0, c1 = 0, r0 = 0, r1 = 0;
};

// Runs of the cells along each band whose owner `owner_at(row, column)` is the same (none: -1, left out), each joined to
// the run of the band above of the same columns and owner (PhotoCraft's "runs of the same columns joined down"): the
// boxes in the order they began (by band, then across), each with its owner.
template <class OwnerAt>
std::vector<std::pair<std::int64_t, SliceBox>> runs_joined(const std::vector<std::int64_t>& xs, const std::vector<std::int64_t>& ys, OwnerAt&& owner_at,
                                                           const std::function<void(std::size_t)>& band) {
    std::vector<std::pair<std::int64_t, SliceBox>> out;
    // (the runs of the band above: their first column, last column + 1, owner and box in `out` — in order across)
    struct Open {
        std::size_t c0, c1;
        std::int64_t owner;
        std::size_t at;
    };
    std::vector<Open> above, here;
    const std::size_t nx = xs.size() - 1;
    for (std::size_t j = 0; j + 1 < ys.size(); ++j) {
        band(j);
        here.clear();
        std::size_t k = 0;  // (the run above looked at: both lists go across)
        for (std::size_t i = 0; i < nx;) {
            const std::int64_t owner = owner_at(j, i);
            std::size_t e = i + 1;
            while (e < nx && owner_at(j, e) == owner) ++e;
            if (owner >= 0) {
                while (k < above.size() && above[k].c0 < i) ++k;
                if (k < above.size() && above[k].c0 == i && above[k].c1 == e && above[k].owner == owner) {
                    out[above[k].at].second.y1 = ys[j + 1];
                    here.push_back(Open{i, e, owner, above[k].at});
                } else {
                    out.emplace_back(owner, SliceBox{xs[i], ys[j], xs[e], ys[j + 1]});
                    here.push_back(Open{i, e, owner, out.size() - 1});
                }
            }
            i = e;
        }
        std::swap(above, here);
    }
    return out;
}

}  // namespace

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
    // (PC5 J-b review P2-2: the covered cells painted by their indices, a band at a time — each slice counted into the
    // columns it covers when its first band comes and out after its last —: the cells and the slices' columns once each,
    // never a cell against every slice)
    std::vector<Span> spans;
    spans.reserve(inside.size());
    for (const SliceBox& r : inside) spans.push_back(Span{index_of(xs, r.x0), index_of(xs, r.x1), index_of(ys, r.y0), index_of(ys, r.y1)});
    std::vector<std::size_t> by_start(spans.size()), by_end(spans.size());
    for (std::size_t i = 0; i < spans.size(); ++i) by_start[i] = by_end[i] = i;
    std::sort(by_start.begin(), by_start.end(), [&](std::size_t a, std::size_t b) { return spans[a].r0 < spans[b].r0; });
    std::sort(by_end.begin(), by_end.end(), [&](std::size_t a, std::size_t b) { return spans[a].r1 < spans[b].r1; });
    std::vector<std::int32_t> count(xs.size() - 1, 0);
    std::size_t started = 0, ended = 0;
    const auto band = [&](std::size_t j) {
        for (; ended < by_end.size() && spans[by_end[ended]].r1 <= j; ++ended)
            for (std::size_t c = spans[by_end[ended]].c0; c < spans[by_end[ended]].c1; ++c) --count[c];
        for (; started < by_start.size() && spans[by_start[started]].r0 <= j; ++started)
            for (std::size_t c = spans[by_start[started]].c0; c < spans[by_start[started]].c1; ++c) ++count[c];
    };
    const auto free_at = [&](std::size_t, std::size_t i) -> std::int64_t { return count[i] == 0 ? 0 : -1; };
    std::vector<SliceBox> out;
    for (auto& [owner, box] : runs_joined(xs, ys, free_at, band)) out.push_back(box);
    return out;
}

std::vector<std::vector<SliceBox>> owned_pieces(const std::vector<SliceBox>& boxes, const SliceBox& canvas) {
    std::vector<std::vector<SliceBox>> out(boxes.size());
    std::vector<SliceBox> clipped(boxes.size());
    for (std::size_t i = 0; i < boxes.size(); ++i) clipped[i] = boxes[i].intersect(canvas);
    // (the ones over or under another: only they are cut — every other one is whole)
    std::vector<std::size_t> order(boxes.size());
    for (std::size_t i = 0; i < order.size(); ++i) order[i] = i;
    std::sort(order.begin(), order.end(), [&](std::size_t a, std::size_t b) { return clipped[a].x0 < clipped[b].x0; });
    std::vector<char> overlaps(boxes.size(), 0);
    for (std::size_t a = 0; a < order.size(); ++a) {
        const SliceBox& p = clipped[order[a]];
        if (p.empty()) continue;
        for (std::size_t b = a + 1; b < order.size() && clipped[order[b]].x0 < p.x1; ++b) {
            const SliceBox& q = clipped[order[b]];
            if (q.empty() || q.y0 >= p.y1 || p.y0 >= q.y1 || q.x1 <= p.x0) continue;
            overlaps[order[a]] = overlaps[order[b]] = 1;
        }
    }
    std::vector<std::size_t> cut;
    for (std::size_t i = 0; i < boxes.size(); ++i) {
        if (clipped[i].empty()) continue;
        if (overlaps[i]) {
            cut.push_back(i);
        } else {
            out[i].push_back(clipped[i]);
        }
    }
    if (cut.empty()) return out;
    std::vector<std::int64_t> xs, ys;
    for (const std::size_t i : cut) {
        xs.insert(xs.end(), {clipped[i].x0, clipped[i].x1});
        ys.insert(ys.end(), {clipped[i].y0, clipped[i].y1});
    }
    std::sort(xs.begin(), xs.end());
    xs.erase(std::unique(xs.begin(), xs.end()), xs.end());
    std::sort(ys.begin(), ys.end());
    ys.erase(std::unique(ys.begin(), ys.end()), ys.end());
    const std::size_t nx = xs.size() - 1, ny = ys.size() - 1;
    if (nx * ny > kOwnedCellsMost) throw Error("value", "the slices overlap in too many places to be cut into cells");
    // (each cell its top slice's — the later in the list, as Photoshop draws and writes them —: painted from the top down,
    // a cell once, the painted ones of a row passed over by their next unpainted one)
    std::vector<std::int32_t> owner(nx * ny, -1);
    std::vector<std::uint32_t> next(ny * (nx + 1));
    for (std::size_t j = 0; j < ny; ++j)
        for (std::size_t i = 0; i <= nx; ++i) next[j * (nx + 1) + i] = static_cast<std::uint32_t>(i);
    const auto find = [&](std::size_t j, std::size_t i) {
        std::uint32_t* row = next.data() + j * (nx + 1);
        std::size_t root = i;
        while (row[root] != root) root = row[root];
        while (row[i] != root) {
            const std::size_t up = row[i];
            row[i] = static_cast<std::uint32_t>(root);
            i = up;
        }
        return root;
    };
    for (auto it = cut.rbegin(); it != cut.rend(); ++it) {
        const SliceBox& b = clipped[*it];
        const Span s{index_of(xs, b.x0), index_of(xs, b.x1), index_of(ys, b.y0), index_of(ys, b.y1)};
        for (std::size_t j = s.r0; j < s.r1; ++j) {
            for (std::size_t i = find(j, s.c0); i < s.c1; i = find(j, i + 1)) {
                owner[j * nx + i] = static_cast<std::int32_t>(*it);
                next[j * (nx + 1) + i] = static_cast<std::uint32_t>(i + 1);
            }
        }
    }
    const auto owner_at = [&](std::size_t j, std::size_t i) -> std::int64_t { return owner[j * nx + i]; };
    for (auto& [who, box] : runs_joined(xs, ys, owner_at, [](std::size_t) {})) out[static_cast<std::size_t>(who)].push_back(box);
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
