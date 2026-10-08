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
#include "gui_support.hpp"
#include "app/brush_panel.hpp"
#include "app/canvas.hpp"
#include "app/config.hpp"
#include "app/fields.hpp"
#include "app/inject.hpp"
#include "app/main_window.hpp"
#include "app/tool_settings.hpp"
#include "core/brushes.hpp"
#include "render/brushes.hpp"
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

    // ツールの設定: each tool's name, hint and page; the panel tool's ways and the selection's settings go to the canvas.
    void eachToolsPage() {
        Studio studio;
        auto* ts = studio.window->tool_settings();
        for (const auto& [tool, title] : {std::pair{"pen", "ペン（B）"}, {"eraser", "消しゴム（E）"}, {"frame", "コマ割り（F）"}, {"select", "選択（V）"},
                                          {"move", "レイヤー移動（Q）"}, {"zoom", "虫めがね（Z）"}, {"lasso", "範囲選択（M・L・W）"}}) {
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
        // a page's command rows trigger their commands
        studio.window->choose_tool(QStringLiteral("select"));
        QList<QPushButton*> rows = ts->current_page()->findChildren<QPushButton*>();
        QVERIFY(!rows.isEmpty());
        // folded: only the name
        ts->set_folded(true);
        QVERIFY(!ts->current_page()->isVisibleTo(ts) || !ts->hint()->isVisibleTo(ts));
        ts->set_folded(false);
    }
};

QTEST_MAIN(TestGuiBrushes)
#include "test_gui_brushes.moc"
