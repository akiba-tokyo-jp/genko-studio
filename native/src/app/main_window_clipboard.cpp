// The window's clipboard (編集 → カット・コピー・ペースト): what is chosen lifted from the layer drawn on (nothing chosen: all
// of it, as Photoshop copies a whole layer), kept in the window and put on the system's clipboard — Genko's own items and
// a picture as the screen shows them —, and put back as a new layer where it was. A picture another program put on the
// clipboard is pasted as a new layer at the view's centre. PC3 B2 adds 結合部分をコピー, 特殊ペースト (同じ位置にペースト・
// 選択範囲内へペースト・選択範囲外へペースト), 選択範囲をコピー／カットしたレイヤー, 塗りつぶし… and 境界線を描く…
// (app/fill_dialogs.cpp); B4 adds コンテンツに応じた塗りつぶし… to this list. Precise pixels and inks are copied as pieces
// that keep their samples (render/color_edit, render/ink_edit).

#include <QBuffer>
#include <QClipboard>
#include <QGuiApplication>
#include <QImage>
#include <QMenu>
#include <QMimeData>

#include <algorithm>
#include <cmath>
#include <cstring>

#include "app/canvas.hpp"
#include "app/layer_panel.hpp"
#include "app/main_window.hpp"
#include "service/wording.hpp"
#include "core/base64.hpp"
#include "core/color_raster.hpp"
#include "core/command_bus.hpp"
#include "core/ids.hpp"
#include "core/pynum.hpp"
#include "render/color_edit.hpp"
#include "render/fill.hpp"
#include "render/image_ops.hpp"
#include "render/ink_edit.hpp"
#include "render/page.hpp"
#include "render/raster_ops.hpp"
#include "render/selection.hpp"

namespace genko::app {

using core::Json;

namespace {

// Genko's items on the system clipboard (paste's "items" as JSON), beside the picture other programs take.
const QString kGenkoItems = QStringLiteral("application/x-genko-items+json");
constexpr int kPictureDpi = 200;  // (a picture on the clipboard: the working 200 dpi, a pixel for a paint layer's pixel)

// A whole layer as an area (Photoshop's copy and cut without a selection): the page and a little past it.
Json whole_page(const core::Page& page) {
    const double m = 30, w = page.spec.width_mm.value(), h = page.spec.height_mm.value();
    return Json{{"poly", Json::array({Json::array({-m, -m}), Json::array({w + m, -m}), Json::array({w + m, h + m}), Json::array({-m, h + m})})}};
}

// The items as the screen shows them (8-bit sRGB), cut to what they cover, at kPictureDpi: what another program gets.
std::optional<QImage> items_picture(const core::Page& page, const core::Document& doc, const Json& items) {
    try {
        render::selection::Items lifted = render::selection::items_from_json(items);
        for (core::Patch& patch : lifted.patches) render::color_edit::patch_to_srgb(patch, doc);
        core::Layer shown;
        shown.id = "clipboard";
        shown.role = core::LayerRole::User;
        shown.kind = core::LayerKind::Raster;
        shown.panel_clip = false;
        render::selection::drop(shown, std::move(lifted), render::selection::kIdentity, true);
        core::Page bare = page;  // (the items alone: no paper, frames or lines of the page's)
        bare.numero = false;
        const render::Image drawn = render::layer_image(bare, shown, kPictureDpi, &doc, true).convert("RGBA");
        const auto box = drawn.getbbox();
        if (!box) return std::nullopt;
        const render::Image piece = drawn.crop(*box);
        const std::string bytes = piece.tobytes();
        QImage picture = QImage(reinterpret_cast<const uchar*>(bytes.data()), piece.width(), piece.height(), piece.width() * 4, QImage::Format_RGBA8888).copy();
        picture.setDotsPerMeterX(static_cast<int>(std::lround(kPictureDpi / 0.0254)));
        picture.setDotsPerMeterY(static_cast<int>(std::lround(kPictureDpi / 0.0254)));
        return picture;
    } catch (const std::exception&) {
        return std::nullopt;
    }
}

// A picture another program put on the clipboard as paste's items: one picture of its pixels at kPictureDpi (fitted
// into the page when it is larger), its centre at `centre` (mm). On a page of more than 8 bits, a picture of 16 bits a
// sample also keeps its samples (a piece of precise pixels, render/color_edit).
std::optional<Json> picture_items(const core::Page& page, const QImage& image, QPointF centre) {
    if (image.isNull() || image.width() <= 0 || image.height() <= 0) return std::nullopt;
    double w = image.width() * 25.4 / kPictureDpi, h = image.height() * 25.4 / kPictureDpi;
    const double pw = page.spec.width_mm.value(), ph = page.spec.height_mm.value();
    if (w > pw || h > ph) {
        const double k = std::min(pw / w, ph / h);
        w *= k;
        h *= k;
    }
    const QImage rgba = image.convertToFormat(QImage::Format_RGBA8888);
    std::string bytes(std::size_t(rgba.width()) * rgba.height() * 4, '\0');
    for (int y = 0; y < rgba.height(); ++y) std::memcpy(bytes.data() + std::size_t(y) * rgba.width() * 4, rgba.constScanLine(y), std::size_t(rgba.width()) * 4);
    const render::Image picture = render::Image::frombytes("RGBA", render::Size{rgba.width(), rgba.height()}, bytes);
    Json patch{{"mode", "image"},
               {"opacity", 1.0},
               {"box", Json::array({core::py_round(centre.x() - w / 2, 3), core::py_round(centre.y() - h / 2, 3), core::py_round(w, 3), core::py_round(h, 3)})},
               {"png", core::b64encode(render::fills::png_data(picture))}};
    const bool deep = image.depth() > 32 && !page.color && page.rgb && page.rgb->depth > 8;
    if (deep) {
        const QImage wide = image.convertToFormat(QImage::Format_RGBA64);
        const auto width = static_cast<std::uint32_t>(wide.width());
        const std::string samples = core::encode_color_pixels(width, static_cast<std::uint32_t>(wide.height()), page.rgb->depth == 32 ? "f32" : "u16",
                                                              [&](std::size_t i) {
                                                                  const QRgba64 p = reinterpret_cast<const QRgba64*>(wide.constScanLine(static_cast<int>(i / width)))[i % width];
                                                                  return std::array<double, 4>{p.red() / 65535.0, p.green() / 65535.0, p.blue() / 65535.0, p.alpha() / 65535.0};
                                                              });
        patch[std::string(render::color_edit::kPieceKey)] = core::b64encode(samples);
        patch[std::string(render::color_edit::kPieceSpaceKey)] = "srgb";
    }
    return Json{{"strokes", Json::array()}, {"patches", Json::array({patch})}};
}

// Does a paste of these items make a paint layer (pieces of inks or precise pixels, the values of a page of a profile,
// a picture) rather than a pen layer?
bool paints(const core::Page& page, const Json& items) {
    if (!items.contains("patches") || !items["patches"].is_array()) return false;
    for (const Json& patch : items["patches"]) {
        if (!patch.is_object()) continue;
        if ((page.color && patch.contains("gkir")) || patch.contains(render::color_edit::kPatchProfileKey) ||
            patch.contains(std::string(render::color_edit::kPieceKey)))
            return true;
    }
    return false;
}

// What a copy lifts: the selection as the bus resolves it for delete_area — its anti-aliased edge and its feather grey (a
// poly of the selection tools carries "antialias" and "feather_mm" unresolved) —, so that what a cut lifts is what it
// clears; nothing chosen: the whole layer.
Json lifted_area(const std::optional<Json>& area, const core::Page& page, const core::Document& doc) {
    if (!area) return whole_page(page);
    if (core::area_needs_resolving(*area)) return render::selection::resolve(*area, page, &doc);
    return *area;
}

// The centre of an area's box (mm).
std::optional<QPointF> centre_of(const Json& area) {
    try {
        const std::vector<double> box = render::selection::area_bbox(area);
        if (box.size() != 4) return std::nullopt;
        return QPointF(box[0] + box[2] / 2, box[1] + box[3] / 2);
    } catch (const std::exception&) {
        return std::nullopt;
    }
}

}  // namespace

void MainWindow::build_clipboard_actions() {
    make("act_copy_merged", QStringLiteral("結合部分をコピー"), [this] { copy_merged(); }, {QKeySequence(QStringLiteral("Ctrl+Shift+C"))},
         QStringLiteral("見えているすべてのレイヤーを合わせた絵をコピーします（選択範囲があればその中だけ）"));
    // (同じ位置にペースト: Genko's ペースト puts the copy where it was already; Photoshop's Ctrl+Shift+V is コマを縦に割る's)
    make("act_paste_in_place", QStringLiteral("同じ位置にペースト"), [this] { paste_special(QStringLiteral("in_place")); }, {},
         QStringLiteral("コピーした所と同じ位置に、新しいレイヤーとしてペーストします"));
    make("act_paste_into", QStringLiteral("選択範囲内へペースト"), [this] { paste_special(QStringLiteral("into")); },
         {QKeySequence(QStringLiteral("Ctrl+Alt+Shift+V"))}, QStringLiteral("選択範囲の中だけが見えるマスクを付けて、新しいレイヤーにペーストします"));
    make("act_paste_outside", QStringLiteral("選択範囲外へペースト"), [this] { paste_special(QStringLiteral("outside")); }, {},
         QStringLiteral("選択範囲の外だけが見えるマスクを付けて、新しいレイヤーにペーストします"));
    make("act_fill_dialog", QStringLiteral("塗りつぶし…"), [this] { fill_dialog(); }, {QKeySequence(QStringLiteral("Shift+F5"))},
         QStringLiteral("選択範囲（なければレイヤー全体）を、色・不透明度・透明部分の保持を決めて塗りつぶします"));
    make("act_stroke_dialog", QStringLiteral("境界線を描く…"), [this] { stroke_dialog(); }, {},
         QStringLiteral("選択範囲（なければレイヤーの描いてある所）の縁に、幅・色・位置を決めて線を描きます"));
    make("act_layer_via_copy", QStringLiteral("選択範囲をコピーしたレイヤー"), [this] { layer_via(false); }, {QKeySequence(QStringLiteral("Ctrl+J"))},
         QStringLiteral("選択範囲の中を新しいレイヤーに写します（選択範囲がなければレイヤーを複製）"));
    make("act_layer_via_cut", QStringLiteral("選択範囲をカットしたレイヤー"), [this] { layer_via(true); }, {QKeySequence(QStringLiteral("Ctrl+Shift+J"))},
         QStringLiteral("選択範囲の中を新しいレイヤーに移します"));
}

void MainWindow::build_clipboard_menu(QMenu* edit) {
    for (const char* name : {"act_cut", "act_copy", "act_copy_merged", "act_paste"}) edit->addAction(action(QString::fromLatin1(name)));
    QMenu* special = edit->addMenu(QStringLiteral("特殊ペースト"));
    for (const char* name : {"act_paste_in_place", "act_paste_into", "act_paste_outside"}) special->addAction(action(QString::fromLatin1(name)));
    for (const char* name : {"act_delete_area", "act_fill_dialog", "act_stroke_dialog", "act_fill_selection"}) edit->addAction(action(QString::fromLatin1(name)));
    edit->addAction(action(QStringLiteral("act_content_aware_fill")));  // (PC3 B4: retouch_tools.cpp)
    edit->addSeparator();
    build_transform_items(edit);  // (コンテンツに応じて拡大・縮小… パペットワープ 遠近法ワープ 自由変形: main_window_transform.cpp)
    build_transform_menu(edit->addMenu(QStringLiteral("変形")));  // (main_window_select.cpp; ゆがみ… is フィルター's: filter_menu.cpp)
}

std::optional<Json> MainWindow::copied_items(const std::optional<Json>& area, const core::Layer* of) {
    const core::Layer* layer = of != nullptr ? of : target_layer();
    const core::Page* page = current_page();
    if (layer == nullptr || page == nullptr) return std::nullopt;
    Json items;
    try {
        render::use_brushes_of(book());
        const Json where = lifted_area(area, *page, book());
        core::Layer copy = *layer;  // (lifted from a copy: the layer itself keeps everything)
        auto lifted = render::selection::lift(copy, where, *page);
        if (page->color && layer->ink_raster) {  // (color-mode@1: its inks there, as a piece that keeps them)
            if (const auto piece = render::ink_edit::lift_piece(*page, *layer, where)) {
                lifted.patches.push_back(render::ink_edit::piece_patch(*piece, *page->color));
            }
        }
        if (layer->color_raster) {  // (PC3 B2: its precise pixels there, as a piece that keeps their samples)
            if (const auto piece = render::color_edit::lift_piece(*page, *layer, where)) lifted.patches.push_back(render::color_edit::piece_patch(*piece));
        }
        if (lifted.empty()) {  // (pen-shapes@1: shape parts too)
            flash(area ? QStringLiteral("選んだ範囲に、このレイヤーの絵がありません") : QStringLiteral("このレイヤーには、まだ何も描いてありません"), 3000);
            return std::nullopt;
        }
        items = render::selection::items_to_json(lifted);
    } catch (const std::exception& error) {
        flash(wording::error(QString::fromUtf8(error.what())), 3000, true);
        return std::nullopt;
    }
    return items;
}

bool MainWindow::copy() {
    const auto area = selection_area();
    const core::Page* page = current_page();
    const auto items = copied_items(area);
    if (!items || page == nullptr) return false;
    clipboard_ = *items;
    clipboard_outline_ = area && canvas_->selection() ? canvas_->selection()->outline : std::vector<QPointF>{};
    clipboard_area_ = area;
    // (the system's clipboard: Genko's items, and the picture as it shows for other programs)
    auto* data = new QMimeData;
    clipboard_stamp_ = QByteArray::fromStdString(clipboard_->dump());
    data->setData(kGenkoItems, clipboard_stamp_);
    if (const auto picture = items_picture(*page, book(), *clipboard_)) data->setImageData(*picture);
    QGuiApplication::clipboard()->setMimeData(data);
    flash(QStringLiteral("コピーしました（Ctrl+V で新しいレイヤーにペースト）"), 2500);
    return true;
}

void MainWindow::cut() {
    const bool chosen = selection_area().has_value();
    if (!copy()) return;
    if (chosen) {
        delete_area();
        return;
    }
    // (nothing chosen: the whole layer's, as Photoshop cuts it)
    if (const core::Layer* layer = paint_layer())
        apply_ops(Json::array({Json{{"op", "delete_area"}, {"page", current_page()->index.json()}, {"layer_id", layer->id}, {"area", whole_page(*current_page())}}}));
}

void MainWindow::copy_merged() {
    const core::Page* page = current_page();
    if (page == nullptr) return;
    const auto area = selection_area();
    Json items;
    try {
        render::use_brushes_of(book());
        const Json where = lifted_area(area, *page, book());
        // (the page as it shows, in its own way: 8 bits, precise values or inks — a layer of it lifted as a piece)
        const render::image_ops::Pixels shown = render::image_ops::page_pixels(book(), *page);
        core::Layer merged;
        merged.id = "merged";
        merged.role = core::LayerRole::User;
        merged.kind = core::LayerKind::Raster;
        render::selection::Items lifted;
        switch (shown.kind()) {
            case render::image_ops::Pixels::Kind::Eight:
                merged.raster_png = render::fills::png_bytes(render::image_ops::to_image(shown));
                lifted = render::selection::lift(merged, where, *page);
                break;
            case render::image_ops::Pixels::Kind::Precise:
                merged.color_raster = std::make_shared<const std::string>(render::image_ops::to_precise(shown));
                if (const auto piece = render::color_edit::lift_piece(*page, merged, where)) lifted.patches.push_back(render::color_edit::piece_patch(*piece));
                break;
            case render::image_ops::Pixels::Kind::Inks:
                merged.ink_raster = std::make_shared<const std::string>(render::image_ops::to_inks(shown));
                if (const auto piece = render::ink_edit::lift_piece(*page, merged, where)) lifted.patches.push_back(render::ink_edit::piece_patch(*piece, *page->color));
                break;
        }
        if (lifted.patches.empty() && lifted.strokes.empty()) {
            flash(QStringLiteral("選んだ範囲に、見えている絵がありません"), 3000);
            return;
        }
        items = render::selection::items_to_json(lifted);
    } catch (const std::exception& error) {
        flash(wording::error(QString::fromUtf8(error.what())), 3000, true);
        return;
    }
    clipboard_ = items;
    clipboard_outline_ = area && canvas_->selection() ? canvas_->selection()->outline : std::vector<QPointF>{};
    clipboard_area_ = area;
    auto* data = new QMimeData;
    clipboard_stamp_ = QByteArray::fromStdString(clipboard_->dump());
    data->setData(kGenkoItems, clipboard_stamp_);
    if (const auto picture = items_picture(*page, book(), *clipboard_)) data->setImageData(*picture);
    QGuiApplication::clipboard()->setMimeData(data);
    flash(QStringLiteral("見えている絵をコピーしました（Ctrl+V で新しいレイヤーにペースト）"), 2500);
}

void MainWindow::paste() { paste_special(QString()); }

void MainWindow::paste_special(const QString& how) {
    const core::Page* page = current_page();
    const core::Layer* layer = target_layer();
    if (page == nullptr) return;
    const bool masked = how == QLatin1String("into") || how == QLatin1String("outside");
    const auto chosen = masked ? need_area() : std::optional<Json>{};
    if (masked && !chosen) return;
    // what to paste: Genko's items on the system's clipboard (this window's copy: where it was, too), else a picture
    // another program put there, else this window's own copy
    std::optional<Json> items;
    std::optional<Json> was;
    std::vector<QPointF> outline;
    bool picture = false;
    const QMimeData* data = QGuiApplication::clipboard()->mimeData();
    if (data != nullptr && data->hasFormat(kGenkoItems)) {
        const QByteArray bytes = data->data(kGenkoItems);
        if (clipboard_ && bytes == clipboard_stamp_) {
            items = clipboard_;
            was = clipboard_area_;
            outline = clipboard_outline_;
        } else {
            const Json parsed = Json::parse(bytes.toStdString(), nullptr, false);
            if (parsed.is_object() && (parsed.contains("strokes") || parsed.contains("patches"))) items = parsed;
        }
    } else if (data != nullptr && data->hasImage()) {
        const QPointF centre = canvas_->to_mm(QPointF(canvas_->width() / 2.0, canvas_->height() / 2.0));
        items = picture_items(*page, qvariant_cast<QImage>(data->imageData()), centre);
        picture = items.has_value();
    } else if (clipboard_) {
        items = clipboard_;
        was = clipboard_area_;
        outline = clipboard_outline_;
    }
    if (!items) {
        flash(QStringLiteral("ペーストするものがありません（先にコピー）"), 2500);
        return;
    }
    const std::string id = core::new_id();
    Json add{{"op", "add_layer"}, {"page", page->index.json()}, {"name", "貼り付け"}, {"kind", picture || paints(*page, *items) ? "paint" : "pen"}, {"id", id}};
    if (layer != nullptr) add["after"] = layer->id;
    Json put{{"op", "paste"}, {"page", page->index.json()}, {"layer_id", id}, {"items", *items}};
    Json ops = Json::array({add});
    if (masked) {
        // (選択範囲内へ・外へ: at the selection's centre, the selection its mask — fixed to the page, as Photoshop's —, and
        // the selection let go)
        if (const auto centre = centre_of(*chosen)) put["center_mm"] = Json::array({core::py_round(centre->x(), 3), core::py_round(centre->y(), 3)});
        ops.push_back(put);
        ops.push_back(Json{{"op", "set_layer_mask"}, {"page", page->index.json()}, {"id", id}, {"area", *chosen},
                           {"invert", how == QLatin1String("outside")}, {"linked", false}});
    } else {
        ops.push_back(put);
    }
    if (!apply_ops(ops)) return;
    set_target_layer(id);
    if (masked) {
        canvas_->set_selection(std::nullopt);
        flash(QStringLiteral("新しいレイヤー「貼り付け」に、%1を見せるマスクを付けて置きました").arg(how == QLatin1String("into") ? QStringLiteral("選択範囲の中") : QStringLiteral("選択範囲の外")), 3500);
        return;
    }
    if (canvas_->tool() != QLatin1String("marquee")) choose_tool(QStringLiteral("rect"));
    if (was) canvas_->set_selection(was, outline);
    else canvas_->set_selection(std::nullopt);
    flash(QStringLiteral("新しいレイヤー「貼り付け」に置きました。選択範囲の中をドラッグで動かせます"), 3500);
}

void MainWindow::layer_via(bool cut) {
    const core::Page* page = current_page();
    const core::Layer* layer = target_layer();
    if (page == nullptr || layer == nullptr) return;
    const auto area = selection_area();
    if (!area) {
        // (nothing chosen: 選択範囲をコピーしたレイヤー duplicates the layer, as Photoshop's Ctrl+J does)
        if (!cut) {
            if (layer_panel_ != nullptr) layer_panel_->duplicate();
            return;
        }
        need_area();
        return;
    }
    const auto items = copied_items(area);
    if (!items) return;
    const std::string id = core::new_id();
    const Json index = page->index.json();
    Json ops = Json::array({Json{{"op", "add_layer"}, {"page", index}, {"name", cut ? "カットしたレイヤー" : "コピーしたレイヤー"},
                                 {"kind", paints(*page, *items) ? "paint" : "pen"}, {"id", id}, {"after", layer->id}},
                            Json{{"op", "paste"}, {"page", index}, {"layer_id", id}, {"items", *items}}});
    if (cut) ops.push_back(Json{{"op", "delete_area"}, {"page", index}, {"layer_id", layer->id}, {"area", *area}});
    if (apply_ops(ops)) {
        set_target_layer(id);
        if (layer_panel_ != nullptr) layer_panel_->refresh();
    }
}

}  // namespace genko::app
