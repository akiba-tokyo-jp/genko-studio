#pragma once

#include <QKeySequence>
#include <QList>
#include <QString>

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "core/json.hpp"
#include "core/model.hpp"

// PC4 節 D (SPEC.md COLOR-08「フィルター」, design 5.1): the フィルター menu's table — its submenus and commands in
// Photoshop's order and words (the user's decision 1) — and the windows of PC4's groups' own (D3 フィルターギャラリー, D4
// ぼかしギャラリー・照明効果・炎・額縁・木, D5 Camera Raw フィルター, D6 レンズ補正・広角補正・消点). A command whose kind is
// not in this build yet (core/filter_kinds.hpp and render/fx.hpp) stays grey: each group lights its own by adding its
// kinds (and its window here), the menu and its words as they are. MainWindow builds it (app/filter_menu.cpp).

namespace genko::app {

class MainWindow;

// One command of the menu.
struct FilterItem {
    QString name;   // the command's attribute ("act_filter_<kind>"; empty: a separator)
    QString title;  // Photoshop's words, "…" when it opens a window
    std::string kind;  // filter_raster's kind (render_picture's for 炎・額縁・木; Python's for 漫画's five)
    std::optional<core::Json> fixed;  // a preset's settings: sent without a window
    std::string op = "filter_raster";  // or render_picture, vanishing_point
    bool python = false;  // 漫画: Python's kind, its old window (filter_params)
    QList<QKeySequence> keys;
};
// The menu's submenus in order (an empty key: the menu itself, before them; "manga", "plugins": Genko's own).
struct FilterSubmenu {
    QString key;    // (its object name: menu_filter_<key>)
    QString title;
    std::vector<FilterItem> items;
};
const std::vector<FilterSubmenu>& filter_menu_table();
const FilterItem* filter_item(const QString& name);

// A group's own window for its commands: given where it starts from, it calls `send` with the settings chosen (the op is
// made and sent as the menu's: on a smart object a smart filter, inside the selection, remembered as 前回のフィルター).
struct FilterWindowCall {
    MainWindow* window = nullptr;
    const FilterItem* item = nullptr;
    core::Json now = core::Json::object();
    std::function<void(const core::Json& params)> send;
    // (D3) the layer it works on as the book has it now, and — a smart object's smart filter set again from the layer
    // panel — that filter's index (the window shows what the filters below it make; -1: a filter added)
    const core::Page* page = nullptr;
    const core::Layer* layer = nullptr;
    int filter = -1;
    // (D5) The seed the command gives a kind with one — a new one each time it is applied (D1's review: 前回のフィルター too)
    // —, which `send` puts in the op when the settings have none: a window's preview of the grain uses it (none: no seed).
    std::optional<std::uint32_t> seed;
};
using FilterWindow = std::function<void(const FilterWindowCall&)>;
// By the item's kind (D3–D6 add theirs).
const std::vector<std::pair<std::string, FilterWindow>>& filter_windows();
const FilterWindow* filter_window(std::string_view kind);

// Can the command be used in this build: its kind in the table with its formula (a preset's too), a group's window, or
// Python's?
bool filter_available(const FilterItem& item);

}  // namespace genko::app
