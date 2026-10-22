#pragma once

#include <QWidget>

#include "core/model.hpp"

// PC3 B2 (masks@1, SPEC.md COLOR-06「マスク」): the 属性 panel's part for a layer's mask (Photoshop's Properties of a
// layer mask): 濃度 (%) and ぼかし (mm), each applied as set_layer_mask when it is let go, and リンク.

namespace genko::app {

class MainWindow;

QWidget* mask_properties(MainWindow* window, const core::Page& page, const core::Layer& layer);

}  // namespace genko::app
