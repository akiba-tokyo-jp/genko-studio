#pragma once

#include <QString>
#include <QWidget>

#include <functional>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "app/adjust_forms.hpp"
#include "app/layer_dialogs.hpp"
#include "core/json.hpp"

// PC4 節 D (SPEC.md COLOR-08「フィルター」, design 5.2): the windows of フィルター's kinds — each kind's fields made from its
// table (core/filter_kinds.hpp: numbers, whole numbers, checks, choices and colours with their ranges and defaults, the
// words here), 既定に戻す, the result on the page while the settings change (250 ms after the last change; a heavy kind's
// worked out on a thread of its own at the screen's resolution, the window saying so while it is: design 4.4), かける /
// やめる. Each group of PC4 adds its kinds' words and its own fields (a 5×5 grid, a curve, a map's file…) in its function
// below; a kind with no form here has its table's fields with the setting's key as its word.

namespace genko::app::filter_forms {

struct FilterForm {
    std::string_view kind;
    std::map<std::string, QString> labels;  // (a setting's word; one not here is not a field — a group's own)
    std::map<std::string, std::vector<std::pair<QString, core::Json>>> choices;  // (a choice's values' words)
    std::map<std::string, int> decimals;  // (a number's places; none: by its range)
    // A group's own fields after the table's (lists, a grid, a curve, a picture to click…).
    std::function<void(adjust_forms::Form& form, const adjust_forms::FormContext& context)> more;
    QString note;  // (under the fields)
};
std::vector<FilterForm> blur_forms();          // D0: ぼかし（ガウス）・ぼかし（ボックス）; D1: the rest of ぼかし
std::vector<FilterForm> sharpen_forms();       // D1: app/filter_forms_basic.cpp
std::vector<FilterForm> noise_forms();         // D1: app/filter_forms_basic.cpp
std::vector<FilterForm> other_forms();         // D1: app/filter_forms_basic.cpp (カスタム…'s 5 × 5 grid)
std::vector<FilterForm> video_forms();         // D1: app/filter_forms_basic.cpp
std::vector<FilterForm> distort_forms();       // D2
std::vector<FilterForm> pixelate_forms();      // D2
std::vector<FilterForm> stylize_forms();       // D2
std::vector<FilterForm> render_forms();        // D4
std::vector<FilterForm> blur_gallery_forms();  // D4
const std::vector<FilterForm>& filter_forms();
const FilterForm* filter_form(std::string_view kind);

// A kind's window's title and its command's name: Photoshop's (core::FilterInfo::name); the kind itself for one not in
// the table.
QString title(std::string_view kind);

// What the window shows of the result: the page as the settings would leave it (none: as it is), and — a heavy kind's,
// worked out on a thread — whether one is being worked out now (the window says プレビューを作っています…).
struct Preview {
    FilterPreview show;
    std::function<bool()> busy;
};

// The window of a kind (`now`: the settings it starts from, its defaults where it has none; colours by the context's main
// and sub colours): the settings, or none when the person stopped (or the kind is not in the table). `opened`: the
// settings as the form showed them when it opened.
std::optional<core::Json> dialog(QWidget* parent, std::string_view kind, const core::Json& now, const adjust_forms::FormContext& context,
                                 const Preview& preview = {}, core::Json* opened = nullptr);

}  // namespace genko::app::filter_forms
