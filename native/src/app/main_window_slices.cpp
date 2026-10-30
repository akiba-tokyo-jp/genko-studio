// PC5 J-b (slices@1, SPEC.md PROD-01「スライス」): Photoshop's slices — the スライスツール and the スライス選択ツール (the
// 切り抜き group: app/paint_panels.cpp; their options: スタイル 標準・縦横比を固定・固定サイズ, ガイドからスライスを作成; ユーザー
// 定義スライスに変換, 分割…, 削除, 自動スライスを隠す, スライスオプション…), レイヤー → 新規レイヤーベースのスライス, 表示 → 表示・
// 非表示 → スライス (the extra "slices": app/canvas_slices.cpp), スナップ先 → スライス (main_window_align.cpp), スライスを
// ロック and スライスを消去 — each change of the book an op through apply_ops (render/ops_slices), as the person. スライスを
// ロック is this window's (not the book's, as Photoshop's): no slice gesture or option changes the slices while it is on.
// No key of their own (C is the 切り抜き group's, which Genko does not go round: the crop tool keeps it).

#include <QAction>
#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>

#include "app/ask.hpp"
#include "app/canvas.hpp"
#include "app/config.hpp"
#include "app/main_window.hpp"
#include "app/theme.hpp"
#include "app/tool_settings.hpp"
#include "render/ops_slices.hpp"

namespace genko::app {

using core::Json;

namespace {

double r4(double v) { return std::round(v * 10000.0) / 10000.0; }

Json rect_json(const QRectF& r) { return Json::array({r4(r.x()), r4(r.y()), r4(r.width()), r4(r.height())}); }

}  // namespace

void MainWindow::build_slice_actions() {
    // the スライスツール and the スライス選択ツール (Photoshop's, in the 切り抜き group)
    QAction* draw = make(QStringLiteral("act_slice_tool"), QStringLiteral("スライスツール"), [this] { choose_tool(QStringLiteral("slice")); }, {},
                         QStringLiteral("ドラッグで Web 用に保存のスライスを作ります（Shift で正方形）"), true);
    QAction* pick = make(QStringLiteral("act_slice_select_tool"), QStringLiteral("スライス選択ツール"),
                         [this] { choose_tool(QStringLiteral("slice_select")); }, {},
                         QStringLiteral("スライスを選んで、動かす・大きさを変える・スライスオプションを決めます"), true);
    for (const auto& [tool, act] : {std::pair{QStringLiteral("slice"), draw}, std::pair{QStringLiteral("slice_select"), pick}}) {
        tools_->addAction(act);
        act->setAutoRepeat(false);
        tool_actions_[tool] = act;
    }
    // 表示 → 表示・非表示 → スライス (view.show.slices): the extra "slices" on the canvas
    QAction* shown = make(QStringLiteral("act_show_slices"), QStringLiteral("スライス"), [] {}, {}, QStringLiteral("スライスの枠と番号を表示します"), true);
    shown->setChecked(settings()->value(QStringLiteral("view/show_slices"), true).toBool());
    connect(shown, &QAction::toggled, this, [this](bool on) {
        settings()->setValue(QStringLiteral("view/show_slices"), on);
        share_view_switch(QStringLiteral("act_show_slices"), on);  // (H-b's: every open window the same)
        canvas_->show_extra(QStringLiteral("slices"), on);
        canvas_->update();
    });
    // 表示 → スライスをロック (view.lockSlices: this window's), スライスを消去 (view.clearSlices)
    QAction* lock = make(QStringLiteral("act_lock_slices"), QStringLiteral("スライスをロック"), [] {}, {},
                         QStringLiteral("スライスを動かしたり変えたりできないようにします（この窓の間）"), true);
    connect(lock, &QAction::toggled, this, [this](bool on) {
        canvas_->lock_slices = on;
        canvas_->update();
    });
    make(QStringLiteral("act_clear_slices"), QStringLiteral("スライスを消去"),
         [this] {
             const core::Page* page = current_page();
             if (page == nullptr || !page->slices || page->slices->list.empty()) {
                 flash(QStringLiteral("このページにはスライスがありません"), 3000);
                 return;
             }
             if (!slices_unlocked()) return;
             if (slice_op(Json{{"op", "delete_slices"}, {"slices", "all"}})) flash(QStringLiteral("スライスを消去しました"), 3000);
         },
         {}, QStringLiteral("このページのユーザー定義スライスとレイヤーベースのスライスをすべて削除します（取り消せます）"));
    // レイヤー → 新規レイヤーベースのスライス (layer.newLayerBasedSlice)
    make(QStringLiteral("act_layer_slice"), QStringLiteral("新規レイヤーベースのスライス"),
         [this] {
             const core::Layer* layer = target_layer();
             if (layer == nullptr) {
                 flash(QStringLiteral("先にスライスにするレイヤーを選びます"), 3000);
                 return;
             }
             if (!slices_unlocked()) return;
             if (slice_op(Json{{"op", "add_slice"}, {"layer", layer->id}})) flash(QStringLiteral("レイヤーベースのスライスを作りました"), 3000);
         },
         {}, QStringLiteral("描く先のレイヤーの描かれた範囲（レイヤー効果を含む）に付いていくスライスを作ります"));
}

void MainWindow::build_slice_menus(QMenu* view, QMenu* layers) {
    // 表示 → 表示・非表示 ▸ スライス (節 H's show_menu, its place among show_rows()); スライスをロック・スライスを消去 after the
    // guides' commands (view_rows)
    show_menu(view)->addAction(action(QStringLiteral("act_show_slices")));
    view->addAction(action(QStringLiteral("act_lock_slices")));
    view->addAction(action(QStringLiteral("act_clear_slices")));
    // レイヤー → 新規レイヤーベースのスライス: after ラスタライズ ▸, as Photoshop's
    QAction* after = nullptr;
    const QList<QAction*> items = layers->actions();
    for (int i = 0; i < items.size(); ++i) {
        if (items[i]->menu() != nullptr && items[i]->menu()->objectName() == QLatin1String("menu_rasterize")) after = i + 1 < items.size() ? items[i + 1] : nullptr;
    }
    if (after != nullptr) {
        layers->insertAction(after, action(QStringLiteral("act_layer_slice")));
    } else {
        layers->addAction(action(QStringLiteral("act_layer_slice")));
    }
}

namespace {

PageCanvas::SliceView made_slice_view(const core::Document& doc, const core::Page& page) {
    PageCanvas::SliceView v;
    v.grid = render::slices::page_grid(page);
    try {
        v.slices = render::slices::resolve(doc, page, v.grid);
    } catch (const std::exception&) {
        v.slices.clear();
    }
    if (page.slices)
        for (const core::Slice& s : page.slices->list) v.order.push_back(s.id);
    return v;
}

}  // namespace

void MainWindow::slices_in_pane(PageCanvas* pane) {
    pane->slice_colour = canvas_->slice_colour;
    pane->show_slice_numbers = canvas_->show_slice_numbers;
    struct Made {
        DocPtr doc;  // (the book it was made of: its page never another's)
        const core::Page* page = nullptr;
        PageCanvas::SliceView view;
    };
    auto made = std::make_shared<Made>();
    pane->slice_view = [pane, made]() -> const PageCanvas::SliceView* {
        const DocPtr& doc = pane->doc();
        const core::Page* page = pane->page();
        if (!doc || page == nullptr) return nullptr;
        if (made->doc != doc || made->page != page) {
            made->view = made_slice_view(*doc, *page);
            made->doc = doc;
            made->page = page;
        }
        return &made->view;
    };
}

const PageCanvas::SliceView* MainWindow::slice_view() {
    const core::Page* page = current_page();
    if (page == nullptr) return nullptr;
    if (page != slice_view_page_ || !slice_view_made_) {
        PageCanvas::SliceView v = made_slice_view(book(), *page);
        slice_view_ = std::move(v);
        slice_view_page_ = page;
        slice_view_keep_ = book().pages.empty() ? nullptr : std::shared_ptr<const void>(session_->snapshot());
        slice_view_made_ = true;
    }
    return &slice_view_;
}

void MainWindow::refresh_slices() {
    slice_view_made_ = false;  // (made again when it is next looked at: the page changed or another is shown)
    const PageCanvas::SliceView* v = slice_view();
    if (canvas_->chosen_slice && (v == nullptr || std::none_of(v->slices.begin(), v->slices.end(), [&](const core::ResolvedSlice& r) {
                                      return r.number == *canvas_->chosen_slice;
                                  }))) {
        canvas_->chosen_slice.reset();
    }
    if (slice_chosen_label_ == nullptr) return;
    const auto chosen = canvas_->chosen_slice_now();
    if (!chosen) {
        slice_chosen_label_->setText(QStringLiteral("スライスを選んでいません"));
    } else {
        const QString origin = chosen->origin == "user" ? QStringLiteral("ユーザー定義") : chosen->origin == "layer" ? QStringLiteral("レイヤーベース") : QStringLiteral("自動");
        slice_chosen_label_->setText(QStringLiteral("%1（%2・%3 × %4 px）")
                                         .arg(QString::fromStdString(chosen->name), origin)
                                         .arg(chosen->box.width())
                                         .arg(chosen->box.height()));
    }
}

bool MainWindow::slices_unlocked() {
    if (!canvas_->lock_slices) return true;
    flash(QStringLiteral("スライスはロックされています（表示 → スライスをロック）"), 4000, true);
    return false;
}

bool MainWindow::slice_op(Json op) {
    const core::Page* page = current_page();
    if (page == nullptr) return false;
    op["page"] = page->index.json();
    const bool done = apply_ops(Json::array({std::move(op)}));
    refresh_slices();
    canvas_->update();
    return done;
}

std::optional<std::int64_t> MainWindow::chosen_slice_id(bool promote) {
    const auto chosen = canvas_->chosen_slice_now();
    if (!chosen) {
        flash(QStringLiteral("先にスライス選択ツールでスライスを選びます"), 3000);
        return std::nullopt;
    }
    if (chosen->id) return chosen->id;
    if (!promote || !slices_unlocked()) return std::nullopt;
    const int number = chosen->number;
    if (!slice_op(Json{{"op", "set_slice"}, {"number", number}, {"promote", true}})) return std::nullopt;
    // (the auto slice made a user one where it was: its number as shown now)
    const PageCanvas::SliceView* v = slice_view();
    if (v == nullptr) return std::nullopt;
    const auto box = chosen->box;
    for (const core::ResolvedSlice& r : v->slices) {
        if (r.id && r.box == box) {
            canvas_->chosen_slice = r.number;
            return r.id;
        }
    }
    return std::nullopt;
}

void MainWindow::build_slice_tools() {
    canvas_->slice_view = [this] { return slice_view(); };
    canvas_->add_extra({QStringLiteral("slices"), [this](QPainter& painter, const core::Page& page, const PageCanvas::ExtraView& view) {
                            canvas_->paint_slices(painter, page, view);
                        }});
    canvas_->show_extra(QStringLiteral("slices"), action(QStringLiteral("act_show_slices"))->isChecked());
    connect(canvas_, &PageCanvas::sliceDrawn, this, [this](QRectF mm) {
        if (slice_op(Json{{"op", "add_slice"}, {"rect_mm", rect_json(mm)}})) {
            const PageCanvas::SliceView* v = slice_view();
            const core::Page* page = current_page();
            if (v != nullptr && page != nullptr && page->slices && !page->slices->list.empty()) {
                const std::int64_t id = page->slices->list.back().id;
                for (const core::ResolvedSlice& r : v->slices)
                    if (r.id == id) canvas_->chosen_slice = r.number;
            }
            refresh_slices();
        }
    });
    connect(canvas_, &PageCanvas::sliceChosen, this, [this](int) { refresh_slices(); });
    connect(canvas_, &PageCanvas::sliceMoved, this, [this](int number, QRectF mm) {
        const PageCanvas::SliceView* v = slice_view();
        if (v == nullptr) return;
        const auto it = std::find_if(v->slices.begin(), v->slices.end(), [&](const core::ResolvedSlice& r) { return r.number == number; });
        if (it == v->slices.end()) return;
        // (an auto or a layer-based slice moved: made a user slice there — by its number, in the same step)
        Json op{{"op", "set_slice"}, {"rect_mm", rect_json(mm)}};
        if (it->id) {
            op["slice"] = *it->id;
        } else {
            op["number"] = number;
        }
        const std::optional<std::int64_t> id = it->id;
        if (slice_op(std::move(op))) {
            if (const PageCanvas::SliceView* now = slice_view()) {
                for (const core::ResolvedSlice& r : now->slices) {
                    const QRectF at = canvas_->slice_mm(r);
                    if ((id && r.id == id) || (!id && r.id && std::abs(at.x() - mm.x()) < 0.5 && std::abs(at.y() - mm.y()) < 0.5)) canvas_->chosen_slice = r.number;
                }
            }
            refresh_slices();
        }
    });
    connect(canvas_, &PageCanvas::sliceOptionsAsked, this, [this](int number) {
        canvas_->chosen_slice = number;
        refresh_slices();
        slice_options_dialog();
    });
    connect(canvas_, &PageCanvas::slicesLocked, this, [this] { (void)slices_unlocked(); });
    if (tool_settings_ == nullptr) return;
    // the スライスツール's options: スタイル (標準・縦横比を固定・固定サイズ, its width and height), ガイドからスライスを作成
    {
        auto* page = new QWidget;
        page->setObjectName(QStringLiteral("slice_settings"));
        auto* form = new QFormLayout(page);
        form->setContentsMargins(0, 0, 0, 0);
        auto* style = new QComboBox;
        style->setObjectName(QStringLiteral("slice_style"));
        style->addItem(QStringLiteral("標準"));
        style->addItem(QStringLiteral("縦横比を固定"));
        style->addItem(QStringLiteral("固定サイズ"));
        auto* w = new QDoubleSpinBox;
        w->setObjectName(QStringLiteral("slice_style_width"));
        auto* h = new QDoubleSpinBox;
        h->setObjectName(QStringLiteral("slice_style_height"));
        for (QDoubleSpinBox* box : {w, h}) {
            box->setRange(0.01, 100000);
            box->setDecimals(2);
            box->setValue(box == w ? settings()->value(QStringLiteral("slice/width"), 1.0).toDouble() : settings()->value(QStringLiteral("slice/height"), 1.0).toDouble());
        }
        style->setCurrentIndex(std::clamp(settings()->value(QStringLiteral("slice/style"), 0).toInt(), 0, 2));
        const auto put = [this, style, w, h] {
            canvas_->slice_style = style->currentIndex();
            canvas_->slice_style_w = w->value();
            canvas_->slice_style_h = h->value();
            w->setEnabled(style->currentIndex() != 0);
            h->setEnabled(style->currentIndex() != 0);
            w->setSuffix(style->currentIndex() == 2 ? QStringLiteral(" px") : QString());
            h->setSuffix(style->currentIndex() == 2 ? QStringLiteral(" px") : QString());
            settings()->setValue(QStringLiteral("slice/style"), style->currentIndex());
            settings()->setValue(QStringLiteral("slice/width"), w->value());
            settings()->setValue(QStringLiteral("slice/height"), h->value());
        };
        connect(style, &QComboBox::currentIndexChanged, this, put);
        connect(w, &QDoubleSpinBox::valueChanged, this, put);
        connect(h, &QDoubleSpinBox::valueChanged, this, put);
        put();
        form->addRow(QStringLiteral("スタイル"), style);
        form->addRow(QStringLiteral("幅"), w);
        form->addRow(QStringLiteral("高さ"), h);
        auto* guides = new QPushButton(QStringLiteral("ガイドからスライスを作成"));
        guides->setObjectName(QStringLiteral("slices_from_guides"));
        guides->setToolTip(QStringLiteral("このページのガイドで区切ったスライスを作ります（今のスライスはすべて置き換わります）"));
        connect(guides, &QPushButton::clicked, this, [this] {
            if (!slices_unlocked()) return;
            if (slice_op(Json{{"op", "slices_from_guides"}})) flash(QStringLiteral("ガイドからスライスを作りました"), 3000);
        });
        form->addRow(guides);
        auto* hint = new QLabel(QStringLiteral("スライスは Web 用に保存（従来）… でスライスごとの画像と HTML に書き出せます。位置は mm で持ち、書き出す大きさの画素に合わせます"));
        hint->setWordWrap(true);
        theme::role(hint, "hint");
        form->addRow(hint);
        tool_settings_->add({QStringLiteral("slice")}, page);
    }
    // the スライス選択ツール's options: ユーザー定義スライスに変換, 分割…, 削除, 自動スライスを隠す, スライスオプション…
    {
        auto* page = new QWidget;
        page->setObjectName(QStringLiteral("slice_select_settings"));
        auto* layout = new QVBoxLayout(page);
        layout->setContentsMargins(0, 0, 0, 0);
        slice_chosen_label_ = new QLabel;
        slice_chosen_label_->setObjectName(QStringLiteral("slice_chosen"));
        slice_chosen_label_->setWordWrap(true);
        layout->addWidget(slice_chosen_label_);
        const auto button = [&](const char* name, const QString& words, const QString& tip, std::function<void()> slot) {
            auto* b = new QPushButton(words);
            b->setObjectName(QString::fromLatin1(name));
            b->setToolTip(tip);
            connect(b, &QPushButton::clicked, this, std::move(slot));
            layout->addWidget(b);
        };
        button("slice_promote", QStringLiteral("ユーザー定義スライスに変換"), QStringLiteral("選んだ自動スライス・レイヤーベースのスライスを、ユーザー定義スライスにします"), [this] {
            const auto chosen = canvas_->chosen_slice_now();
            if (!chosen) {
                flash(QStringLiteral("先にスライス選択ツールでスライスを選びます"), 3000);
                return;
            }
            if (chosen->origin == "user" || !slices_unlocked()) return;
            if (chosen->id) {
                slice_op(Json{{"op", "set_slice"}, {"slice", *chosen->id}, {"promote", true}});
            } else {
                (void)chosen_slice_id(true);
            }
            refresh_slices();
        });
        button("slice_divide", QStringLiteral("分割…"), QStringLiteral("選んだスライスを縦・横に等しく分けます"), [this] { divide_slice_dialog(); });
        button("slice_delete", QStringLiteral("削除"), QStringLiteral("選んだスライスを削除します（自動スライスは削除できません）"), [this] {
            const auto chosen = canvas_->chosen_slice_now();
            if (!chosen || !chosen->id) {
                flash(QStringLiteral("削除するユーザー定義スライスかレイヤーベースのスライスを選びます"), 3000);
                return;
            }
            if (!slices_unlocked()) return;
            if (slice_op(Json{{"op", "delete_slices"}, {"slices", Json::array({*chosen->id})}})) canvas_->chosen_slice.reset();
            refresh_slices();
        });
        auto* hide = new QCheckBox(QStringLiteral("自動スライスを隠す"));
        hide->setObjectName(QStringLiteral("slice_hide_auto"));
        connect(hide, &QCheckBox::toggled, this, [this](bool on) {
            canvas_->hide_auto_slices = on;
            canvas_->update();
        });
        layout->addWidget(hide);
        button("slice_options", QStringLiteral("スライスオプション…"), QStringLiteral("選んだスライスの名前・種類・URL などを決めます（ダブルクリックでも）"),
               [this] { slice_options_dialog(); });
        layout->addStretch(1);
        tool_settings_->add({QStringLiteral("slice_select")}, page);
    }
    refresh_slices();
}

void MainWindow::slice_options_dialog() {
    const Asked asked = asking();
    const auto chosen = canvas_->chosen_slice_now();
    const core::Page* page = current_page();
    if (!chosen || page == nullptr) {
        flash(QStringLiteral("先にスライス選択ツールでスライスを選びます"), 3000);
        return;
    }
    const core::Slices none;
    const core::Slice* s = chosen->id ? core::find_slice(page->slices ? *page->slices : none, *chosen->id) : nullptr;
    const core::Slice blank;
    const core::Slice& now = s != nullptr ? *s : blank;
    QDialog dialog(this);
    dialog.setWindowTitle(QStringLiteral("スライスオプション"));
    dialog.setObjectName(QStringLiteral("slice_options"));
    auto* form = new QFormLayout(&dialog);
    auto* kind = new QComboBox;
    kind->setObjectName(QStringLiteral("slice_kind"));
    kind->addItem(QStringLiteral("画像"), QStringLiteral("image"));
    kind->addItem(QStringLiteral("画像なし"), QStringLiteral("noImage"));
    kind->addItem(QStringLiteral("テーブル"), QStringLiteral("table"));
    kind->setCurrentIndex(std::max(0, kind->findData(QString::fromStdString(now.kind))));
    form->addRow(QStringLiteral("スライスの種類"), kind);
    const auto line = [&](const char* name, const std::string& value) {
        auto* edit = new QLineEdit(QString::fromStdString(value));
        edit->setObjectName(QString::fromLatin1(name));
        return edit;
    };
    auto* name = line("slice_name", s != nullptr && !now.name.empty() ? now.name : chosen->name);
    auto* url = line("slice_url", now.url);
    auto* target = line("slice_target", now.target);
    auto* message = line("slice_message", now.message);
    auto* alt = line("slice_alt", now.alt);
    form->addRow(QStringLiteral("名前"), name);
    form->addRow(QStringLiteral("URL"), url);
    form->addRow(QStringLiteral("ターゲット"), target);
    form->addRow(QStringLiteral("メッセージテキスト"), message);
    form->addRow(QStringLiteral("Alt テキスト"), alt);
    auto* cell = new QPlainTextEdit(QString::fromStdString(now.cell_text));
    cell->setObjectName(QStringLiteral("slice_cell_text"));
    cell->setMaximumHeight(80);
    auto* html = new QCheckBox(QStringLiteral("テキストは HTML"));
    html->setObjectName(QStringLiteral("slice_cell_html"));
    html->setChecked(now.cell_text_is_html);
    form->addRow(QStringLiteral("セルに表示するテキスト"), cell);
    form->addRow(QString(), html);
    // 寸法 (the page's pixels: X, Y, W, H)
    auto* x = new QSpinBox, *y = new QSpinBox, *w = new QSpinBox, *h = new QSpinBox;
    const std::array<std::pair<QSpinBox*, std::int64_t>, 4> sizes{{{x, chosen->box.x0}, {y, chosen->box.y0}, {w, chosen->box.width()}, {h, chosen->box.height()}}};
    const char* names[] = {"slice_x", "slice_y", "slice_w", "slice_h"};
    for (std::size_t i = 0; i < 4; ++i) {
        sizes[i].first->setObjectName(QString::fromLatin1(names[i]));
        sizes[i].first->setRange(i < 2 ? -1000000 : 1, 1000000);
        sizes[i].first->setValue(static_cast<int>(sizes[i].second));
        sizes[i].first->setSuffix(QStringLiteral(" px"));
    }
    auto* size_row = new QWidget;
    auto* row = new QHBoxLayout(size_row);
    row->setContentsMargins(0, 0, 0, 0);
    for (const auto& [label, box] : {std::pair{"X", x}, {"Y", y}, {"W", w}, {"H", h}}) {
        row->addWidget(new QLabel(QString::fromLatin1(label)));
        row->addWidget(box);
    }
    form->addRow(QStringLiteral("寸法"), size_row);
    auto* background = new QComboBox;
    background->setObjectName(QStringLiteral("slice_background"));
    background->addItem(QStringLiteral("なし"), QString());
    background->addItem(QStringLiteral("カラー…"), QStringLiteral("colour"));
    std::optional<QColor> colour;
    if (now.background) colour = QColor(QString::fromStdString(*now.background));
    background->setCurrentIndex(colour ? 1 : 0);
    if (colour) background->setItemText(1, QStringLiteral("カラー（%1）").arg(colour->name()));
    connect(background, &QComboBox::activated, &dialog, [&, background](int i) {
        if (i != 1) return;
        if (const auto chosen_colour = ask::colour(&dialog, colour.value_or(QColor(Qt::white)), QStringLiteral("スライスの背景のカラー"))) {
            colour = *chosen_colour;
            background->setItemText(1, QStringLiteral("カラー（%1）").arg(colour->name()));
        } else if (!colour) {
            background->setCurrentIndex(0);
        }
    });
    form->addRow(QStringLiteral("スライスの背景の種類"), background);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    form->addRow(buttons);
    if (ask::exec(&dialog) != QDialog::Accepted || !still(asked)) return;
    if (!slices_unlocked()) return;
    Json op{{"op", "set_slice"},
            {"kind", kind->currentData().toString().toStdString()},
            {"url", url->text().toStdString()},
            {"target", target->text().toStdString()},
            {"message", message->text().toStdString()},
            {"alt", alt->text().toStdString()},
            {"cell_text", cell->toPlainText().toStdString()},
            {"cell_text_is_html", html->isChecked()},
            {"background", background->currentIndex() == 1 && colour ? Json(colour->name().toStdString()) : Json(nullptr)}};
    // (the made name — <base>_NN — left as it was shown: the slice keeps none of its own)
    const std::string given = name->text().trimmed().toStdString();
    op["name"] = now.name.empty() && given == chosen->name ? std::string() : given;
    if (chosen->id) {
        op["slice"] = *chosen->id;
    } else {
        op["number"] = chosen->number;
    }
    const bool moved = x->value() != chosen->box.x0 || y->value() != chosen->box.y0 || w->value() != chosen->box.width() || h->value() != chosen->box.height();
    if (moved) op["rect_px"] = Json::array({x->value(), y->value(), w->value(), h->value()});
    const std::optional<std::int64_t> id = chosen->id;
    const core::SliceBox box = chosen->box;
    if (slice_op(std::move(op))) {
        if (const PageCanvas::SliceView* after = slice_view()) {
            for (const core::ResolvedSlice& r : after->slices)
                if ((id && r.id == id) || (!id && !moved && r.id && r.box == box)) canvas_->chosen_slice = r.number;
        }
        refresh_slices();
        flash(QStringLiteral("スライスオプションを決めました"), 3000);
    }
}

void MainWindow::divide_slice_dialog() {
    const Asked asked = asking();
    const auto chosen = canvas_->chosen_slice_now();
    if (!chosen) {
        flash(QStringLiteral("先にスライス選択ツールでスライスを選びます"), 3000);
        return;
    }
    QDialog dialog(this);
    dialog.setWindowTitle(QStringLiteral("スライスを分割"));
    dialog.setObjectName(QStringLiteral("divide_slice"));
    auto* form = new QFormLayout(&dialog);
    auto* across = new QCheckBox(QStringLiteral("水平方向に分割"));
    across->setObjectName(QStringLiteral("divide_horizontal"));
    auto* rows = new QSpinBox;
    rows->setObjectName(QStringLiteral("divide_rows"));
    rows->setRange(1, 1000);
    rows->setValue(2);
    rows->setSuffix(QStringLiteral(" 個（縦に並ぶ）"));
    auto* down = new QCheckBox(QStringLiteral("垂直方向に分割"));
    down->setObjectName(QStringLiteral("divide_vertical"));
    auto* columns = new QSpinBox;
    columns->setObjectName(QStringLiteral("divide_columns"));
    columns->setRange(1, 1000);
    columns->setValue(2);
    columns->setSuffix(QStringLiteral(" 個（横に並ぶ）"));
    across->setChecked(true);
    form->addRow(across, rows);
    form->addRow(down, columns);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    form->addRow(buttons);
    if (ask::exec(&dialog) != QDialog::Accepted || !still(asked)) return;
    if (!slices_unlocked()) return;
    const int r = across->isChecked() ? rows->value() : 1;
    const int c = down->isChecked() ? columns->value() : 1;
    if (r == 1 && c == 1) return;
    Json op{{"op", "divide_slice"}, {"rows", r}, {"columns", c}};
    if (chosen->id) {
        op["slice"] = *chosen->id;
    } else {
        op["number"] = chosen->number;
    }
    if (slice_op(std::move(op))) flash(QStringLiteral("スライスを %1 個に分割しました").arg(r * c), 3000);
}

}  // namespace genko::app
