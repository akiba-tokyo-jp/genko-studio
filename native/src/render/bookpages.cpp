#include "render/bookpages.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <string>
#include <utility>
#include <vector>

#include "core/ink.hpp"
#include "core/ops_util.hpp"
#include "core/paths.hpp"
#include "core/pyconv.hpp"
#include "render/channels.hpp"
#include "render/color_edit.hpp"
#include "render/op_limits.hpp"
#include "render/page.hpp"
#include "render/page_pixels.hpp"
#include "render/png.hpp"
#include "render/raster.hpp"
#include "render/selection.hpp"
#include "render/ink_edit.hpp"
#include "render/smart_filters.hpp"
#include "storage/reader.hpp"

namespace genko::render {

namespace {

constexpr int kMaskDpi = 150;  // ops.MASK_DPI

// pagespec._raster's work before it draws: the new paper at raster.WORKING_DPI and the map of its pixels onto the old
// picture's (taken for one at 200 dpi), each refused as Python refuses it — a paper of no finite size (Python's words), too
// large to hold, a basic frame of no width (ZeroDivisionError) or of no finite size.
struct WorkingMap {
    int width = 0;
    int height = 0;
    std::array<double, 6> coeffs{};
};

WorkingMap working_map(const core::PageSpec& spec, const core::Rect& old_frame, const core::Rect& new_frame) {
    const double k = raster::kWorkingDpi / 25.4;
    WorkingMap out;
    out.width = mm_to_px(spec.width_mm.value(), raster::kWorkingDpi);
    out.height = mm_to_px(spec.height_mm.value(), raster::kWorkingDpi);
    limits::check_picture(out.width, out.height, "the paint layer on the new paper");
    // sx = new_frame.width / old_frame.width (Python's ZeroDivisionError for a frame of no width)
    const auto ratio = [](const core::Num& a, const core::Num& b) {
        if (!b.truthy()) {
            throw core::PyUncaught("ZeroDivisionError", a.is_int() && b.is_int() ? "division by zero" : "float division by zero");
        }
        return (a / b).value();
    };
    const double sx = ratio(new_frame.width, old_frame.width);
    const double sy = ratio(new_frame.height, old_frame.height);
    const auto inverse = [](double v) {
        if (v == 0.0) throw core::PyUncaught("ZeroDivisionError", "float division by zero");
        return 1 / v;
    };
    // output pixel (u, v) takes input ((u/k - nx)/sx + ox) * k, … (PIL maps output → input)
    out.coeffs = {inverse(sx), 0, (old_frame.x.value() - new_frame.x.value() / sx) * k,
                  0, inverse(sy), (old_frame.y.value() - new_frame.y.value() / sy) * k};
    // (a basic frame of no finite size — margins or a trim of NaN on a finite paper — makes Pillow sample nowhere and
    // write the picture as it comes: refused here, as the op refuses such a paper)
    for (const double c : out.coeffs) {
        if (!std::isfinite(c)) throw core::OpError("the basic frame must be a finite size");
    }
    return out;
}

}  // namespace

std::string relayout_raster(const std::string& png, const core::PageSpec& old_paper, const core::PageSpec& spec, const core::Rect& old_frame,
                            const core::Rect& new_frame) {
    // (its size from its header; one that cannot be read goes the working way, which says why as Python does)
    std::optional<Size> size;
    try {
        size = picture_size(png);
    } catch (const core::Error&) {
    }
    if (!size || (size->width == mm_to_px(old_paper.width_mm.value(), raster::kWorkingDpi) &&
                  size->height == mm_to_px(old_paper.height_mm.value(), raster::kWorkingDpi))) {
        return relayout_working_raster(png, spec, old_frame, new_frame);
    }
    (void)working_map(spec, old_frame, new_frame);  // (a paper or a frame refused as before, whatever the picture)
    return page_pixels::move_png(png, page_pixels::frame_how(), page_pixels::frame_map(old_frame, new_frame), old_paper, spec);
}

std::string relayout_working_raster(const std::string& png, const core::PageSpec& spec, const core::Rect& old_frame,
                                    const core::Rect& new_frame) {
    const Image image = selection::open_picture(png).convert("RGBA");
    const WorkingMap map = working_map(spec, old_frame, new_frame);
    const Image out = image.transform(Size{map.width, map.height}, TransformMethod::Affine, map.coeffs, Resample::Bilinear);
    return write_png(out);
}

std::optional<std::string> relayout_mask(const std::string& png, const core::PageSpec& old_paper, const core::PageSpec& paper,
                                         const core::Rect& old_frame, const core::Rect& new_frame) {
    // (a mask that is no picture that can be read — not a picture, cut short, too large to open — is left exactly as it
    // is, as Python leaves every mask: the op goes on)
    std::optional<Image> opened;
    try {
        opened = selection::open_picture(png).convert("L");
    } catch (const core::Error&) {
        return std::nullopt;
    }
    const Image& mask = *opened;
    // (one value everywhere — a mask showing, or hiding, all of its layer — is the same moved)
    const auto [low, high] = mask.getextrema().front();
    if (low == high) return std::nullopt;
    const bool same_frame = old_frame.x == new_frame.x && old_frame.y == new_frame.y && old_frame.width == new_frame.width &&
                            old_frame.height == new_frame.height;
    if (same_frame && old_paper.width_mm == paper.width_mm && old_paper.height_mm == paper.height_mm) return std::nullopt;
    // the new mask over the new paper at the mask's resolution: on the grid of whole dots per inch it lies on over the old
    // paper — ops.MASK_DPI's 150, as ops._mask_image makes a page's, or a precise or ink page's finer one (PC3 B2) — the
    // same grid over the new paper; else as many pixels a mm as it has
    const double old_w = old_paper.width_mm.value();
    const double old_h = old_paper.height_mm.value();
    const double new_w = paper.width_mm.value();
    const double new_h = paper.height_mm.value();
    const auto on_grid = [](double mm, double dpi) { return std::max(1.0, core::py_round_whole(mm / 25.4 * dpi)); };
    const auto grid_of = [&](double dpi) { return on_grid(old_w, dpi) == mask.width() && on_grid(old_h, dpi) == mask.height(); };
    const double near_dpi = std::round(mask.width() * 25.4 / old_w);
    const double dpi = grid_of(kMaskDpi) ? static_cast<double>(kMaskDpi) : (near_dpi >= 1 && grid_of(near_dpi) ? near_dpi : 0.0);
    const double width = dpi > 0 ? on_grid(new_w, dpi) : std::max(1.0, std::round(new_w * mask.width() / old_w));
    const double height = dpi > 0 ? on_grid(new_h, dpi) : std::max(1.0, std::round(new_h * mask.height() / old_h));
    limits::check_sides(width, height, "the layer mask on the new paper");
    const Size size{static_cast<int>(width), static_cast<int>(height)};
    // as pagespec._Map moves a point: the frame's scale in x and y (1 for an old frame of no size)
    const double sx = old_frame.width.truthy() ? (new_frame.width / old_frame.width).value() : 1.0;
    const double sy = old_frame.height.truthy() ? (new_frame.height / old_frame.height).value() : 1.0;
    // each pixel of the new mask (its middle, in mm on the new paper) takes the old mask's value at the place of the old
    // page that moved there, bilinear; the old mask's edge carried on past its paper
    const auto sampling = [](int out, double out_mm, int in, double in_mm, double old_at, double new_at, double scale) {
        std::vector<std::pair<int, double>> taps;  // (the lower of the two pixels, the weight of the upper)
        taps.reserve(static_cast<std::size_t>(out));
        for (int u = 0; u < out; ++u) {
            const double mm = (u + 0.5) * out_mm / out;
            const double at = (old_at + (mm - new_at) / scale) * in / in_mm - 0.5;
            if (!std::isfinite(at)) throw core::OpError("the basic frame must be a finite size");
            const double floor = std::floor(at);
            int lower = 0;
            double t = 0.0;
            if (floor < 0) {
                lower = 0;
            } else if (floor >= in - 1) {
                lower = in - 1;
            } else {
                lower = static_cast<int>(floor);
                t = at - floor;
            }
            taps.emplace_back(lower, t);
        }
        return taps;
    };
    const auto xs = sampling(size.width, new_w, mask.width(), old_w, old_frame.x.value(), new_frame.x.value(), sx);
    const auto ys = sampling(size.height, new_h, mask.height(), old_h, old_frame.y.value(), new_frame.y.value(), sy);
    const std::string in = mask.tobytes();
    const auto value = [&](int x, int y) {
        return static_cast<double>(static_cast<unsigned char>(in[static_cast<std::size_t>(y) * static_cast<std::size_t>(mask.width()) +
                                                                static_cast<std::size_t>(x)]));
    };
    std::string out(static_cast<std::size_t>(size.width) * static_cast<std::size_t>(size.height), '\0');
    for (int v = 0; v < size.height; ++v) {
        const auto [y0, ty] = ys[static_cast<std::size_t>(v)];
        const int y1 = std::min(y0 + 1, mask.height() - 1);
        for (int u = 0; u < size.width; ++u) {
            const auto [x0, tx] = xs[static_cast<std::size_t>(u)];
            const int x1 = std::min(x0 + 1, mask.width() - 1);
            const double top = value(x0, y0) * (1 - tx) + value(x1, y0) * tx;
            const double bottom = value(x0, y1) * (1 - tx) + value(x1, y1) * tx;
            const double shown = top * (1 - ty) + bottom * ty;
            out[static_cast<std::size_t>(v) * static_cast<std::size_t>(size.width) + static_cast<std::size_t>(u)] =
                static_cast<char>(static_cast<unsigned char>(std::clamp(std::floor(shown + 0.5), 0.0, 255.0)));
        }
    }
    return write_png(Image::frombytes("L", size, out));
}

void relayout_filter_mask(core::Document& doc, core::Layer& layer, const core::PageSpec& old_paper, const core::PageSpec& paper,
                          const core::Rect& old_frame, const core::Rect& new_frame) {
    if (layer.kind != core::LayerKind::Smart || !layer.smart || !layer.smart->is_object()) return;
    const auto mask = layer.smart->find("filter_mask");
    if (mask == layer.smart->end() || !mask->is_object() || !mask->contains("asset") || !(*mask)["asset"].is_string()) return;
    // (one that is not in the book stays as it is: the book says so where it is read)
    const core::Bytes bytes = smart_filters::mask_bytes(doc, (*mask)["asset"].get<std::string>());
    if (!bytes) return;
    std::optional<std::string> moved = relayout_mask(*bytes, old_paper, paper, old_frame, new_frame);
    if (!moved) return;
    const std::string ref = core::bytes_ref(*moved);
    doc.smart_files[ref] = std::make_shared<const std::string>(std::move(*moved));
    (*mask)["asset"] = ref;
}

core::Document load_other_book(const std::string& from) {
    storage::LoadResult loaded;
    try {
        loaded = storage::load_document(core::path_from_utf8(from));
    } catch (const core::Error& error) {
        // (Python's OSError, ValueError and KeyError: the folder or its project.json cannot be read, is not JSON, is of
        // a newer version, or is not a book)
        throw core::OpError(std::string("the other book cannot be read (") + error.what() + ")");
    }
    if (!loaded.document.read_only_reason.empty()) {
        throw core::OpError("the other book cannot be read (" + loaded.document.read_only_reason + ")");
    }
    return std::move(loaded.document);
}

void register_bookpage_ops(core::OpRegistry& registry) {
    core::BookPageHooks hooks;
    hooks.relayout_raster = relayout_raster;
    hooks.relayout_mask = relayout_mask;
    hooks.relayout_inks = ink_edit::relayout_inks;
    hooks.relayout_color = color_edit::relayout;  // (PC3 C-e: a precise layer's pixels)
    hooks.relayout_channel = channels::relayout;
    hooks.relayout_filter_mask = relayout_filter_mask;  // (PC3 A2: a smart object's filter mask)
    hooks.load_book = load_other_book;
    core::register_bookpage_ops(registry, std::move(hooks));
}

}  // namespace genko::render
