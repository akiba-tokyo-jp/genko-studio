#include "render/seam.hpp"

#include <algorithm>
#include <cmath>
#include <optional>

// PhotoCraft v0.2.0 crates/algo/src/seam.rs (1-240 and its tests from 240), MIT: native/third_party/photocraft/NOTICE.md.

namespace genko::render::seam {

namespace {

constexpr float kProtectEnergy = 1.0e4f;  // (an energy per unit of protection far above any gradient's)

std::vector<float> remove_seam(std::size_t w, std::size_t h, std::size_t ch, const std::vector<float>& img, const std::vector<std::size_t>& seam) {
    std::vector<float> out;
    out.reserve((w - 1) * h * ch);
    for (std::size_t y = 0; y < h; ++y) {
        const float* row = img.data() + y * w * ch;
        const std::size_t sx = seam[y];
        out.insert(out.end(), row, row + sx * ch);
        out.insert(out.end(), row + (sx + 1) * ch, row + w * ch);
    }
    return out;
}

std::vector<float> transpose(std::size_t w, std::size_t h, std::size_t ch, const std::vector<float>& img) {
    std::vector<float> out(img.size());
    for (std::size_t y = 0; y < h; ++y)
        for (std::size_t x = 0; x < w; ++x) std::copy_n(img.data() + (y * w + x) * ch, ch, out.data() + (x * h + y) * ch);
    return out;
}

void add_protection(std::vector<float>& e, const std::vector<float>* p) {
    if (p == nullptr) return;
    for (std::size_t i = 0; i < e.size() && i < p->size(); ++i) e[i] += std::clamp((*p)[i], 0.0f, 1.0f) * kProtectEnergy;
}

}  // namespace

std::vector<float> energy(std::size_t w, std::size_t h, std::size_t ch, const std::vector<float>& img) {
    std::vector<float> e(w * h, 0.0f);
    const auto at = [&](std::size_t x, std::size_t y, std::size_t c) { return img[(y * w + x) * ch + c]; };
    for (std::size_t y = 0; y < h; ++y) {
        for (std::size_t x = 0; x < w; ++x) {
            const std::size_t xl = x > 0 ? x - 1 : 0, xr = std::min(x + 1, w - 1);
            const std::size_t yu = y > 0 ? y - 1 : 0, yd = std::min(y + 1, h - 1);
            float s = 0.0f;
            for (std::size_t c = 0; c < ch; ++c) {
                const float v = at(x, y, c);
                s += std::fabs(at(xr, y, c) - v) + std::fabs(v - at(xl, y, c));
                s += std::fabs(at(x, yd, c) - v) + std::fabs(v - at(x, yu, c));
            }
            e[y * w + x] = s * 0.5f;
        }
    }
    return e;
}

std::vector<std::size_t> find_seam(std::size_t w, std::size_t h, const std::vector<float>& en) {
    std::vector<float> cost(en.begin(), en.begin() + static_cast<std::ptrdiff_t>(w));
    std::vector<unsigned char> from(w * h, 0);  // (0 up-left, 1 up, 2 up-right)
    std::vector<float> next(w, 0.0f);
    for (std::size_t y = 1; y < h; ++y) {
        for (std::size_t x = 0; x < w; ++x) {
            float best = cost[x];
            unsigned char way = 1;
            if (x > 0 && cost[x - 1] < best) {
                best = cost[x - 1];
                way = 0;
            }
            if (x + 1 < w && cost[x + 1] < best) {
                best = cost[x + 1];
                way = 2;
            }
            next[x] = best + en[y * w + x];
            from[y * w + x] = way;
        }
        std::swap(cost, next);
    }
    std::size_t x = 0;
    for (std::size_t i = 1; i < w; ++i)
        if (cost[i] < cost[x]) x = i;
    std::vector<std::size_t> seam(h, 0);
    for (std::size_t y = h; y-- > 0;) {
        seam[y] = x;
        if (y > 0) {
            const unsigned char way = from[y * w + x];
            if (way == 0) --x;
            if (way == 2) ++x;
        }
    }
    return seam;
}

std::vector<float> carve_width(std::size_t w, std::size_t h, std::size_t ch, const std::vector<float>& img, const std::vector<float>* protect,
                               std::size_t new_w) {
    new_w = std::max<std::size_t>(new_w, 1);
    if (new_w == w || w == 0 || h == 0) return img;
    std::vector<float> cur = img;
    std::size_t cw = w;
    std::optional<std::vector<float>> prot;
    if (protect != nullptr) prot = *protect;
    if (new_w < w) {
        while (cw > new_w) {
            std::vector<float> e = energy(cw, h, ch, cur);
            add_protection(e, prot ? &*prot : nullptr);
            const auto seam = find_seam(cw, h, e);
            cur = remove_seam(cw, h, ch, cur, seam);
            if (prot) prot = remove_seam(cw, h, 1, *prot, seam);
            --cw;
        }
        return cur;
    }
    // (enlarged in rounds of at most half the width, each doubling the k lowest seams)
    while (cw < new_w) {
        const std::size_t k = std::min(new_w - cw, std::max<std::size_t>(cw / 2, 1));
        // (k seams found on a shrinking copy, each mapped back to the columns it was)
        std::vector<std::vector<std::size_t>> idx(h);
        for (auto& row : idx) {
            row.resize(cw);
            for (std::size_t x = 0; x < cw; ++x) row[x] = x;
        }
        std::vector<float> tmp = cur;
        std::size_t tw = cw;
        std::optional<std::vector<float>> tprot = prot;
        std::vector<std::vector<std::size_t>> dup(h);
        for (std::size_t s = 0; s < k; ++s) {
            if (tw <= 1) break;
            std::vector<float> e = energy(tw, h, ch, tmp);
            add_protection(e, tprot ? &*tprot : nullptr);
            const auto seam = find_seam(tw, h, e);
            for (std::size_t y = 0; y < h; ++y) {
                dup[y].push_back(idx[y][seam[y]]);
                idx[y].erase(idx[y].begin() + static_cast<std::ptrdiff_t>(seam[y]));
            }
            tmp = remove_seam(tw, h, ch, tmp, seam);
            if (tprot) tprot = remove_seam(tw, h, 1, *tprot, seam);
            --tw;
        }
        const std::size_t added = dup[0].size();
        if (added == 0) break;
        const std::size_t nw = cw + added;
        std::vector<float> out;
        out.reserve(nw * h * ch);
        std::optional<std::vector<float>> nprot;
        if (prot) {
            nprot.emplace();
            nprot->reserve(nw * h);
        }
        for (std::size_t y = 0; y < h; ++y) {
            std::vector<std::size_t>& d = dup[y];
            std::sort(d.begin(), d.end());
            const float* row = cur.data() + y * cw * ch;
            std::size_t di = 0;
            for (std::size_t x = 0; x < cw; ++x) {
                out.insert(out.end(), row + x * ch, row + (x + 1) * ch);
                if (nprot) nprot->push_back((*prot)[y * cw + x]);
                while (di < d.size() && d[di] == x) {
                    const std::size_t xr = std::min(x + 1, cw - 1);
                    for (std::size_t c = 0; c < ch; ++c) out.push_back((row[x * ch + c] + row[xr * ch + c]) * 0.5f);
                    if (nprot) nprot->push_back((*prot)[y * cw + x]);
                    ++di;
                }
            }
        }
        cur = std::move(out);
        prot = std::move(nprot);
        cw = nw;
    }
    return cur;
}

std::vector<float> carve(std::size_t w, std::size_t h, std::size_t ch, const std::vector<float>& img, const std::vector<float>* protect,
                         std::size_t new_w, std::size_t new_h) {
    new_w = std::max<std::size_t>(new_w, 1);
    new_h = std::max<std::size_t>(new_h, 1);
    const std::vector<float> wide = carve_width(w, h, ch, img, protect, new_w);
    if (new_h == h) return wide;
    // (the protection follows the width's change by a plain resize: it only steers the seams)
    std::optional<std::vector<float>> prot_t;
    if (protect != nullptr) prot_t = transpose(new_w, h, 1, resize_bilinear(w, h, 1, *protect, new_w, h));
    const std::vector<float> t = transpose(new_w, h, ch, wide);
    const std::vector<float> tall = carve_width(h, new_w, ch, t, prot_t ? &*prot_t : nullptr, new_h);
    return transpose(new_h, new_w, ch, tall);
}

std::vector<float> resize_bilinear(std::size_t w, std::size_t h, std::size_t ch, const std::vector<float>& img, std::size_t nw, std::size_t nh) {
    std::vector<float> out(nw * nh * ch, 0.0f);
    if (w == 0 || h == 0) return out;
    for (std::size_t y = 0; y < nh; ++y) {
        const float fy = std::clamp((static_cast<float>(y) + 0.5f) * static_cast<float>(h) / static_cast<float>(nh) - 0.5f, 0.0f, static_cast<float>(h - 1));
        const std::size_t y0 = static_cast<std::size_t>(std::floor(fy));
        const float ty = fy - std::floor(fy);
        const std::size_t y1 = std::min(y0 + 1, h - 1);
        for (std::size_t x = 0; x < nw; ++x) {
            const float fx = std::clamp((static_cast<float>(x) + 0.5f) * static_cast<float>(w) / static_cast<float>(nw) - 0.5f, 0.0f, static_cast<float>(w - 1));
            const std::size_t x0 = static_cast<std::size_t>(std::floor(fx));
            const float tx = fx - std::floor(fx);
            const std::size_t x1 = std::min(x0 + 1, w - 1);
            for (std::size_t c = 0; c < ch; ++c) {
                const auto p = [&](std::size_t xx, std::size_t yy) { return img[(yy * w + xx) * ch + c]; };
                const float top = p(x0, y0) + (p(x1, y0) - p(x0, y0)) * tx;
                const float bot = p(x0, y1) + (p(x1, y1) - p(x0, y1)) * tx;
                out[(y * nw + x) * ch + c] = top + (bot - top) * ty;
            }
        }
    }
    return out;
}

std::vector<float> skin_mask(std::size_t w, std::size_t h, std::size_t ch, const std::vector<float>& img) {
    std::vector<float> m(w * h, 0.0f);
    if (ch < 3) return m;
    for (std::size_t i = 0; i < m.size(); ++i) {
        const float r = img[i * ch] * 255.0f, g = img[i * ch + 1] * 255.0f, b = img[i * ch + 2] * 255.0f;
        const float mx = std::max({r, g, b}), mn = std::min({r, g, b});
        if (r > 95.0f && g > 40.0f && b > 20.0f && mx - mn > 15.0f && std::fabs(r - g) > 15.0f && r > g && r > b) m[i] = 1.0f;
    }
    return m;
}

}  // namespace genko::render::seam
