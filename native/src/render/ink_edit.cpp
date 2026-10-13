#include "render/ink_edit.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <array>
#include <memory>
#include <unordered_map>
#include <utility>

#include "core/color_raster.hpp"
#include "core/strokes.hpp"
#include "render/image.hpp"
#include "render/imaging.hpp"
#include "render/ink.hpp"
#include "render/ink_canvas.hpp"
#include "render/page.hpp"
#include "render/png.hpp"
#include "render/raster.hpp"

namespace genko::render::ink_edit {

namespace {

using core::InkPixels;
using core::PageColor;

using Shown = std::unordered_map<std::uint32_t, std::vector<std::uint16_t>>;

std::uint32_t key(const unsigned char* p) { return (std::uint32_t(p[0]) << 16) | (std::uint32_t(p[1]) << 8) | p[2]; }

std::uint16_t alpha_sample(unsigned char a, int depth) { return depth == 16 ? static_cast<std::uint16_t>(a * 257) : a; }

// The page's colours as it showed them: each colour's first inks (its colour samples) — the inks elements are drawn
// with first (its table's, Genko's black, the greys, no ink), then its layers' pixels.
Shown shown_colours(const PageColor& color, const std::vector<Lending::Loan>& loans) {
    Shown out;
    {
        std::vector<std::vector<double>> inks;
        for (const core::InkEntry& entry : color.inks) inks.push_back(entry.ink);
        for (int v = 0; v <= 255; ++v) {  // (the greys by the rules: Genko's blacks among them, white the paper)
            const unsigned char grey[3] = {static_cast<unsigned char>(v), static_cast<unsigned char>(v), static_cast<unsigned char>(v)};
            std::vector<double> ink(core::colour_channels(color.space));
            ink::inks_of(color, grey, nullptr, ink.data(), 1);
            inks.push_back(std::move(ink));
        }
        InkPixels row = core::blank_ink_pixels(color.space, color.depth, static_cast<std::uint32_t>(inks.size()), 1);
        const unsigned n = core::colour_channels(color.space), all = n + 1;
        for (std::size_t i = 0; i < inks.size(); ++i) {
            for (unsigned c = 0; c < n; ++c) row.samples[i * all + c] = ink::encode_sample(color.space, c, inks[i][c], color.depth);
            row.samples[i * all + n] = static_cast<std::uint16_t>(row.max());
        }
        const std::string shown = ink::to_rgba8(row, color).tobytes();
        for (std::size_t i = 0; i < inks.size(); ++i) {
            out.try_emplace(key(reinterpret_cast<const unsigned char*>(shown.data()) + i * 4), row.samples.begin() + std::ptrdiff_t(i * all),
                            row.samples.begin() + std::ptrdiff_t(i * all + n));
        }
    }
    for (const Lending::Loan& loan : loans) {
        const InkPixels& p = loan.pixels;
        const unsigned n = core::colour_channels(p.space), all = n + 1;
        const auto* shown = reinterpret_cast<const unsigned char*>(loan.shown.data());
        for (std::size_t i = 0; i < p.pixel_count(); ++i) {
            if (p.samples[i * all + n] == 0) continue;
            out.try_emplace(key(shown + i * 4), p.samples.begin() + std::ptrdiff_t(i * all), p.samples.begin() + std::ptrdiff_t(i * all + n));
        }
    }
    return out;
}

// The inks of a colour the page showed: that colour's, or the nearest within 2 a channel (what straight alpha's
// rounding leaves of it where only the alpha changed), or none.
const std::vector<std::uint16_t>* nearest_shown(const Shown& shown, const unsigned char* p) {
    if (shown.empty()) return nullptr;
    if (const auto it = shown.find(key(p)); it != shown.end()) return &it->second;
    const std::vector<std::uint16_t>* best = nullptr;
    int far = 7;  // (more than 3 × 2)
    for (int dr = -2; dr <= 2; ++dr) {
        for (int dg = -2; dg <= 2; ++dg) {
            for (int db = -2; db <= 2; ++db) {
                const int r = p[0] + dr, g = p[1] + dg, b = p[2] + db;
                const int d = std::abs(dr) + std::abs(dg) + std::abs(db);
                if (r < 0 || g < 0 || b < 0 || r > 255 || g > 255 || b > 255 || d >= far) continue;
                const unsigned char c[3] = {static_cast<unsigned char>(r), static_cast<unsigned char>(g), static_cast<unsigned char>(b)};
                if (const auto it = shown.find(key(c)); it != shown.end()) {
                    best = &it->second;
                    far = d;
                }
            }
        }
    }
    return best;
}

// An op's picture (RGBA) back in inks: what it kept as it was lent keeps its inks (see the header).
InkPixels into_inks(const Image& picture, const PageColor& color, const Lending::Loan* loan, const Shown& shown) {
    const auto w = static_cast<std::uint32_t>(picture.width()), h = static_cast<std::uint32_t>(picture.height());
    InkPixels out = core::blank_ink_pixels(color.space, color.depth, w, h);
    const unsigned n = core::colour_channels(color.space), all = n + 1;
    const bool same = loan != nullptr && loan->pixels.width == w && loan->pixels.height == h;
    const auto* lent = same ? reinterpret_cast<const unsigned char*>(loan->shown.data()) : nullptr;
    std::vector<unsigned char> rgb;
    std::vector<std::size_t> where;
    for (std::uint32_t y = 0; y < h; ++y) {
        const auto* row = reinterpret_cast<const unsigned char*>(picture.raw()->image[y]);
        for (std::uint32_t x = 0; x < w; ++x) {
            const unsigned char* p = row + std::size_t(x) * 4;
            if (p[3] == 0) continue;  // (transparent: no ink)
            const std::size_t i = std::size_t(y) * w + x;
            std::uint16_t* dst = out.samples.data() + i * all;
            if (lent != nullptr) {
                const unsigned char* was = lent + i * 4;
                if (was[3] != 0 && was[0] == p[0] && was[1] == p[1] && was[2] == p[2]) {  // (its colour as it was)
                    const std::uint16_t* inks = loan->pixels.samples.data() + i * all;
                    std::copy_n(inks, n, dst);
                    dst[n] = was[3] == p[3] ? inks[n] : alpha_sample(p[3], color.depth);
                    continue;
                }
            }
            dst[n] = alpha_sample(p[3], color.depth);
            // (an element's colour the rules keep — Genko's blacks, white, the table's — is that element's; else a
            // colour shown, then a grey by the rules, then one near a colour shown)
            const bool grey = p[0] == p[1] && p[1] == p[2];
            const bool kept = (grey && (p[0] == 0 || p[0] == 10 || p[0] == 15 || p[0] == 20 || p[0] == 255)) ||
                              core::ink_for(color, {p[0], p[1], p[2]}) != nullptr;
            const std::vector<std::uint16_t>* inks = nullptr;
            if (!kept) {
                if (const auto it = shown.find(key(p)); it != shown.end()) inks = &it->second;
                else if (!grey) inks = nearest_shown(shown, p);
            }
            if (inks != nullptr) {
                std::copy(inks->begin(), inks->end(), dst);
                continue;
            }
            rgb.insert(rgb.end(), p, p + 3);
            where.push_back(i);
        }
    }
    if (!where.empty()) {  // (a new colour: the page's rules)
        std::vector<double> inks(where.size() * n);
        ink::inks_of(color, rgb.data(), nullptr, inks.data(), where.size());
        for (std::size_t k = 0; k < where.size(); ++k) {
            std::uint16_t* dst = out.samples.data() + where[k] * all;
            for (unsigned c = 0; c < n; ++c) dst[c] = ink::encode_sample(color.space, c, inks[k * n + c], color.depth);
        }
    }
    return out;
}

}  // namespace

std::optional<std::vector<double>> inks_at(const core::Page& page, const core::Document* episode, double x_mm, double y_mm, int dpi) {
    if (!page.color || !std::isfinite(x_mm) || !std::isfinite(y_mm)) return std::nullopt;
    const int x = static_cast<int>(std::floor(x_mm / 25.4 * dpi)), y = static_cast<int>(std::floor(y_mm / 25.4 * dpi));
    if (x < 0 || y < 0 || x >= mm_to_px(page.spec.width_mm.value(), dpi) || y >= mm_to_px(page.spec.height_mm.value(), dpi)) return std::nullopt;
    RenderOptions options = proof_options();
    options.inks = true;
    options.skip_unported = true;
    options.region = RenderRegion{x, y, 1, 1};
    const RenderResult made = render_page(page, dpi, options, episode);
    if (!made.inks || made.inks->pixel_count() == 0) return std::nullopt;
    const unsigned n = core::colour_channels(made.inks->space);
    std::vector<double> out(n);
    for (unsigned c = 0; c < n; ++c) out[c] = ink::decode_sample(made.inks->space, c, made.inks->samples[c], made.inks->depth);
    return out;
}

std::optional<std::vector<double>> layer_inks_at(const core::Page& page, const core::Layer& layer, double x_mm, double y_mm) {
    if (!page.color || !layer.ink_raster || !std::isfinite(x_mm) || !std::isfinite(y_mm)) return std::nullopt;
    const double w_mm = page.spec.width_mm.value(), h_mm = page.spec.height_mm.value();
    if (!(w_mm > 0) || !(h_mm > 0) || x_mm < 0 || y_mm < 0 || x_mm >= w_mm || y_mm >= h_mm) return std::nullopt;
    const std::shared_ptr<const InkPixels> pixels = decoded_ink(layer.ink_raster);
    if (!pixels || pixels->pixel_count() == 0) return std::nullopt;
    // (the raster lies stretched over the page)
    const auto x = std::min<std::uint32_t>(pixels->width - 1, static_cast<std::uint32_t>(x_mm / w_mm * pixels->width));
    const auto y = std::min<std::uint32_t>(pixels->height - 1, static_cast<std::uint32_t>(y_mm / h_mm * pixels->height));
    const unsigned n = core::colour_channels(pixels->space), all = n + 1;
    const std::uint16_t* p = pixels->samples.data() + (std::size_t(y) * pixels->width + x) * all;
    if (p[n] * 255.0 <= 20.0 * pixels->max()) return std::nullopt;
    std::vector<double> out(n);
    for (unsigned c = 0; c < n; ++c) out[c] = ink::decode_sample(pixels->space, c, p[c], pixels->depth);
    return out;
}

bool lends(std::string_view op) {
    static constexpr std::array<std::string_view, 22> kLent{
        "transform_area", "delete_area", "paste", "erase_raster", "erase", "smudge", "liquify", "merge_down",
        "merge_layers", "merge_visible", "convert_layer", "flood_fill", "gradient_fill", "add_shape", "fill_gaps",
        "stamp_material", "lt_convert", "put_color_raster", "render_prims", "trace_prims", "ruler_to_layer", "set_page_spec"};
    return std::find(kLent.begin(), kLent.end(), op) != kLent.end();
}

Lending::Lending(core::Document& doc, const std::vector<std::size_t>& pages) {
    for (const std::size_t p : pages) {
        if (p >= doc.pages.size() || doc.is_deferred(p) || !doc.page(p).color) continue;
        Page lent;
        lent.id = doc.page(p).id;
        lent.color = *doc.page(p).color;
        const auto& layers = doc.page(p).layers;
        for (const core::Layer& layer : layers) lent.patches.emplace_back(layer.id, layer.patches.size());
        if (std::any_of(layers.begin(), layers.end(), [](const core::Layer& l) { return bool(l.ink_raster); })) {
            core::Page& page = doc.edit_page(p);
            for (core::Layer& layer : page.layers) {
                if (!layer.ink_raster) continue;
                Loan loan;
                loan.layer = layer.id;
                loan.ink_raster = layer.ink_raster;
                loan.pixels = core::decode_ink_raster(*layer.ink_raster);
                const Image shown = ink::to_rgba8(loan.pixels, lent.color);
                loan.shown = shown.tobytes();
                loan.lent = std::make_shared<const std::string>(write_png(shown, 1));  // (lent for one op: written fast)
                layer.raster_png = loan.lent;
                layer.raster_memo = {};
                layer.raster_relpath.reset();
                layer.ink_raster = nullptr;
                layer.ink_memo = {};
                lent.loans.push_back(std::move(loan));
            }
        }
        pages_.push_back(std::move(lent));
    }
}

void Lending::take_back(core::Document& doc) const {
    for (const Page& lent : pages_) {
        std::size_t at = doc.pages.size();
        for (std::size_t i = 0; i < doc.pages.size(); ++i) {
            if (!doc.is_deferred(i) && doc.page(i).id == lent.id) at = i;
        }
        if (at == doc.pages.size() || !doc.page(at).color) continue;
        const auto had_patches = [&](const core::Layer& l) {
            for (const auto& [id, count] : lent.patches) {
                if (id == l.id) return count;
            }
            return std::size_t{0};
        };
        // (a paint layer's fills and pictures the op left: drawn into its pixels)
        const auto new_patches = [&](const core::Layer& l) { return l.kind == core::LayerKind::Raster && l.patches.size() > had_patches(l); };
        const auto& layers = doc.page(at).layers;
        if (std::none_of(layers.begin(), layers.end(), [&](const core::Layer& l) {
                return (l.raster_png && !l.raster_png->empty()) || l.color_raster || new_patches(l);
            })) {
            continue;
        }
        const Shown shown = shown_colours(lent.color, lent.loans);
        core::Page& page = doc.edit_page(at);
        const PageColor& color = *page.color;
        for (core::Layer& layer : page.layers) {
            if (layer.color_raster) {  // (precise RGB pixels: through the profile)
                const core::ColorRasterView view(*layer.color_raster);
                std::vector<float> rgba(std::size_t(view.width()) * view.height() * 4);
                for (std::size_t i = 0; i < std::size_t(view.width()) * view.height(); ++i) {
                    const auto px = view.pixel(i);
                    for (unsigned c = 0; c < 4; ++c) rgba[i * 4 + c] = static_cast<float>(px[c]);
                }
                layer.ink_raster = std::make_shared<const std::string>(core::encode_ink_raster(ink::from_srgb(rgba, view.width(), view.height(), color)));
                layer.ink_memo = {};
                layer.color_raster = nullptr;
                continue;
            }
            if (new_patches(layer)) {
                const std::size_t had = had_patches(layer);
                core::Layer drawn = layer;
                drawn.strokes = core::empty_strokes();
                drawn.patches.assign(layer.patches.begin() + std::ptrdiff_t(had), layer.patches.end());
                if (!drawn.raster_png || drawn.raster_png->empty()) {
                    if (layer.ink_raster) {  // (not lent: shown as it is)
                        drawn.raster_png = std::make_shared<const std::string>(write_png(ink::to_rgba8(core::decode_ink_raster(*layer.ink_raster), color), 1));
                    }
                }
                raster::bake_vectors(page, drawn);
                layer.raster_png = drawn.raster_png;
                layer.patches.resize(had);
            }
            if (!layer.raster_png || layer.raster_png->empty()) continue;
            const auto loan = std::find_if(lent.loans.begin(), lent.loans.end(), [&](const Loan& l) { return l.layer == layer.id; });
            const Loan* own = loan != lent.loans.end() ? &*loan : nullptr;
            if (own != nullptr && (layer.raster_png == own->lent || *layer.raster_png == *own->lent)) {
                layer.ink_raster = own->ink_raster;  // (as it was lent: its own bytes)
            } else {
                const Image picture = open_image(*layer.raster_png, kPillowOpenLimits).convert("RGBA");
                layer.ink_raster = std::make_shared<const std::string>(core::encode_ink_raster(into_inks(picture, color, own, shown)));
            }
            layer.ink_memo = {};
            layer.raster_png = nullptr;
            layer.raster_memo = {};
            layer.raster_relpath.reset();
        }
    }
}

}  // namespace genko::render::ink_edit
