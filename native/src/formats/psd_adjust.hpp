#pragma once

#include <optional>
#include <string>
#include <vector>

#include "core/model.hpp"
#include "render/image.hpp"

// PC2 (補正・色管理, the user's decision 5: a PSD's adjustment layers are always written): a correction layer as a
// Photoshop adjustment layer — the common part of its layer record (its name, opacity, blend, clip, mask) and the
// adjustment block its kind's codec writes (render/psd_adjust.hpp, each group's). The PSD writer (formats/psd_export)
// lays it in its place among the layers (render::page_layers' adjust_layers). A correction no codec writes yet is not
// written (none): its change is in the merged picture only, as in Python's PSD — a book without correction layers, or
// with none a codec writes, keeps Python's bytes.

namespace genko::formats {

struct PsdAdjustLayer {
    std::string name;
    double opacity = 1.0;
    std::string blend = "normal";  // (its native.blend: a correction layer's own blend is not looked at)
    bool clip = false;
    std::optional<render::Image> mask;  // ("L" over the page at the PSD's size, white shows)
    std::string blocks;                 // its tagged blocks (8BIM, key, length, payload padded to 4 bytes)
};

// The correction layer `layer` of `page` as an adjustment layer of a PSD of `size` pixels; none when no codec writes
// its kind (`notes`: what a codec says of it, e.g. a Python form held by the nearest PSD adjustment). `doc`: the book
// (a lookup table's asset).
std::optional<PsdAdjustLayer> psd_adjust_layer(const core::Layer& layer, const core::Page& page, render::Size size,
                                               std::vector<std::string>& notes, const core::Document* doc = nullptr);

// What a PSD of these pages says of their correction layers (the codecs' words, each once): the export's notes.
std::vector<std::string> psd_adjust_notes(const core::Document& doc);

}  // namespace genko::formats
