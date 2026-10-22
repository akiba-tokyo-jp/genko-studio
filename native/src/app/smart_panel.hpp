#pragma once

class QWidget;

namespace genko::core {
struct Layer;
struct Page;
}  // namespace genko::core

// smart-objects@1 (PC3 A0, design section A 5): the 属性 panel's part for a smart object (app/main_window_smart.cpp) —
// what its contents are (a file: its name, format, depth, colours, size and resolution where it lands; a page: its layers),
// コンテンツを編集 (PC3 A1), its size, turn and place (smart_transform), its stack mode (PC3 A4) and, a file's, how its
// colours come onto the page (smart_profile).

namespace genko::app {

class MainWindow;

QWidget* smart_properties(MainWindow* window, const core::Page& page, const core::Layer& layer);

}  // namespace genko::app
