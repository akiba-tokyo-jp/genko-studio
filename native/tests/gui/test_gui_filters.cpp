// PC4 節 D (SPEC.md COLOR-08「フィルター」, design 5; the user's decision 1) on the offscreen platform: the フィルター menu after
// レイヤー — its commands in Photoshop's order and words, their keys (前回のフィルター Ctrl+F, Camera Raw フィルター…
// Ctrl+Shift+A, 広角補正… Ctrl+Alt+Shift+A, 消点… Ctrl+Alt+V, ゆがみ… Ctrl+Shift+X moved from 編集), the kinds not in this
// build yet grey —; ぼかし（ガウス）…'s window made from its table with the result on the page, one op and one Undo; ぼかし
// without a window; 前回のフィルター sending the last one again; フェード… after it; on a smart object a smart filter (its
// window again from the panel); 漫画's 光彩… in its old window; a heavy one's preview on a thread and its progress with 中止;
// the layer panel's かける… opening the menu. (The frame of D1…D7's windows and kinds: their own tests add theirs.) D1: its
// commands lit — ぼかし・シャープ・ノイズ・その他・ビデオ —, カスタム…'s 5 × 5 grid, 平均 and シャープ without a window, new grains
// each time ノイズを加える… is applied, ぼかし（レンズ）… on a smart object without the layer mask as its depth. D2: its commands lit
// — 変形・ピクセレート・表現手法 — in Photoshop's words, 渦巻き…'s window, ぶれ without one, シアー…'s curve, 置き換え…'s map (a layer
// of the page; on a smart object made the book's file). D6: its commands lit — レンズ補正…・広角補正…・消点… — and their own
// windows (the result on their pictures, 角度補正ツール, a constraint drawn, a plane made and a layer pasted on it). D4: its
// commands lit — 描画・ぼかしギャラリー —, 雲模様 1・2 without a window, ファイバー…・逆光… from their tables, 照明効果…'s window,
// 炎…・額縁…・木…'s windows (render_picture on a thread), ぼかしギャラリー's work over the canvas (and from the layer panel).
// D5: Camera Raw フィルター… lit — its window's tabs, its result on its picture, 補正前, 初期化, one op and one Undo, a smart
// filter; its panel's settings untouched as they came, the point curve's 19 points, a new seed each time, F-e's hooks.

#include <QtTest>
#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QLabel>
#include <QListWidget>
#include <QMenu>
#include <QMenuBar>
#include <QMouseEvent>
#include <QPointer>
#include <QProgressDialog>
#include <QPushButton>
#include <QRadioButton>
#include <QTabWidget>
#include <QSpinBox>
#include <QTimer>
#include <QTreeWidget>

#include "gui_support.hpp"
#include "app/adjust_forms.hpp"
#include "app/camera_raw_window.hpp"
#include "app/canvas.hpp"
#include "app/colours.hpp"
#include "app/history_panel.hpp"
#include "app/layer_dialogs.hpp"
#include "app/inject.hpp"
#include "app/layer_panel.hpp"
#include "app/config.hpp"
#include "app/blur_gallery_tool.hpp"
#include "app/filter_gallery_window.hpp"
#include "app/lens_window.hpp"
#include "app/main_window.hpp"
#include "app/preferences.hpp"
#include "app/workspace.hpp"
#include "core/adjust.hpp"
#include "core/base64.hpp"
#include "core/command_bus.hpp"
#include "core/vanishing.hpp"
#include "render/fx.hpp"
#include "render/ops_registry.hpp"
#include "render/page.hpp"
#include "render/png.hpp"

using namespace genko;
using core::Json;

namespace {

// A book whose page 1 has a paint layer "pic" (a ramp, a red block) and, on a 600 dpi precise page 2, none.
core::Document book_doc() {
    auto doc = core::new_episode("フィルター", core::Num(1), 1, core::PageSpec::custom(40, 40, 30, 30, 1, 2, 2, 2, 2, 72, "color"));
    std::string bytes;
    for (int y = 0; y < 80; ++y)
        for (int x = 0; x < 80; ++x) {
            const bool red = x >= 50 && x < 70 && y >= 10 && y < 30;
            const int v = x * 255 / 79;
            for (const int c : red ? std::array<int, 4>{220, 30, 30, 255} : std::array<int, 4>{v, v * 4 / 5, v * 3 / 5 + 20, 255}) bytes.push_back(static_cast<char>(c));
        }
    const render::Image picture = render::Image::frombytes("RGBA", render::Size{80, 80}, bytes);
    return core::CommandBus(render::ops_registry())
        .apply(doc, Json::array({Json{{"op", "add_layer"}, {"page", 1}, {"kind", "paint"}, {"id", "pic"}},
                                 Json{{"op", "set_layer"}, {"page", 1}, {"id", "pic"}, {"panel_clip", false}},
                                 Json{{"op", "put_raster"}, {"page", 1}, {"id", "pic"}, {"png_base64", core::b64encode(render::write_png(picture))}}}),
               core::Actor("human:tester"))
        .doc;
}

// A book of one page with a precise paint layer "deep" of side × side pixels (a pattern): a heavy Gaussian's.
core::Document deep_doc(int side) {
    auto doc = core::new_episode("重い", core::Num(1), 1, core::PageSpec::custom(side * 25.4 / 200, side * 25.4 / 200, 30, 30, 1, 2, 2, 2, 2, 72, "color"));
    doc.edit_page(0).layers.clear();
    doc.edit_page(0).frames.clear();
    Json pixels = Json::array();
    for (int y = 0; y < side; ++y)
        for (int x = 0; x < side; ++x)
            for (const int v : {(x * 37) % 65536, (y * 53) % 65536, ((x ^ y) * 97) % 65536, 65535}) pixels.push_back(v);
    doc = core::CommandBus().apply(doc, Json::array({Json{{"op", "put_color_raster"}, {"page", 1}, {"width", side}, {"height", side}, {"precision", "u16"},
                                                          {"pixels", pixels}}}), core::Actor("human:tester")).doc;
    doc.edit_page(0).layers[0].id = "deep";
    return doc;
}

struct Book {
    QTemporaryDir tmp;
    std::shared_ptr<app::Session> session;
    std::unique_ptr<app::MainWindow> window;
    explicit Book(const core::Document& doc = book_doc()) {
        (void)gui_test::config_folder();
        const auto path = gui_test::path_of(tmp.path() + "/book");
        gui_test::write_book(path, doc);
        session = app::Session::open(path, gui_test::quick(gui_test::path_of(tmp.path() + "/recovery")));
        window = std::make_unique<app::MainWindow>(session);
        window->resize(1200, 800);
        window->show();
        QCoreApplication::processEvents();
    }
    const core::Page& page() const { return session->document().page(0); }
    const core::Layer* layer(const std::string& id) const {
        for (const auto& l : page().layers)
            if (l.id == id) return &l;
        return nullptr;
    }
    QAction* act(const char* name) const { return window->action(QString::fromLatin1(name)); }
    QMenu* menu(const char* name) const { return window->findChild<QMenu*>(QString::fromLatin1(name)); }
};

QStringList menus_of(app::MainWindow* w, QAction* a) {
    QStringList out;
    for (QMenu* menu : w->findChildren<QMenu*>())
        if (menu->actions().contains(a)) out << menu->title();
    return out;
}

// The words of a menu's commands and submenus in order ("-" a separator; a submenu "▸ <title>").
QStringList words_of(QMenu* menu) {
    QStringList out;
    for (QAction* a : menu->actions()) {
        if (a->isSeparator()) out << QStringLiteral("-");
        else if (a->menu() != nullptr) out << QStringLiteral("▸ ") + a->menu()->title();
        else out << a->text();
    }
    return out;
}

// `then` once a heavy filter's progress shows (over every window of the app).
void when_working(std::function<void()> then) {
    auto* timer = new QTimer;
    timer->setInterval(10);
    QObject::connect(timer, &QTimer::timeout, timer, [timer, then] {
        for (QWidget* w : QApplication::topLevelWidgets()) {
            auto* progress = qobject_cast<QProgressDialog*>(w);
            if (progress == nullptr || progress->objectName() != QStringLiteral("filter_progress") || !progress->isVisible()) continue;
            if (progress->windowModality() != Qt::ApplicationModal) qWarning("the filter's progress does not hold every window");
            timer->stop();
            timer->deleteLater();
            then();
            return;
        }
    });
    timer->start();
}

// The window's answer: the filter window of `kind`, its fields set by `fill`, accepted (any other: stopped).
std::function<int(QDialog*)> answer(const QString& kind, std::function<void(QDialog*)> fill = {}) {
    return [kind, fill](QDialog* dialog) {
        if (dialog->objectName() != QStringLiteral("filter_dialog") || dialog->property("kind").toString() != kind) return int(QDialog::Rejected);
        if (fill) fill(dialog);
        return int(QDialog::Accepted);
    };
}

}  // namespace

class TestGuiFilters : public QObject {
    Q_OBJECT

private slots:
    void initTestCase() { (void)gui_test::config_folder(); }

    // The menu bar: フィルター after レイヤー, its commands and submenus in Photoshop's order and words, their keys.
    void theMenu() {
        Book book;
        app::MainWindow* w = book.window.get();
        QStringList bar;
        for (QAction* a : w->menuBar()->actions()) bar << a->text();
        QCOMPARE(bar.indexOf(QStringLiteral("フィルター")), bar.indexOf(QStringLiteral("レイヤー")) + 1);
        QMenu* filter = book.menu("menu_filter");
        QVERIFY(filter != nullptr);
        QCOMPARE(words_of(filter),
                 (QStringList{QStringLiteral("フィルターの再実行"), QStringLiteral("スマートフィルター用に変換"), QStringLiteral("-"),
                              QStringLiteral("フィルターギャラリー…"), QStringLiteral("広角補正…"), QStringLiteral("Camera Raw フィルター…"),
                              QStringLiteral("レンズ補正…"), QStringLiteral("ゆがみ…"), QStringLiteral("消点…"), QStringLiteral("-"),
                              QStringLiteral("▸ シャープ"), QStringLiteral("▸ ノイズ"), QStringLiteral("▸ ピクセレート"), QStringLiteral("▸ ビデオ"),
                              QStringLiteral("▸ ぼかし"), QStringLiteral("▸ ぼかしギャラリー"), QStringLiteral("▸ 表現手法"), QStringLiteral("▸ 描画"),
                              QStringLiteral("▸ 変形"), QStringLiteral("▸ その他"), QStringLiteral("-"), QStringLiteral("▸ 漫画"),
                              QStringLiteral("▸ プラグイン")}));
        QCOMPARE(words_of(book.menu("menu_filter_blur")),
                 (QStringList{QStringLiteral("ぼかし"), QStringLiteral("ぼかし（強）"), QStringLiteral("ぼかし（ガウス）…"), QStringLiteral("ぼかし（シェイプ）…"),
                              QStringLiteral("ぼかし（ボックス）…"), QStringLiteral("ぼかし（レンズ）…"), QStringLiteral("ぼかし（移動）…"),
                              QStringLiteral("ぼかし（詳細）…"), QStringLiteral("ぼかし（表面）…"), QStringLiteral("ぼかし（放射状）…"), QStringLiteral("平均")}));
        QCOMPARE(words_of(book.menu("menu_filter_sharpen")),
                 (QStringList{QStringLiteral("アンシャープマスク…"), QStringLiteral("シャープ"), QStringLiteral("シャープ（強）"), QStringLiteral("シャープ（輪郭のみ）"),
                              QStringLiteral("スマートシャープ…")}));
        QCOMPARE(words_of(book.menu("menu_filter_noise")),
                 (QStringList{QStringLiteral("ダスト＆スクラッチ…"), QStringLiteral("ノイズを加える…"), QStringLiteral("ノイズを軽減…"), QStringLiteral("明るさの中間値…"),
                              QStringLiteral("輪郭以外をぼかす")}));
        QCOMPARE(words_of(book.menu("menu_filter_render")),
                 (QStringList{QStringLiteral("炎…"), QStringLiteral("額縁…"), QStringLiteral("木…"), QStringLiteral("-"), QStringLiteral("ファイバー…"),
                              QStringLiteral("雲模様 1"), QStringLiteral("雲模様 2"), QStringLiteral("照明効果…"), QStringLiteral("逆光…")}));
        // (D2's: Photoshop's words and order)
        QCOMPARE(words_of(book.menu("menu_filter_pixelate")),
                 (QStringList{QStringLiteral("カラーハーフトーン…"), QStringLiteral("ぶれ"), QStringLiteral("メゾティント…"), QStringLiteral("モザイク…"),
                              QStringLiteral("水晶…"), QStringLiteral("点描…"), QStringLiteral("ファセット")}));
        QCOMPARE(words_of(book.menu("menu_filter_stylize")),
                 (QStringList{QStringLiteral("エンボス…"), QStringLiteral("ソラリゼーション"), QStringLiteral("タイル…"), QStringLiteral("押し出し…"),
                              QStringLiteral("拡散…"), QStringLiteral("風…"), QStringLiteral("輪郭検出"), QStringLiteral("輪郭のトレース…"), QStringLiteral("油彩…")}));
        QCOMPARE(words_of(book.menu("menu_filter_distort")),
                 (QStringList{QStringLiteral("シアー…"), QStringLiteral("ジグザグ…"), QStringLiteral("つまむ…"), QStringLiteral("波形…"), QStringLiteral("波紋…"),
                              QStringLiteral("球面…"), QStringLiteral("極座標…"), QStringLiteral("置き換え…"), QStringLiteral("渦巻き…")}));
        QCOMPARE(words_of(book.menu("menu_filter_manga")),
                 (QStringList{QStringLiteral("線画抽出…"), QStringLiteral("ゴミ取り…"), QStringLiteral("雨…"), QStringLiteral("光彩…"), QStringLiteral("白黒にする…")}));
        QCOMPARE(words_of(book.menu("menu_filter_plugins")), (QStringList{QStringLiteral("プラグインのフォルダーを開く"), QStringLiteral("プラグインの設定…")}));
        for (const char* key : {"menu_filter_pixelate", "menu_filter_video", "menu_filter_blur_gallery", "menu_filter_stylize", "menu_filter_distort", "menu_filter_other"})
            QVERIFY2(book.menu(key) != nullptr && !book.menu(key)->isEmpty(), key);
        // the keys (Photoshop's; 前回のフィルター Ctrl+F — Ctrl+Alt+F is 台詞の検索・置換 —, no key for レンズ補正…)
        QCOMPARE(book.act("act_filter_last")->shortcut(), QKeySequence(QStringLiteral("Ctrl+F")));
        QCOMPARE(book.act("act_filter_camera_raw")->shortcut(), QKeySequence(QStringLiteral("Ctrl+Shift+A")));
        QCOMPARE(book.act("act_filter_adaptive_wide_angle")->shortcut(), QKeySequence(QStringLiteral("Ctrl+Alt+Shift+A")));
        QCOMPARE(book.act("act_filter_vanishing_point")->shortcut(), QKeySequence(QStringLiteral("Ctrl+Alt+V")));
        QVERIFY(book.act("act_filter_lens_correction")->shortcuts().isEmpty());
        // ゆがみ… moved from 編集 (its id and key as they were); スマートフィルター用に変換 in both places
        QVERIFY(menus_of(w, book.act("act_liquify_filter")).contains(QStringLiteral("フィルター")));
        QVERIFY(!menus_of(w, book.act("act_liquify_filter")).contains(QStringLiteral("編集")));
        QCOMPARE(book.act("act_liquify_filter")->shortcut(), QKeySequence(QStringLiteral("Ctrl+Shift+X")));
        QVERIFY(menus_of(w, book.act("act_smart_filters_convert")).contains(QStringLiteral("フィルター")));
        QVERIFY(menus_of(w, book.act("act_smart_filters_convert")).contains(QStringLiteral("スマートフィルター")));
        // in this build: the blurs of D0, D1's ぼかし・シャープ・ノイズ・その他・ビデオ, D3's フィルターギャラリー (and Genko's own); the rest grey until their groups
        w->set_target_layer("pic");
        emit filter->aboutToShow();
        for (const char* on : {"act_filter_blur", "act_filter_blur_more", "act_filter_blur_gaussian", "act_filter_blur_box", "act_filter_manga_glow",
                               "act_filter_manga_lineart", "act_filter_blur_motion", "act_filter_blur_radial", "act_filter_blur_average", "act_filter_blur_lens",
                               "act_filter_blur_surface", "act_filter_blur_smart", "act_filter_blur_shape",
                               "act_filter_sharpen", "act_filter_sharpen_more", "act_filter_sharpen_edges", "act_filter_sharpen_unsharp_mask",
                               "act_filter_sharpen_smart", "act_filter_despeckle", "act_filter_noise_add", "act_filter_noise_median",
                               "act_filter_noise_dust_and_scratches", "act_filter_noise_reduce", "act_filter_other_custom", "act_filter_other_high_pass",
                               "act_filter_other_hsb_hsl", "act_filter_other_maximum", "act_filter_other_minimum", "act_filter_other_offset",
                               "act_filter_video_de_interlace", "act_filter_video_ntsc_colors", "act_filter_gallery",
                               // (D2: 変形・ピクセレート・表現手法)
                               "act_filter_distort_shear", "act_filter_distort_zigzag", "act_filter_distort_pinch", "act_filter_distort_wave",
                               "act_filter_distort_ripple", "act_filter_distort_spherize", "act_filter_distort_polar_coordinates", "act_filter_distort_displace",
                               "act_filter_distort_twirl", "act_filter_pixelate_color_halftone", "act_filter_pixelate_fragment", "act_filter_pixelate_mezzotint",
                               "act_filter_pixelate_mosaic", "act_filter_pixelate_crystallize", "act_filter_pixelate_pointillize", "act_filter_pixelate_facet",
                               "act_filter_stylize_emboss", "act_filter_stylize_solarize", "act_filter_stylize_tiles", "act_filter_stylize_extrude",
                               "act_filter_stylize_diffuse", "act_filter_stylize_wind", "act_filter_stylize_find_edges", "act_filter_stylize_trace_contour",
                               "act_filter_stylize_oil_paint",
                               // (D6: レンズ補正・広角補正・消点)
                               "act_filter_lens_correction", "act_filter_adaptive_wide_angle", "act_filter_vanishing_point",
                               // (D5: Camera Raw フィルター)
                               "act_filter_camera_raw",
                               // (D4: 描画・ぼかしギャラリー)
                               "act_filter_render_flame", "act_filter_render_picture_frame", "act_filter_render_tree", "act_filter_render_fibers",
                               "act_filter_render_clouds", "act_filter_render_difference_clouds", "act_filter_render_lighting_effects",
                               "act_filter_render_lens_flare", "act_filter_blur_field", "act_filter_blur_iris", "act_filter_blur_tilt_shift",
                               "act_filter_blur_path", "act_filter_blur_spin"})
            QVERIFY2(book.act(on)->isEnabled(), on);
        for (const char* off : {"act_filter_last"})
            QVERIFY2(!book.act(off)->isEnabled(), off);
        // 履歴's words of the new ops
        QCOMPARE(app::history::describe(Json::array({Json{{"op", "render_picture"}}})), QStringLiteral("描画した"));
    }

    // ぼかし（ガウス）…: its window from the table (半径, 既定に戻す) with the result on the page while it is open, one op, one Undo;
    // then 前回のフィルター sends it again without a window, and フェード… is on after it.
    void aKindsWindowThenTheLastFilter() {
        Book book;
        gui_test::Answers answers;
        book.window->set_target_layer("pic");
        const core::Bytes before = book.layer("pic")->raster_png;
        auto* canvas = book.window->canvas();
        bool previewed = false;
        int windows = 0;
        answers.responder->exec = answer(QStringLiteral("blur_gaussian"), [&](QDialog* d) {
            ++windows;
            QCOMPARE(d->windowTitle(), QStringLiteral("ぼかし（ガウス）"));
            auto* radius = d->findChild<QDoubleSpinBox*>(QStringLiteral("field_radius"));
            QVERIFY(radius != nullptr);
            QCOMPARE(radius->value(), 1.0);
            QCOMPARE(radius->decimals(), 1);
            radius->setValue(4.5);
            QVERIFY(d->findChild<QPushButton*>(QStringLiteral("filter_reset")) != nullptr);
            previewed = gui_test::wait_for([&] { return canvas->doc() != book.session->snapshot(); }, 10000);
        });
        book.act("act_filter_blur_gaussian")->trigger();
        QVERIFY(previewed);
        QCOMPARE(windows, 1);
        const core::Bytes once = book.layer("pic")->raster_png;
        QVERIFY(once != before);
        QTRY_VERIFY(canvas->doc() == book.session->snapshot());
        // 前回のフィルター: its name, the same settings again without the window
        emit book.menu("menu_filter")->aboutToShow();
        QVERIFY(book.act("act_filter_last")->isEnabled());
        QCOMPARE(book.act("act_filter_last")->text(), QStringLiteral("ぼかし（ガウス）"));
        book.act("act_filter_last")->trigger();
        QCOMPARE(windows, 1);
        QVERIFY(book.layer("pic")->raster_png != once);
        // フェード… right after it
        QVERIFY(book.act("act_fade")->isEnabled());
        // one op each: undone twice, the pixels as they were
        book.window->action(QStringLiteral("act_undo"))->trigger();
        QCOMPARE(book.layer("pic")->raster_png, once);
        book.window->action(QStringLiteral("act_undo"))->trigger();
        QCOMPARE(book.layer("pic")->raster_png, before);
        // stopped: nothing changes
        answers.responder->exec = [](QDialog*) { return int(QDialog::Rejected); };
        book.act("act_filter_blur_box")->trigger();
        QCOMPARE(book.layer("pic")->raster_png, before);
    }

    // ぼかし: a preset without a window (blur_gaussian 0.6); the same as the op itself.
    void aPresetWithoutAWindow() {
        Book book;
        gui_test::Answers answers;
        int windows = 0;
        answers.responder->exec = [&](QDialog*) {
            ++windows;
            return int(QDialog::Rejected);
        };
        book.window->set_target_layer("pic");
        const core::Document start = *book.session->snapshot();
        book.act("act_filter_blur")->trigger();
        QCOMPARE(windows, 0);
        const auto expected = core::CommandBus(render::ops_registry())
                                  .apply(start, Json::array({Json{{"op", "filter_raster"}, {"page", 1}, {"id", "pic"}, {"kind", "blur_gaussian"}, {"radius", 0.6}}}),
                                         core::Actor("human:tester"))
                                  .doc;
        QCOMPARE(*book.layer("pic")->raster_png, *expected.page(0).layers.back().raster_png);
        emit book.menu("menu_filter")->aboutToShow();
        QCOMPARE(book.act("act_filter_last")->text(), QStringLiteral("ぼかし"));
    }

    // On a smart object: a smart filter (its kind and settings kept), its window again from the panel; 漫画's 光彩… in its old
    // window; with no paint layer drawn on, said so.
    void onASmartObjectAndGenkosOwn() {
        Book book;
        gui_test::Answers answers;
        QVERIFY(book.window->apply_ops(Json::array({Json{{"op", "smart_convert"}, {"page", 1}, {"ids", Json::array({"pic"})}, {"new_id", "so"}}})));
        book.window->set_target_layer("so");
        answers.responder->exec = answer(QStringLiteral("blur_box"), [](QDialog* d) { d->findChild<QDoubleSpinBox*>(QStringLiteral("field_radius"))->setValue(3); });
        book.act("act_filter_blur_box")->trigger();
        const Json filters = (*book.layer("so")->smart)["filters"];
        QCOMPARE(filters.size(), std::size_t(1));
        QCOMPARE(filters[0]["kind"], Json("blur_box"));
        QCOMPARE(filters[0]["params"], (Json{{"radius", 3}}));
        // (its window again, the settings it has)
        double shown = 0;
        answers.responder->exec = answer(QStringLiteral("blur_box"), [&](QDialog* d) {
            auto* radius = d->findChild<QDoubleSpinBox*>(QStringLiteral("field_radius"));
            shown = radius->value();
            radius->setValue(5);
        });
        book.window->layer_panel()->edit_smart_filter(0);
        QCOMPARE(shown, 3.0);
        QCOMPARE((*book.layer("so")->smart)["filters"][0]["params"], (Json{{"radius", 5}}));
        // 光彩… (Genko's glow, its old window) as a smart filter too
        answers.responder->exec = [](QDialog* d) { return d->objectName() == QStringLiteral("filter_dialog") ? int(QDialog::Accepted) : int(QDialog::Rejected); };
        book.act("act_filter_manga_glow")->trigger();
        QCOMPARE((*book.layer("so")->smart)["filters"].size(), std::size_t(2));
        QCOMPARE((*book.layer("so")->smart)["filters"][1]["kind"], Json("glow"));
    }

    // A pen layer's lines: asked first that they become pixels (as the layer panel's かける… did) — no: nothing changes;
    // yes: the filter.
    void aPenLayersLinesAreAskedFirst() {
        Book book;
        gui_test::Answers answers;
        QVERIFY(book.window->apply_ops(Json::array({Json{{"op", "add_stroke"}, {"page", 1}, {"layer", "ink"},
                                                         {"points", Json::array({Json::array({10, 10}), Json::array({30, 30})})}, {"width_mm", 1}}})));
        std::string pen;
        for (const auto& l : book.page().layers)
            if (l.stroke_count() > 0) pen = l.id;
        QVERIFY(!pen.empty());
        book.window->set_target_layer(pen);
        book.act("act_filter_blur")->trigger();
        QCOMPARE(answers.asked.size(), qsizetype(1));
        QVERIFY2(answers.asked[0].contains(QStringLiteral("ペンの線は画像になり")), qPrintable(answers.asked[0]));
        QVERIFY(book.layer(pen)->stroke_count() > 0);
        answers.responder->question = [](const QString&, const QString&) { return true; };
        book.act("act_filter_blur")->trigger();
        QCOMPARE(book.layer(pen)->stroke_count(), std::size_t(0));
        QVERIFY(book.layer(pen)->raster_png != nullptr);
    }

    // 前回のフィルター's name as a command stays フィルターの再実行 (review P2-2): a key the person gave it is kept when filters
    // change its words in the menu, and when the window is made again (a restart); its words are the last one's only while
    // the menu is open; no two commands come to share a name (ぼかし).
    void theLastFiltersKeyIsKept() {
        app::settings()->setValue(QStringLiteral("shortcuts/フィルターの再実行"), QStringLiteral("Ctrl+Alt+Shift+L"));
        {
            Book book;
            gui_test::Answers answers;
            QCOMPARE(book.act("act_filter_last")->shortcut(), QKeySequence(QStringLiteral("Ctrl+Alt+Shift+L")));
            book.window->set_target_layer("pic");
            book.act("act_filter_blur")->trigger();
            QMenu* menu = book.menu("menu_filter");
            emit menu->aboutToShow();
            QCOMPARE(book.act("act_filter_last")->text(), QStringLiteral("ぼかし"));
            emit menu->aboutToHide();
            QCOMPARE(book.act("act_filter_last")->text(), QStringLiteral("フィルターの再実行"));
            const auto names = app::command_names(book.window.get());
            QCOMPARE(app::command_name(names, book.act("act_filter_last")), QStringLiteral("フィルターの再実行"));
            QCOMPARE(app::command_name(names, book.act("act_filter_blur")), QStringLiteral("ぼかし"));
            app::preferences::apply_all(book.window.get());
            QCOMPARE(book.act("act_filter_last")->shortcut(), QKeySequence(QStringLiteral("Ctrl+Alt+Shift+L")));
        }
        {
            Book again;
            QCOMPARE(again.act("act_filter_last")->shortcut(), QKeySequence(QStringLiteral("Ctrl+Alt+Shift+L")));
        }
        app::settings()->remove(QStringLiteral("shortcuts/フィルターの再実行"));
    }

    // On a smart object a filter is added as Photoshop adds it, not asked — but when a selection would not be used, the
    // smart object holding a filter mask already (review P3-7): said and asked.
    void aSmartObjectsMaskAlreadyThereIsSaid() {
        Book book;
        gui_test::Answers answers;
        QVERIFY(book.window->apply_ops(Json::array({Json{{"op", "smart_convert"}, {"page", 1}, {"ids", Json::array({"pic"})}, {"new_id", "so"}}})));
        book.window->set_target_layer("so");
        book.act("act_filter_blur")->trigger();  // (no selection, no mask: not asked)
        QVERIFY(answers.asked.isEmpty());
        QCOMPARE((*book.layer("so")->smart)["filters"].size(), std::size_t(1));
        QVERIFY(book.window->apply_ops(Json::array({Json{{"op", "smart_filter_add"}, {"page", 1}, {"id", "so"}, {"kind", "blur_box"}, {"radius", 2},
                                                         {"area", Json{{"rect", Json::array({5, 5, 10, 10})}}}}})));
        QVERIFY(book.layer("so")->smart->contains("filter_mask"));
        book.act("act_select_all")->trigger();
        book.act("act_filter_blur")->trigger();  // (asked; no: nothing added)
        QCOMPARE(answers.asked.size(), qsizetype(1));
        QVERIFY2(answers.asked[0].contains(QStringLiteral("フィルターマスクが既にある")), qPrintable(answers.asked[0]));
        QCOMPARE((*book.layer("so")->smart)["filters"].size(), std::size_t(2));
        answers.responder->question = [](const QString&, const QString&) { return true; };
        book.act("act_filter_blur")->trigger();
        QCOMPARE((*book.layer("so")->smart)["filters"].size(), std::size_t(3));
    }

    // Closing while a heavy one is worked (review P1-1): its window over every window; the window's own close and Genko を
    // 終わる (every window, a second one of the same book among them) ask the work to stop — nothing changed, nothing
    // falls —, and the window closes once the thread is back; a filter stopped is not 前回のフィルター.
    void closingWhileAHeavyOneIsWorked() {
        const auto heavy = [](QDialog* d) { d->findChild<QDoubleSpinBox*>(QStringLiteral("field_radius"))->setValue(60); };
        {  // the window's own close
            Book book(deep_doc(1400));
            gui_test::Answers answers;
            answers.responder->exec = answer(QStringLiteral("blur_gaussian"), heavy);
            book.window->set_target_layer("deep");
            const auto before = book.layer("deep")->color_raster;
            bool closed = false;
            when_working([&] {
                closed = true;
                book.window->close();
            });
            book.act("act_filter_blur_gaussian")->trigger();
            QVERIFY(closed);
            QCOMPARE(book.layer("deep")->color_raster, before);
            QTRY_VERIFY(!book.window->isVisible());
            QVERIFY(!book.act("act_filter_last")->statusTip().contains(QStringLiteral("ぼかし（ガウス）")));  // (stopped: not remembered)
        }
        {  // Genko を終わる with a second window of the book
            Book book(deep_doc(1400));
            gui_test::Answers answers;
            answers.responder->exec = answer(QStringLiteral("blur_gaussian"), heavy);
            book.window->set_target_layer("deep");
            QPointer<app::MainWindow> second = book.window->new_window();
            QVERIFY(second);
            const auto before = book.layer("deep")->color_raster;
            bool quit = false;
            when_working([&] {
                quit = true;
                QApplication::closeAllWindows();
            });
            book.act("act_filter_blur_gaussian")->trigger();
            QVERIFY(quit);
            QCOMPARE(book.layer("deep")->color_raster, before);
            QCoreApplication::processEvents();
            if (second) second->close();
            QTRY_VERIFY(!second);
        }
    }

    // The book changed while a heavy one is worked (another window of it, an agent's change taken in — review P2-3): worked
    // again on the book as it is now, on the thread; both the change and the filter in the book.
    void aBookChangedMeanwhileIsWorkedAgain() {
        Book book(deep_doc(1400));
        gui_test::Answers answers;
        answers.responder->exec = answer(QStringLiteral("blur_gaussian"), [](QDialog* d) { d->findChild<QDoubleSpinBox*>(QStringLiteral("field_radius"))->setValue(60); });
        book.window->set_target_layer("deep");
        const core::Document start = *book.session->snapshot();
        bool changed = false;
        when_working([&] { changed = book.window->apply_ops(Json::array({Json{{"op", "add_layer"}, {"page", 1}, {"kind", "paint"}, {"id", "other"}}})); });
        book.act("act_filter_blur_gaussian")->trigger();
        QVERIFY(changed);
        QVERIFY(book.layer("other") != nullptr);
        const auto expected = core::CommandBus(render::ops_registry())
                                  .apply(start, Json::array({Json{{"op", "filter_raster"}, {"page", 1}, {"id", "deep"}, {"kind", "blur_gaussian"}, {"radius", 60}}}),
                                         core::Actor("human:tester"))
                                  .doc;
        QCOMPARE(*book.layer("deep")->color_raster, *expected.page(0).layers[0].color_raster);
        emit book.menu("menu_filter")->aboutToShow();
        QCOMPARE(book.act("act_filter_last")->text(), QStringLiteral("ぼかし（ガウス）"));
        emit book.menu("menu_filter")->aboutToHide();
    }

    // D1: カスタム…'s window — its 5 × 5 grid (the centre 1), 拡大・縮小, オフセット — sends its kernel; 平均 and シャープ are sent
    // without a window, the same as their ops; アンシャープマスク…'s window in Photoshop's words.
    void theWindowsOfD1() {
        Book book;
        gui_test::Answers answers;
        book.window->set_target_layer("pic");
        const core::Document start = *book.session->snapshot();
        bool shown = false;
        answers.responder->exec = answer(QStringLiteral("other_custom"), [&](QDialog* d) {
            QCOMPARE(d->windowTitle(), QStringLiteral("カスタム"));
            auto* centre = d->findChild<QSpinBox*>(QStringLiteral("kernel_12"));
            auto* right = d->findChild<QSpinBox*>(QStringLiteral("kernel_13"));
            QVERIFY(centre != nullptr && right != nullptr && d->findChild<QDoubleSpinBox*>(QStringLiteral("field_scale")) != nullptr);
            QCOMPARE(centre->value(), 1);
            QCOMPARE(right->value(), 0);
            centre->setValue(0);
            right->setValue(1);
            shown = true;
        });
        book.act("act_filter_other_custom")->trigger();
        QVERIFY(shown);
        Json kernel = Json::array();
        for (int i = 0; i < 25; ++i) kernel.push_back(i == 13 ? 1 : 0);
        const auto expected = core::CommandBus(render::ops_registry())
                                  .apply(start, Json::array({Json{{"op", "filter_raster"}, {"page", 1}, {"id", "pic"}, {"kind", "other_custom"},
                                                                  {"kernel", kernel}, {"scale", 1}, {"offset", 0}}}),
                                         core::Actor("human:tester"))
                                  .doc;
        QCOMPARE(*book.layer("pic")->raster_png, *expected.page(0).layers.back().raster_png);
        emit book.menu("menu_filter")->aboutToShow();
        QCOMPARE(book.act("act_filter_last")->text(), QStringLiteral("カスタム"));
        emit book.menu("menu_filter")->aboutToHide();
        book.window->action(QStringLiteral("act_undo"))->trigger();
        // 平均 and シャープ: no window
        int windows = 0;
        answers.responder->exec = [&](QDialog*) {
            ++windows;
            return int(QDialog::Rejected);
        };
        // (シャープ first: after 平均 the layer is flat)
        for (const auto& [name, op] : {std::pair{"act_filter_sharpen", Json{{"kind", "sharpen_unsharp_mask"}, {"amount", 60}, {"radius", 0.5}, {"threshold", 0}}},
                                       std::pair{"act_filter_blur_average", Json{{"kind", "blur_average"}}}}) {
            const core::Document before = *book.session->snapshot();
            book.act(name)->trigger();
            Json sent{{"op", "filter_raster"}, {"page", 1}, {"id", "pic"}};
            sent.update(op);
            const auto done = core::CommandBus(render::ops_registry()).apply(before, Json::array({sent}), core::Actor("human:tester")).doc;
            QVERIFY2(*book.layer("pic")->raster_png == *done.page(0).layers.back().raster_png, name);  // (PhotoCraft's settings, sent as they are)
            if (std::string_view(name) == "act_filter_blur_average")  // (シャープ's 0.5 px hardly reaches this smooth picture's stored pixels)
                QVERIFY2(*book.layer("pic")->raster_png != *before.page(0).layers.back().raster_png, name);
        }
        QCOMPARE(windows, 0);
        // アンシャープマスク…: 量・半径・しきい値
        answers.responder->exec = answer(QStringLiteral("sharpen_unsharp_mask"), [&](QDialog* d) {
            QCOMPARE(d->windowTitle(), QStringLiteral("アンシャープマスク"));
            for (const char* field : {"field_amount", "field_radius", "field_threshold"}) QVERIFY2(d->findChild<QDoubleSpinBox*>(QString::fromLatin1(field)) != nullptr, field);
            QCOMPARE(d->findChild<QDoubleSpinBox*>(QStringLiteral("field_amount"))->value(), 50.0);
            ++windows;
        });
        book.act("act_filter_sharpen_unsharp_mask")->trigger();
        QCOMPARE(windows, 1);
    }

    // ノイズを加える…: a new seed each time it is applied — the window's, again, 前回のフィルター's — held in the op (on a smart
    // object: in its smart filters); ぼかし（レンズ）… on a smart object does not offer the layer mask as its depth (said
    // under the fields), on a paint layer it does (review P3-7, P3-4).
    void aSeedEachTimeAndTheLensOnASmartObject() {
        Book book;
        gui_test::Answers answers;
        // (a paint layer: two applications from the same pixels differ)
        book.window->set_target_layer("pic");
        answers.responder->exec = answer(QStringLiteral("noise_add"));
        book.act("act_filter_noise_add")->trigger();
        const core::Bytes first = book.layer("pic")->raster_png;
        book.window->action(QStringLiteral("act_undo"))->trigger();
        book.act("act_filter_noise_add")->trigger();
        QVERIFY(book.layer("pic")->raster_png != first);
        book.window->action(QStringLiteral("act_undo"))->trigger();
        // (a smart object: each smart filter's seed)
        QVERIFY(book.window->apply_ops(Json::array({Json{{"op", "smart_convert"}, {"page", 1}, {"ids", Json::array({"pic"})}, {"new_id", "so"}}})));
        book.window->set_target_layer("so");
        book.act("act_filter_noise_add")->trigger();
        book.act("act_filter_noise_add")->trigger();
        emit book.menu("menu_filter")->aboutToShow();
        book.act("act_filter_last")->trigger();
        emit book.menu("menu_filter")->aboutToHide();
        const Json filters = (*book.layer("so")->smart)["filters"];
        QCOMPARE(filters.size(), std::size_t(3));
        std::vector<Json> seeds;
        for (const Json& f : filters) {
            QVERIFY(f["params"].contains("seed"));
            seeds.push_back(f["params"]["seed"]);
        }
        QVERIFY(seeds[0] != seeds[1] && seeds[1] != seeds[2] && seeds[0] != seeds[2]);
        // ぼかし（レンズ）…'s depth: なし・透明部分 on a smart object, said; なし・透明部分・レイヤーマスク on a paint layer
        QStringList shown;
        bool said = false;
        answers.responder->exec = [&](QDialog* d) {
            if (d->property("kind").toString() != QStringLiteral("blur_lens")) return int(QDialog::Rejected);
            auto* depth = d->findChild<QComboBox*>(QStringLiteral("field_depth_map"));
            if (depth != nullptr)
                for (int i = 0; i < depth->count(); ++i) shown << depth->itemText(i);
            for (QLabel* label : d->findChildren<QLabel*>())
                if (label->text().contains(QStringLiteral("スマートフィルターでは、深度情報のソースにレイヤーマスクは選べません"))) said = true;
            return int(QDialog::Rejected);
        };
        book.act("act_filter_blur_lens")->trigger();
        QCOMPARE(shown, (QStringList{QStringLiteral("なし"), QStringLiteral("透明部分")}));
        QVERIFY(said);
        book.window->action(QStringLiteral("act_undo"))->trigger();
        book.window->action(QStringLiteral("act_undo"))->trigger();
        book.window->action(QStringLiteral("act_undo"))->trigger();
        book.window->action(QStringLiteral("act_undo"))->trigger();
        QVERIFY(book.layer("pic") != nullptr);
        book.window->set_target_layer("pic");
        shown.clear();
        said = false;
        book.act("act_filter_blur_lens")->trigger();
        QCOMPARE(shown, (QStringList{QStringLiteral("なし"), QStringLiteral("透明部分"), QStringLiteral("レイヤーマスク")}));
        QVERIFY(!said);
    }

    // The layer panel's かける…: the フィルター menu (the user's decision 1).
    void thePanelOpensTheMenu() {
        Book book;
        auto* panel = book.window->layer_panel();
        QVERIFY(panel->filter_button() != nullptr);
        QCOMPARE(panel->filter_button()->text(), QStringLiteral("かける…"));
        panel->apply_filter();
        QTRY_VERIFY(book.menu("menu_filter")->isVisible());
        book.menu("menu_filter")->close();
    }

    // A heavy one (a Gaussian of 60 on a precise layer of 1400 × 1400): its preview worked on a thread (the window says
    // so), sent on a thread with its progress — the same pixels as the op —, and 中止 leaving the book as it was.
    void aHeavyOneOnAThread() {
        Book book(deep_doc(1400));
        gui_test::Answers answers;
        book.window->set_target_layer("deep");
        const core::Document start = *book.session->snapshot();
        auto* canvas = book.window->canvas();
        bool busy_seen = false, previewed = false;
        answers.responder->exec = answer(QStringLiteral("blur_gaussian"), [&](QDialog* d) {
            d->findChild<QDoubleSpinBox*>(QStringLiteral("field_radius"))->setValue(60);
            auto* busy = d->findChild<QLabel*>(QStringLiteral("filter_preview_busy"));
            QVERIFY(busy != nullptr);
            busy_seen = gui_test::wait_for([&] { return !busy->isHidden(); }, 10000);  // (the window is answered, not shown)
            previewed = gui_test::wait_for([&] { return canvas->doc() != book.session->snapshot(); }, 60000);
        });
        book.act("act_filter_blur_gaussian")->trigger();
        QVERIFY(busy_seen);
        QVERIFY(previewed);
        const auto expected = core::CommandBus(render::ops_registry())
                                  .apply(start, Json::array({Json{{"op", "filter_raster"}, {"page", 1}, {"id", "deep"}, {"kind", "blur_gaussian"}, {"radius", 60}}}),
                                         core::Actor("human:tester"))
                                  .doc;
        QCOMPARE(*book.layer("deep")->color_raster, *expected.page(0).layers[0].color_raster);
        book.window->action(QStringLiteral("act_undo"))->trigger();
        // 中止: its button pressed as soon as its progress shows
        bool pressed = false;
        QTimer press;
        press.setInterval(10);
        QObject::connect(&press, &QTimer::timeout, [&] {
            for (QWidget* w : QApplication::topLevelWidgets()) {
                auto* progress = qobject_cast<QProgressDialog*>(w);
                if (progress == nullptr || progress->objectName() != QStringLiteral("filter_progress") || !progress->isVisible()) continue;
                for (QPushButton* button : progress->findChildren<QPushButton*>()) {
                    if (button->text() != QStringLiteral("中止")) continue;
                    button->click();
                    pressed = true;
                    press.stop();
                }
            }
        });
        press.start();
        const auto before = book.layer("deep")->color_raster;
        book.act("act_filter_last")->trigger();
        press.stop();
        QVERIFY(pressed);
        QCOMPARE(book.layer("deep")->color_raster, before);
        QVERIFY2(book.window->last_notice().contains(QStringLiteral("中止")), qPrintable(book.window->last_notice()));
    }

    // フィルターギャラリー… (D3): its window — the 47 effects in Photoshop's six folders, each with its small picture made on
    // a thread while the window answers; an effect chosen from its folder or the list, its settings; the stack (新しいエフェ
    // クトレイヤー, another order, its eye, エフェクトレイヤーを削除); the result over the layer worked out on a thread — sent as
    // one op with the main and sub colours (the same pixels as the op itself), one Undo, 前回のフィルター; on a smart object a
    // smart filter, its window again from the layer panel with its stack.
    void theGallery() {
        Book book;
        gui_test::Answers answers;
        book.window->set_target_layer("pic");
        const core::Document start = *book.session->snapshot();
        const core::Bytes before = book.layer("pic")->raster_png;
        int windows = 0;
        std::function<void(QDialog*)> fill;
        answers.responder->exec = [&](QDialog* d) {
            if (d->objectName() != QStringLiteral("filter_gallery")) return int(QDialog::Rejected);
            ++windows;
            if (fill) fill(d);
            return int(QDialog::Accepted);
        };
        const auto previews = [](QDialog* d) { return d->property("previews").toInt(); };
        fill = [&](QDialog* d) {
            QCOMPARE(d->windowTitle(), QStringLiteral("フィルターギャラリー"));
            auto* folders = d->findChild<QTreeWidget*>(QStringLiteral("gallery_effects"));
            QVERIFY(folders != nullptr);
            QCOMPARE(folders->topLevelItemCount(), 6);
            QStringList names;
            QList<int> sizes;
            for (int i = 0; i < 6; ++i) {
                names << folders->topLevelItem(i)->text(0);
                sizes << folders->topLevelItem(i)->childCount();
            }
            QCOMPARE(names, (QStringList{QStringLiteral("アーティスティック"), QStringLiteral("ブラシストローク"), QStringLiteral("変形"), QStringLiteral("スケッチ"),
                                         QStringLiteral("表現手法"), QStringLiteral("テクスチャ")}));
            QCOMPARE(sizes, (QList<int>{15, 8, 3, 14, 1, 6}));
            QCOMPARE(folders->topLevelItem(0)->child(0)->text(0), QStringLiteral("色鉛筆"));
            QTreeWidgetItem* stamp_item = folders->topLevelItem(3)->child(11);
            QCOMPARE(stamp_item->text(0), QStringLiteral("スタンプ"));
            // the small pictures: every one made, on a thread (the window answering meanwhile)
            QVERIFY(gui_test::wait_for([&] { return d->property("thumbnails").toInt() == 47; }, 60000));
            QVERIFY(!stamp_item->icon(0).isNull());
            // the preview worked out
            auto* preview = d->findChild<QLabel*>(QStringLiteral("gallery_preview"));
            QVERIFY(preview != nullptr);
            QVERIFY(gui_test::wait_for([&] { return previews(d) >= 1; }, 30000));
            QVERIFY(!preview->pixmap().isNull());
            // one effect left, スタンプ from its folder
            auto* stack = d->findChild<QListWidget*>(QStringLiteral("gallery_stack"));
            auto* remove = d->findChild<QPushButton*>(QStringLiteral("gallery_remove"));
            while (stack->count() > 1) remove->click();
            QVERIFY(!remove->isEnabled());
            emit folders->itemClicked(stamp_item, 0);
            QCOMPARE(stack->item(0)->text(), QStringLiteral("スタンプ"));
            // 新しいエフェクトレイヤー: over it, カットアウト chosen from the list, its レベル数 3
            const int seen = previews(d);
            d->findChild<QPushButton*>(QStringLiteral("gallery_add"))->click();
            QCOMPARE(stack->count(), 2);
            auto* effect = d->findChild<QComboBox*>(QStringLiteral("gallery_effect"));
            const int cutout = effect->findData(QStringLiteral("cutout"));
            QVERIFY(cutout >= 0);
            emit effect->activated(cutout);
            QCOMPARE(stack->item(0)->text(), QStringLiteral("カットアウト"));
            QCOMPARE(stack->item(1)->text(), QStringLiteral("スタンプ"));
            auto* levels = d->findChild<QDoubleSpinBox*>(QStringLiteral("field_number_of_levels"));
            QVERIFY(levels != nullptr);
            QCOMPARE(levels->value(), 4.0);
            levels->setValue(3);
            QVERIFY(gui_test::wait_for([&] { return previews(d) > seen; }, 30000));
            // another order (dragged): スタンプ over カットアウト; its eye: スタンプ hidden
            QVERIFY(stack->model()->moveRow(QModelIndex(), 1, QModelIndex(), 0));
            QCOMPARE(stack->item(0)->text(), QStringLiteral("スタンプ"));
            stack->item(0)->setCheckState(Qt::Unchecked);
            // (a layer added and taken away again)
            d->findChild<QPushButton*>(QStringLiteral("gallery_add"))->click();
            QCOMPARE(stack->count(), 3);
            remove->click();
            QCOMPARE(stack->count(), 2);
        };
        book.act("act_filter_gallery")->trigger();
        QCOMPARE(windows, 1);
        // one op: the stack bottom up, the main and sub colours
        const auto& pen = book.window->pen().rgb;
        const std::array<int, 3> sub = book.window->colours()->sub_rgb();
        const Json effects = Json::array({Json{{"filter", "cutout"}, {"params", Json{{"number_of_levels", 3}}}}, Json{{"filter", "stamp"}, {"visible", false}}});
        const auto expected = core::CommandBus(render::ops_registry())
                                  .apply(start, Json::array({Json{{"op", "filter_raster"}, {"page", 1}, {"id", "pic"}, {"kind", "filter_gallery"}, {"effects", effects},
                                                                  {"foreground", Json::array({static_cast<int>(pen[0]), static_cast<int>(pen[1]), static_cast<int>(pen[2])})},
                                                                  {"background", Json::array({sub[0], sub[1], sub[2]})}}}),
                                         core::Actor("human:tester"))
                                  .doc;
        QVERIFY(book.layer("pic")->raster_png != before);
        QCOMPARE(*book.layer("pic")->raster_png, *expected.page(0).layers.back().raster_png);
        emit book.menu("menu_filter")->aboutToShow();
        QCOMPARE(book.act("act_filter_last")->text(), QStringLiteral("フィルターギャラリー"));
        emit book.menu("menu_filter")->aboutToHide();
        book.window->action(QStringLiteral("act_undo"))->trigger();
        QCOMPARE(book.layer("pic")->raster_png, before);
        // stopped: nothing changes
        fill = nullptr;
        answers.responder->exec = [&](QDialog* d) {
            if (d->objectName() == QStringLiteral("filter_gallery")) ++windows;
            return int(QDialog::Rejected);
        };
        book.act("act_filter_gallery")->trigger();
        QCOMPARE(windows, 2);
        QCOMPARE(book.layer("pic")->raster_png, before);
        // on a smart object: a smart filter of the stack (the one used last), its window again from the layer panel
        QVERIFY(book.window->apply_ops(Json::array({Json{{"op", "smart_convert"}, {"page", 1}, {"ids", Json::array({"pic"})}, {"new_id", "so"}}})));
        book.window->set_target_layer("so");
        answers.responder->exec = [&](QDialog* d) { return d->objectName() == QStringLiteral("filter_gallery") ? int(QDialog::Accepted) : int(QDialog::Rejected); };
        book.act("act_filter_gallery")->trigger();
        const Json filters = (*book.layer("so")->smart)["filters"];
        QCOMPARE(filters.size(), std::size_t(1));
        QCOMPARE(filters[0]["kind"], Json("filter_gallery"));
        QCOMPARE(filters[0]["params"]["effects"].size(), std::size_t(2));
        QCOMPARE(filters[0]["params"]["effects"][0]["filter"], Json("cutout"));
        int rows = 0;
        answers.responder->exec = [&](QDialog* d) {
            if (d->objectName() != QStringLiteral("filter_gallery")) return int(QDialog::Rejected);
            auto* stack = d->findChild<QListWidget*>(QStringLiteral("gallery_stack"));
            rows = stack->count();
            stack->setCurrentRow(0);  // (スタンプ, hidden: taken away)
            d->findChild<QPushButton*>(QStringLiteral("gallery_remove"))->click();
            return int(QDialog::Accepted);
        };
        book.window->layer_panel()->edit_smart_filter(0);
        QCOMPARE(rows, 2);
        const Json set = (*book.layer("so")->smart)["filters"];
        QCOMPARE(set.size(), std::size_t(1));
        QCOMPARE(set[0]["params"]["effects"].size(), std::size_t(1));
        QCOMPARE(set[0]["params"]["effects"][0]["filter"], Json("cutout"));
        QCOMPARE(set[0]["params"]["effects"][0]["params"]["number_of_levels"], Json(3));
        // set again without a change: nothing sent (no step in the history)
        const auto before_edit = book.session->snapshot();
        answers.responder->exec = [](QDialog* d) { return d->objectName() == QStringLiteral("filter_gallery") ? int(QDialog::Accepted) : int(QDialog::Rejected); };
        book.window->layer_panel()->edit_smart_filter(0);
        QVERIFY(book.session->snapshot() == before_edit);
    }

    // フィルターギャラリー's preview (review P2-2, P3-4): what the op lays on the same part of the layer — the same pixels away
    // from the part's edges, its marks where the op puts them —, the same page's place the same pixels whichever part is
    // looked at (dragged); inside the selection only, what is outside it as it was, deep inside it what the op makes there.
    void theGalleryPreviewIsTheResult() {
        // (a paint layer whose pixels are the page's at the working 200 dpi: 315 × 315 over 40 mm)
        auto doc = core::new_episode("ギャラリー", core::Num(1), 1, core::PageSpec::custom(40, 40, 30, 30, 1, 2, 2, 2, 2, 72, "color"));
        std::string bytes;
        for (int y = 0; y < 315; ++y)
            for (int x = 0; x < 315; ++x)
                for (const int c : {(x * 7 + y * 3) % 256, (x * x + y) % 200, y % 9 * 28, 255}) bytes.push_back(static_cast<char>(c));
        const render::Image picture = render::Image::frombytes("RGBA", render::Size{315, 315}, bytes);
        doc = core::CommandBus(render::ops_registry())
                  .apply(doc, Json::array({Json{{"op", "add_layer"}, {"page", 1}, {"kind", "paint"}, {"id", "pic"}},
                                           Json{{"op", "set_layer"}, {"page", 1}, {"id", "pic"}, {"panel_clip", false}},
                                           Json{{"op", "put_raster"}, {"page", 1}, {"id", "pic"}, {"png_base64", core::b64encode(render::write_png(picture))}}}),
                         core::Actor("human:tester"))
                  .doc;
        Book book(doc);
        book.window->set_target_layer("pic");
        const Json settings = core::normalize_filter(
            "filter_gallery",
            Json{{"effects", Json::array({Json{{"filter", "texturizer"}, {"params", Json{{"relief", 30}}}}, Json{{"filter", "stained_glass"}, {"params", Json{{"cell_size", 6}}}},
                                          Json{{"filter", "halftone_pattern"}, {"params", Json{{"pattern_type", "circle"}}}, {"visible", false}},
                                          Json{{"filter", "grain"}}})}},
            nullptr);
        const int halo = render::fx::kernel("filter_gallery")->halo(settings, render::fx::Ctx{}).radius;
        QVERIFY2(halo > 0 && 2 * halo + 20 <= 160, qPrintable(QString::number(halo)));  // (pixels left to compare away from the edges)
        const auto source = app::filter_gallery::source_of(*book.window, *book.window->current_page(), *book.layer("pic"), -1);
        QCOMPARE(source.dpi, 200.0);
        Json op{{"op", "filter_raster"}, {"page", 1}, {"id", "pic"}, {"kind", "filter_gallery"}};
        op.update(settings);
        const core::Document start = *book.session->snapshot();
        const core::Document done = core::CommandBus(render::ops_registry()).apply(start, Json::array({op}), core::Actor("human:tester")).doc;
        QCOMPARE(done.page(0).layers.back().id, std::string("pic"));
        const std::string result = render::layer_image(done.page(0), done.page(0).layers.back(), 200, &done).convert("RGBA").tobytes();
        const auto same_as_result = [&](const render::Box& area, const render::Image& shown) {
            const std::string got = shown.convert("RGBA").tobytes();
            for (int y = area.y0 + halo; y < area.y1 - halo; ++y)
                for (int x = area.x0 + halo; x < area.x1 - halo; ++x)
                    for (int c = 0; c < 4; ++c)
                        if (got[(std::size_t(y - area.y0) * std::size_t(area.width()) + std::size_t(x - area.x0)) * 4 + std::size_t(c)] !=
                            result[(std::size_t(y) * 315 + std::size_t(x)) * 4 + std::size_t(c)])
                            return false;
            return true;
        };
        const render::Box part{20, 30, 180, 190}, dragged{70, 90, 250, 260};
        const render::Image looked = app::filter_gallery::laid_on(source, 200, part, settings);
        QCOMPARE(looked.width(), part.width());
        QVERIFY(same_as_result(part, looked));
        QVERIFY(same_as_result(dragged, app::filter_gallery::laid_on(source, 200, dragged, settings)));
        // inside a selection (5..35 mm: 39..276 px): outside it as it was, deep inside it the op's with the same selection
        book.window->canvas()->set_selection(Json{{"poly", Json::array({Json::array({5, 5}), Json::array({35, 5}), Json::array({35, 35}), Json::array({5, 35})})}});
        const auto selected = app::filter_gallery::source_of(*book.window, *book.window->current_page(), *book.layer("pic"), -1);
        QVERIFY(selected.selection.has_value());
        const render::Box wide{30, 30, 290, 290};
        const std::string inside = app::filter_gallery::laid_on(selected, 200, wide, settings).convert("RGBA").tobytes();
        const auto at = [&](const std::string& b, int w, int x, int y, int c) { return b[(std::size_t(y) * std::size_t(w) + std::size_t(x)) * 4 + std::size_t(c)]; };
        for (const auto& [x, y] : std::vector<std::pair<int, int>>{{33, 100}, {100, 33}, {285, 285}, {280, 60}})
            for (int c = 0; c < 4; ++c) QCOMPARE(at(inside, wide.width(), x - wide.x0, y - wide.y0, c), at(bytes, 315, x, y, c));
        Json op_in = op;
        op_in["area"] = *selected.selection;
        const core::Document done_in = core::CommandBus(render::ops_registry()).apply(start, Json::array({op_in}), core::Actor("human:tester")).doc;
        const std::string result_in = render::layer_image(done_in.page(0), done_in.page(0).layers.back(), 200, &done_in).convert("RGBA").tobytes();
        int compared = 0;
        for (int y = 39 + halo + 2; y < 276 - halo - 2; ++y)
            for (int x = 39 + halo + 2; x < 276 - halo - 2; ++x, ++compared)
                for (int c = 0; c < 4; ++c) QCOMPARE(at(inside, wide.width(), x - wide.x0, y - wide.y0, c), at(result_in, 315, x, y, c));
        QVERIFY(compared >= 100 * 100);
        book.window->canvas()->set_selection(std::nullopt);
    }

    // D2: 渦巻き… from its table's window (角度), ぶれ without a window — each the op's pixels, one Undo —; シアー…'s curve dragged
    // (a point added in the middle and moved across: three points sent); 置き換え…'s map a layer of the page (the page's merged
    // picture by default), and on a smart object the smart filter's map a file of the book.
    void theWindowsOfD2() {
        Book book;
        gui_test::Answers answers;
        book.window->set_target_layer("pic");
        const core::Document start = *book.session->snapshot();
        const auto op_result = [&](const core::Document& from, const Json& op) {
            Json sent{{"op", "filter_raster"}, {"page", 1}, {"id", "pic"}};
            sent.update(op);
            return *core::CommandBus(render::ops_registry()).apply(from, Json::array({sent}), core::Actor("human:tester")).doc.page(0).layers.back().raster_png;
        };
        // 渦巻き…
        bool shown = false;
        answers.responder->exec = answer(QStringLiteral("distort_twirl"), [&](QDialog* d) {
            QCOMPARE(d->windowTitle(), QStringLiteral("渦巻き"));
            auto* angle = d->findChild<QDoubleSpinBox*>(QStringLiteral("field_angle"));
            QVERIFY(angle != nullptr);
            QCOMPARE(angle->value(), 50.0);
            angle->setValue(120);
            shown = true;
        });
        book.act("act_filter_distort_twirl")->trigger();
        QVERIFY(shown);
        QCOMPARE(*book.layer("pic")->raster_png, op_result(start, Json{{"kind", "distort_twirl"}, {"angle", 120}}));
        QVERIFY(*book.layer("pic")->raster_png != *start.page(0).layers.back().raster_png);
        book.window->action(QStringLiteral("act_undo"))->trigger();
        QCOMPARE(*book.layer("pic")->raster_png, *start.page(0).layers.back().raster_png);
        // ぶれ, 輪郭検出: no window
        int windows = 0;
        answers.responder->exec = [&](QDialog*) {
            ++windows;
            return int(QDialog::Rejected);
        };
        book.act("act_filter_pixelate_fragment")->trigger();
        QCOMPARE(windows, 0);
        QCOMPARE(*book.layer("pic")->raster_png, op_result(start, Json{{"kind", "pixelate_fragment"}}));
        book.window->action(QStringLiteral("act_undo"))->trigger();
        // シアー…: the curve dragged — a point added in its middle, moved across
        Json sheared;
        answers.responder->exec = answer(QStringLiteral("distort_shear"), [&](QDialog* d) {
            QWidget* curve = d->findChild<QWidget*>(QStringLiteral("field_points"));
            QVERIFY(curve != nullptr);
            QTest::mousePress(curve, Qt::LeftButton, {}, QPoint(curve->width() / 2, curve->height() / 2));
            QTest::mouseMove(curve, QPoint(curve->width() - 20, curve->height() / 2));
            QTest::mouseRelease(curve, Qt::LeftButton, {}, QPoint(curve->width() - 20, curve->height() / 2));
            sheared = d->findChild<app::adjust_forms::Form*>()->values();
        });
        book.act("act_filter_distort_shear")->trigger();
        QCOMPARE(sheared["points"].size(), std::size_t(3));
        QVERIFY(sheared["points"][1][1].get<double>() > 0.5);
        QCOMPARE(*book.layer("pic")->raster_png, op_result(start, Json{{"kind", "distort_shear"}, {"points", sheared["points"]}, {"undefined_areas", "wrap"}}));
        book.window->action(QStringLiteral("act_undo"))->trigger();
        // 置き換え…: its map the page's merged picture by default, a layer of the page chosen
        Json displaced;
        answers.responder->exec = answer(QStringLiteral("distort_displace"), [&](QDialog* d) {
            auto* map = d->findChild<QComboBox*>(QStringLiteral("displace_map"));
            QVERIFY(map != nullptr);
            QCOMPARE(map->currentText(), QStringLiteral("統合した絵"));
            QVERIFY(d->findChild<QPushButton*>(QStringLiteral("displace_map_file")) != nullptr);
            map->setCurrentIndex(map->findData(QStringLiteral("layer:pic")));
            d->findChild<QDoubleSpinBox*>(QStringLiteral("field_horizontal"))->setValue(40);
            displaced = d->findChild<app::adjust_forms::Form*>()->values();
        });
        book.act("act_filter_distort_displace")->trigger();
        QCOMPARE(displaced["map"], (Json{{"layer", "pic"}}));
        QCOMPARE(*book.layer("pic")->raster_png, op_result(start, Json{{"kind", "distort_displace"}, {"horizontal", 40}, {"map", Json{{"layer", "pic"}}}}));
        book.window->action(QStringLiteral("act_undo"))->trigger();
        // on a smart object: the smart filter's map a file of the book (the layer drawn then)
        QVERIFY(book.window->apply_ops(Json::array({Json{{"op", "add_layer"}, {"page", 1}, {"kind", "paint"}, {"id", "map"}},
                                                    Json{{"op", "smart_convert"}, {"page", 1}, {"ids", Json::array({"pic"})}, {"new_id", "so"}}})));
        book.window->set_target_layer("so");
        answers.responder->exec = answer(QStringLiteral("distort_displace"), [&](QDialog* d) {
            auto* map = d->findChild<QComboBox*>(QStringLiteral("displace_map"));
            map->setCurrentIndex(map->findData(QStringLiteral("layer:map")));
        });
        book.act("act_filter_distort_displace")->trigger();
        const Json filter = (*book.layer("so")->smart)["filters"][0];
        QCOMPARE(filter["kind"], Json("distort_displace"));
        const std::string ref = filter["params"]["map"].get<std::string>();
        QVERIFY2(ref.starts_with("sha256:"), filter.dump().c_str());
        QCOMPARE(filter["params"]["map_suffix"], Json(".png"));
        QVERIFY(book.session->snapshot()->filter_files.contains(ref));
        // (its window again from the panel: the map it holds offered as it is)
        QString offered;
        answers.responder->exec = answer(QStringLiteral("distort_displace"), [&](QDialog* d) { offered = d->findChild<QComboBox*>(QStringLiteral("displace_map"))->currentText(); });
        book.window->layer_panel()->edit_smart_filter(0);
        QCOMPARE(offered, QStringLiteral("今の置き換えマップ"));
        QCOMPARE((*book.layer("so")->smart)["filters"][0]["params"]["map"], Json(ref));
    }

    // D6: レンズ補正… — its window (自動補正 and カスタム, the result on its picture, 角度補正ツール's line levelling it), OK sends
    // lens_correction as set, キャンセル nothing; 広角補正… — a constraint drawn on its picture, sent; 消点… on a smart object
    // grey; on the paint layer a plane made by four clicks and a layer pasted on it, OK sends vanishing_point (the planes kept
    // on the page), its window again showing them; not 前回のフィルター.
    void theLensAndVanishingPointWindows() {
        Book book;
        gui_test::Answers answers;
        book.window->set_target_layer("pic");
        const core::Bytes before = book.layer("pic")->raster_png;
        int previews = 0;
        double angle = 0;
        answers.responder->exec = answer(QStringLiteral("lens_correction"), [&](QDialog* d) {
            QCOMPARE(d->windowTitle(), QStringLiteral("レンズ補正"));
            auto* tabs = d->findChild<QTabWidget*>(QStringLiteral("lens_tabs"));
            QVERIFY(tabs != nullptr);
            QCOMPARE(tabs->tabText(0), QStringLiteral("自動補正"));
            QCOMPARE(tabs->tabText(1), QStringLiteral("カスタム"));
            d->findChild<QDoubleSpinBox*>(QStringLiteral("lens_distortion"))->setValue(-50);
            QTRY_VERIFY(d->property("previews").toInt() > 0);
            previews = d->property("previews").toInt();
            // 角度補正ツール: a line 5 px up over 100 px levelled
            auto* view = dynamic_cast<app::lens_window::PictureView*>(d->findChild<QWidget*>(QStringLiteral("lens_view")));
            QVERIFY(view != nullptr);
            d->findChild<QPushButton*>(QStringLiteral("lens_straighten"))->setChecked(true);
            view->press({5, 5}, Qt::NoModifier, Qt::LeftButton);
            view->drag({10, 5.25}, Qt::NoModifier);
            view->release({15, 5.5}, Qt::NoModifier);
            angle = d->findChild<QDoubleSpinBox*>(QStringLiteral("lens_angle"))->value();
        });
        book.act("act_filter_lens_correction")->trigger();
        QVERIFY(previews > 0);
        QVERIFY2(std::fabs(angle + 2.862) < 0.01, qPrintable(QString::number(angle)));
        QVERIFY(book.layer("pic")->raster_png != before);
        book.window->action(QStringLiteral("act_undo"))->trigger();
        QCOMPARE(book.layer("pic")->raster_png, before);
        answers.responder->exec = [](QDialog*) { return int(QDialog::Rejected); };
        book.act("act_filter_lens_correction")->trigger();
        QCOMPARE(book.layer("pic")->raster_png, before);
        // 広角補正: a constraint drawn (Shift: level), sent
        int constraints = 0;
        answers.responder->exec = answer(QStringLiteral("adaptive_wide_angle"), [&](QDialog* d) {
            QCOMPARE(d->windowTitle(), QStringLiteral("広角補正"));
            auto* model = d->findChild<QComboBox*>(QStringLiteral("wide_model"));
            model->setCurrentIndex(model->findData(QStringLiteral("fisheye")));
            d->findChild<QDoubleSpinBox*>(QStringLiteral("wide_focal_length"))->setValue(10);
            auto* view = dynamic_cast<app::lens_window::PictureView*>(d->findChild<QWidget*>(QStringLiteral("lens_view")));
            QVERIFY(view != nullptr);
            view->press({4, 4}, Qt::NoModifier, Qt::LeftButton);
            view->drag({8, 4.2}, Qt::NoModifier);
            view->release({12, 4.3}, Qt::ShiftModifier);
            constraints = d->findChild<QListWidget*>(QStringLiteral("wide_constraints"))->count();
        });
        book.act("act_filter_adaptive_wide_angle")->trigger();
        QCOMPARE(constraints, 1);
        QVERIFY(book.layer("pic")->raster_png != before);
        book.window->action(QStringLiteral("act_undo"))->trigger();
        // 消点: a plane of four clicks, the layer pasted on it
        int planes_again = 0;
        answers.responder->exec = answer(QStringLiteral("vanishing_point"), [&](QDialog* d) {
            QCOMPARE(d->windowTitle(), QStringLiteral("消点"));
            auto* view = dynamic_cast<app::lens_window::PictureView*>(d->findChild<QWidget*>(QStringLiteral("vanishing_view")));
            QVERIFY(view != nullptr);
            QVERIFY(d->findChild<QRadioButton*>(QStringLiteral("vanishing_create"))->isChecked());
            for (const std::array<double, 2> at : {std::array<double, 2>{12, 12}, {28, 12}, {36, 30}, {4, 30}}) view->press(at, Qt::NoModifier, Qt::LeftButton);
            QCOMPARE(d->property("planes").toInt(), 1);
            auto* source = d->findChild<QComboBox*>(QStringLiteral("vanishing_source"));
            source->setCurrentIndex(source->findData(QStringLiteral("pic")));
            d->findChild<QPushButton*>(QStringLiteral("vanishing_paste_button"))->click();
        });
        book.act("act_filter_vanishing_point")->trigger();
        QVERIFY(book.layer("pic")->raster_png != before);
        QVERIFY(core::page_vanishing(book.page()) != nullptr);
        QVERIFY(!book.act("act_filter_last")->isEnabled() || book.act("act_filter_last")->statusTip().indexOf(QStringLiteral("消点")) < 0);
        // (元に戻す: the paste and the planes both gone; やり直す: both back)
        const core::Bytes pasted = book.layer("pic")->raster_png;
        book.window->action(QStringLiteral("act_undo"))->trigger();
        QCOMPARE(book.layer("pic")->raster_png, before);
        QVERIFY(core::page_vanishing(book.page()) == nullptr);
        book.window->action(QStringLiteral("act_redo"))->trigger();
        QCOMPARE(book.layer("pic")->raster_png, pasted);
        QVERIFY(core::page_vanishing(book.page()) != nullptr);
        answers.responder->exec = answer(QStringLiteral("vanishing_point"), [&](QDialog* d) { planes_again = d->property("planes").toInt(); });
        book.act("act_filter_vanishing_point")->trigger();
        QCOMPARE(planes_again, 1);
        // 面を削除 (the chosen plane and those torn off it), OK with no plane: the page's planes taken away, the pixels kept
        bool could_ok = false;
        answers.responder->exec = answer(QStringLiteral("vanishing_point"), [&](QDialog* d) {
            d->findChild<QRadioButton*>(QStringLiteral("vanishing_edit"))->setChecked(true);
            d->findChild<QPushButton*>(QStringLiteral("vanishing_tear"))->click();
            QCOMPARE(d->property("planes").toInt(), 2);
            auto* view = dynamic_cast<app::lens_window::PictureView*>(d->findChild<QWidget*>(QStringLiteral("vanishing_view")));
            view->press({12, 12}, Qt::NoModifier, Qt::LeftButton);  // (the first plane chosen by its corner)
            view->release({12, 12}, Qt::NoModifier);
            d->findChild<QPushButton*>(QStringLiteral("vanishing_remove"))->click();
            QCOMPARE(d->property("planes").toInt(), 0);
            auto* buttons = d->findChild<QDialogButtonBox*>();
            could_ok = buttons != nullptr && buttons->button(QDialogButtonBox::Ok)->isEnabled();
        });
        book.act("act_filter_vanishing_point")->trigger();
        QVERIFY(could_ok);
        QVERIFY(core::page_vanishing(book.page()) == nullptr);
        QCOMPARE(book.layer("pic")->raster_png, pasted);
        book.window->action(QStringLiteral("act_undo"))->trigger();
        QVERIFY(core::page_vanishing(book.page()) != nullptr);
        // on a smart object: grey (ラスタライズしてから)
        QVERIFY(book.window->apply_ops(Json::array({Json{{"op", "smart_convert"}, {"page", 1}, {"ids", Json::array({"pic"})}, {"new_id", "so"}}})));
        book.window->set_target_layer("so");
        emit book.menu("menu_filter")->aboutToShow();
        QVERIFY(!book.act("act_filter_vanishing_point")->isEnabled());
        QVERIFY(book.act("act_filter_lens_correction")->isEnabled());
    }

    // D5: Camera Raw フィルター…'s window (Ctrl+Shift+A) — its six tabs in Camera Raw's words, the result on its picture (made
    // on a thread), 補正前, 初期化 setting the sliders back, OK one op (one Undo), キャンセル nothing; on a smart object a smart
    // filter, its window again from the layer panel with its settings (unchanged: nothing sent).
    void theCameraRawWindow() {
        Book book;
        gui_test::Answers answers;
        book.window->set_target_layer("pic");
        emit book.menu("menu_filter")->aboutToShow();
        QVERIFY(book.act("act_filter_camera_raw")->isEnabled());
        const core::Bytes before = book.layer("pic")->raster_png;
        QStringList titles;
        int previews = 0;
        double after_reset = -1;
        bool before_shown = false;
        answers.responder->exec = answer(QStringLiteral("camera_raw"), [&](QDialog* d) {
            QCOMPARE(d->windowTitle(), QStringLiteral("Camera Raw フィルター"));
            auto* tabs = d->findChild<QTabWidget*>(QStringLiteral("camera_raw_tabs"));
            QVERIFY(tabs != nullptr);
            for (int i = 0; i < tabs->count(); ++i) titles << tabs->tabText(i);
            QWidget* panel = d->findChild<QWidget*>(QStringLiteral("camera_raw_panel"));
            QVERIFY(panel != nullptr);
            QTRY_VERIFY(panel->property("previews").toInt() > 0);  // (its picture made, the first result shown)
            const int first = panel->property("previews").toInt();
            auto* exposure = d->findChild<QDoubleSpinBox*>(QStringLiteral("camera_raw_exposure"));
            QVERIFY(exposure != nullptr);
            exposure->setValue(1.5);
            d->findChild<QDoubleSpinBox*>(QStringLiteral("camera_raw_vignette_amount"))->setValue(-60);
            QTRY_VERIFY(panel->property("previews").toInt() > first);
            previews = panel->property("previews").toInt();
            // 初期化: every slider back to its default
            d->findChild<QPushButton*>(QStringLiteral("camera_raw_reset"))->click();
            after_reset = exposure->value() + d->findChild<QDoubleSpinBox*>(QStringLiteral("camera_raw_vignette_amount"))->value();
            exposure->setValue(1.0);
            auto* shown_before = d->findChild<QCheckBox*>(QStringLiteral("camera_raw_before"));
            shown_before->setChecked(true);
            before_shown = shown_before->isChecked();
        });
        book.act("act_filter_camera_raw")->trigger();
        QCOMPARE(titles, (QStringList{QStringLiteral("基本補正"), QStringLiteral("トーンカーブ"), QStringLiteral("ディテール"), QStringLiteral("カラーミキサー"),
                                      QStringLiteral("カラーグレーディング"), QStringLiteral("効果")}));
        QVERIFY(previews > 1);
        QCOMPARE(after_reset, 0.0);
        QVERIFY(before_shown);
        QVERIFY(book.layer("pic")->raster_png != before);
        // its op: the settings in their normal form (exposure 1, the rest at the defaults) — the op's pixels
        const auto expected = core::CommandBus(render::ops_registry())
                                  .apply(book_doc(), Json::array({Json{{"op", "filter_raster"}, {"page", 1}, {"id", "pic"}, {"kind", "camera_raw"}, {"exposure", 1.0}}}),
                                         core::Actor("human:tester"))
                                  .doc;
        for (const auto& l : expected.page(0).layers)
            if (l.id == "pic") QCOMPARE(*book.layer("pic")->raster_png, *l.raster_png);
        // one Undo
        book.window->action(QStringLiteral("act_undo"))->trigger();
        QCOMPARE(book.layer("pic")->raster_png, before);
        // キャンセル: nothing
        answers.responder->exec = [](QDialog*) { return int(QDialog::Rejected); };
        book.act("act_filter_camera_raw")->trigger();
        QCOMPARE(book.layer("pic")->raster_png, before);
        // on a smart object: a smart filter; its window again from the layer panel shows its settings
        QVERIFY(book.window->apply_ops(Json::array({Json{{"op", "smart_convert"}, {"page", 1}, {"ids", Json::array({"pic"})}, {"new_id", "so"}}})));
        book.window->set_target_layer("so");
        emit book.menu("menu_filter")->aboutToShow();
        QVERIFY(book.act("act_filter_camera_raw")->isEnabled());
        answers.responder->exec = answer(QStringLiteral("camera_raw"), [](QDialog* d) {
            d->findChild<QDoubleSpinBox*>(QStringLiteral("camera_raw_saturation"))->setValue(-100);
        });
        book.act("act_filter_camera_raw")->trigger();
        const Json filters = (*book.layer("so")->smart)["filters"];
        QCOMPARE(filters.size(), std::size_t(1));
        QCOMPARE(filters[0]["kind"], Json("camera_raw"));
        QCOMPARE(filters[0]["params"]["saturation"], Json(-100));
        double saturation = 0;
        answers.responder->exec = answer(QStringLiteral("camera_raw"), [&](QDialog* d) {
            saturation = d->findChild<QDoubleSpinBox*>(QStringLiteral("camera_raw_saturation"))->value();
        });
        const auto before_edit = book.session->snapshot();
        book.window->layer_panel()->edit_smart_filter(0);
        QCOMPARE(saturation, -100.0);
        QVERIFY(book.session->snapshot() == before_edit);  // (unchanged: nothing sent)
    }

    // D5's review: the panel's settings come back as they were given (their normal form — its fields' rounding never changing
    // one not touched), a touched one its field's; a smart filter of values the fields round, set again from the layer panel
    // and OK untouched: nothing sent; the point curve holds 19 points (one more not added, said; OK sends); a new seed each
    // time from the menu (the window's the op's), 前回のフィルター another; F-e's hooks (a widget at the top of 基本補正, the rows
    // of keys hidden — their values kept —, a picture not well formed refused).
    void theCameraRawPanelAndItsSettings() {
        namespace crw = app::camera_raw_window;
        const Json given{{"temperature", 12.5},
                         {"exposure", 0.333},
                         {"grade_shadows", Json{{"hue", 210.4}, {"sat", 33.3}, {"lum", -7.25}}},
                         {"hsl_sat", Json::array({1.5, 0, 0, 0, 0, 0, 0, -9.75})},
                         {"point_curve", Json::array({Json::array({0, 0}), Json::array({63.33, 40}), Json::array({255, 255})})},
                         {"point_curve_blue", Json::array({Json::array({0, 20}), Json::array({255, 230})})},
                         {"vignette_style", "color_priority"},
                         {"seed", 77}};
        const Json normal = core::normalize_filter("camera_raw", given, nullptr);
        {
            crw::CameraRawPanel panel;
            panel.set_settings(given);
            QVERIFY(panel.settings() == normal);
            panel.findChild<QDoubleSpinBox*>(QStringLiteral("camera_raw_exposure"))->setValue(1.25);
            Json expected = normal;
            expected["exposure"] = 1.25;
            QVERIFY2(panel.settings() == expected, panel.settings().dump().c_str());
            // F-e: its own widget at the top of 基本補正, the relative white balance hidden (its value kept)
            auto* raw = new QLabel(QStringLiteral("RAW"));
            panel.add_to_basic(raw);
            QVERIFY(raw->parentWidget() != nullptr);
            panel.set_hidden({"temperature", "tint"});
            QVERIFY(panel.findChild<QDoubleSpinBox*>(QStringLiteral("camera_raw_temperature"))->parentWidget()->isHidden());
            QVERIFY(panel.settings()["temperature"] == Json(12.5));
            // a picture not well formed: refused, said
            auto bad = std::make_shared<crw::Picture>();
            bad->width = 4;
            bad->height = 4;
            bad->rgba.assign(10, 0.5f);
            panel.set_picture(bad);
            QVERIFY(panel.findChild<QLabel*>(QStringLiteral("camera_raw_status"))->text().contains(QStringLiteral("正しくない")));
            QVERIFY(crw::shown(*bad).isNull() && crw::developed(*bad, normal).isNull());
            // 初期化: the defaults, its seed kept
            panel.reset();
            Json defaults = core::normalize_filter("camera_raw", Json{{"seed", 77}}, nullptr);
            QVERIFY(panel.settings() == defaults);
        }
        Book book;
        gui_test::Answers answers;
        // a smart filter of values the fields round, set again untouched: nothing sent
        Json add{{"op", "smart_filter_add"}, {"page", 1}, {"id", "so"}, {"kind", "camera_raw"}};
        add.update(given);
        QVERIFY(book.window->apply_ops(Json::array({Json{{"op", "smart_convert"}, {"page", 1}, {"ids", Json::array({"pic"})}, {"new_id", "so"}}, add})));
        book.window->set_target_layer("so");
        const auto before_edit = book.session->snapshot();
        bool opened = false;
        answers.responder->exec = answer(QStringLiteral("camera_raw"), [&](QDialog*) { opened = true; });
        book.window->layer_panel()->edit_smart_filter(0);
        QVERIFY(opened);
        QVERIFY(book.session->snapshot() == before_edit);
        // a new seed each time from the menu (the window's the op's), 前回のフィルター another
        std::vector<std::int64_t> seen;
        answers.responder->exec = answer(QStringLiteral("camera_raw"), [&](QDialog* d) {
            d->findChild<QDoubleSpinBox*>(QStringLiteral("camera_raw_grain_amount"))->setValue(40);
            auto* panel = dynamic_cast<crw::CameraRawPanel*>(d->findChild<QWidget*>(QStringLiteral("camera_raw_panel")));
            QVERIFY(panel != nullptr);
            seen.push_back(panel->settings()["seed"].get<std::int64_t>());
        });
        book.act("act_filter_camera_raw")->trigger();
        book.act("act_filter_camera_raw")->trigger();
        Json filters = (*book.layer("so")->smart)["filters"];
        QCOMPARE(filters.size(), std::size_t(3));
        QCOMPARE(seen.size(), std::size_t(2));
        QCOMPARE(filters[1]["params"]["seed"].get<std::int64_t>(), seen[0]);
        QCOMPARE(filters[2]["params"]["seed"].get<std::int64_t>(), seen[1]);
        QVERIFY(seen[0] != seen[1]);
        book.act("act_filter_last")->trigger();
        filters = (*book.layer("so")->smart)["filters"];
        QCOMPARE(filters.size(), std::size_t(4));
        QVERIFY(filters[3]["params"]["seed"] != filters[2]["params"]["seed"] && filters[3]["params"]["grain_amount"] == Json(40));
        // the point curve: 19 points at most (one more not added, said), OK sends
        Book paint;
        paint.window->set_target_layer("pic");
        const core::Bytes before = paint.layer("pic")->raster_png;
        std::size_t points = 0;
        QString said;
        answers.responder->exec = answer(QStringLiteral("camera_raw"), [&](QDialog* d) {
            auto* editor = d->findChild<app::CurveEditor*>(QStringLiteral("camera_raw_curve"));
            QVERIFY(editor != nullptr);
            for (int i = 0; i < 25; ++i) {
                const QPointF at(6 + i * 8, 60 + (i % 2) * 4);
                QMouseEvent press(QEvent::MouseButtonPress, at, at, Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
                QApplication::sendEvent(editor, &press);
                QMouseEvent release(QEvent::MouseButtonRelease, at, at, Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
                QApplication::sendEvent(editor, &release);
            }
            points = editor->points().size();
            said = d->findChild<QLabel*>(QStringLiteral("camera_raw_status"))->text();
        });
        paint.act("act_filter_camera_raw")->trigger();
        QCOMPARE(points, std::size_t(19));
        QVERIFY2(said.contains(QStringLiteral("19")), qPrintable(said));
        QVERIFY(paint.layer("pic")->raster_png != before);

    // D4's 描画: 雲模様 1・2 without a window (the main and sub colours, a new seed each time: the op's pixels, one Undo);
    // ファイバー… from its table (ランダマイズ a new seed); 逆光… (its centre, レンズの種類); 照明効果…'s window (a point light
    // added and dragged on its picture, the result on it, the page's alpha channel offered as its texture); 木…・額縁… — the 34
    // types' and 16 designs' small pictures made on a thread, one chosen, 新しいレイヤーに描く —, 炎… along the page's work path:
    // render_picture on a thread, the op's pixels; on a smart object 炎・額縁・木 grey, 照明効果 a smart filter without the
    // page's channels offered.
    void theRenderCommands() {
        core::Document doc = core::CommandBus(render::ops_registry())
                                 .apply(book_doc(), Json::array({Json{{"op", "add_channel"}, {"page", 1}, {"fill", "area"}, {"area", Json{{"rect", Json::array({0, 0, 20, 40})}}}}}),
                                        core::Actor("human:tester"))
                                 .doc;
        core::vector::Subpath across;
        across.closed = false;
        across.knots = {core::vector::Knot::corner(8, 34), core::vector::Knot::corner(32, 30)};
        core::vector::Path work;
        work.subpaths = {across};
        doc.edit_page(0).work_path = work;
        Book book(doc);
        gui_test::Answers answers;
        book.window->set_target_layer("pic");
        const core::Document start = *book.session->snapshot();
        const core::Bytes before = book.layer("pic")->raster_png;
        const auto last_op = [&] { return book.session->history().done.back().ops.at(0); };
        const auto op_doc = [&](const Json& op) { return core::CommandBus(render::ops_registry()).apply(start, Json::array({op}), core::Actor("human:tester")).doc; };
        const auto undo = [&] { book.window->action(QStringLiteral("act_undo"))->trigger(); };
        // 雲模様 1・雲模様 2: no window, the colours of now, a new seed each time
        int windows = 0;
        answers.responder->exec = [&](QDialog*) {
            ++windows;
            return int(QDialog::Rejected);
        };
        const auto& pen = book.window->pen().rgb;
        const Json main_colour = Json::array({static_cast<int>(pen[0]), static_cast<int>(pen[1]), static_cast<int>(pen[2])});
        const std::array<int, 3> sub = book.window->colours()->sub_rgb();
        const Json sub_colour = Json::array({sub[0], sub[1], sub[2]});
        std::vector<Json> seeds;
        for (const std::string name : {"act_filter_render_clouds", "act_filter_render_difference_clouds", "act_filter_render_clouds"}) {
            book.act(name.c_str())->trigger();
            QCOMPARE(windows, 0);
            const Json sent = last_op();
            QCOMPARE(sent["kind"], Json(name == "act_filter_render_clouds" ? "render_clouds" : "render_difference_clouds"));
            QCOMPARE(sent["foreground"], main_colour);
            QCOMPARE(sent["background"], sub_colour);
            QVERIFY(sent["seed"].is_number());
            seeds.push_back(sent["seed"]);
            QVERIFY(book.layer("pic")->raster_png != before);
            QCOMPARE(*book.layer("pic")->raster_png, *op_doc(sent).page(0).layers.back().raster_png);
            undo();
            QCOMPARE(book.layer("pic")->raster_png, before);
        }
        QVERIFY(seeds[0] != seeds[2]);
        // ファイバー…: its table's window (変化), ランダマイズ a new seed
        Json fibers_seed;
        answers.responder->exec = answer(QStringLiteral("render_fibers"), [&](QDialog* d) {
            QCOMPARE(d->windowTitle(), QStringLiteral("ファイバー"));
            d->findChild<QDoubleSpinBox*>(QStringLiteral("field_variance"))->setValue(30);
            auto* form = d->findChild<app::adjust_forms::Form*>();
            const Json first = form->values()["seed"];
            d->findChild<QPushButton*>(QStringLiteral("field_seed"))->click();  // (ランダマイズ)
            fibers_seed = form->values()["seed"];
            QVERIFY(fibers_seed.is_number() && fibers_seed != first);
        });
        book.act("act_filter_render_fibers")->trigger();
        Json sent = last_op();
        QCOMPARE(sent["kind"], Json("render_fibers"));
        QCOMPARE(sent["variance"], Json(30));
        QCOMPARE(sent["seed"], fibers_seed);
        QCOMPARE(*book.layer("pic")->raster_png, *op_doc(sent).page(0).layers.back().raster_png);
        undo();
        // 逆光…: its centre (the box to click and its fields), レンズの種類
        answers.responder->exec = answer(QStringLiteral("render_lens_flare"), [&](QDialog* d) {
            QVERIFY(d->findChild<QWidget*>(QStringLiteral("flare_centre")) != nullptr);
            d->findChild<QDoubleSpinBox*>(QStringLiteral("field_center_x"))->setValue(0.25);
            auto* lens = d->findChild<QComboBox*>(QStringLiteral("field_lens"));
            QVERIFY(lens != nullptr);
            lens->setCurrentIndex(lens->findText(QStringLiteral("105mm プライム")));
        });
        book.act("act_filter_render_lens_flare")->trigger();
        sent = last_op();
        QCOMPARE(sent["center_x"], Json(0.25));
        QCOMPARE(sent["lens"], Json("prime105"));
        QCOMPARE(*book.layer("pic")->raster_png, *op_doc(sent).page(0).layers.back().raster_png);
        undo();
        // 照明効果…: upstream's spot light, a point light added (強度) and dragged, the page's alpha channel as the texture
        QStringList textures;
        answers.responder->exec = answer(QStringLiteral("render_lighting_effects"), [&](QDialog* d) {
            QCOMPARE(d->windowTitle(), QStringLiteral("照明効果"));
            auto* lights = d->findChild<QListWidget*>(QStringLiteral("lighting_lights"));
            QCOMPARE(lights->count(), 1);
            d->findChild<QPushButton*>(QStringLiteral("lighting_add_point"))->click();
            QCOMPARE(lights->count(), 2);
            QCOMPARE(lights->item(1)->text(), QStringLiteral("点光源"));
            d->findChild<QDoubleSpinBox*>(QStringLiteral("lighting_intensity"))->setValue(40);
            auto* texture = d->findChild<QComboBox*>(QStringLiteral("lighting_texture"));
            for (int i = 0; i < texture->count(); ++i) textures << texture->itemText(i);
            texture->setCurrentIndex(texture->count() - 1);
            auto* view = dynamic_cast<app::lens_window::PictureView*>(d->findChild<QWidget*>(QStringLiteral("lighting_view")));
            QVERIFY(view != nullptr);
            view->press({20, 16}, Qt::NoModifier, Qt::LeftButton);  // (the point light, at 0.5, 0.4 of the page)
            view->drag({15, 16}, Qt::NoModifier);
            view->drag({10, 16}, Qt::NoModifier);
            view->release({10, 16}, Qt::NoModifier);
            QVERIFY(gui_test::wait_for([&] { return d->property("previews").toInt() > 0; }, 30000));
        });
        book.act("act_filter_render_lighting_effects")->trigger();
        QCOMPARE(textures.size(), 7);
        QCOMPARE(textures.back(), QString::fromStdString(book.page().channels.at(0).name));
        sent = last_op();
        QCOMPARE(sent["lights"].size(), std::size_t(2));
        QCOMPARE(sent["lights"][1]["kind"], Json("point"));
        QCOMPARE(sent["lights"][1]["intensity"], Json(40));
        QVERIFY2(std::fabs(sent["lights"][1]["x"].get<double>() - 0.25) < 0.02, sent.dump().c_str());
        QCOMPARE(sent["texture"], (Json{{"channel", book.page().channels.at(0).id}}));
        QVERIFY(book.layer("pic")->raster_png != before);
        QCOMPARE(*book.layer("pic")->raster_png, *op_doc(sent).page(0).layers.back().raster_png);
        undo();
        // 木…: the 34 types with their small pictures (made on a thread), マツ, 新しいレイヤーに描く — render_picture on a thread
        answers.responder->exec = answer(QStringLiteral("tree"), [&](QDialog* d) {
            QCOMPARE(d->windowTitle(), QStringLiteral("木"));
            auto* types = d->findChild<QListWidget*>(QStringLiteral("render_tree_types"));
            QVERIFY(types != nullptr);
            QCOMPARE(types->count(), 34);
            QCOMPARE(types->item(2)->text(), QStringLiteral("マツ"));
            QVERIFY(gui_test::wait_for([&] { return d->property("pictures").toInt() == 34; }, 60000));
            QVERIFY(!types->item(2)->icon().isNull());
            types->setCurrentRow(2);
            d->findChild<QCheckBox*>(QStringLiteral("render_new_layer"))->setChecked(true);
        });
        const std::size_t layers = book.page().layers.size();
        book.act("act_filter_render_tree")->trigger();
        sent = last_op();
        QCOMPARE(sent["op"], Json("render_picture"));
        QCOMPARE(sent["kind"], Json("tree"));
        QCOMPARE(sent["base_tree_type"], Json(3));
        QCOMPARE(sent["new_layer"], Json(true));
        QVERIFY(sent["seed"].is_number());
        QCOMPARE(book.page().layers.size(), layers + 1);
        QCOMPARE(book.page().layers.back().title, std::string("木"));
        QCOMPARE(book.layer("pic")->raster_png, before);
        QCOMPARE(*book.page().layers.back().raster_png, *op_doc(sent).page(0).layers.back().raster_png);
        undo();
        QCOMPARE(book.page().layers.size(), layers);
        // 額縁…: the 16 designs, バラ, on the layer itself
        answers.responder->exec = answer(QStringLiteral("picture_frame"), [&](QDialog* d) {
            QCOMPARE(d->windowTitle(), QStringLiteral("額縁"));
            auto* designs = d->findChild<QListWidget*>(QStringLiteral("render_frames"));
            QCOMPARE(designs->count(), 16);
            QCOMPARE(designs->item(3)->text(), QStringLiteral("バラ"));
            designs->setCurrentRow(3);
        });
        book.act("act_filter_render_picture_frame")->trigger();
        sent = last_op();
        QCOMPARE(sent["frame"], Json(4));
        QVERIFY(!sent.contains("new_layer"));
        QCOMPARE(book.page().layers.size(), layers);
        QVERIFY(book.layer("pic")->raster_png != before);
        QCOMPARE(*book.layer("pic")->raster_png, *op_doc(sent).page(0).layers.back().raster_png);
        undo();
        // 炎…: along the page's work path (offered first)
        answers.responder->exec = answer(QStringLiteral("flame"), [&](QDialog* d) {
            QCOMPARE(d->windowTitle(), QStringLiteral("炎"));
            auto* path = d->findChild<QComboBox*>(QStringLiteral("field_path"));
            QVERIFY(path != nullptr);
            QCOMPARE(path->currentText(), QStringLiteral("作業用パス"));
            d->findChild<QDoubleSpinBox*>(QStringLiteral("field_length"))->setValue(40);
            d->findChild<QDoubleSpinBox*>(QStringLiteral("field_width"))->setValue(12);
        });
        book.act("act_filter_render_flame")->trigger();
        sent = last_op();
        QCOMPARE(sent["kind"], Json("flame"));
        QCOMPARE(sent["path"], Json("work"));
        QCOMPARE(sent["length"], Json(40));
        QVERIFY(book.layer("pic")->raster_png != before);
        QCOMPARE(*book.layer("pic")->raster_png, *op_doc(sent).page(0).layers.back().raster_png);
        undo();
        // a pen layer's lines: asked about after the window, only when the shapes go on that layer (review P3-7)
        QVERIFY(book.window->apply_ops(Json::array({Json{{"op", "add_layer"}, {"page", 1}, {"kind", "pen"}, {"id", "pen"}},
                                                    Json{{"op", "add_stroke"}, {"page", 1}, {"layer_id", "pen"}, {"points", Json::array({Json::array({8, 20}), Json::array({30, 20})})},
                                                         {"width_mm", 1}, {"rgb", Json::array({0, 0, 0})}}})));
        book.window->set_target_layer("pen");
        answers.asked.clear();
        bool new_layer = true;
        answers.responder->exec = answer(QStringLiteral("picture_frame"), [&](QDialog* d) {
            QVERIFY(answers.asked.isEmpty());  // (nothing asked before the window)
            d->findChild<QCheckBox*>(QStringLiteral("render_new_layer"))->setChecked(new_layer);
        });
        const std::size_t before_pen = book.page().layers.size();
        book.act("act_filter_render_picture_frame")->trigger();
        QVERIFY2(answers.asked.isEmpty(), qPrintable(answers.asked.join(QStringLiteral(" / "))));
        QCOMPARE(book.page().layers.size(), before_pen + 1);
        QVERIFY(book.layer("pen")->stroke_count() > 0);
        undo();
        new_layer = false;
        book.act("act_filter_render_picture_frame")->trigger();  // (asked; no: nothing drawn)
        QCOMPARE(answers.asked.size(), 1);
        QVERIFY2(answers.asked.front().contains(QStringLiteral("そのレイヤーに「額縁」を描きますか")), qPrintable(answers.asked.front()));
        QCOMPARE(book.page().layers.size(), before_pen);
        QVERIFY(book.layer("pen")->stroke_count() > 0);
        QVERIFY(book.window->apply_ops(Json::array({Json{{"op", "delete_layer"}, {"page", 1}, {"id", "pen"}}})));
        book.window->set_target_layer("pic");
        // on a smart object: 炎・額縁・木 on a new layer above it (the op's: a layer not a paint layer); 照明効果 a smart
        // filter, the page's channels not offered
        QVERIFY(book.window->apply_ops(Json::array({Json{{"op", "smart_convert"}, {"page", 1}, {"ids", Json::array({"pic"})}, {"new_id", "so"}}})));
        book.window->set_target_layer("so");
        emit book.menu("menu_filter")->aboutToShow();
        for (const char* on : {"act_filter_render_flame", "act_filter_render_picture_frame", "act_filter_render_tree", "act_filter_render_clouds",
                               "act_filter_render_lighting_effects", "act_filter_blur_iris"})
            QVERIFY2(book.act(on)->isEnabled(), on);
        emit book.menu("menu_filter")->aboutToHide();
        const auto so_before = *book.layer("so")->smart;
        const std::size_t before_so = book.page().layers.size();
        answers.responder->exec = answer(QStringLiteral("tree"));
        book.act("act_filter_render_tree")->trigger();
        QCOMPARE(book.page().layers.size(), before_so + 1);
        QCOMPARE(book.page().layers.back().title, std::string("木"));
        QCOMPARE(*book.layer("so")->smart, so_before);
        undo();
        textures.clear();
        answers.responder->exec = answer(QStringLiteral("render_lighting_effects"), [&](QDialog* d) {
            auto* texture = d->findChild<QComboBox*>(QStringLiteral("lighting_texture"));
            for (int i = 0; i < texture->count(); ++i) textures << texture->itemText(i);
        });
        book.act("act_filter_render_lighting_effects")->trigger();
        QCOMPARE(textures.size(), 6);
        const Json filters = (*book.layer("so")->smart)["filters"];
        QCOMPARE(filters.size(), std::size_t(1));
        QCOMPARE(filters[0]["kind"], Json("render_lighting_effects"));
    }

    // ぼかしギャラリー (D4): not a window but a work over the canvas — 虹彩絞りぼかし…'s pin, a second one added by a click and
    // dragged, the ぼかしツール panel's ぼかし, the result shown on the page meanwhile —; Enter sends the op (its pixels, one
    // Undo), キャンセル nothing; パスぼかし's path across the middle, a click adding a point at its end, Delete taking it back;
    // on a smart object a smart filter, its work again from the layer panel with its pins, sent as smart_filter_set.
    void theBlurGalleryOverTheCanvas() {
        Book book;
        gui_test::Answers answers;
        book.window->set_target_layer("pic");
        const core::Document start = *book.session->snapshot();
        const core::Bytes before = book.layer("pic")->raster_png;
        auto* canvas = book.window->canvas();
        const auto panel = [&]() -> QDialog* {  // (the one shown: a panel let go is hidden, then deleted later)
            for (QDialog* d : book.window->findChildren<QDialog*>(QStringLiteral("blur_gallery_panel")))
                if (d->isVisible()) return d;
            return nullptr;
        };
        const auto pins = [] { return app::blur_gallery::settings()["pins"]; };
        const auto op_pixels = [&](const Json& op) {
            Json sent{{"op", "filter_raster"}, {"page", 1}, {"id", "pic"}};
            sent.update(op);
            return *core::CommandBus(render::ops_registry()).apply(start, Json::array({sent}), core::Actor("human:tester")).doc.page(0).layers.back().raster_png;
        };
        // 虹彩絞りぼかし…: its pin; a click adds one there, the panel's ぼかし its, a drag moves it
        book.act("act_filter_blur_iris")->trigger();
        QVERIFY(app::blur_gallery::working());
        QVERIFY(canvas->has_work());
        QVERIFY(panel() != nullptr && panel()->isVisible());
        QCOMPARE(panel()->property("kind").toString(), QStringLiteral("blur_iris"));
        QCOMPARE(pins().size(), std::size_t(1));
        const QPoint at = canvas->to_widget(QPointF(8, 30)).toPoint();
        QTest::mouseClick(canvas, Qt::LeftButton, {}, at);
        QCOMPARE(pins().size(), std::size_t(2));
        QVERIFY2(std::fabs(pins()[1]["x"].get<double>() - 0.2) < 0.05, pins().dump().c_str());
        panel()->findChild<QDoubleSpinBox*>(QStringLiteral("gallery_blur"))->setValue(40);
        QCOMPARE(pins()[1]["blur"], Json(40));
        QTest::mousePress(canvas, Qt::LeftButton, {}, at);
        QTest::mouseMove(canvas, at + QPoint(20, 0));
        QTest::mouseMove(canvas, at + QPoint(40, 0));
        QTest::mouseRelease(canvas, Qt::LeftButton, {}, at + QPoint(40, 0));
        QVERIFY2(pins()[1]["x"].get<double>() > 0.2 + 0.01, pins().dump().c_str());
        QCOMPARE(pins().size(), std::size_t(2));
        // the result on the page meanwhile, the book as it was
        QVERIFY(gui_test::wait_for([&] { return canvas->doc() != book.session->snapshot(); }, 30000));
        QCOMPARE(book.layer("pic")->raster_png, before);
        // Enter: sent, the op's pixels; one Undo
        const Json chosen = pins();
        QTest::keyClick(canvas, Qt::Key_Return);
        QVERIFY(!app::blur_gallery::working());
        QVERIFY(!canvas->has_work());
        QVERIFY(gui_test::wait_for([&] { return panel() == nullptr; }, 5000));
        QVERIFY(book.layer("pic")->raster_png != before);
        QCOMPARE(*book.layer("pic")->raster_png, op_pixels(Json{{"kind", "blur_iris"}, {"pins", chosen}}));
        book.window->action(QStringLiteral("act_undo"))->trigger();
        QCOMPARE(book.layer("pic")->raster_png, before);
        // a pen — and its eraser end — with a drawing tool chosen: the work's, not the tool's (a pin, no line, the tool kept); a
        // double click: a pin; 切り取り with a selection: nothing cut, the pins kept (review P2-1, P3-1)
        book.act("act_pen")->trigger();
        const QString tool = canvas->tool();
        const std::size_t steps = book.session->history().done.size();
        book.act("act_filter_blur_iris")->trigger();
        QVERIFY(app::blur_gallery::working());
        QCOMPARE(pins().size(), std::size_t(1));
        app::inject::tablet(canvas, app::inject::Phase::Press, app::inject::PenInput{QPointF(30, 10)});
        app::inject::tablet(canvas, app::inject::Phase::Release, app::inject::PenInput{QPointF(30, 10)});
        QCOMPARE(pins().size(), std::size_t(2));
        app::inject::tablet(canvas, app::inject::Phase::Press, app::inject::PenInput{QPointF(10, 10)}, true);
        app::inject::tablet(canvas, app::inject::Phase::Release, app::inject::PenInput{QPointF(10, 10)}, true);
        QCOMPARE(pins().size(), std::size_t(3));
        QCOMPARE(canvas->tool(), tool);
        QVERIFY(app::blur_gallery::working());
        QCOMPARE(book.layer("pic")->raster_png, before);
        QCOMPARE(book.session->history().done.size(), steps);  // (no line drawn)
        QTest::mouseDClick(canvas, Qt::LeftButton, {}, canvas->to_widget(QPointF(30, 32)).toPoint());
        QCOMPARE(pins().size(), std::size_t(4));
        canvas->set_selection(Json{{"poly", Json::array({Json::array({2, 2}), Json::array({20, 2}), Json::array({20, 20}), Json::array({2, 20})})}});
        book.window->action(QStringLiteral("act_cut"))->trigger();
        QCOMPARE(pins().size(), std::size_t(4));
        QVERIFY(app::blur_gallery::working());
        QCOMPARE(book.layer("pic")->raster_png, before);
        QCOMPARE(book.session->history().done.size(), steps);
        canvas->set_selection(std::nullopt);
        panel()->findChild<QDialogButtonBox*>()->button(QDialogButtonBox::Cancel)->click();
        QVERIFY(!app::blur_gallery::working());
        QCOMPARE(book.layer("pic")->raster_png, before);
        // チルトシフト…: its centre moved, キャンセル — nothing
        book.act("act_filter_blur_tilt_shift")->trigger();
        QVERIFY(app::blur_gallery::working());
        QTest::mouseClick(canvas, Qt::LeftButton, {}, canvas->to_widget(QPointF(20, 8)).toPoint());
        QVERIFY(app::blur_gallery::settings()["center_y"].get<double>() < 0.3);
        panel()->findChild<QDialogButtonBox*>()->button(QDialogButtonBox::Cancel)->click();
        QVERIFY(!app::blur_gallery::working());
        QVERIFY(!canvas->has_work());
        QCOMPARE(book.layer("pic")->raster_png, before);
        // パスぼかし…: upstream's path across the middle, a point added at its end by a click, Delete taking it back, Enter
        book.act("act_filter_blur_path")->trigger();
        Json paths = app::blur_gallery::settings()["paths"];
        QCOMPARE(paths.size(), std::size_t(1));
        QCOMPARE(paths[0]["points"].size(), std::size_t(2));
        QTest::mouseClick(canvas, Qt::LeftButton, {}, canvas->to_widget(QPointF(36, 8)).toPoint());
        QCOMPARE(app::blur_gallery::settings()["paths"][0]["points"].size(), std::size_t(3));
        QTest::keyClick(canvas, Qt::Key_Delete);
        QCOMPARE(app::blur_gallery::settings()["paths"][0]["points"].size(), std::size_t(2));
        panel()->findChild<QDoubleSpinBox*>(QStringLiteral("gallery_speed"))->setValue(20);
        paths = app::blur_gallery::settings()["paths"];
        QTest::keyClick(canvas, Qt::Key_Return);
        QVERIFY(!app::blur_gallery::working());
        QCOMPARE(paths[0]["speed"], Json(20));
        QCOMPARE(*book.layer("pic")->raster_png, op_pixels(Json{{"kind", "blur_path"}, {"paths", paths}}));
        book.window->action(QStringLiteral("act_undo"))->trigger();
        // on a smart object: a smart filter; its work again from the layer panel with its pins, one more, smart_filter_set
        QVERIFY(book.window->apply_ops(Json::array({Json{{"op", "smart_convert"}, {"page", 1}, {"ids", Json::array({"pic"})}, {"new_id", "so"}}})));
        book.window->set_target_layer("so");
        book.act("act_filter_blur_field")->trigger();
        QVERIFY(app::blur_gallery::working());
        QTest::mouseClick(canvas, Qt::LeftButton, {}, canvas->to_widget(QPointF(30, 30)).toPoint());
        QCOMPARE(pins().size(), std::size_t(2));
        QTest::keyClick(canvas, Qt::Key_Return);
        Json filters = (*book.layer("so")->smart)["filters"];
        QCOMPARE(filters.size(), std::size_t(1));
        QCOMPARE(filters[0]["kind"], Json("blur_field"));
        QCOMPARE(filters[0]["params"]["pins"].size(), std::size_t(2));
        book.window->layer_panel()->edit_smart_filter(0);
        QVERIFY(app::blur_gallery::working());
        QCOMPARE(pins().size(), std::size_t(2));
        QTest::mouseClick(canvas, Qt::LeftButton, {}, canvas->to_widget(QPointF(10, 10)).toPoint());
        QCOMPARE(pins().size(), std::size_t(3));
        QTest::keyClick(canvas, Qt::Key_Return);
        filters = (*book.layer("so")->smart)["filters"];
        QCOMPARE(filters.size(), std::size_t(1));
        QCOMPARE(filters[0]["params"]["pins"].size(), std::size_t(3));
    }
};

QTEST_MAIN(TestGuiFilters)
#include "test_gui_filters.moc"
