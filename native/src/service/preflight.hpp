#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include "core/json.hpp"
#include "core/model.hpp"
#include "render/image.hpp"
#include "storage/asset_store.hpp"

// What stops the final export, and watermarked proofs the agent may export (Python's genko/studio/preflight.py). Every
// refusal has a reason and a place: low resolution is refused unless forced, test images (origin.kind "fixture")
// unless allowed; missing provenance only warns.

namespace genko::service::preflight {

inline constexpr int kMinDpi = 350;  // grey art; line art that is thresholded wants 600 (M6)
inline constexpr int kProofDpi = 150;

// preflight.effective_dpi(width_px, width_mm)
double effective_dpi(double width_px, double width_mm);
// preflight.art_layers(page): the placed layers that print (exportable and visible).
std::vector<const core::Layer*> art_layers(const core::Page& page);
// preflight.layer_dpi(episode, page, layer, store): the placed picture's dpi on the page (its width in pixels over its
// placement's width), round(…, 1); nothing without an asset, a placement or the picture's file. Image.open's errors
// for a file that is no picture (PyUncaught UnidentifiedImageError, DecompressionBombError).
std::optional<double> layer_dpi(const core::Page& page, const core::Layer& layer, const storage::AssetStore& store);
// preflight._image_size(store, ref): Image.open(assets/…).size of the asset's file, nothing when there is none.
std::optional<std::pair<std::int64_t, std::int64_t>> image_size(const storage::AssetStore& store, const std::string& ref);

// preflight.watermark(image, text): the picture in RGB with `text` written over it in grey, in rows turned across the
// page (the bundled Dela Gothic, a size of a ninth of the width).
render::Image watermark(const render::Image& image, const std::string& text);
// preflight.export_proof(episode, project, fmt, dpi): every page at proof resolution (at most kProofDpi) with a 校正
// watermark, into studio/proofs/r<revision>/ (proof.pdf, or p001.png …); the files written. PyValueError "format は pdf
// か png" for another format.
std::vector<std::filesystem::path> export_proof(const core::Document& episode, const std::filesystem::path& project, const std::string& fmt = "pdf",
                                                int dpi = kProofDpi);

}  // namespace genko::service::preflight
