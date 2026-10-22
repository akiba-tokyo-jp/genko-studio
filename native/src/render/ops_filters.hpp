#pragma once

#include <optional>

#include "core/command_bus.hpp"
#include "render/ops_adjust.hpp"

// PC4 節 D (SPEC.md COLOR-08「フィルター」): フィルター's kinds as filter_raster's local filters (render/ops_filters.cpp,
// by their kernels: render/fx.hpp) and the frames of PC4's new ops.

namespace genko::render {

// render_picture (D4: 炎・額縁・木) and vanishing_point (D6: 消点), refused (not_yet_ported) until their groups write them.
void register_filter_ops(core::OpRegistry& registry);

// (D1) While it lives, the layer filter_raster works on this thread: what a kind reads of it besides its pixels
// (ぼかし（レンズ）'s depth from its layer mask, design 2.6). A smart filter's stand-in has none (no depth from a mask).
class FilterLayer {
public:
    explicit FilterLayer(const core::Layer& layer);
    ~FilterLayer();
    FilterLayer(const FilterLayer&) = delete;
    FilterLayer& operator=(const FilterLayer&) = delete;

private:
    const core::Layer* before_;
};

// filter_raster's report for a kind of フィルター's (design 3.1): {"filter": {"kind", …the settings it was drawn with — its
// normal form}}; none for any other kind. (D6 adds what it resolves: レンズ補正's profile, 広角補正's model.)
std::optional<core::Json> filter_report(const core::Document& doc, const core::Json& op);

// What filter_raster refuses of a kind of フィルター's that a smart filter keeps (OpError): D3's フィルターギャラリー with every
// effect hidden (it would change nothing; a smart filter may hold its effects so).
void refuse_filter(const core::Document& doc, const core::Json& op);

}  // namespace genko::render
