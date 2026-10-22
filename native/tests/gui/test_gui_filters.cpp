// PC4 節 D (SPEC.md COLOR-08「フィルター」, design 5; the user's decision 1) on the offscreen platform: the フィルター menu after
// レイヤー — its commands in Photoshop's order and words, their keys (前回のフィルター Ctrl+F, Camera Raw フィルター…
// Ctrl+Shift+A, 広角補正… Ctrl+Alt+Shift+A, 消点… Ctrl+Alt+V, ゆがみ… Ctrl+Shift+X moved from 編集), the kinds not in this
// build yet grey —; ぼかし（ガウス）…'s window made from its table with the result on the page, one op and one Undo; ぼかし
// without a window; 前回のフィルター sending the last one again; フェード… after it; on a smart object a smart filter (its
// window again from the panel); 漫画's 光彩… in its old window; a heavy one's preview on a thread and its progress with 中止;
// the layer panel's かける… opening the menu. (The frame of D1…D7's windows and kinds: their own tests add theirs.) D1: its
// commands lit — ぼかし・シャープ・ノイズ・その他・ビデオ —, カスタム…'s 5 × 5 grid, 平均 and シャープ without a window.

#include <QtTest>
#include <QAction>
#include <QApplication>
#include <QDialog>
#include <QDoubleSpinBox>
#include <QLabel>
#include <QMenu>
#include <QMenuBar>
#include <QPointer>
#include <QProgressDialog>
#include <QPushButton>
#include <QSpinBox>
#include <QTimer>

#include "gui_support.hpp"
#include "app/canvas.hpp"
#include "app/history_panel.hpp"
#include "app/layer_panel.hpp"
#include "app/config.hpp"
#include "app/main_window.hpp"
#include "app/preferences.hpp"
#include "app/workspace.hpp"
#include "core/base64.hpp"
#include "core/command_bus.hpp"
#include "render/ops_registry.hpp"
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
        // in this build: the blurs of D0, D1's ぼかし・シャープ・ノイズ・その他・ビデオ (and Genko's own); the rest grey until their groups
        w->set_target_layer("pic");
        emit filter->aboutToShow();
        for (const char* on : {"act_filter_blur", "act_filter_blur_more", "act_filter_blur_gaussian", "act_filter_blur_box", "act_filter_manga_glow",
                               "act_filter_manga_lineart", "act_filter_blur_motion", "act_filter_blur_radial", "act_filter_blur_average", "act_filter_blur_lens",
                               "act_filter_sharpen", "act_filter_sharpen_more", "act_filter_sharpen_edges", "act_filter_sharpen_unsharp_mask",
                               "act_filter_sharpen_smart", "act_filter_despeckle", "act_filter_noise_add", "act_filter_noise_median",
                               "act_filter_noise_dust_and_scratches", "act_filter_noise_reduce", "act_filter_other_custom", "act_filter_other_high_pass",
                               "act_filter_other_hsb_hsl", "act_filter_other_maximum", "act_filter_other_minimum", "act_filter_other_offset",
                               "act_filter_video_de_interlace", "act_filter_video_ntsc_colors"})
            QVERIFY2(book.act(on)->isEnabled(), on);
        for (const char* off : {"act_filter_pixelate_mosaic", "act_filter_distort_twirl", "act_filter_stylize_emboss", "act_filter_gallery",
                                "act_filter_camera_raw", "act_filter_render_tree", "act_filter_vanishing_point", "act_filter_blur_tilt_shift", "act_filter_last"})
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
};

QTEST_MAIN(TestGuiFilters)
#include "test_gui_filters.moc"
