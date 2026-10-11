#include "service/preflight.hpp"

#include <algorithm>
#include <cmath>
#include <numbers>

#include "core/clock.hpp"
#include "formats/pillow_save.hpp"
#include "render/draw.hpp"
#include "render/page.hpp"
#include "render/text/fonts.hpp"
#include "service/service_util.hpp"

#include "core/error.hpp"
#include "core/pyops.hpp"
#include "core/paths.hpp"
#include "core/pyconv.hpp"
#include "core/pynum.hpp"
#include "render/not_yet_ported.hpp"
#include "render/png.hpp"

namespace genko::service::preflight {

double effective_dpi(double width_px, double width_mm) { return width_mm > 0 ? width_px / (width_mm / 25.4) : 0.0; }

std::vector<const core::Layer*> art_layers(const core::Page& page) {
    std::vector<const core::Layer*> out;
    for (const core::Layer& layer : page.layers) {
        if (layer.kind == core::LayerKind::Placed && layer.exportable && layer.visible) out.push_back(&layer);
    }
    return out;
}

std::optional<std::pair<std::int64_t, std::int64_t>> image_size(const storage::AssetStore& store, const std::string& ref) {
    if (!storage::AssetStore::is_ref(ref)) return std::nullopt;  // (no such file: this build reads only asset refs)
    const auto bytes = store.get_bytes(ref, ".png");
    if (!bytes) return std::nullopt;
    try {
        const render::Size size = render::picture_size(*bytes);
        return std::make_pair(std::int64_t{size.width}, std::int64_t{size.height});
    } catch (const render::NotYetPorted&) {
        throw;
    } catch (const core::Error& error) {
        if (error.code() == "unidentified_image") {
            throw core::PyUncaught("UnidentifiedImageError",
                                   "cannot identify image file " + core::py_repr_str(core::path_to_utf8(store.path(ref, ".png"))));
        }
        if (error.code() == "image_too_large") throw core::PyUncaught("DecompressionBombError", error.what());
        throw core::PyUncaught("OSError", error.what());
    }
}

std::optional<double> layer_dpi(const core::Page& page, const core::Layer& layer, const storage::AssetStore& store) {
    (void)page;
    if (!layer.asset || layer.asset->empty() || !layer.placement_mm) return std::nullopt;
    const auto size = image_size(store, *layer.asset);
    if (!size) return std::nullopt;
    return core::py_round(effective_dpi(static_cast<double>(size->first), layer.placement_mm->width.value()), 1);
}

render::Image watermark(const render::Image& image, const std::string& text) {
    render::Image base = image.convert("RGB");
    const int size = std::max(24, base.width() / 9);
    render::TrueTypeFonts fonts;
    const render::TrueTypeFont& font = fonts.truetype("sfx", size);  // (render._DELA, which always opens)
    render::Image layer = render::Image::create("L", base.size(), render::Ink(0));
    {
        render::Draw draw(layer);
        const std::u32string letters = render::text::u32(text);
        const int step_y = size * 4;
        for (int y = -base.height(); y < base.height() * 2; y += step_y) {
            draw.text(render::PointD{base.width() * 0.1, static_cast<double>(y)}, letters, font, render::Ink(70));
        }
    }
    // (math.degrees: radToDeg * x, radToDeg = 180.0 / pi)
    const double angle = (180.0 / std::numbers::pi) * std::atan2(static_cast<double>(base.height()), static_cast<double>(base.width())) - 90 + 60;
    const render::Image rotated = layer.rotate(angle, render::Resample::Bilinear);
    const render::Image grey = render::Image::create("RGB", base.size(), render::Ink{150, 150, 150});
    base.paste(grey, render::Point{0, 0}, &rotated);
    return base;
}

std::vector<std::filesystem::path> export_proof(const core::Document& episode, const std::filesystem::path& project, const std::string& fmt, int dpi) {
    if (fmt != "pdf" && fmt != "png") throw core::PyValueError("format は pdf か png");
    dpi = std::min(dpi, kProofDpi);
    std::string number = std::to_string(episode.revision);
    if (number.size() < 4 && episode.revision >= 0) number.insert(0, 4 - number.size(), '0');
    const std::filesystem::path out = project / "studio" / "proofs" / core::path_from_utf8("r" + number);
    std::filesystem::create_directories(out);
    const std::string label = "校正 PROOF r" + std::to_string(episode.revision);
    std::vector<render::Image> images;
    for (const auto& page : episode.pages) {
        render::RenderOptions options;
        options.mode = "print";
        images.push_back(watermark(render::render_page(*page, dpi, options, &episode).image, label));
    }
    if (fmt == "pdf") {
        if (images.empty()) core::raise_index_error();  // (images[0])
        const std::filesystem::path path = out / "proof.pdf";
        util::write_file(path, formats::pdf_bytes(images, dpi, "proof", core::utc_time_text("%Y%m%d%H%M%SZ")));
        return {path};
    }
    std::vector<std::filesystem::path> written;
    for (std::size_t i = 0; i < images.size(); ++i) {
        const std::filesystem::path path = out / core::path_from_utf8("p" + util::zero3(episode.pages[i]->index.json()) + ".png");
        util::write_file(path, formats::png_bytes(images[i]));
        written.push_back(path);
    }
    return written;
}

}  // namespace genko::service::preflight
