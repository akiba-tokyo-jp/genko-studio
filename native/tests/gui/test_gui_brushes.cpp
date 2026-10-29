// The brush panel and ツールの設定 (M3②-2, Python's genko/app/brush_panel.py, tool_settings.py and main.py) on the
// offscreen platform: the brushes listed with a short line each, the pen's settings kept per brush and carried by every
// line, the colour, the size presets; one's own brush made, opened again, forgotten, written to a .genkobrush and read
// from it or from a Photoshop .abr; a line drawn with one's own brush brings its settings into the book; the eraser's
// ways in its op; each tool's page.
#include <QtTest>
#include <QAction>
#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QDoubleSpinBox>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QPushButton>
#include <QSlider>
#include <QSpinBox>
#include <fstream>
#include <stdexcept>
#include "gui_support.hpp"
#include "app/brush_panel.hpp"
#include "app/canvas.hpp"
#include "app/colours.hpp"
#include "app/config.hpp"
#include "app/fields.hpp"
#include "app/inject.hpp"
#include "app/main_window.hpp"
#include "app/tool_settings.hpp"
#include "core/base64.hpp"
#include "core/brush_dabs.hpp"
#include "core/brushes.hpp"
#include "render/brushes.hpp"
#include "render/dabs/lines.hpp"
#include "render/png.hpp"
using namespace genko;
using core::Json;
namespace inject = genko::app::inject;

namespace {

core::Document book_doc() {
    return core::new_episode("ブラシ", core::Num(1), 1, core::PageSpec::custom(60, 60, 50, 50, 1, 2, 2, 2, 2, 72, "mono"));
}

struct Studio {
    QTemporaryDir tmp;
    std::shared_ptr<app::Session> session;
    std::unique_ptr<app::MainWindow> window;
    gui_test::Answers answers;
    Studio() {
        (void)gui_test::config_folder();
        const auto path = gui_test::path_of(tmp.path() + "/book");
        gui_test::write_book(path, book_doc());
        session = app::Session::open(path, gui_test::quick(gui_test::path_of(tmp.path() + "/recovery")));
        window = std::make_unique<app::MainWindow>(session);
        window->resize(1280, 860);
        window->show();
        window->canvas()->fit_page();
    }
    app::BrushPanel& brush() const { return *window->brush_panel(); }
    void choose(const std::string& kind) const {
        auto* list = brush().kinds;
        for (int i = 0; i < list->count(); ++i)
            if (list->item(i)->data(Qt::UserRole).toString().toStdString() == kind) list->setCurrentRow(i);
    }
    const core::Layer& ink() const { return *gui_test::ink_of(session->document().page(0)); }
    void draw(std::vector<QPointF> points) const {
        window->choose_tool(QStringLiteral("pen"));
        inject::mouse_stroke(window->canvas(), points);
    }
};

// a version 1 .abr of one sampled tip (8 × 6)
std::string small_abr() {
    std::string body;
    const auto u8 = [&](int v) { body.push_back(static_cast<char>(v & 0xff)); };
    const auto u16 = [&](int v) { u8(v >> 8); u8(v); };
    const auto u32 = [&](int v) { u16(v >> 16); u16(v & 0xffff); };
    u32(0);
    u16(40);
    u8(1);
    for (int k = 0; k < 4; ++k) u16(0);
    u32(0);
    u32(0);
    u32(6);
    u32(8);
    u16(8);
    u8(0);
    for (int i = 0; i < 48; ++i) u8(i % 7 < 3 ? 230 : 10);
    std::string out;
    out += std::string("\0\x01\0\x01\0\x02", 6);
    out += std::string{static_cast<char>((body.size() >> 24) & 0xff), static_cast<char>((body.size() >> 16) & 0xff), static_cast<char>((body.size() >> 8) & 0xff),
                       static_cast<char>(body.size() & 0xff)};
    return out + body;
}

}  // namespace

class TestGuiBrushes : public QObject {
    Q_OBJECT
private slots:
    void init() {
        (void)gui_test::config_folder();
        app::settings()->clear();
        std::filesystem::remove(render::brushes::library_path(app::config_dir()));
        render::brushes::clear_custom();
    }

    // Every brush in the list with its line; choosing one brings its own settings (kept per brush), and the line
    // drawn carries them.
    void listedAndKeptPerBrush() {
        Studio studio;
        auto& brush = studio.brush();
        QCOMPARE(brush.kinds->count(), static_cast<int>(core::builtin_brushes().size()));
        for (int i = 0; i < brush.kinds->count(); ++i) QVERIFY(!brush.kinds->item(i)->icon().isNull());
        QCOMPARE(brush.kind(), std::string(core::kDefaultBrush));
        QVERIFY(!brush.forget->isEnabled());
        studio.choose("pencil");
        const core::Brush pencil = render::brushes::brush("pencil");
        QCOMPARE(brush.size->value(), pencil.width_mm);
        QCOMPARE(brush.opacity->value(), static_cast<int>(pencil.opacity * 100));
        brush.size->setValue(1.25);
        brush.pressure->setCurrentIndex(2);  // かたい
        brush.ink_pressure->setValue(40);
        studio.choose("gpen");
        QVERIFY(brush.size->value() != 1.25);
        studio.choose("pencil");  // (its own settings again)
        QCOMPARE(brush.size->value(), 1.25);
        QCOMPARE(brush.pressure->currentData().toDouble(), 1.6);
        const Json fields = brush.stroke_fields();
        QCOMPARE(fields["kind"], Json("pencil"));
        QCOMPARE(fields["width_mm"], Json(1.25));
        QCOMPARE(fields["pressure_gamma"], Json(1.6));
        QCOMPARE(fields["pressure_opacity"], Json(0.4));
        QVERIFY(!fields.contains("opacity"));  // (the brush's own opacity)
        brush.opacity->setValue(51);
        QCOMPARE(brush.stroke_fields()["opacity"], Json(core::py_round(0.51 / pencil.opacity, 3)));
        // the line drawn carries the settings
        studio.draw({QPointF(10, 10), QPointF(20, 15), QPointF(30, 12)});
        const auto& stroke = *studio.ink().strokes->items.back();
        QCOMPARE(stroke.kind, std::string("pencil"));
        QCOMPARE(stroke.width_mm, 1.25);
        // the colour: a chip, and the swatch's question
        brush.set_colour({72, 118, 164});
        QCOMPARE(studio.window->pen().rgb, (std::vector<std::int64_t>{72, 118, 164}));
        studio.answers.responder->colour = [](const QColor&, const QString&) { return std::optional<QColor>(QColor(1, 2, 3)); };
        studio.window->action("act_color")->trigger();
        QCOMPARE(brush.rgb(), (std::array<int, 3>{1, 2, 3}));
        // the sample line is drawn
        brush.sample->draw();
        QVERIFY(!brush.sample->pixmap().isNull());
        // a new window reads them back (the app's settings)
        Studio again;
        QCOMPARE(again.brush().kind(), std::string("pencil"));
        QCOMPARE(again.brush().size->value(), 1.25);
        QCOMPARE(again.brush().rgb(), (std::array<int, 3>{1, 2, 3}));
    }

    // The size chips: nine to start with, the size in use added, the nearest one up or down.
    void sizePresets() {
        Studio studio;
        auto& brush = studio.brush();
        QCOMPARE(brush.sizes().size(), std::size_t(9));
        brush.size->setValue(0.77);
        brush.set_sizes({0.5, 0.77, 2.0});
        QCOMPARE(brush.sizes(), (std::vector<double>{0.5, 0.77, 2.0}));
        QCOMPARE(brush.nudge_size(1), 2.0);
        QCOMPARE(brush.nudge_size(-1), 0.77);
        QCOMPARE(brush.nudge_size(-5), 0.5);
        brush.set_sizes({});
        QCOMPARE(brush.sizes().size(), std::size_t(9));
    }

    // One's own brush: made from the one chosen (the dialog's settings), kept in the library and the list (★), opened
    // again, drawn with (the book gets its settings), written to a file, forgotten; read back from the file and from
    // an .abr.
    void ownBrushes() {
        Studio studio;
        auto& brush = studio.brush();
        studio.choose("maru");
        studio.answers.responder->exec = [](QDialog* dialog) {
            auto* d = qobject_cast<app::BrushDialog*>(dialog);
            if (d == nullptr) return int(QDialog::Rejected);
            d->name->setText(QStringLiteral("ざらざら丸"));
            d->texture->setCurrentIndex(d->texture->findData(QStringLiteral("grain")));
            d->width->setValue(1.4);
            d->draw_sample();
            if (d->sample->pixmap().isNull()) return int(QDialog::Rejected);
            return int(QDialog::Accepted);
        };
        brush.make->click();
        const std::string key = brush.kind();
        QVERIFY(key.starts_with("my_"));
        QVERIFY(brush.forget->isEnabled());
        QVERIFY(brush.kinds->currentItem()->text().startsWith(QStringLiteral("★ ざらざら丸")));
        const Json library = render::brushes::load_library(app::config_dir());
        QVERIFY(library.contains(key));
        QCOMPARE(library[key]["texture"], Json("grain"));
        QCOMPARE(library[key]["label"], Json("ざらざら丸"));
        QCOMPARE(render::brushes::brush("my_sample").key, std::string(core::kDefaultBrush));  // (the dialog's sample forgotten)
        // opened again: its name kept, a setting changed
        studio.answers.responder->exec = [](QDialog* dialog) {
            auto* d = qobject_cast<app::BrushDialog*>(dialog);
            if (d == nullptr || d->name->text() != QStringLiteral("ざらざら丸")) return int(QDialog::Rejected);
            d->opacity->setValue(70);
            return int(QDialog::Accepted);
        };
        brush.edit->click();
        QCOMPARE(render::brushes::load_library(app::config_dir())[key]["opacity"], Json(0.7));
        QCOMPARE(brush.kind(), key);
        // drawn with: the book keeps the brush (define_brush before the line)
        studio.draw({QPointF(10, 30), QPointF(30, 30)});
        QVERIFY(studio.session->document().brush_custom.contains(key));
        QCOMPARE(studio.ink().strokes->items.back()->kind, key);
        // written to a file
        const QString file = studio.tmp.path() + "/ざらざら.genkobrush";
        QVERIFY(studio.window->export_brush(file));
        std::ifstream in(gui_test::path_of(file), std::ios::binary);
        const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        const Json written = core::parse_python_json(text);
        QCOMPARE(written["genko_brush"], Json(1));
        QCOMPARE(written["brushes"][key]["label"], Json("ざらざら丸"));
        QCOMPARE(written["brushes"][key]["base"], Json("gpen"));
        // forgotten: out of the library (the book's lines keep it: still listed, from the book)
        brush.forget->click();
        QVERIFY(!render::brushes::load_library(app::config_dir()).contains(key));
        QCOMPARE(brush.kind(), std::string(core::kDefaultBrush));
        // read back: a new key, the same settings
        const auto keys = studio.window->import_brushes(file);
        QCOMPARE(keys.size(), std::size_t(1));
        QVERIFY(keys[0] != key);
        QCOMPARE(render::brushes::brush(keys[0]).texture, std::string("grain"));
        QCOMPARE(brush.kind(), keys[0]);
        // from a Photoshop .abr (through the dialog's file question)
        const QString abr = studio.tmp.path() + "/stamps.abr";
        const std::string bytes = small_abr();
        std::ofstream(gui_test::path_of(abr), std::ios::binary).write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        studio.answers.responder->open_path = [abr](const QString&, const QString&) { return abr; };
        QMenu* files = brush.files->menu();
        files->actions().last()->trigger();
        QCOMPARE(studio.window->last_notice(), QStringLiteral("ブラシを 1 本読み込みました（一覧の ★）"));
        QCOMPARE(render::brushes::brush(brush.kind()).tip, std::string("image"));
        QVERIFY(brush.kinds->currentItem()->text().contains(QStringLiteral("stamps ブラシ 1")));
        // a file that is not a brush file: a word, nothing added
        const QString bad = studio.tmp.path() + "/bad.abr";
        std::ofstream(gui_test::path_of(bad), std::ios::binary) << "no";
        studio.answers.responder->open_path = [bad](const QString&, const QString&) { return bad; };
        const int count = brush.kinds->count();
        files->actions().last()->trigger();
        QVERIFY(studio.window->last_error().startsWith(QStringLiteral("読み込めませんでした")));
        QCOMPARE(brush.kinds->count(), count);
    }

    // One's own brushes outlive the ops: a library brush still draws as itself after an erase (and the book gets its
    // settings, not the G pen's), an edit of a brush the book already has stays after the next change, and a library
    // that cannot be read is never written over.
    void ownBrushesOutliveTheOps() {
        render::brushes::save_to_library(app::config_dir(), "my_lib",
                                         Json{{"label", "かすれ"}, {"base", "gpen"}, {"texture", "dry"}, {"width_mm", 1.2}});
        Studio studio;
        auto& brush = studio.brush();
        studio.draw({QPointF(10, 20), QPointF(40, 20)});
        studio.window->choose_tool(QStringLiteral("eraser"));
        inject::mouse_stroke(studio.window->canvas(), {QPointF(20, 16), QPointF(20, 24)});  // (an op that draws)
        QCOMPARE(render::brushes::brush("my_lib").texture, std::string("dry"));
        studio.choose("my_lib");
        QCOMPARE(brush.kind(), std::string("my_lib"));
        studio.draw({QPointF(10, 35), QPointF(40, 35)});
        const Json& kept = studio.session->document().brush_custom;
        QVERIFY(kept.contains("my_lib"));
        QCOMPARE(kept["my_lib"]["label"], Json("かすれ"));
        QCOMPARE(kept["my_lib"]["texture"], Json("dry"));
        QCOMPARE(studio.ink().strokes->items.back()->kind, std::string("my_lib"));
        // edited while the book has it: the edit stays through the next change (Python's CUSTOM keeps it)
        studio.answers.responder->exec = [](QDialog* dialog) {
            auto* d = qobject_cast<app::BrushDialog*>(dialog);
            if (d == nullptr) return int(QDialog::Rejected);
            d->opacity->setValue(50);
            return int(QDialog::Accepted);
        };
        brush.edit->click();
        QCOMPARE(render::brushes::brush("my_lib").opacity, 0.5);
        studio.draw({QPointF(10, 45), QPointF(40, 45)});
        studio.window->choose_tool(QStringLiteral("eraser"));
        inject::mouse_stroke(studio.window->canvas(), {QPointF(30, 41), QPointF(30, 49)});
        QCOMPARE(render::brushes::brush("my_lib").opacity, 0.5);
        QCOMPARE(render::brushes::load_library(app::config_dir())["my_lib"]["opacity"], Json(0.5));
        // a library that cannot be read: making a brush says so and leaves the file as it is
        const std::string broken = "{\"brushes\": {\"my_lib\": ";
        std::ofstream(render::brushes::library_path(app::config_dir()), std::ios::binary | std::ios::trunc) << broken;
        studio.answers.responder->exec = [](QDialog* dialog) {
            return qobject_cast<app::BrushDialog*>(dialog) != nullptr ? int(QDialog::Accepted) : int(QDialog::Rejected);
        };
        brush.make->click();
        QVERIFY(!studio.window->last_error().isEmpty());
        std::ifstream in(render::brushes::library_path(app::config_dir()), std::ios::binary);
        QCOMPARE(std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>()), broken);
    }

    // The eraser's ways in its op: how it cuts, how soft; the crossing switch.
    void eraserWays() {
        Studio studio;
        studio.draw({QPointF(10, 20), QPointF(40, 20)});
        studio.draw({QPointF(25, 10), QPointF(25, 40)});
        std::vector<Json> ops;
        QObject::connect(studio.session.get(), &app::Session::changed, studio.window.get(), [&](const app::BookChange& c) {
            if (c.why == app::BookChange::Why::Edit) ops.push_back(c.ops);
        });
        auto* mode = studio.window->findChild<QComboBox*>(QStringLiteral("eraser_mode"));
        auto* texture = studio.window->findChild<QComboBox*>(QStringLiteral("eraser_texture"));
        QVERIFY(mode != nullptr && texture != nullptr);
        mode->setCurrentIndex(mode->findData(QStringLiteral("whole")));
        texture->setCurrentIndex(texture->findData(QStringLiteral("soft")));
        studio.window->choose_tool(QStringLiteral("eraser"));
        QCOMPARE(studio.window->tool_settings()->current_page(), studio.window->tool_settings()->page_for(QStringLiteral("eraser")));
        inject::mouse_stroke(studio.window->canvas(), {QPointF(20, 18), QPointF(20, 22)});
        QVERIFY(!ops.empty());
        QCOMPARE(ops.back()[0]["op"], Json("erase"));
        QCOMPARE(ops.back()[0]["mode"], Json("whole"));
        QCOMPARE(ops.back()[0]["texture"], Json("soft"));
        mode->setCurrentIndex(0);
        texture->setCurrentIndex(0);
        studio.brush().crossing->setChecked(true);
        inject::mouse_stroke(studio.window->canvas(), {QPointF(24, 30), QPointF(26, 30)});
        QCOMPARE(ops.back()[0]["mode"], Json("to_crossing"));
        QVERIFY(!ops.back()[0].contains("texture"));
        // its size
        auto* size = studio.window->findChild<QDoubleSpinBox*>(QStringLiteral("eraser_size"));
        size->setValue(6.5);
        QCOMPARE(studio.window->canvas()->eraser_mm, 6.5);
    }

    // brushes@1 (PC4 E1): the 鉛筆ツール — its page and 自動消去, its own hard dab brush the book keeps (not in the brush
    // list); 表示 › ブラシプレビュー (off: the cross alone) and 環境設定's pointers.
    void pencilAndBrushPreview() {
        Studio studio;
        std::vector<Json> ops;
        QObject::connect(studio.session.get(), &app::Session::changed, studio.window.get(), [&](const app::BookChange& c) {
            if (c.why == app::BookChange::Why::Edit) ops.push_back(c.ops);
        });
        auto* ts = studio.window->tool_settings();
        QVERIFY(studio.window->action(QStringLiteral("act_pencil")) != nullptr);
        studio.window->choose_tool(QStringLiteral("pencil"));
        QCOMPARE(ts->title()->text(), QStringLiteral("鉛筆ツール"));
        QVERIFY(studio.brush().auto_erase->isVisibleTo(studio.window.get()));
        studio.brush().size->setValue(3.0);
        inject::mouse_stroke(studio.window->canvas(), {QPointF(10, 20), QPointF(40, 20)});
        QVERIFY(!ops.empty());
        QCOMPARE(ops.back()[0]["op"], Json("define_brush"));
        const std::string pencil(render::dabs::kPencilKey);
        QCOMPARE(ops.back()[0]["key"], Json(pencil));
        QCOMPARE(ops.back().back()["kind"], Json(pencil));
        QCOMPARE(studio.session->document().brush_custom[pencil]["engine"], Json("dabs"));
        QCOMPARE(studio.ink().strokes->items.back()->kind, pencil);
        for (int i = 0; i < studio.brush().kinds->count(); ++i) {
            QVERIFY(studio.brush().kinds->item(i)->data(Qt::UserRole).toString().toStdString() != pencil);
        }
        // 自動消去: begun on the 描画色, the line is in the 背景色; begun elsewhere, in the 描画色
        studio.brush().auto_erase->setChecked(true);
        const auto back = studio.window->colours()->background();
        const Json back8 = Json::array({std::lround(std::clamp(back[0], 0.0, 1.0) * 255), std::lround(std::clamp(back[1], 0.0, 1.0) * 255),
                                        std::lround(std::clamp(back[2], 0.0, 1.0) * 255)});
        inject::mouse_stroke(studio.window->canvas(), {QPointF(25, 20), QPointF(25, 40)});
        QCOMPARE(ops.back().back()["rgb"], back8);
        inject::mouse_stroke(studio.window->canvas(), {QPointF(45, 45), QPointF(50, 50)});
        QVERIFY(ops.back().back()["rgb"] != back8);
        // a book with a brush of one's own under the pencil's key (review P3-2): kept as it is, the pencil the next key
        Studio other;
        QVERIFY(other.window->apply_ops(Json::array({Json{{"op", "define_brush"}, {"key", pencil}, {"label", "わたしの"}, {"base", "gpen"}}})));
        const Json mine = other.session->document().brush_custom[pencil];
        other.window->choose_tool(QStringLiteral("pencil"));
        inject::mouse_stroke(other.window->canvas(), {QPointF(10, 20), QPointF(40, 20)});
        QCOMPARE(other.session->document().brush_custom[pencil], mine);
        QCOMPARE(other.ink().strokes->items.back()->kind, pencil + "_2");
        QCOMPARE(other.session->document().brush_custom[pencil + "_2"]["engine"], Json("dabs"));
        // the brush tool: no 自動消去, its own brush
        studio.window->choose_tool(QStringLiteral("pen"));
        QVERIFY(!studio.brush().auto_erase->isVisibleTo(studio.window.get()));
        studio.draw({QPointF(10, 50), QPointF(30, 50)});
        QCOMPARE(studio.ink().strokes->items.back()->kind, std::string(core::kDefaultBrush));
        // ブラシプレビュー: on at first; off, the pointer is the cross alone
        auto* preview = studio.window->action(QStringLiteral("act_brush_preview"));
        QVERIFY(preview != nullptr && preview->isCheckable() && preview->isChecked());
        QVERIFY(studio.window->canvas()->brush_preview);
        preview->setChecked(false);
        QVERIFY(!studio.window->canvas()->brush_preview);
        QCOMPARE(studio.window->canvas()->cursor().shape(), Qt::CrossCursor);
        preview->setChecked(true);
        QVERIFY(studio.window->canvas()->brush_preview);
    }

    // brushes@1 (PC4 E1): the 消しゴムツール's モード — ブラシ (the brush panel's when it is a dab brush, else the eraser's
    // own 質 as before), 鉛筆, ブロック — with its 不透明度 and 流量 in the op; the book given the brush first.
    void eraserModes() {
        render::brushes::save_to_library(app::config_dir(), "my_dab", Json{{"label", "ダブ"}, {"engine", "dabs"}, {"dabs", Json{{"hardness", 0.5}}}});
        Studio studio;
        studio.draw({QPointF(10, 20), QPointF(40, 20)});
        // a paint layer of pixels (opaque grey)
        const render::Image grey = render::Image::create("RGBA", render::Size{170, 170}, render::Ink{200, 200, 200, 255});
        QVERIFY(studio.window->apply_ops(Json::array({Json{{"op", "add_layer"}, {"page", 1}, {"kind", "paint"}, {"id", "paint"}},
                                                      Json{{"op", "set_layer"}, {"page", 1}, {"id", "paint"}, {"panel_each", false}, {"panel_clip", false}},
                                                      Json{{"op", "put_raster"}, {"page", 1}, {"id", "paint"}, {"png_base64", core::b64encode(render::write_png(grey))}}})));
        std::vector<Json> ops;
        QObject::connect(studio.session.get(), &app::Session::changed, studio.window.get(), [&](const app::BookChange& c) {
            if (c.why == app::BookChange::Why::Edit) ops.push_back(c.ops);
        });
        auto* kind = studio.window->findChild<QComboBox*>(QStringLiteral("eraser_kind"));
        auto* opacity = studio.window->findChild<QSlider*>(QStringLiteral("eraser_opacity"));
        auto* flow = studio.window->findChild<QSlider*>(QStringLiteral("eraser_flow"));
        QVERIFY(kind != nullptr && opacity != nullptr && flow != nullptr);
        QCOMPARE(kind->currentData().toString(), QStringLiteral("brush"));
        studio.window->choose_tool(QStringLiteral("eraser"));
        studio.window->set_target_layer("paint");
        inject::mouse_stroke(studio.window->canvas(), {QPointF(14, 16), QPointF(14, 24)});
        QCOMPARE(ops.back().back()["op"], Json("erase"));
        QCOMPARE(ops.back().back()["layer_id"], Json("paint"));
        QVERIFY(!ops.back().back().contains("brush") && !ops.back().back().contains("block"));
        // ブロック, half its 不透明度, its 流量 0.8
        kind->setCurrentIndex(kind->findData(QStringLiteral("block")));
        opacity->setValue(50);
        flow->setValue(80);
        inject::mouse_stroke(studio.window->canvas(), {QPointF(18, 16), QPointF(18, 24)});
        QCOMPARE(ops.back().back()["block"], Json(true));
        QCOMPARE(ops.back().back()["opacity"], Json(0.5));
        QCOMPARE(ops.back().back()["flow"], Json(0.8));
        // 鉛筆: the pencil's brush, the book given it first
        kind->setCurrentIndex(kind->findData(QStringLiteral("pencil")));
        inject::mouse_stroke(studio.window->canvas(), {QPointF(22, 16), QPointF(22, 24)});
        QCOMPARE(ops.back()[0]["op"], Json("define_brush"));
        QCOMPARE(ops.back().back()["brush"], Json(std::string(render::dabs::kPencilKey)));
        // ブラシ with a dab brush chosen in the brush panel
        kind->setCurrentIndex(kind->findData(QStringLiteral("brush")));
        studio.choose("my_dab");
        QCOMPARE(studio.brush().kind(), std::string("my_dab"));
        studio.window->choose_tool(QStringLiteral("eraser"));
        studio.window->set_target_layer("paint");
        inject::mouse_stroke(studio.window->canvas(), {QPointF(26, 16), QPointF(26, 24)});
        QCOMPARE(ops.back()[0]["op"], Json("define_brush"));
        QCOMPARE(ops.back().back()["brush"], Json("my_dab"));
        QVERIFY(!ops.back().back().contains("texture"));
        QCOMPARE(app::settings()->value(QStringLiteral("eraser/kind")).toString(), QStringLiteral("brush"));
        QCOMPARE(app::settings()->value(QStringLiteral("eraser/opacity")).toDouble(), 0.5);
        // a pen layer: its lines cut as before, in every モード — the book given no brush (review P2-4)
        Studio pen;
        pen.choose("gpen");  // (the brush panel remembers the last brush chosen: the G pen's line)
        pen.draw({QPointF(10, 20), QPointF(40, 20)});
        std::vector<Json> pen_ops;
        QObject::connect(pen.session.get(), &app::Session::changed, pen.window.get(), [&](const app::BookChange& c) {
            if (c.why == app::BookChange::Why::Edit) pen_ops.push_back(c.ops);
        });
        auto* pen_kind = pen.window->findChild<QComboBox*>(QStringLiteral("eraser_kind"));
        pen.window->choose_tool(QStringLiteral("eraser"));
        for (const char* way : {"pencil", "block", "brush"}) {
            pen_kind->setCurrentIndex(pen_kind->findData(QString::fromLatin1(way)));
            if (std::string(way) == "brush") pen.choose("my_dab");
            pen.window->choose_tool(QStringLiteral("eraser"));
            inject::mouse_stroke(pen.window->canvas(), {QPointF(20, 16), QPointF(20, 24)});
            QCOMPARE(pen_ops.back().size(), std::size_t(1));
            QCOMPARE(pen_ops.back()[0]["op"], Json("erase"));
            QVERIFY2(!pen_ops.back()[0].contains("brush") && !pen_ops.back()[0].contains("block") && !pen_ops.back()[0].contains("opacity"), way);
        }
        QVERIFY(!core::uses_brushes(pen.session->document()));
    }

    // brushes@1 (PC4 E1, review P2-6): a line with a blend on a paint layer that has lines of its own — they become its
    // pixels — is asked first (やめる: nothing changes; 描く: drawn, not asked again for the layer); a blend other than
    // 通常 shows beside the tool's name; on a pen layer the モード is grey and lines are drawn normal (review P3-6).
    void aBlendAsksBeforeTheLinesBecomePixels() {
        Studio studio;
        auto* window = studio.window.get();
        QVERIFY(window->apply_ops(Json::array({Json{{"op", "add_layer"}, {"page", 1}, {"kind", "paint"}, {"id", "paint"}},
                                               Json{{"op", "set_layer"}, {"page", 1}, {"id", "paint"}, {"panel_each", false}, {"panel_clip", false}}})));
        window->set_target_layer("paint");
        studio.draw({QPointF(10, 20), QPointF(40, 20)});  // (a line of the paint layer's own)
        const auto paint = [&]() -> const core::Layer& {
            for (const core::Layer& l : studio.session->document().page(0).layers)
                if (l.id == "paint") return l;
            throw std::runtime_error("no paint layer");
        };
        QCOMPARE(paint().stroke_count(), std::size_t(1));
        auto& brush = studio.brush();
        brush.blend->setCurrentIndex(brush.blend->findData(QStringLiteral("multiply")));
        QVERIFY(brush.blend->isEnabled());
        QVERIFY(window->tool_settings()->badge()->isVisibleTo(window));
        QVERIFY(window->tool_settings()->badge()->text().contains(QStringLiteral("乗算")));
        int asked = 0;
        int answer = QDialog::Rejected;
        studio.answers.responder->exec = [&](QDialog* dialog) {
            if (dialog->objectName() != QStringLiteral("blend_bakes")) return int(QDialog::Rejected);
            ++asked;
            return answer;
        };
        const std::int64_t revision = studio.session->document().revision;
        inject::mouse_stroke(window->canvas(), {QPointF(10, 30), QPointF(40, 30)});
        QCOMPARE(asked, 1);
        QCOMPARE(studio.session->document().revision, revision);  // (やめる: nothing)
        QCOMPARE(paint().stroke_count(), std::size_t(1));
        answer = QDialog::Accepted;
        inject::mouse_stroke(window->canvas(), {QPointF(10, 35), QPointF(40, 35)});
        QCOMPARE(asked, 2);
        QCOMPARE(paint().stroke_count(), std::size_t(0));  // (its line became pixels, the blend's line painted in)
        QVERIFY(paint().raster_png && !paint().raster_png->empty());
        studio.draw({QPointF(10, 40), QPointF(40, 40)});
        studio.draw({QPointF(10, 45), QPointF(40, 45)});
        QCOMPARE(asked, 2);  // (not asked again for the layer)
        // a pen layer: the モード grey, the line normal, nothing asked
        window->set_target_layer(studio.ink().id);
        QVERIFY(!brush.blend->isEnabled());
        QVERIFY(!window->tool_settings()->badge()->isVisibleTo(window));
        std::vector<Json> ops;
        QObject::connect(studio.session.get(), &app::Session::changed, window, [&](const app::BookChange& c) {
            if (c.why == app::BookChange::Why::Edit) ops.push_back(c.ops);
        });
        studio.draw({QPointF(10, 50), QPointF(40, 50)});
        QVERIFY(!ops.back().back().contains("blend"));
        // back to the paint layer: its モード as it shows (review P3-6)
        window->set_target_layer("paint");
        studio.draw({QPointF(10, 55), QPointF(40, 55)});
        QCOMPARE(ops.back().back()["blend"], Json("multiply"));
        brush.blend->setCurrentIndex(0);
        QVERIFY(!window->tool_settings()->badge()->isVisibleTo(window));
    }

    // brushes@1 (PC4 E1): the brush tool's 流量 (a dab brush's), 滑らかさ (紐で引く: the window smooths, no stabilize sent)
    // and the cursor's 描いている間は十字だけにする (review P2-8: the cross while drawing, the circle's blank pointer after).
    void theBrushToolsSettingsAndPointer() {
        render::brushes::save_to_library(app::config_dir(), "my_dab", Json{{"label", "ダブ"}, {"engine", "dabs"}, {"dabs", Json{{"hardness", 0.5}}}});
        Studio studio;
        auto& brush = studio.brush();
        std::vector<Json> ops;
        QObject::connect(studio.session.get(), &app::Session::changed, studio.window.get(), [&](const app::BookChange& c) {
            if (c.why == app::BookChange::Why::Edit) ops.push_back(c.ops);
        });
        studio.choose("my_dab");
        brush.flow->setValue(30);
        brush.smoothing->setCurrentIndex(brush.smoothing->findData(QStringLiteral("string")));
        QVERIFY(brush.smoothing_amount->isEnabled());
        studio.draw({QPointF(10, 20), QPointF(20, 22), QPointF(30, 20), QPointF(40, 24)});
        const Json& op = ops.back().back();
        QCOMPARE(op["kind"], Json("my_dab"));
        QCOMPARE(op["flow"], Json(0.3));
        QCOMPARE(op["stabilize"], Json(0));
        QCOMPARE(studio.window->canvas()->smoothing, QStringLiteral("string"));
        // the G pen: no flow sent (its line as before)
        studio.choose("gpen");
        studio.draw({QPointF(10, 30), QPointF(40, 30)});
        QVERIFY(!ops.back().back().contains("flow"));
        // the pointer: the circle (blank system pointer), the cross while drawing
        auto* canvas = studio.window->canvas();
        canvas->cursor_kind = QStringLiteral("circle");
        canvas->cross_while_drawing = true;
        studio.window->choose_tool(QStringLiteral("pen"));
        canvas->refresh_cursor();
        QCOMPARE(canvas->cursor().shape(), Qt::BlankCursor);
        inject::mouse(canvas, inject::Phase::Press, QPointF(10, 40));
        QCOMPARE(canvas->cursor().shape(), Qt::CrossCursor);
        inject::mouse(canvas, inject::Phase::Move, QPointF(30, 40));
        inject::mouse(canvas, inject::Phase::Release, QPointF(30, 40));
        QCOMPARE(canvas->cursor().shape(), Qt::BlankCursor);
        canvas->cross_while_drawing = false;
        inject::mouse(canvas, inject::Phase::Press, QPointF(10, 45));
        QCOMPARE(canvas->cursor().shape(), Qt::BlankCursor);
        inject::mouse(canvas, inject::Phase::Release, QPointF(30, 45));
        // ブラシの先端の形: a dab brush's tip drawn as the pointer (the system's blank)
        canvas->cursor_kind = QStringLiteral("tip");
        canvas->refresh_cursor();
        QCOMPARE(canvas->cursor().shape(), Qt::BlankCursor);
    }

    // ツールの設定: each tool's name, hint and page; the panel tool's ways and the selection's settings go to the canvas.
    void eachToolsPage() {
        Studio studio;
        auto* ts = studio.window->tool_settings();
        for (const auto& [tool, title] : {std::pair{"pen", "ブラシツール（B）"}, {"eraser", "消しゴムツール（E）"}, {"frame", "コマ割り（F）"}, {"select", "選択（V）"},
                                          {"move", "移動ツール（Q）"}, {"zoom", "虫めがね（Z）"}, {"lasso", "選択範囲ツール（M・L・W）"}}) {
            studio.window->choose_tool(QString::fromLatin1(tool));
            QCOMPARE(ts->title()->text(), QString::fromUtf8(title));
        }
        QCOMPARE(ts->current_page(), ts->page_for(QStringLiteral("marquee")));
        auto* way = studio.window->findChild<QComboBox*>(QStringLiteral("marquee_mode"));
        QCOMPARE(way->currentData().toString(), QStringLiteral("lasso"));
        way->setCurrentIndex(way->findData(QStringLiteral("selpen")));
        emit way->activated(way->currentIndex());
        QCOMPARE(studio.window->canvas()->marquee, QStringLiteral("pen"));
        studio.window->findChild<QDoubleSpinBox*>(QStringLiteral("selection_pen"))->setValue(9);
        QCOMPARE(studio.window->canvas()->selection_pen_mm, 9.0);
        studio.window->findChild<QSpinBox*>(QStringLiteral("colour_tolerance"))->setValue(80);
        QCOMPARE(studio.window->colour_tolerance, 80.0);
        studio.window->findChild<QCheckBox*>(QStringLiteral("colour_contiguous"))->setChecked(true);
        QVERIFY(studio.window->colour_contiguous);
        auto* frame = studio.window->findChild<QComboBox*>(QStringLiteral("frame_mode"));
        frame->setCurrentIndex(frame->findData(QStringLiteral("poly")));
        emit frame->activated(frame->currentIndex());
        QCOMPARE(studio.window->canvas()->frame_mode, QStringLiteral("poly"));
        studio.window->choose_tool(QStringLiteral("zoom"));
        QVERIFY(ts->page_for(QStringLiteral("zoom")) == nullptr);
        // a page's command rows trigger their commands: プリントサイズ (原寸 before PC5 H-0) on the selection tool's page
        studio.window->choose_tool(QStringLiteral("select"));
        QPushButton* actual = nullptr;
        for (QPushButton* row : ts->current_page()->findChildren<QPushButton*>()) {
            if (row->toolTip().contains(QStringLiteral("プリントサイズ")) || row->text().contains(QStringLiteral("プリントサイズ"))) actual = row;
        }
        QVERIFY(actual != nullptr);
        studio.window->canvas()->fit_page();
        const double fitted = studio.window->canvas()->scale();
        actual->click();
        QTRY_VERIFY(studio.window->canvas()->scale() != fitted);
        // folded: only the name (neither the page nor the hint)
        ts->set_folded(true);
        QVERIFY(!ts->current_page()->isVisibleTo(ts));
        QVERIFY(!ts->hint()->isVisibleTo(ts));
        ts->set_folded(false);
        QVERIFY(ts->current_page()->isVisibleTo(ts));
    }
};

QTEST_MAIN(TestGuiBrushes)
#include "test_gui_brushes.moc"
