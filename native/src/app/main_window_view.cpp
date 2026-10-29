// 表示 (PC5 H-0, 節 H 5.1; SPEC VIEW-01): the view menu in Photoshop's order, Genko's own commands after a line. H-0
// lays it out and names the places; H-a (画素の表示・スクリーンモード・32 bit プレビュー) and H-b (ガイド・エクストラ・表示・非表示 ▸・
// 単位) make their commands in build_view_actions and put them in their places in kViewRows (a row naming a command not
// made yet is passed over, so a place may wait for its part). Commands the rows do not name stay after them (a later
// part's own, until it joins the rows).

#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QLabel>
#include <QMenu>
#include <QMenuBar>
#include <QStatusBar>
#include <QTimer>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>
#include <tuple>

#include "app/ask.hpp"
#include "app/comfort.hpp"
#include "app/main_window.hpp"
#include "app/theme.hpp"
#include "app/tool_settings.hpp"
#include "app/workspace.hpp"
#include "render/hdr_preview.hpp"
#include "render/layer_bounds.hpp"

namespace genko::app {

namespace {

// (rows: a command's attribute, "▸<title>" a menu inside, "|" a line — lay_out_menu)
const QStringList& view_rows() {
    static const QStringList rows{
        // 校正設定 ▸・色の校正・色域外警告 (PC1b)
        QStringLiteral("▸校正の設定"), QStringLiteral("act_cmyk_proof"), QStringLiteral("act_gamut_warning"),
        QStringLiteral("|"),
        // ピクセル縦横比 ▸・ピクセル縦横比を補正 (H-f, the user's decision 10), 32 bit プレビューオプション… (H-a)
        QStringLiteral("▸ピクセル縦横比"), QStringLiteral("act_pixel_aspect_correction"), QStringLiteral("act_32bit_preview"),
        QStringLiteral("|"),
        // ズームイン・ズームアウト・画面サイズに合わせる・アートボードを画面サイズに合わせる (I)・レイヤーを画面サイズに合わせる (H-a)・
        // 100%・200% (H-a)・プリントサイズ
        QStringLiteral("act_zoom_in"), QStringLiteral("act_zoom_out"), QStringLiteral("act_fit"), QStringLiteral("act_fit_artboard"),
        QStringLiteral("act_fit_layers"), QStringLiteral("act_actual_pixels"), QStringLiteral("act_200_percent"), QStringLiteral("act_actual"),
        QStringLiteral("|"),
        // 水平方向に反転・パターンプレビュー (PC4 E3)・ピクセルアートプレビュー (H-a)
        QStringLiteral("act_mirror"), QStringLiteral("act_pattern_preview"), QStringLiteral("act_pixel_art"),
        QStringLiteral("|"),
        // スクリーンモード ▸ (H-a)
        QStringLiteral("▸スクリーンモード"),
        QStringLiteral("|"),
        // エクストラ・表示・非表示 ▸ (H-b: ターゲットパス, ブラシプレビュー, 選択範囲のエッジ and グリッド in it)
        QStringLiteral("act_extras"), QStringLiteral("▸表示・非表示"),
        QStringLiteral("|"),
        // 定規 (the user's decision 12: Genko's 目盛り)
        QStringLiteral("act_scale"),
        QStringLiteral("|"),
        // スナップ・スナップ先 ▸ (PC3 C-d)
        QStringLiteral("act_snap_on"), QStringLiteral("▸スナップ先"),
        QStringLiteral("|"),
        // ガイドをロック・ガイドを消去・選択したアートボードのガイドを消去 (I)・カンバスガイドを消去・新規ガイド…・新規ガイドレイアウトを作成…
        // (H-b)・シェイプから新規ガイドを作成 (PC3 C-b)
        QStringLiteral("act_lock_guides"), QStringLiteral("act_clear_guides"), QStringLiteral("act_clear_artboard_guides"),
        QStringLiteral("act_clear_canvas_guides"), QStringLiteral("act_new_guide"), QStringLiteral("act_new_guide_layout"),
        QStringLiteral("act_guides_from_shape"),
        QStringLiteral("|"),
        // スライスをロック・スライスを消去 (PC5 J-b)
        QStringLiteral("act_lock_slices"), QStringLiteral("act_clear_slices"),
        QStringLiteral("|"),
        // Genko's own
        QStringLiteral("act_view_flip_v"), QStringLiteral("act_turn_left"), QStringLiteral("act_turn_right"), QStringLiteral("act_turn_reset"),
        QStringLiteral("act_zoom_value"), QStringLiteral("act_zoom_tool"), QStringLiteral("act_hand_tool"),
        QStringLiteral("|"),
        QStringLiteral("act_overview"), QStringLiteral("act_prev"), QStringLiteral("act_next"),
        QStringLiteral("|"),
        QStringLiteral("act_guides"), QStringLiteral("act_phone"), QStringLiteral("act_onion"),
        QStringLiteral("act_screen_dots"),
        QStringLiteral("|"),
        QStringLiteral("act_tool_names")};
    return rows;
}

// 表示 → 表示・非表示 ▸ in Photoshop's order (節 H 4.4, 5.1; the owner 節 H). Each part puts its command in with
// show_menu(view)->addAction(...) under the attribute named here — a row not made yet is passed over, a command not named
// here goes after a line. (H-b: レイヤーエッジ…メッシュ・編集ピン, すべて・なし, エクストラオプション… — 選択範囲のエッジ is PC3's
// act_sel_edges, グリッド 定規's act_grid —; J-d1: カウント; J-b: スライス; J-c: 注釈; H-a: ピクセルグリッド)
const QStringList& show_rows() {
    static const QStringList rows{
        QStringLiteral("act_show_layer_edges"), QStringLiteral("act_sel_edges"), QStringLiteral("act_show_target_path"),
        QStringLiteral("act_grid"), QStringLiteral("act_show_guides"), QStringLiteral("act_show_canvas_guides"),
        QStringLiteral("act_show_artboard_guides"), QStringLiteral("act_show_artboard_names"), QStringLiteral("act_show_count"),
        QStringLiteral("act_show_smart_guides"), QStringLiteral("act_show_slices"), QStringLiteral("act_show_notes"),
        QStringLiteral("act_pixel_grid"), QStringLiteral("act_brush_preview"), QStringLiteral("act_show_mesh"),
        QStringLiteral("act_show_edit_pins"),
        QStringLiteral("|"),
        QStringLiteral("act_show_all"), QStringLiteral("act_show_none"),
        QStringLiteral("|"),
        QStringLiteral("act_extras_options")};
    return rows;
}

}  // namespace

void MainWindow::build_view_actions() {
    // (H-a: 100%, 200%, レイヤーを画面サイズに合わせる, スクリーンモード ▸, 32 bit プレビューオプション…, ピクセルグリッド; H-b:
    // エクストラ, 表示・非表示 ▸, ガイドをロック, ガイドを消去, カンバスガイドを消去, 新規ガイド…, 新規ガイドレイアウトを作成…; H-f:
    // ピクセル縦横比 ▸ — each part's commands made here, their Photoshop keys in key_sets::photoshop_keys)
    PageCanvas* c = canvas_;
    // --- H-a: the page's pixels (the user's decision 11: the zoom is in the page's pixels; the paper's size is プリントサイズ)
    make("act_actual_pixels", QStringLiteral("100%"), [c] { c->glide([c] { c->actual_pixels(100); }); }, {},
         QStringLiteral("原稿の 1 画素を画面の 1 画素で表示します（画素等倍）"));
    make("act_200_percent", QStringLiteral("200%"), [c] { c->glide([c] { c->actual_pixels(200); }); }, {},
         QStringLiteral("原稿の 1 画素を画面の 2 画素四方で表示します"));
    make("act_fit_layers", QStringLiteral("レイヤーを画面サイズに合わせる"), [this] { fit_layers(); }, {},
         QStringLiteral("選んだレイヤー（なければ描く先のレイヤー）の描かれた範囲を画面いっぱいに表示します"));
    // (the hand and zoom tools' option 画面にフィット: the page filling the view, its edges past it if they must)
    make("act_fill_screen", QStringLiteral("画面にフィット"), [c] {
        const core::Page* p = c->page();
        if (p == nullptr || c->width() <= 0 || c->height() <= 0) return;
        const double w = p->spec.width_mm.value(), h = p->spec.height_mm.value();
        if (!(w > 0) || !(h > 0)) return;
        c->glide([c, w, h] {
            c->fit_box(QRectF(0, 0, w, h));
            c->zoom_by(std::max(c->width() / w, c->height() / h) / c->scale());
        });
    }, {}, QStringLiteral("原稿の幅か高さを画面いっぱいに表示します（はみ出す側はスクロールで）"));
    QAction* grid = make("act_pixel_grid", QStringLiteral("ピクセルグリッド"), [] {}, {},
                         QStringLiteral("画素等倍の 500% 以上で、描く先のレイヤーの画素の格子を表示します（ペンのレイヤーはページの解像度）"), true);
    connect(grid, &QAction::toggled, this, [c](bool on) {
        c->show_pixel_grid = on;
        c->update();
    });
    QAction* pixel_art = make("act_pixel_art", QStringLiteral("ピクセルアートプレビュー"), [] {}, {},
                              QStringLiteral("原稿の画素をなめらかにせず、そのままの四角で拡大・縮小して表示します（表示だけ）"), true);
    connect(pixel_art, &QAction::toggled, this, [c](bool on) { c->set_pixel_art(on); });
    make("act_32bit_preview", QStringLiteral("32 bit プレビューオプション…"), [this] { ask_32bit_preview(); }, {},
         QStringLiteral("32 bit のページの見え方（露光量・ガンマ・ハイライト圧縮）を決めます（表示だけ。原稿・書き出しは変わりません）"));
    // 手のひらツール (tool:Hand): Photoshop's H (Genko's H is 水平方向に反転: none in Genko's keys); Space still moves the view
    QAction* hand = make("act_hand_tool", QStringLiteral("手のひらツール"), [this] { choose_tool(QStringLiteral("hand")); }, {},
                         QStringLiteral("ドラッグで表示を動かします（どの道具でも Space を押しながら）"), true);
    tools_->addAction(hand);
    hand->setAutoRepeat(false);  // (a held key chooses the tool once)
    tool_actions_[QStringLiteral("hand")] = hand;
    theme::iconed(hand, "hand");
    theme::iconed(action(QStringLiteral("act_zoom_tool")), "zoom");
    // スクリーンモード ▸: Photoshop's three, and F going round them
    auto* screens = new QActionGroup(this);
    for (const auto& [attribute, title, mode] : {std::tuple{"act_screen_standard", "標準スクリーンモード", ScreenMode::Standard},
                                                 std::tuple{"act_screen_full_menu", "メニュー付きフルスクリーンモード", ScreenMode::FullWithMenu},
                                                 std::tuple{"act_screen_full", "フルスクリーンモード", ScreenMode::Full}}) {
        const ScreenMode m = mode;
        screens->addAction(make(QString::fromLatin1(attribute), QString::fromUtf8(title), [this, m] { set_screen_mode(m); }, {}, {}, true));
    }
    action(QStringLiteral("act_screen_standard"))->setChecked(true);
    make("act_screen_next", QStringLiteral("次のスクリーンモード"), [this] {
        set_screen_mode(screen_mode_ == ScreenMode::Standard       ? ScreenMode::FullWithMenu
                        : screen_mode_ == ScreenMode::FullWithMenu ? ScreenMode::Full
                                                                   : ScreenMode::Standard);
    }, {}, QStringLiteral("標準 → メニュー付きフルスクリーン → フルスクリーン → 標準の順に切り替えます"));
    // (render:GPU-CPU-parity: the card could not draw the page — the processor draws it now, and the person is told)
    connect(canvas_, &PageCanvas::gpuFailed, this, [this](const QString& message) { flash(message, 6000, true); });
    build_guide_view_actions();  // (PC5 H-b: main_window_guide_view.cpp)
}

void MainWindow::build_view_menus(QMenu* view) {
    // (H-a's commands in 表示 — and スクリーンモード ▸ and 表示・非表示 ▸ (ピクセルグリッド; H-b fills the rest) —, then laid out)
    for (const char* name : {"act_32bit_preview", "act_fit_layers", "act_actual_pixels", "act_200_percent", "act_pixel_art", "act_hand_tool"})
        view->addAction(action(QString::fromLatin1(name)));
    QMenu* screens = view->addMenu(QStringLiteral("スクリーンモード"));
    screens->setObjectName(QStringLiteral("menu_screen_mode"));
    for (const char* name : {"act_screen_standard", "act_screen_full_menu", "act_screen_full"}) screens->addAction(action(QString::fromLatin1(name)));
    screens->addSeparator();
    screens->addAction(action(QStringLiteral("act_screen_next")));
    show_menu(view)->addAction(action(QStringLiteral("act_pixel_grid")));
    build_guide_view_menus(view);  // (PC5 H-b: エクストラ, 表示・非表示 ▸'s, the guides' — main_window_guide_view.cpp)
    // (after every part has put its commands in 表示 and in 表示・非表示 ▸: laid out once)
    lay_out_menu(show_menu(view), show_rows());
    lay_out_menu(view, view_rows());
}

QMenu* MainWindow::show_menu(QMenu* view) {
    // (one 表示・非表示 ▸ whichever part comes first: found by its objectName, else made and put in 表示 — view_rows'
    // "▸表示・非表示" places it)
    for (QAction* item : view->actions()) {
        if (item->menu() != nullptr && item->menu()->objectName() == QLatin1String("menu_show")) return item->menu();
    }
    QMenu* shown = view->addMenu(QStringLiteral("表示・非表示"));
    shown->setObjectName(QStringLiteral("menu_show"));
    return shown;
}

void MainWindow::build_view_pages(ToolSettings* ts) {
    // 手のひらツール and ズームツール (PC5 H-a): Photoshop's buttons 100%・画面サイズ・画面にフィット; the zoom tool's ways
    // (PC5 H-c: すべてのウィンドウをスクロール, すべてのウィンドウをズーム — main_window_arrange.cpp)
    ts->add({QStringLiteral("hand")}, action_page({action("act_scroll_all"), QStringLiteral("表示"), action("act_actual_pixels"), action("act_fit"),
                                                   action("act_fill_screen")}));
    auto* mode = new QComboBox;
    mode->setObjectName(QStringLiteral("zoom_mode"));
    mode->addItem(QStringLiteral("ズームイン"), false);
    mode->addItem(QStringLiteral("ズームアウト"), true);
    mode->setToolTip(QStringLiteral("クリックで拡大するか縮小するか（Alt を押している間は逆）"));
    connect(mode, &QComboBox::currentIndexChanged, this, [this, mode](int) { canvas_->zoom_out_mode = mode->currentData().toBool(); });
    QAction* scrub = make("act_scrubby_zoom", QStringLiteral("スクラブズーム"), [] {}, {},
                          QStringLiteral("ドラッグで左右に動かすと、押した所を中心に縮小・拡大します（切ると、囲んだ所を画面いっぱいに）"), true);
    connect(scrub, &QAction::toggled, this, [this](bool on) { canvas_->scrubby_zoom = on; });
    ts->add({QStringLiteral("zoom")}, action_page({QStringLiteral("ズーム"), static_cast<QWidget*>(mode), scrub, action("act_zoom_all"), QStringLiteral("表示"),
                                                   action("act_actual_pixels"), action("act_fit"), action("act_fill_screen")}));
}

void MainWindow::fit_layers() {
    const core::Page* page = canvas_->page();
    if (page == nullptr || !canvas_->doc()) return;
    std::optional<render::layer_bounds::Box> box;
    for (const std::string& id : transform_targets()) {
        try {
            const auto b = render::layer_bounds::of_layer(*canvas_->doc(), *page, id);
            if (!b) continue;
            box = box ? render::layer_bounds::Box{std::min((*box)[0], (*b)[0]), std::min((*box)[1], (*b)[1]), std::max((*box)[2], (*b)[2]),
                                                  std::max((*box)[3], (*b)[3])}
                      : *b;
        } catch (const std::exception&) {  // (a layer gone meanwhile)
        }
    }
    // (nothing drawn on them: the page, as PhotoCraft's fit_layers)
    const QRectF r = box ? QRectF(QPointF((*box)[0], (*box)[1]), QPointF((*box)[2], (*box)[3])).normalized()
                         : QRectF(0, 0, page->spec.width_mm.value(), page->spec.height_mm.value());
    canvas_->glide([this, r] { canvas_->fit_box(r); });
}

void MainWindow::set_screen_mode(ScreenMode mode) {
    const auto check = [this] {
        const char* name = screen_mode_ == ScreenMode::Standard       ? "act_screen_standard"
                           : screen_mode_ == ScreenMode::FullWithMenu ? "act_screen_full_menu"
                                                                      : "act_screen_full";
        if (QAction* act = action(QString::fromLatin1(name))) act->setChecked(true);
    };
    if (mode == screen_mode_) {
        check();
        return;
    }
    if (screen_mode_ == ScreenMode::Standard) screen_before_ = windowState();
    if (screen_mode_ == ScreenMode::Full) {  // (the bars and panels the full screen hid, back)
        menuBar()->setVisible(true);
        statusBar()->setVisible(true);
        if (screen_hid_panels_ && canvas_only_ != nullptr && canvas_only_->on()) canvas_only_->leave();
        screen_hid_panels_ = false;
    }
    screen_mode_ = mode;
    if (mode == ScreenMode::Standard) {
        setWindowState(screen_before_ & ~Qt::WindowFullScreen);
    } else {
        setWindowState(windowState() | Qt::WindowFullScreen);
    }
    if (mode == ScreenMode::Full) {  // (the page alone: a side's panels come back at its edge, as 原稿だけを表示)
        menuBar()->setVisible(false);
        statusBar()->setVisible(false);
        if (canvas_only_ != nullptr && !canvas_only_->on()) {
            canvas_only_->enter();
            screen_hid_panels_ = true;
        }
    }
    check();
    if (mode != ScreenMode::Standard) {
        // (how to come back, by the keys as they are now — review P2-6: the menus may be hidden; also over the page, the
        // status line being hidden in フルスクリーンモード)
        const QString next = action(QStringLiteral("act_screen_next"))->shortcut().toString(QKeySequence::NativeText);
        const QString told = QStringLiteral("%1。Esc で標準スクリーンモードに戻ります%2")
                                 .arg(mode == ScreenMode::Full ? QStringLiteral("フルスクリーンモード") : QStringLiteral("メニュー付きフルスクリーンモード"),
                                      next.isEmpty() ? QString() : QStringLiteral("（%1 で次のスクリーンモード）").arg(next));
        flash(told, 4000);
        auto* over = new QLabel(told, canvas_);
        over->setObjectName(QStringLiteral("screen_mode_notice"));
        over->setStyleSheet(QStringLiteral("QLabel { background: rgba(0, 0, 0, 170); color: white; padding: 8px 14px; border-radius: 6px; }"));
        over->adjustSize();
        over->move(std::max(0, (canvas_->width() - over->width()) / 2), 24);
        over->show();
        over->raise();
        QTimer::singleShot(3500, over, &QObject::deleteLater);
    }
}

void MainWindow::ask_32bit_preview() {
    const core::Page* page = canvas_->page();
    if (documents_.empty() || page == nullptr || !render::hdr_page(*page)) {
        flash(QStringLiteral("32 bit プレビューオプションは、32 bit/チャンネルのページで使えます"), 4000);
        return;
    }
    const render::HdrPreview before = documents_[static_cast<std::size_t>(doc_)].proof.hdr;
    // (PC5 H-a review P3-10: the window turns a loop of its own — the book it was opened for is found again by its session,
    // and left alone when it was closed meanwhile; the canvas follows only while that book is the one shown)
    const std::weak_ptr<Session> opened_for = documents_[static_cast<std::size_t>(doc_)].session;
    const auto set_preview = [this, opened_for](const render::HdrPreview& preview) {
        const auto session = opened_for.lock();
        for (std::size_t i = 0; session && i < documents_.size(); ++i) {
            if (documents_[i].session != session) continue;
            documents_[i].proof.hdr = preview;
            if (static_cast<int>(i) == doc_) canvas_->renderer().set_hdr(preview);
            return;
        }
    };
    QDialog dialog(this);
    dialog.setObjectName(QStringLiteral("hdr_preview"));
    dialog.setWindowTitle(QStringLiteral("32 bit プレビューオプション"));
    auto* form = new QFormLayout;
    auto* method = new QComboBox;
    method->setObjectName(QStringLiteral("hdr_method"));
    method->addItem(QStringLiteral("露光量とガンマ"), false);
    method->addItem(QStringLiteral("ハイライト圧縮"), true);
    method->setCurrentIndex(before.highlight_compression ? 1 : 0);
    auto* exposure = new QDoubleSpinBox;
    exposure->setObjectName(QStringLiteral("hdr_exposure"));
    exposure->setRange(render::HdrPreview::kMinExposure, render::HdrPreview::kMaxExposure);
    exposure->setDecimals(2);
    exposure->setSingleStep(0.1);
    exposure->setValue(before.exposure);
    auto* gamma = new QDoubleSpinBox;
    gamma->setObjectName(QStringLiteral("hdr_gamma"));
    gamma->setRange(render::HdrPreview::kMinGamma, render::HdrPreview::kMaxGamma);
    gamma->setDecimals(2);
    gamma->setSingleStep(0.05);
    gamma->setValue(before.gamma);
    form->addRow(QStringLiteral("方法"), method);
    form->addRow(QStringLiteral("露光量"), exposure);
    form->addRow(QStringLiteral("ガンマ"), gamma);
    auto* note = new QLabel(QStringLiteral("表示だけを変えます（原稿の値・書き出しは変わりません）。ハイライト圧縮は、1 より明るい所も白に飛ばさずに見せます"));
    note->setWordWrap(true);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    auto* layout = new QVBoxLayout(&dialog);
    layout->addLayout(form);
    layout->addWidget(note);
    layout->addWidget(buttons);
    // (shown on the page as the numbers change; キャンセル puts it back)
    const auto chosen = [method, exposure, gamma] {
        render::HdrPreview out;
        out.highlight_compression = method->currentData().toBool();
        out.exposure = exposure->value();
        out.gamma = gamma->value();
        return out.clamped();
    };
    const auto follow = [set_preview, chosen, exposure, gamma, method] {
        const bool compress = method->currentData().toBool();
        exposure->setEnabled(!compress);  // (as Photoshop's window: the highlights compressed take neither)
        gamma->setEnabled(!compress);
        set_preview(chosen());
    };
    connect(method, &QComboBox::currentIndexChanged, &dialog, [follow](int) { follow(); });
    connect(exposure, &QDoubleSpinBox::valueChanged, &dialog, [follow](double) { follow(); });
    connect(gamma, &QDoubleSpinBox::valueChanged, &dialog, [follow](double) { follow(); });
    exposure->setEnabled(!before.highlight_compression);
    gamma->setEnabled(!before.highlight_compression);
    if (ask::exec(&dialog) == QDialog::Accepted) {
        follow();
        return;
    }
    set_preview(before);
}


}  // namespace genko::app
