#include "render/bookpages.hpp"

#include <array>
#include <cmath>

#include "core/ops_util.hpp"
#include "core/paths.hpp"
#include "core/pyconv.hpp"
#include "render/op_limits.hpp"
#include "render/page.hpp"
#include "render/png.hpp"
#include "render/raster.hpp"
#include "render/selection.hpp"
#include "storage/reader.hpp"

namespace genko::render {

std::string relayout_raster(const std::string& png, const core::PageSpec& spec, const core::Rect& old_frame,
                            const core::Rect& new_frame) {
    const Image image = selection::open_picture(png).convert("RGBA");
    const double k = raster::kWorkingDpi / 25.4;
    const int width = mm_to_px(spec.width_mm.value(), raster::kWorkingDpi);
    const int height = mm_to_px(spec.height_mm.value(), raster::kWorkingDpi);
    limits::check_picture(width, height, "the paint layer on the new paper");
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
    const std::array<double, 6> coeffs{inverse(sx), 0, (old_frame.x.value() - new_frame.x.value() / sx) * k,
                                       0, inverse(sy), (old_frame.y.value() - new_frame.y.value() / sy) * k};
    // (a basic frame of no finite size — margins or a trim of NaN on a finite paper — makes Pillow sample nowhere and
    // write the picture as it comes: refused here, as the op refuses such a paper)
    for (const double c : coeffs) {
        if (!std::isfinite(c)) throw core::OpError("the basic frame must be a finite size");
    }
    const Image out = image.transform(Size{width, height}, TransformMethod::Affine, coeffs, Resample::Bilinear);
    return write_png(out);
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
    hooks.load_book = load_other_book;
    core::register_bookpage_ops(registry, std::move(hooks));
}

}  // namespace genko::render
