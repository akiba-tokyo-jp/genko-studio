// The window's clipboard (編集 → カット・コピー・ペースト): what is chosen lifted from the layer drawn on, kept in the
// window, and put back as a new layer where it was. 編集's items of the clipboard and the selection's contents are built
// here (B2 adds 結合部分をコピー, 特殊ペースト, 塗りつぶし…, 境界線を描く… and コンテンツに応じた塗りつぶし… to this list).

#include <QMenu>

#include "app/canvas.hpp"
#include "app/main_window.hpp"
#include "service/wording.hpp"
#include "core/ids.hpp"
#include "render/color_edit.hpp"
#include "render/ink_edit.hpp"
#include "render/raster_ops.hpp"
#include "render/selection.hpp"

namespace genko::app {

using core::Json;

void MainWindow::build_clipboard_menu(QMenu* edit) {
    for (const char* name : {"act_cut", "act_copy", "act_paste", "act_delete_area", "act_fill_selection"}) edit->addAction(action(QString::fromLatin1(name)));
    edit->addSeparator();
    build_transform_menu(edit->addMenu(QStringLiteral("変形")));  // (main_window_select.cpp)
}

bool MainWindow::copy() {
    const auto area = need_area();
    const core::Layer* layer = target_layer();
    const core::Page* page = current_page();
    if (!area || layer == nullptr || page == nullptr) return false;
    Json items;
    try {
        render::use_brushes_of(book());
        core::Layer copy = *layer;  // (lifted from a copy: the layer itself keeps everything)
        auto lifted = render::selection::lift(copy, *area, *page);
        if (page->color && layer->ink_raster) {  // (color-mode@1: its inks there, as a piece that keeps them)
            if (const auto piece = render::ink_edit::lift_piece(*page, *layer, *area)) {
                lifted.patches.push_back(render::ink_edit::piece_patch(*piece, *page->color));
            }
        }
        if (lifted.empty()) {  // (pen-shapes@1: shape parts too)
            flash(QStringLiteral("選んだ範囲に、このレイヤーの絵がありません"), 3000);
            return false;
        }
        items = render::selection::items_to_json(lifted);
    } catch (const std::exception& error) {
        flash(wording::error(QString::fromUtf8(error.what())), 3000, true);
        return false;
    }
    clipboard_ = std::move(items);
    clipboard_outline_ = canvas_->selection()->outline;
    clipboard_area_ = area;
    flash(QStringLiteral("コピーしました（Ctrl+V で新しいレイヤーにペースト）"), 2500);
    return true;
}

void MainWindow::cut() {
    if (copy()) delete_area();
}

void MainWindow::paste() {
    const core::Page* page = current_page();
    const core::Layer* layer = target_layer();
    if (page == nullptr || !clipboard_) {
        flash(QStringLiteral("ペーストするものがありません（先にコピー）"), 2500);
        return;
    }
    const std::string id = core::new_id();
    // (color-mode@1: pieces of inks go into a paint layer, which keeps them as they were; color-model@1: so do the
    // values of an RGB page of a profile)
    bool inks = false;
    if (clipboard_->contains("patches") && (*clipboard_)["patches"].is_array()) {
        for (const Json& patch : (*clipboard_)["patches"]) {
            inks = inks || (patch.is_object() && ((page->color && patch.contains("gkir")) || patch.contains(render::color_edit::kPatchProfileKey)));
        }
    }
    Json add{{"op", "add_layer"}, {"page", page->index.json()}, {"name", "貼り付け"}, {"kind", inks ? "paint" : "pen"}, {"id", id}};
    if (layer != nullptr) add["after"] = layer->id;
    const Json ops = Json::array({add, Json{{"op", "paste"}, {"page", page->index.json()}, {"layer_id", id}, {"items", *clipboard_}}});
    if (apply_ops(ops)) {
        set_target_layer(id);
        if (canvas_->tool() != QLatin1String("marquee")) choose_tool(QStringLiteral("rect"));
        canvas_->set_selection(clipboard_area_, clipboard_outline_);
        flash(QStringLiteral("新しいレイヤー「貼り付け」に置きました。選択範囲の中をドラッグで動かせます"), 3500);
    }
}

}  // namespace genko::app
