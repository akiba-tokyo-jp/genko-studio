// PC3 B2 (SPEC.md COLOR-06「範囲の中身の編集」; the user's decision 5: Photoshop's names) on the offscreen platform: 編集's
// clipboard with the system's — a copy put there as Genko's items and as a picture, a picture another program put there
// pasted as a new layer at the view's centre, Genko's items another window put there pasted —, 結合部分をコピー, 特殊ペースト
// (同じ位置・選択範囲内へ・選択範囲外へ: a mask fixed to the page, the selection let go), 選択範囲をコピー／カットしたレイヤー
// (Ctrl+J, Ctrl+Shift+J; Ctrl+J without a selection duplicates the layer), 塗りつぶし… and 境界線を描く…, copying on a
// precise page making a precise layer, and the commands' words, keys and places.
#include <QtTest>
#include <QAction>
#include <QCheckBox>
#include <QClipboard>
#include <QComboBox>
#include <QDialog>
#include <QDoubleSpinBox>
#include <QGuiApplication>
#include <QImage>
#include <QMenu>
#include <QMenuBar>
#include <QMimeData>
#include <QSpinBox>
#include "gui_support.hpp"
#include "app/canvas.hpp"
#include "app/inject.hpp"
#include "app/main_window.hpp"
#include "core/color_raster.hpp"
#include "core/command_bus.hpp"
#include "render/image.hpp"
#include "render/png.hpp"
using namespace genko;
using core::Json;
namespace inject = genko::app::inject;

namespace {

// A menu of the menu bar by its path ("編集/特殊ペースト").
QMenu* menu_at(const app::MainWindow* window, const QString& path) {
    QMenu* menu = nullptr;
    QList<QAction*> actions = window->menuBar()->actions();
    for (const QString& part : path.split(QLatin1Char('/'))) {
        QMenu* found = nullptr;
        for (QAction* a : actions)
            if (a->menu() != nullptr && a->menu()->title() == part) found = a->menu();
        if (found == nullptr) return nullptr;
        menu = found;
        actions = menu->actions();
    }
    return menu;
}

// A book with one colour page (60 × 60 mm, one panel), a line on the ink layer from (10, 10) to (30, 30).
core::Document book_doc() {
    auto doc = core::new_episode("クリップボード", core::Num(1), 1, core::PageSpec::custom(60, 60, 50, 50, 1, 2, 2, 2, 2, 72, "color"));
    return core::CommandBus().apply(doc, Json::array({Json{{"op", "add_stroke"}, {"page", 1}, {"layer", "ink"},
                                                           {"points", Json::array({Json::array({10.0, 10.0, 0.7}), Json::array({30.0, 30.0, 0.7})})},
                                                           {"stabilize", 0}}}), core::Actor("human:tester")).doc;
}

struct Studio {
    QTemporaryDir tmp;
    std::shared_ptr<app::Session> session;
    std::unique_ptr<app::MainWindow> window;
    std::string ink;
    gui_test::Answers answers;
    Studio() {
        (void)gui_test::config_folder();
        QGuiApplication::clipboard()->clear();
        const auto path = gui_test::path_of(tmp.path() + "/book");
        gui_test::write_book(path, book_doc());
        session = app::Session::open(path, gui_test::quick(gui_test::path_of(tmp.path() + "/recovery")));
        ink = gui_test::ink_of(session->document().page(0))->id;
        window = std::make_unique<app::MainWindow>(session);
        window->resize(1200, 860);
        window->show();
        canvas()->fit_page();
    }
    app::PageCanvas* canvas() const { return window->canvas(); }
    const core::Page& page() const { return session->document().page(0); }
    const core::Layer* layer(const std::string& id) const {
        for (const auto& l : page().layers) if (l.id == id) return &l;
        return nullptr;
    }
    void trigger(const char* name) const { window->action(QString::fromLatin1(name))->trigger(); }
    void choose_around_the_line() const {
        trigger("act_marquee");
        inject::mouse_stroke(canvas(), {QPointF(5, 5), QPointF(35, 35)});
    }
};

}  // namespace

class TestGuiClipboard : public QObject {
    Q_OBJECT
private slots:
    void initTestCase() { qRegisterMetaType<genko::app::StrokeInput>(); }

    // The commands' words, keys and places (Photoshop's).
    void theCommands() {
        Studio studio;
        struct Expected { const char* attribute; const char* text; const char* key; const char* place; };
        for (const Expected& e : std::initializer_list<Expected>{
                 {"act_copy_merged", "結合部分をコピー", "Ctrl+Shift+C", "編集"},
                 {"act_paste_in_place", "同じ位置にペースト", "", "編集/特殊ペースト"},
                 {"act_paste_into", "選択範囲内へペースト", "Ctrl+Alt+Shift+V", "編集/特殊ペースト"},
                 {"act_paste_outside", "選択範囲外へペースト", "", "編集/特殊ペースト"},
                 {"act_fill_dialog", "塗りつぶし…", "Shift+F5", "編集"},
                 {"act_stroke_dialog", "境界線を描く…", "", "編集"},
                 {"act_layer_via_copy", "選択範囲をコピーしたレイヤー", "Ctrl+J", "レイヤー/新規"},
                 {"act_layer_via_cut", "選択範囲をカットしたレイヤー", "Ctrl+Shift+J", "レイヤー/新規"},
                 {"act_select_all_layers", "すべてのレイヤー", "Ctrl+Alt+A", "選択範囲"},
                 {"act_deselect_layers", "レイヤーの選択を解除", "", "選択範囲"},
                 {"act_find_layers", "レイヤーを検索", "Ctrl+Alt+Shift+F", "選択範囲"},
                 {"act_isolate_layers", "レイヤーを分離", "", "選択範囲"},
                 {"act_flatten_masks", "すべてのマスクを統合", "", "ファイル/スクリプト"}}) {
            QAction* a = studio.window->action(QString::fromLatin1(e.attribute));
            QVERIFY2(a != nullptr, e.attribute);
            QCOMPARE(a->text(), QString::fromUtf8(e.text));
            if (*e.key != '\0') QCOMPARE(a->shortcut(), QKeySequence(QString::fromLatin1(e.key)));
            else QVERIFY2(a->shortcut().isEmpty(), e.attribute);
            QMenu* menu = menu_at(studio.window.get(), QString::fromUtf8(e.place));
            QVERIFY2(menu != nullptr && menu->actions().contains(a), e.attribute);
        }
        QVERIFY(studio.window->action(QStringLiteral("act_layer_dup"))->shortcut().isEmpty());  // (Ctrl+J: 選択範囲をコピーしたレイヤー's)
    }

    // A copy goes on the system's clipboard as Genko's items and as a picture; pasted back where it was. A picture another
    // program put there: a new paint layer at the view's centre. Genko's items from another window: pasted (no selection).
    void systemClipboard() {
        Studio studio;
        studio.window->set_target_layer(studio.ink);
        studio.choose_around_the_line();
        studio.trigger("act_copy");
        const QMimeData* data = QGuiApplication::clipboard()->mimeData();
        QVERIFY(data != nullptr && data->hasFormat(QStringLiteral("application/x-genko-items+json")) && data->hasImage());
        const QImage picture = qvariant_cast<QImage>(data->imageData());
        QVERIFY2(std::abs(picture.width() - 20 * 200 / 25.4) < 20, qPrintable(QString::number(picture.width())));  // (the line's box, 200 dpi)
        const auto count = studio.page().layers.size();
        studio.trigger("act_paste");
        QCOMPARE(studio.page().layers.size(), count + 1);
        QCOMPARE(studio.window->target_layer()->stroke_count(), std::size_t(1));
        QVERIFY(studio.canvas()->selection());  // (where it was copied: chosen again)
        // a picture of another program
        QImage red(50, 40, QImage::Format_RGBA8888);
        red.fill(QColor(220, 20, 30));
        QGuiApplication::clipboard()->setImage(red);
        studio.trigger("act_paste");
        QCOMPARE(studio.page().layers.size(), count + 2);
        const core::Layer* pasted = studio.window->target_layer();
        QCOMPARE(pasted->kind, core::LayerKind::Raster);
        QCOMPARE(pasted->patches.size(), std::size_t(1));
        const Json box = pasted->patches[0].attrs["box"];
        QVERIFY(std::abs(box[2].get<double>() - 50 * 25.4 / 200) < 0.01 && std::abs(box[3].get<double>() - 40 * 25.4 / 200) < 0.01);
        const QPointF centre = studio.canvas()->to_mm(QPointF(studio.canvas()->width() / 2.0, studio.canvas()->height() / 2.0));
        QVERIFY(std::abs(box[0].get<double>() + box[2].get<double>() / 2 - centre.x()) < 0.01);
        QVERIFY(!studio.canvas()->selection());
        // Genko's items another window put there
        auto* items = new QMimeData;
        const Json stroke{{"points", Json::array({Json::array({40.0, 40.0}), Json::array({50.0, 45.0})})}, {"width_mm", 0.8}};
        items->setData(QStringLiteral("application/x-genko-items+json"), QByteArray::fromStdString(Json{{"strokes", Json::array({stroke})}}.dump()));
        QGuiApplication::clipboard()->setMimeData(items);
        studio.trigger("act_paste");
        QCOMPARE(studio.page().layers.size(), count + 3);
        QCOMPARE(studio.window->target_layer()->stroke_count(), std::size_t(1));
        QCOMPARE(studio.window->target_layer()->kind, core::LayerKind::Strokes);
        // nothing on either clipboard: a word
        QGuiApplication::clipboard()->clear();
        Studio fresh;
        fresh.trigger("act_paste");
        QCOMPARE(fresh.window->last_notice(), QStringLiteral("ペーストするものがありません（先にコピー）"));
        // copy without a selection: the whole layer
        fresh.window->set_target_layer(fresh.ink);
        fresh.trigger("act_copy");
        QVERIFY(QGuiApplication::clipboard()->mimeData()->hasFormat(QStringLiteral("application/x-genko-items+json")));
    }

    // 選択範囲内へ・外へペースト: the copy at the selection's centre, the selection (outside: the rest) its mask, fixed to the
    // page; the selection let go. 同じ位置にペースト: as ペースト.
    void pasteIntoAndOutside() {
        Studio studio;
        studio.window->set_target_layer(studio.ink);
        studio.choose_around_the_line();
        studio.trigger("act_copy");
        for (const char* how : {"act_paste_into", "act_paste_outside"}) {
            studio.trigger("act_deselect");
            studio.choose_around_the_line();
            studio.trigger(how);
            const core::Layer* pasted = studio.window->target_layer();
            QVERIFY2(pasted->mask && !pasted->mask->linked, how);
            QVERIFY2(!studio.canvas()->selection(), how);
            const render::Image mask = render::open_image(*pasted->mask->png).convert("L");
            const int inside = mask.getpixel(int(20 * 150 / 25.4), int(20 * 150 / 25.4))[0];
            QCOMPARE(inside, std::string(how) == "act_paste_into" ? 255 : 0);
        }
        const auto count = studio.page().layers.size();
        studio.trigger("act_paste_in_place");
        QCOMPARE(studio.page().layers.size(), count + 1);
        QVERIFY(!studio.window->target_layer()->mask);
    }

    // Ctrl+J and Ctrl+Shift+J: the selection's part copied or cut to a new layer just above; Ctrl+J without a selection
    // duplicates the layer.
    void layerViaCopyAndCut() {
        Studio studio;
        studio.window->set_target_layer(studio.ink);
        studio.choose_around_the_line();
        const auto count = studio.page().layers.size();
        studio.trigger("act_layer_via_copy");
        QCOMPARE(studio.page().layers.size(), count + 1);
        QCOMPARE(studio.window->target_layer()->stroke_count(), std::size_t(1));
        QCOMPARE(studio.layer(studio.ink)->stroke_count(), std::size_t(1));
        studio.window->set_target_layer(studio.ink);
        studio.trigger("act_layer_via_cut");
        QCOMPARE(studio.page().layers.size(), count + 2);
        QCOMPARE(studio.window->target_layer()->stroke_count(), std::size_t(1));
        QCOMPARE(studio.layer(studio.ink)->stroke_count(), std::size_t(0));
        studio.window->action(QStringLiteral("act_undo"))->trigger();  // (one Undo: the new layer and the cut)
        QCOMPARE(studio.page().layers.size(), count + 1);
        QCOMPARE(studio.layer(studio.ink)->stroke_count(), std::size_t(1));
        studio.trigger("act_deselect");
        studio.window->set_target_layer(studio.ink);
        studio.trigger("act_layer_via_copy");  // (nothing chosen: レイヤーを複製)
        QCOMPARE(studio.page().layers.size(), count + 2);
    }

    // 塗りつぶし… (内容・不透明度・透明部分の保持; nothing chosen: the whole layer) and 境界線を描く… (幅・カラー・位置).
    void fillAndStroke() {
        Studio studio;
        studio.window->set_target_layer(studio.ink);
        studio.answers.responder->exec = [](QDialog* dialog) {
            if (dialog->objectName() == QStringLiteral("fill_dialog")) {
                auto* contents = dialog->findChild<QComboBox*>(QStringLiteral("fill_contents"));
                contents->setCurrentIndex(contents->findData(QStringLiteral("black")));
                dialog->findChild<QSpinBox*>(QStringLiteral("fill_opacity"))->setValue(50);
                return int(QDialog::Accepted);
            }
            if (dialog->objectName() == QStringLiteral("stroke_dialog")) {
                dialog->findChild<QDoubleSpinBox*>(QStringLiteral("stroke_width"))->setValue(1.5);
                auto* location = dialog->findChild<QComboBox*>(QStringLiteral("stroke_location"));
                location->setCurrentIndex(location->findData(QStringLiteral("inside")));
                return int(QDialog::Accepted);
            }
            return int(QDialog::Rejected);
        };
        studio.choose_around_the_line();
        studio.trigger("act_fill_dialog");
        const core::Layer* ink = studio.layer(studio.ink);
        QCOMPARE(ink->patches.size(), std::size_t(1));
        QCOMPARE(ink->patches[0].attrs["rgb"], Json::array({0, 0, 0}));
        QCOMPARE(ink->patches[0].attrs["opacity"], Json(0.5));
        studio.trigger("act_stroke_dialog");
        QCOMPARE(studio.layer(studio.ink)->patches.size(), std::size_t(2));
        studio.trigger("act_deselect");
        studio.trigger("act_fill_dialog");  // (the whole layer)
        const Json box = studio.layer(studio.ink)->patches.back().attrs["box"];
        QVERIFY2(box[2].get<double>() > 59 && box[3].get<double>() > 59, box.dump().c_str());
    }

    // 結合部分をコピー: the page as it shows in the selection, pasted as a picture on a new paint layer.
    void copyMerged() {
        Studio studio;
        studio.choose_around_the_line();
        studio.trigger("act_copy_merged");
        const auto count = studio.page().layers.size();
        studio.trigger("act_paste");
        QCOMPARE(studio.page().layers.size(), count + 1);
        const core::Layer* pasted = studio.window->target_layer();
        QCOMPARE(pasted->patches.size(), std::size_t(1));
        QCOMPARE(pasted->patches[0].attrs["mode"], Json("image"));
    }

    // On a page of 16 bits, a precise layer's part copied and pasted: a new precise layer of its samples.
    void precisePieces() {
        Studio studio;
        Json pixels = Json::array();
        for (int i = 0; i < 120 * 120; ++i)
            for (const std::int64_t v : {std::int64_t{1000}, std::int64_t{40000}, std::int64_t{65535}, std::int64_t{65535}}) pixels.push_back(v);
        QVERIFY(studio.window->apply_ops(Json::array({Json{{"op", "set_color_mode"}, {"page", 1}, {"mode", "rgb"}, {"depth", 16}},
                                                      Json{{"op", "put_color_raster"}, {"page", 1}, {"width", 120}, {"height", 120}, {"precision", "u16"},
                                                           {"pixels", pixels}}})));
        studio.window->set_target_layer(studio.page().layers.back().id);
        studio.choose_around_the_line();
        studio.trigger("act_copy");
        studio.trigger("act_paste");
        const core::Layer* pasted = studio.window->target_layer();
        QVERIFY(pasted->color_raster && !pasted->raster_png && pasted->patches.empty());
        const core::ColorRasterView view(*pasted->color_raster);
        QCOMPARE(view.metadata("")["precision"], Json("u16"));
        const auto p = view.pixel(std::size_t(view.height() / 3) * view.width() + view.width() / 3);  // (20, 20 mm: inside)
        QVERIFY2(std::abs(p[1] - 40000 / 65535.0) < 1e-6 && p[3] == 1, qPrintable(QString::number(p[1])));
    }
};

QTEST_MAIN(TestGuiClipboard)
#include "test_gui_clipboard.moc"
