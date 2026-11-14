// フキダシ (fukidashi@1, SPEC BALLOON-01; the user's 「今風の漫画の吹き出しを各種作って置けるように」): the palette of balloons
// in the window. Its dock (a tab beside 台詞, in ウィンドウ, kept with the layout as every dock), and what a pick does:
// the chosen line takes the preset's look — one edit_line, one step of 取り消し —, or the preset becomes the look of the
// next line the text tool places (type_new_line and balloon_drawn read it: main_window_lines.cpp), until another is
// picked or the text tool's own フキダシ is chosen.

#include <QCheckBox>
#include <QComboBox>
#include <QDockWidget>
#include <QListWidget>
#include <QMenu>

#include "app/balloon_palette.hpp"
#include "app/canvas.hpp"
#include "app/lettering.hpp"
#include "app/main_window.hpp"
#include "app/tool_settings.hpp"
#include "core/pyconv.hpp"
#include "core/pyops.hpp"
#include "render/text/lettering.hpp"

namespace genko::app {

using core::Json;

void MainWindow::build_balloon_actions() {
    make("act_balloon_palette", QStringLiteral("フキダシのパレットを開く"), [this] { show_dock(QStringLiteral("フキダシ")); }, {},
         QStringLiteral("今風のフキダシの形を選んで、選んだ台詞に当てるか、次に置く台詞の形にします"));
}

void MainWindow::build_balloon_dock() {
    balloon_palette_ = new BalloonPalette(this);
    balloon_dock_ = new QDockWidget(QStringLiteral("フキダシ"), this);
    balloon_dock_->setObjectName(QStringLiteral("フキダシ"));
    balloon_dock_->setWidget(balloon_palette_);
    balloon_dock_->setMinimumWidth(200);
    // (for occasional work: its tab has a close button and it joins the row of tabs when opened)
    balloon_dock_->setFeatures(QDockWidget::DockWidgetMovable | QDockWidget::DockWidgetFloatable | QDockWidget::DockWidgetClosable);
    addDockWidget(Qt::RightDockWidgetArea, balloon_dock_);
    tabifyDockWidget(lines_dock_, balloon_dock_);  // (beside the lines it shapes)
    balloon_dock_->hide();  // (ウィンドウ → フキダシ, ページ → 台詞 → フキダシのパレットを開く, or the 写植 stage brings it)
    view_menu_->addAction(balloon_dock_->toggleViewAction());
    connect(balloon_palette_, &BalloonPalette::picked, this, [this](const QString& key, const QString& how) { use_balloon_preset(key, how); });
    // (the text tool's own フキダシ chosen: that kind, plain — the palette's look is let go)
    connect(text_settings_->balloon, &QComboBox::activated, this, [this](int) { arm_balloon_preset(QString()); });
    connect(text_settings_, &TextToolSettings::presetCleared, this, [this] { arm_balloon_preset(QString()); });
}

const BalloonPreset* MainWindow::armed_balloon_preset() const { return balloon_preset(armed_balloon_); }

void MainWindow::arm_balloon_preset(const QString& key) {
    const BalloonPreset* preset = balloon_preset(key);
    armed_balloon_ = preset != nullptr ? key : QString();
    if (balloon_palette_ != nullptr) balloon_palette_->mark(armed_balloon_);
    // (said with the text tool's settings too: the palette may be closed)
    text_settings_->show_preset(preset != nullptr ? QString::fromUtf8(preset->name) : QString());
    if (preset == nullptr) return;
    // the text tool in hand, its settings saying the kind: the next click on the page places a line of this look
    text_settings_->make->setCurrentIndex(std::max(0, text_settings_->make->findData(QStringLiteral("line"))));
    text_settings_->balloon->setCurrentIndex(std::max(0, text_settings_->balloon->findData(QString::fromLatin1(preset->kind))));
    if (text_settings_->draw_balloon->isChecked()) text_settings_->draw_balloon->setChecked(false);  // (its own shape, not one drawn by hand)
    if (!PageCanvas::is_text_tool(canvas_->tool()) || (preset->across && canvas_->tool() != QLatin1String("text_h"))) {
        choose_tool(preset->across ? QStringLiteral("text_h") : QStringLiteral("text"));
    }
    flash(QStringLiteral("フキダシ「%1」を選びました。置きたい所をクリックして、台詞を打ちます").arg(QString::fromUtf8(preset->name)), 6000);
}

bool MainWindow::restyle_with_balloon_preset(const QString& key) {
    const BalloonPreset* preset = balloon_preset(key);
    const core::StoryLine* line = canvas_->selected_line_id ? line_by_id(*canvas_->selected_line_id) : nullptr;
    if (preset == nullptr) return false;
    if (line == nullptr) {
        flash(QStringLiteral("先に選択ツールで、形を変える台詞（フキダシ）をクリックして選びます"), 4000);
        return false;
    }
    if (!render::text::shape_of(*line).empty()) {  // (a point or box text of a type layer's kind: it has no balloon)
        flash(QStringLiteral("この文字はフキダシに入っていません。フキダシの形は、台詞に当てます"), 5000);
        return false;
    }
    const bool drawn = line->path.has_value() && !line->path->empty();
    const std::string id = line->id;
    // (to or from a capsule or a polygon: the box fitted to the words about its middle, in the same step)
    const auto fitted = [this, preset](const core::StoryLine& l) {
        Json after = l.style.is_object() ? l.style : Json::object();
        const Json change = preset_style_change(*preset, l);
        for (const auto& [name, value] : change.items()) {
            if (value.is_null()) {
                after.erase(name);
            } else {
                after[name] = value;
            }
        }
        Json box = lettering::refit_for_kind(l, frame_by_id(l.frame_id), preset->kind, after);
        // (fukidashi@2: drips that come or go with the look — the box as much taller or shorter, its top kept: the body,
        // the box less the drips, stays the size it was; a balloon drawn by hand has none)
        const std::string was = l.balloon.empty() ? std::string("speech") : l.balloon;
        const double grown = lettering::drip_height(preset->kind, after) - lettering::drip_height(was, l.style);
        if (std::abs(grown) > 1e-9 && !(l.path && !l.path->empty())) {
            if (!box.is_object()) {
                box = Json{{"x_mm", l.x_mm.json()}, {"y_mm", l.y_mm.json()}, {"w_mm", l.w_mm.truthy() ? l.w_mm.json() : Json(40.0)},
                           {"h_mm", l.h_mm.truthy() ? l.h_mm.json() : Json(20.0)}};
                box["h_mm"] = core::py_round(core::to_float(box["h_mm"]) + grown, 2);
            }
            // (a box refitted to the words is the body's own: the drips are box_size's already)
        }
        return box;
    };
    // (the lines joined with it: one balloon, drawn with its first line's inside and line — each takes the look)
    std::vector<JoinedLine> joined;
    if (const Json group = core::get_or(line->style, "group", Json()); core::py_truthy(group)) {
        for (const core::StoryLine* other : book().story_for_page(line->page_index)) {
            if (other->id != id && render::text::shape_of(*other).empty() && core::py_equals(core::get_or(other->style, "group", Json()), group)) {
                joined.push_back(JoinedLine{other, fitted(*other)});
            }
        }
    }
    const Json ops = preset_restyle_ops(*preset, *line, fitted(*line), joined);
    const QString name = QString::fromUtf8(preset->name);
    if (ops.empty()) {  // (that look already: no edit, no step of 取り消し)
        flash(QStringLiteral("選んだ台詞のフキダシは、もう「%1」です").arg(name), 4000);
        return false;
    }
    const bool many = !joined.empty();
    if (!apply_ops(ops)) return false;
    canvas_->selected_line_id = id;
    on_line_selected(id, false);
    const bool drips = preset->style.contains("drip_mm");
    flash(drawn && drips ? QStringLiteral("手で描いたフキダシには、しずくは付きません（線と塗りだけ当てました）")
          : drawn        ? QStringLiteral("手で描いたフキダシは形をそのままに、線と塗りを「%1」にしました（元に戻す で取り消せます）").arg(name)
          : many ? QStringLiteral("つないだフキダシをまとめて「%1」にしました（元に戻す で取り消せます）").arg(name)
                 : QStringLiteral("選んだ台詞のフキダシを「%1」にしました（元に戻す で取り消せます）").arg(name),
          4000);
    return true;
}

void MainWindow::use_balloon_preset(const QString& key, const QString& how) {
    if (balloon_preset(key) == nullptr) return;
    const core::StoryLine* line = canvas_->selected_line_id ? line_by_id(*canvas_->selected_line_id) : nullptr;
    // (a line of this page chosen, and the text tool not in hand: that line takes the look; else the next line placed)
    const core::Page* page = current_page();
    const bool here = line != nullptr && page != nullptr && line->page_index == page->index;
    const bool to_line = how == QLatin1String("line") || (how.isEmpty() && here && !PageCanvas::is_text_tool(canvas_->tool()));
    if (to_line) {
        restyle_with_balloon_preset(key);
        if (balloon_palette_ != nullptr) balloon_palette_->mark(armed_balloon_);  // (the mark stays on the look the next line takes)
        return;
    }
    arm_balloon_preset(key);
}

}  // namespace genko::app
