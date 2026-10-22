// 画素を変える筆 (PC4 E5, SPEC.md COLOR-09「画素を変える筆」) on the offscreen platform: ぼかしツール・シャープツール・指先ツール・
// 覆い焼きツール・焼き込みツール・スポンジツール・混合ブラシツール・背景消しゴムツール・色の置き換えツール by Photoshop's names, in
// ツール and their groups (a palette button's group, the tools group), one settings page showing each tool's rows; a stroke
// sends its op with the tool's settings and the 描画色 and changes the layer (履歴's words); a layer with lines asks before
// they become pixels (やめる: nothing); the Mixer Brush's paint kept between strokes, 補充 and クリーン; the mask edited.

#include <QtTest>
#include <QAction>
#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QDoubleSpinBox>
#include <QLabel>
#include <QMenu>
#include <QMenuBar>
#include <QPushButton>
#include <QSpinBox>
#include <QToolBar>
#include <QToolButton>

#include <cmath>

#include "gui_support.hpp"
#include "app/canvas.hpp"
#include "app/history_panel.hpp"
#include "app/inject.hpp"
#include "app/main_window.hpp"
#include "app/session.hpp"
#include "app/tool_settings.hpp"
#include "core/base64.hpp"
#include "core/command_bus.hpp"
#include "render/ops_registry.hpp"
#include "render/png.hpp"

using namespace genko;
using core::Json;
namespace inject = genko::app::inject;

namespace {

constexpr int kSide = 472;  // (the 60 mm page at the working 200 dpi)

double px_of(double mm) { return mm * kSide / 60.0; }

// A book of one 60 × 60 mm colour page: a paint layer "paint" — red on the left half, white on the right — and a pen
// layer "pen" with a line.
core::Document book_doc() {
    auto doc = core::new_episode("画素の道具", core::Num(1), 1, core::PageSpec::custom(60, 60, 50, 50, 1, 2, 2, 2, 2, 72, "color"));
    std::string bytes;
    for (int y = 0; y < kSide; ++y)
        for (int x = 0; x < kSide; ++x) {
            const std::array<int, 4> c = x < kSide / 2 ? std::array<int, 4>{230, 30, 30, 255} : std::array<int, 4>{255, 255, 255, 255};
            for (const int v : c) bytes.push_back(static_cast<char>(v));
        }
    const render::Image picture = render::Image::frombytes("RGBA", render::Size{kSide, kSide}, bytes);
    return core::CommandBus(render::ops_registry())
        .apply(doc,
               Json::array({Json{{"op", "add_layer"}, {"page", 1}, {"kind", "paint"}, {"id", "paint"}},
                            Json{{"op", "set_layer"}, {"page", 1}, {"id", "paint"}, {"panel_each", false}, {"panel_clip", false}},
                            Json{{"op", "put_raster"}, {"page", 1}, {"id", "paint"}, {"png_base64", core::b64encode(render::write_png(picture))}},
                            Json{{"op", "add_layer"}, {"page", 1}, {"kind", "pen"}, {"id", "pen"}},
                            Json{{"op", "add_stroke"}, {"page", 1}, {"layer_id", "pen"}, {"width_mm", 1},
                                 {"points", Json::array({Json::array({10, 50}), Json::array({50, 50})})}}}),
               core::Actor("human:tester"))
        .doc;
}

struct Studio {
    QTemporaryDir tmp;
    std::shared_ptr<app::Session> session;
    std::unique_ptr<app::MainWindow> window;
    gui_test::Answers answers;
    std::vector<Json> ops;  // (each change's ops, as the window sent them)
    Studio() {
        (void)gui_test::config_folder();
        const auto path = gui_test::path_of(tmp.path() + "/book");
        gui_test::write_book(path, book_doc());
        session = app::Session::open(path, gui_test::quick(gui_test::path_of(tmp.path() + "/recovery")));
        window = std::make_unique<app::MainWindow>(session);
        window->resize(1200, 860);
        window->show();
        canvas()->fit_page();
        window->set_target_layer("paint");
        QObject::connect(session.get(), &app::Session::changed, window.get(), [this](const app::BookChange& c) {
            if (c.why == app::BookChange::Why::Edit) ops.push_back(c.ops);
        });
    }
    app::PageCanvas* canvas() const { return window->canvas(); }
    const core::Page& page() const { return session->document().page(0); }
    const core::Layer* layer(const std::string& id) const {
        for (const auto& l : page().layers)
            if (l.id == id) return &l;
        return nullptr;
    }
    void trigger(const char* name) const { window->action(QString::fromLatin1(name))->trigger(); }
    // The layer's colour at a point (mm), 0..255.
    std::vector<double> at(double x_mm, double y_mm, const std::string& id = "paint") const {
        const render::Image image = render::open_image(*layer(id)->raster_png).convert("RGBA");
        return image.getpixel(static_cast<int>(px_of(x_mm)), static_cast<int>(px_of(y_mm)));
    }
    template <class T>
    T* field(const char* name) const {
        return window->findChild<T*>(QString::fromLatin1(name));
    }
};

QMenu* menu_at(const app::MainWindow* window, const QString& title) {
    for (QAction* a : window->menuBar()->actions())
        if (a->menu() != nullptr && a->menu()->title() == title) return a->menu();
    return nullptr;
}

}  // namespace

class TestGuiPaintTools : public QObject {
    Q_OBJECT
private slots:
    void initTestCase() { qRegisterMetaType<genko::app::StrokeInput>(); }

    // The nine tools by Photoshop's names, in ツール once and in the tools group, each in its group (the palette's button of
    // the group offers it on a long press or right click).
    void theTools() {
        Studio studio;
        app::MainWindow* w = studio.window.get();
        const std::vector<std::pair<const char*, const char*>> tools{
            {"act_blur_tool", "ぼかしツール"},       {"act_sharpen_tool", "シャープツール"}, {"act_smudge_tool", "指先ツール"},
            {"act_dodge_tool", "覆い焼きツール"},    {"act_burn_tool", "焼き込みツール"},     {"act_sponge_tool", "スポンジツール"},
            {"act_mixer_brush", "混合ブラシツール"}, {"act_background_eraser", "背景消しゴムツール"}, {"act_color_replacement", "色の置き換えツール"}};
        QMenu* menu = menu_at(w, QStringLiteral("ツール"));
        QVERIFY(menu != nullptr);
        for (const auto& [name, text] : tools) {
            QAction* a = w->action(QString::fromLatin1(name));
            QVERIFY2(a != nullptr, name);
            QCOMPARE(a->text(), QString::fromUtf8(text));
            QVERIFY(a->isCheckable());
            QVERIFY(!a->statusTip().isEmpty());
            QCOMPARE(menu->actions().count(a), 1);
            QVERIFY(w->tools_group()->actions().contains(a));
            QVERIFY2(w->tool_group_of(a).size() >= 3, name);
        }
        QCOMPARE(w->tool_group_of(w->action(QStringLiteral("act_blend"))),
                 (std::vector<QAction*>{w->action(QStringLiteral("act_blur_tool")), w->action(QStringLiteral("act_sharpen_tool")),
                                        w->action(QStringLiteral("act_smudge_tool")), w->action(QStringLiteral("act_blend"))}));
        auto* blend = qobject_cast<QToolButton*>(w->tool_palette()->widgetForAction(w->action(QStringLiteral("act_blend"))));
        QVERIFY(blend != nullptr && blend->menu() != nullptr);
        QVERIFY(blend->menu()->actions().contains(w->action(QStringLiteral("act_smudge_tool"))));
        auto* eraser = qobject_cast<QToolButton*>(w->tool_palette()->widgetForAction(w->action(QStringLiteral("act_eraser"))));
        QVERIFY(eraser->menu()->actions().contains(w->action(QStringLiteral("act_background_eraser"))));
        auto* pen = qobject_cast<QToolButton*>(w->tool_palette()->widgetForAction(w->action(QStringLiteral("act_pen"))));
        QVERIFY(pen->menu()->actions().contains(w->action(QStringLiteral("act_mixer_brush"))));
        QVERIFY(pen->menu()->actions().contains(w->action(QStringLiteral("act_color_replacement"))));
        QCOMPARE(w->tool_group_of(w->action(QStringLiteral("act_dodge_tool"))).size(), std::size_t{3});
    }

    // One settings page: each tool shows its own rows (the toning's 範囲, the focus's 強さ, the Mixer Brush's buttons…).
    void theSettingsPage() {
        Studio studio;
        app::MainWindow* w = studio.window.get();
        studio.trigger("act_dodge_tool");
        QCOMPARE(studio.canvas()->tool(), QStringLiteral("dodge"));
        auto* range = studio.field<QComboBox>("pixel_range");
        auto* strength = studio.field<QSpinBox>("pixel_strength");
        auto* load = studio.field<QPushButton>("pixel_mixer_load");
        QVERIFY(range != nullptr && strength != nullptr && load != nullptr);
        QVERIFY(range->isVisibleTo(w));
        QVERIFY(!strength->isVisibleTo(w));
        studio.trigger("act_blur_tool");
        QVERIFY(!range->isVisibleTo(w));
        QVERIFY(strength->isVisibleTo(w));
        QVERIFY(!studio.field<QCheckBox>("pixel_protect_detail")->isVisibleTo(w));
        studio.trigger("act_sharpen_tool");
        QVERIFY(studio.field<QCheckBox>("pixel_protect_detail")->isVisibleTo(w));
        studio.trigger("act_mixer_brush");
        QVERIFY(load->isVisibleTo(w));
        QVERIFY(!strength->isVisibleTo(w));
        studio.trigger("act_background_eraser");
        QVERIFY(studio.field<QComboBox>("pixel_sampling")->isVisibleTo(w));
        QVERIFY(studio.field<QCheckBox>("pixel_protect_foreground")->isVisibleTo(w));
        QVERIFY(!studio.field<QComboBox>("pixel_replace_mode")->isVisibleTo(w));
        studio.trigger("act_color_replacement");
        QVERIFY(studio.field<QComboBox>("pixel_replace_mode")->isVisibleTo(w));
        QVERIFY(!studio.field<QCheckBox>("pixel_protect_foreground")->isVisibleTo(w));
        QCOMPARE(w->tool_settings()->findChild<QWidget*>(QStringLiteral("pixel_tools_page")) != nullptr, true);
    }

    // A stroke sends the tool's op with its settings: Burn darkens the red, Dodge leaves the white; Blur mixes across the
    // edge; Color Replacement takes the 描画色; 履歴's words.
    void strokesSendTheirOps() {
        Studio studio;
        studio.trigger("act_burn_tool");
        studio.field<QComboBox>("pixel_range")->setCurrentIndex(studio.field<QComboBox>("pixel_range")->findData(QStringLiteral("highlights")));
        studio.field<QSpinBox>("pixel_exposure")->setValue(80);
        studio.field<QDoubleSpinBox>("pixel_size")->setValue(8);
        const std::vector<double> red = studio.at(15, 20);
        inject::mouse_stroke(studio.canvas(), {QPointF(5, 20), QPointF(25, 20)});
        QVERIFY(!studio.ops.empty());
        const Json op = studio.ops.back().back();
        QCOMPARE(op["op"], Json("burn"));
        QCOMPARE(op["range"], Json("highlights"));
        QCOMPARE(op["exposure"], Json(80.0));
        QCOMPARE(op["width_mm"], Json(8.0));
        QCOMPARE(op["layer_id"], Json("paint"));
        QVERIFY(studio.at(15, 20)[0] < red[0]);
        QCOMPARE(app::history::describe(studio.ops.back()), QStringLiteral("焼き込んだ"));
        studio.trigger("act_dodge_tool");
        const std::vector<double> white = studio.at(45, 30);
        inject::mouse_stroke(studio.canvas(), {QPointF(40, 30), QPointF(55, 30)});
        QCOMPARE(studio.ops.back().back()["op"], Json("dodge"));
        QCOMPARE(studio.at(45, 30), white);  // (white stays white)
        studio.trigger("act_blur_tool");
        studio.field<QSpinBox>("pixel_strength")->setValue(100);
        inject::mouse_stroke(studio.canvas(), {QPointF(30, 40), QPointF(30, 45)});
        QCOMPARE(studio.ops.back().back()["op"], Json("blur_brush"));
        const std::vector<double> edge = studio.at(30, 42);
        QVERIFY2(edge[1] > 40 && edge[1] < 250, qPrintable(QString::number(edge[1])));
        studio.trigger("act_color_replacement");
        inject::mouse_stroke(studio.canvas(), {QPointF(8, 8), QPointF(20, 8)});
        const Json replace = studio.ops.back().back();
        QCOMPARE(replace["op"], Json("color_replacement"));
        QVERIFY(replace["rgb"].is_array() && replace["swatch_rgb"].is_array());
        QCOMPARE(replace["mode"], Json("color"));
    }

    // A pen layer's lines would become pixels: asked first — やめる changes nothing, 描く makes them pixels, not asked again.
    void aLayerWithLinesAsksFirst() {
        Studio studio;
        studio.window->set_target_layer("pen");
        int asked = 0;
        int answer = QDialog::Rejected;
        studio.answers.responder->exec = [&](QDialog* dialog) {
            if (dialog->objectName() != QStringLiteral("pixel_tool_bakes")) return int(QDialog::Rejected);
            ++asked;
            return answer;
        };
        studio.trigger("act_smudge_tool");
        const std::int64_t revision = studio.session->document().revision;
        inject::mouse_stroke(studio.canvas(), {QPointF(20, 48), QPointF(20, 52)});
        QCOMPARE(asked, 1);
        QCOMPARE(studio.session->document().revision, revision);
        QCOMPARE(studio.layer("pen")->stroke_count(), std::size_t(1));
        answer = QDialog::Accepted;
        inject::mouse_stroke(studio.canvas(), {QPointF(20, 48), QPointF(20, 52)});
        QCOMPARE(asked, 2);
        QCOMPARE(studio.layer("pen")->stroke_count(), std::size_t(0));
        QVERIFY(studio.layer("pen")->kind == core::LayerKind::Raster);
        inject::mouse_stroke(studio.canvas(), {QPointF(30, 48), QPointF(30, 52)});
        QCOMPARE(asked, 2);
    }

    // The Mixer Brush: the paint the stroke left kept for the next (sent back as its reservoir), shown; クリーン empties it,
    // 補充 fills it with the 描画色.
    void theMixerBrushsPaint() {
        Studio studio;
        app::MainWindow* w = studio.window.get();
        studio.trigger("act_mixer_brush");
        studio.field<QCheckBox>("pixel_clean_after")->setChecked(false);
        QVERIFY(w->mixer_reservoir.is_null());
        inject::mouse_stroke(studio.canvas(), {QPointF(5, 10), QPointF(40, 10)});
        const Json first = studio.ops.back().back();
        QCOMPARE(first["op"], Json("mixer_brush"));
        QVERIFY(!first.contains("reservoir"));
        QCOMPARE(first["clean_after"], Json(false));
        QVERIFY2(w->mixer_reservoir.is_object() && w->mixer_reservoir["paint"].is_array(), w->mixer_reservoir.dump().c_str());
        QVERIFY(w->mixer_reservoir["pickup"].is_array());
        QVERIFY(studio.field<QLabel>("pixel_mixer_paint")->text().contains(QStringLiteral("取り込んだ色")));
        const Json kept = w->mixer_reservoir;
        inject::mouse_stroke(studio.canvas(), {QPointF(5, 14), QPointF(40, 14)});
        QCOMPARE(studio.ops.back().back()["reservoir"], kept);  // (the paint the last stroke left)
        studio.field<QPushButton>("pixel_mixer_clean")->click();
        QVERIFY(w->mixer_reservoir.is_null());
        QVERIFY(studio.field<QLabel>("pixel_mixer_paint")->text().contains(QStringLiteral("きれい")));
        studio.field<QPushButton>("pixel_mixer_load")->click();
        QVERIFY(w->mixer_reservoir["paint"].is_array());
        QCOMPARE(w->mixer_reservoir["amount"], Json(1.0));
    }

    // The Background Eraser takes the colour under its centre once; マスクを編集する: the brush paints the mask.
    void theEraserAndTheMask() {
        Studio studio;
        studio.trigger("act_background_eraser");
        studio.field<QComboBox>("pixel_sampling")->setCurrentIndex(studio.field<QComboBox>("pixel_sampling")->findData(QStringLiteral("once")));
        studio.field<QDoubleSpinBox>("pixel_size")->setValue(10);
        inject::mouse_stroke(studio.canvas(), {QPointF(36, 30), QPointF(26, 30)});
        const Json op = studio.ops.back().back();
        QCOMPARE(op["op"], Json("background_eraser"));
        QCOMPARE(op["sampling"], Json("once"));
        QCOMPARE(studio.at(35, 30)[3], 0.0);    // (the white, sampled first, erased)
        QCOMPARE(studio.at(27, 30)[3], 255.0);  // (the red kept)
        // the mask: a layer mask painted by the blur, its pixels untouched
        QVERIFY(studio.window->apply_ops(Json::array({Json{{"op", "set_layer_mask"}, {"page", 1}, {"id", "paint"},
                                                           {"area", Json{{"rect", Json::array({0, 0, 30, 60})}}}}})));
        const std::string pixels = *studio.layer("paint")->raster_png;
        const std::string mask = *studio.layer("paint")->mask->png;
        studio.window->mask_edit = true;
        studio.trigger("act_blur_tool");
        studio.field<QSpinBox>("pixel_strength")->setValue(100);
        inject::mouse_stroke(studio.canvas(), {QPointF(30, 20), QPointF(30, 40)});
        QCOMPARE(studio.ops.back().back()["layer_mask"], Json(true));
        QCOMPARE(*studio.layer("paint")->raster_png, pixels);
        QVERIFY(*studio.layer("paint")->mask->png != mask);
    }
};

QTEST_MAIN(TestGuiPaintTools)
#include "test_gui_paint_tools.moc"
