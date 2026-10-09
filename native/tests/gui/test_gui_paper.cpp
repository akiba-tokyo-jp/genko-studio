// 紙質 (BRUSH-01, this build only) in the app on the offscreen platform: the brush dialog's 紙質 tab (a picture file
// taken in, refused in Japanese when it cannot be, its settings, the patch of paper and the sample line drawn with it);
// one's own brush with a paper kept in the person's library without touching what Python's app reads (brushes.json:
// read by Python's own reader, which still writes beside it); a line drawn with it brings the brush, its paper's picture
// and the feature into the book; the source file deleted, the book saved and opened on "another computer" (nothing
// known to the process, no library) draws the same pixels; the zoom never changes them; Undo and Redo; a .genkobrush
// with its paper's picture in it.

#include <QtTest>

#include <QAction>
#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QSpinBox>
#include <QTabWidget>

#include <filesystem>
#include <fstream>

#include "gui_support.hpp"
#include "app/brush_panel.hpp"
#include "app/canvas.hpp"
#include "app/config.hpp"
#include "app/inject.hpp"
#include "app/main_window.hpp"
#include "app/wording.hpp"
#include "core/base64.hpp"
#include "core/brushes.hpp"
#include "core/strokes.hpp"
#include "render/brushes.hpp"
#include "render/page.hpp"
#include "render/paper.hpp"
#include "render/png.hpp"
#include "storage/asset_store.hpp"
#include "testsupport.hpp"

using namespace genko;
using core::Json;
namespace inject = genko::app::inject;
namespace fs = std::filesystem;

namespace {

constexpr int kDpi = 150;

core::Document book_doc() {
    core::Document doc = core::new_episode("紙質", core::Num(1), 1, core::PageSpec::custom(60, 60, 50, 50, 1, 2, 2, 2, 2, kDpi, "mono"));
    return doc;
}

std::string grain_png() { return genko::test::read_bytes(genko::test::test_data(QStringLiteral("paper/grain.png"))); }

struct Studio {
    QTemporaryDir tmp;
    fs::path book;
    std::shared_ptr<app::Session> session;
    std::unique_ptr<app::MainWindow> window;
    gui_test::Answers answers;
    explicit Studio(std::optional<fs::path> existing = std::nullopt) {
        (void)gui_test::config_folder();
        book = existing ? *existing : gui_test::path_of(tmp.path() + "/book.genko");
        if (!existing) gui_test::write_book(book, book_doc());
        session = app::Session::open(book, gui_test::quick(gui_test::path_of(tmp.path() + "/recovery")));
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
    render::Image page(const core::Document& doc) const { return render::render_page(doc.page(0), kDpi, {}, &doc).image; }
};

// A paper file of its own in `dir` (deleted later to show the book keeps its own copy).
QString paper_file(const QTemporaryDir& dir, const QString& name = QStringLiteral("紙の目.png")) {
    const QString path = dir.path() + QLatin1Char('/') + name;
    genko::test::write_bytes(path, grain_png());
    return path;
}

void forget_library() {
    std::error_code ec;
    fs::remove(render::brushes::library_path(app::config_dir()), ec);
    fs::remove(render::brushes::library_papers_path(app::config_dir()), ec);
    fs::remove_all(app::config_dir() / "brush_papers", ec);
}

Json read_json_file(const fs::path& path) { return core::parse_python_json(genko::test::read_bytes(gui_test::qpath(path))); }

}  // namespace

class TestGuiPaper : public QObject {
    Q_OBJECT
private slots:
    void init() {
        (void)gui_test::config_folder();
        app::settings()->clear();
        forget_library();
        render::brushes::clear_custom();
        render::paper::clear();
    }

    // The dialog's 紙質: a picture taken in (or refused in Japanese), every setting in its data, the patch of paper and
    // the sample line drawn with it.
    void dialog() {
        Studio studio;
        app::BrushDialog d(studio.window.get(), "fill_pen");
        QVERIFY(!d.paper_on->isChecked());
        QVERIFY(d.data()["paper"].is_null());
        d.draw_sample();
        const QImage without = d.sample->pixmap().toImage();
        // refused: not a picture, a format this build does not read
        const QString broken = studio.tmp.path() + "/broken.png";
        genko::test::write_bytes(broken, "not a picture");
        QVERIFY(!d.take_paper(broken));
        QVERIFY2(d.paper_error.contains(QStringLiteral("紙質の画像を読めません")), qPrintable(d.paper_error));
        const QString tiff = studio.tmp.path() + "/paper.tif";
        genko::test::write_bytes(tiff, std::string("II*\0", 4) + std::string(64, '\0'));
        QVERIFY(!d.take_paper(tiff));
        QVERIFY2(d.paper_error.contains(QStringLiteral("PNG・JPEG・BMP・GIF")), qPrintable(d.paper_error));
        QFile huge(studio.tmp.path() + "/huge.png");  // (more than 64 MiB: refused by its size, never read)
        QVERIFY(huge.open(QIODevice::WriteOnly) && huge.resize(qint64(render::paper::kMaxFileBytes) + 1));
        huge.close();
        QVERIFY(!d.take_paper(huge.fileName()));
        QCOMPARE(d.paper_error, QStringLiteral("紙質の画像のファイルが大きすぎます（64 MB まで）"));
        QVERIFY(!d.take_paper(studio.tmp.path() + "/no such picture.png"));
        QVERIFY(d.paper_error.startsWith(QStringLiteral("紙質の画像のファイルを開けません")));
        QVERIFY(!d.paper_on->isChecked());
        // taken in
        QVERIFY(d.take_paper(paper_file(studio.tmp)));
        QVERIFY(d.paper_on->isChecked());
        QVERIFY(storage::AssetStore::is_ref(d.paper_asset));
        QVERIFY(render::paper::bytes(d.paper_asset) != nullptr);
        QVERIFY(!d.paper_preview->pixmap().isNull());
        QVERIFY(d.paper_about->text().contains(QStringLiteral("96 × 96")));
        d.paper_density->setValue(100);
        d.draw_sample();
        QVERIFY(d.sample->pixmap().toImage() != without);
        // every setting in its data
        d.paper_scale->setValue(250);
        d.paper_rotation->setValue(-30);
        d.paper_flip_x->setChecked(true);
        d.paper_flip_y->setChecked(true);
        d.paper_invert->setChecked(true);
        d.paper_blend->setCurrentIndex(d.paper_blend->findData(QStringLiteral("subtract")));
        d.paper_coords->setCurrentIndex(d.paper_coords->findData(QStringLiteral("stroke")));
        d.paper_seam->setCurrentIndex(d.paper_seam->findData(QStringLiteral("mirror")));
        d.paper_seed->setValue(4321);
        const Json paper = d.data()["paper"];
        QCOMPARE(paper, (Json{{"asset", d.paper_asset}, {"density", 1.0}, {"scale", 2.5}, {"rotation", -30.0}, {"flip_x", true},
                              {"flip_y", true}, {"invert", true}, {"blend", "subtract"}, {"coords", "stroke"}, {"seam", "mirror"},
                              {"seed", 4321}}));
        QVERIFY(core::paper_from_json(paper) == core::paper_from_json(paper));  // (settings a brush takes)
        if (qEnvironmentVariableIsSet("GENKO_TEST_ARTIFACTS")) {  // (a picture of the tab, for a look)
            d.resize(980, 720);
            d.show();
            if (auto* tabs = d.findChild<QTabWidget*>()) tabs->setCurrentIndex(2);
            QTest::qWait(50);
            d.grab().save(qEnvironmentVariable("GENKO_TEST_ARTIFACTS") + QStringLiteral("/paper-dialog.png"));
            d.hide();
        }
        d.paper_on->setChecked(false);
        QVERIFY(d.data()["paper"].is_null());
        // a brush with a paper opened again: its paper with it
        render::brushes::define_brush("my_open", Json{{"label", "開く"}, {"base", "fill_pen"}, {"paper", paper}});
        app::BrushDialog again(studio.window.get(), "my_open", true);
        QVERIFY(again.paper_on->isChecked());
        QCOMPARE(again.data()["paper"], paper);
    }

    // The refusals of define_brush's paper (this build's own: SPEC.md COMP-01a) in Japanese, as the window shows them.
    void words() {
        const std::vector<std::pair<std::string, QString>> cases{
            {"ops[0] define_brush: the paper's picture cannot be read (it is not a PNG, JPEG, BMP or GIF picture, or it is broken)",
             QStringLiteral("紙質の画像を読めません（PNG・JPEG・BMP・GIF の画像を選んでください。壊れた画像は読めません）")},
            {"this build does not read that kind of picture (TIFF, WebP, PSD): give the paper as PNG, JPEG, BMP or GIF",
             QStringLiteral("この版の Genko では読めない形式の画像です（紙質は PNG・JPEG・BMP・GIF で読み込みます）")},
            {"the paper's picture is too large (at most 4096 pixels a side)", QStringLiteral("紙質の画像が大きすぎます（縦横 4096 画素まで）")},
            {"the paper's picture file is too large (at most 64 MiB)", QStringLiteral("紙質の画像のファイルが大きすぎます（64 MB まで）")},
            {"the paper's picture has no pixels", QStringLiteral("紙質の画像に画素がありません")},
            {"the book has no paper picture sha256:" + std::string(64, 'b') + " (give the picture as png)",
             QStringLiteral("この原稿には、その紙質の画像がありません（画像を読み込み直してください）")},
            {"a paper needs its picture: png (base64) or asset (sha256:…)", QStringLiteral("紙質には画像が要ります（png か asset で指定します）")},
            {"a paper takes its picture once: png or asset, not both", QStringLiteral("紙質の画像は png か asset のどちらか一つで指定します")},
            {"paper density must be between 0 and 1", QStringLiteral("紙質の濃さは 0〜1 の間で決めます")},
            {"paper scale must be between 0.1 and 10", QStringLiteral("紙質の倍率は 0.1〜10 の間で決めます")},
            {"paper rotation must be between -360 and 360", QStringLiteral("紙質の回転は -360〜360 の間で決めます")},
            {"paper blend must be multiply or subtract", QStringLiteral("紙質の合成は乗算（multiply）か減算（subtract）です")},
            {"paper coords must be paper or stroke", QStringLiteral("紙目の位置は紙面に固定（paper）か線ごと（stroke）です")},
            {"paper seam must be repeat or mirror", QStringLiteral("紙質の継ぎ目は繰り返し（repeat）か折り返し（mirror）です")},
            {"paper flip_y must be true or false", QStringLiteral("紙質の反転・濃淡の反転は true か false で決めます")},
            {"paper seed must be a whole number between 0 and 2147483647", QStringLiteral("紙質の乱数の種は 0〜2147483647 の整数で決めます")},
            {"paper has an unknown setting: colour", QStringLiteral("紙質に知らない設定があります（colour）")},
            {"paper asset must be an asset ref (sha256:<64 hex>)", QStringLiteral("紙質の画像（asset）は sha256: と 64 桁の 16 進数で指定します")},
            {"paper must be an object or null", QStringLiteral("紙質は設定のまとまり（オブジェクト）か null で指定します")},
        };
        for (const auto& [english, japanese] : cases) QCOMPARE(app::wording::error(english), japanese);
    }

    // One's own brush with a paper: the library keeps it beside Python's file (which Python's reader reads as before and
    // its app still writes); a line drawn with it brings the brush, the picture and the feature into the book, which is
    // saved with them; Undo and Redo; the zoom changes nothing; a .genkobrush carries the picture.
    void ownBrushWithPaper() {
        Studio studio;
        auto& brush = studio.brush();
        studio.choose("fill_pen");
        const QString source = paper_file(studio.tmp);
        studio.answers.responder->exec = [source](QDialog* dialog) {
            auto* d = qobject_cast<app::BrushDialog*>(dialog);
            if (d == nullptr) return int(QDialog::Rejected);
            d->name->setText(QStringLiteral("ざらざら紙"));
            d->width->setValue(4);
            if (!d->take_paper(source)) return int(QDialog::Rejected);
            d->paper_density->setValue(90);
            d->paper_scale->setValue(150);
            return int(QDialog::Accepted);
        };
        brush.make->click();
        const std::string key = brush.kind();
        QVERIFY(key.starts_with("my_"));
        const core::Brush made = render::brushes::brush(key);
        QVERIFY(made.paper.has_value());
        QCOMPARE(made.paper->density, 0.9);
        // the library: brushes.json as Python's app writes it (no paper), the paper beside it with its picture
        const Json raw = read_json_file(render::brushes::library_path(app::config_dir()));
        QVERIFY(raw["brushes"].contains(key));
        QVERIFY(!raw["brushes"][key].contains("paper"));
        const Json papers = read_json_file(render::brushes::library_papers_path(app::config_dir()));
        QCOMPARE(papers["genko_brush_papers"], Json(1));
        QCOMPARE(papers["papers"][key], core::paper_to_json(*made.paper));
        const std::string hex = made.paper->asset.substr(7);
        QVERIFY(fs::exists(app::config_dir() / "brush_papers" / (hex + ".png")));
        QVERIFY(render::brushes::load_own_brushes(app::config_dir())[key].contains("paper"));
        // Python's own reader: the library reads as before (the brush without its paper), and Python's app still saves
        // its brushes into it without losing ours
        if (!genko::test::python_ref().isEmpty()) {
            QProcessEnvironment env = genko::test::python_env(studio.tmp.path() + "/py");
            env.insert(QStringLiteral("GENKO_CONFIG_DIR"), gui_test::qpath(app::config_dir()));
            const QString script = QStringLiteral(
                "import json\n"
                "from genko import brushes\n"
                "lib = brushes.load_library()\n"
                "brushes.register(lib)\n"
                "brushes.save_to_library('my_python', {'label': 'パイソン', 'width_mm': 1.5})\n"
                "print(json.dumps({'keys': sorted(lib), 'custom': {k: brushes.to_dict(brushes.CUSTOM[k]) for k in lib}}, ensure_ascii=False))\n");
            const auto run = genko::test::run(genko::test::python_ref(), {QStringLiteral("-c"), script}, env);
            QVERIFY2(run.finished && run.exit_code == 0, run.err.constData());
            qInfo("Python's reader of the library: %s", run.out.trimmed().constData());
            const Json seen = genko::test::one_line(run.out);
            QVERIFY2(seen["custom"].contains(key), run.out.constData());
            QCOMPARE(seen["custom"][key]["label"], Json("ざらざら紙"));
            QVERIFY(!seen["custom"][key].contains("paper"));
            const Json own = render::brushes::load_own_brushes(app::config_dir());
            QVERIFY(own.contains("my_python"));
            QVERIFY(own[key].contains("paper"));  // (beside Python's file: untouched by its save)
        }
        // drawn with: the book gets the brush with its paper and the picture; the source file is not needed
        QVERIFY(QFile::remove(source));
        studio.draw({QPointF(10, 15), QPointF(30, 25), QPointF(50, 15)});
        const core::Document& doc = studio.session->document();
        QVERIFY(doc.brush_custom.contains(key));
        const std::string ref = doc.brush_custom[key]["paper"]["asset"].get<std::string>();
        QVERIFY(doc.papers.contains(ref));
        QCOMPARE(doc.features, std::vector<std::string>{std::string(core::kPaperFeature)});
        QCOMPARE(studio.ink().strokes->items.back()->kind, key);
        const render::Image drawn = studio.page(doc);
        // the zoom changes nothing: the same line drawn zoomed in is the same line, drawn the same
        studio.window->canvas()->zoom_by(3.0);
        QVERIFY(studio.page(studio.session->document()).tobytes() == drawn.tobytes());
        studio.draw({QPointF(10, 40), QPointF(30, 50), QPointF(50, 40)});
        studio.window->canvas()->fit_page();
        studio.draw({QPointF(10, 40), QPointF(30, 50), QPointF(50, 40)});
        const auto& items = studio.ink().strokes->items;
        QCOMPARE(items.size(), std::size_t(3));
        QCOMPARE(items[1]->points, items[2]->points);
        const render::Size size{render::mm_to_px(60, kDpi), render::mm_to_px(60, kDpi)};
        const auto zoomed = render::brushes::draw(size, core::stroke_points(*items[1]), kDpi, items[1]->width_mm, key, items[1]->id);
        const auto fitted = render::brushes::draw(size, core::stroke_points(*items[2]), kDpi, items[2]->width_mm, key, items[2]->id);
        QVERIFY(zoomed && fitted);
        QVERIFY(zoomed->mask.tobytes() == fitted->mask.tobytes());
        // saved with them
        QVERIFY(studio.session->wait_saved(std::chrono::milliseconds(10000)));
        const core::Document disk = gui_test::read_book(studio.book);
        QCOMPARE(disk.features, std::vector<std::string>{std::string(core::kPaperFeature)});
        QVERIFY(disk.papers.contains(ref));
        QVERIFY(studio.page(disk).tobytes() == studio.page(studio.session->document()).tobytes());
        // Undo, Redo
        for (std::size_t left : {2, 1, 0}) {
            studio.window->action("act_undo")->trigger();
            QVERIFY(gui_test::wait_for([&] { return gui_test::ink_strokes(studio.session->document()) == left; }));
            QVERIFY(studio.session->wait_idle(std::chrono::milliseconds(10000)));
        }
        QVERIFY(!studio.session->document().brush_custom.contains(key));
        QVERIFY(studio.session->document().features.empty());
        studio.window->action("act_redo")->trigger();
        QVERIFY(gui_test::wait_for([&] { return gui_test::ink_strokes(studio.session->document()) == 1; }));
        QCOMPARE(studio.session->document().features, std::vector<std::string>{std::string(core::kPaperFeature)});
        QVERIFY(studio.page(studio.session->document()).tobytes() == drawn.tobytes());
        // a .genkobrush carries the picture; read back elsewhere (nothing known), the same brush with the same paper
        const QString file = studio.tmp.path() + "/紙.genkobrush";
        studio.choose(key);
        QVERIFY(studio.window->export_brush(file));
        const Json written = core::parse_python_json(genko::test::read_bytes(file));
        const Json& carried = written["brushes"][key]["paper"];
        QVERIFY(carried.contains("png") && !carried.contains("asset"));
        QVERIFY(render::paper::take_in(core::a2b_base64(carried["png"].get<std::string>())) == *render::paper::bytes(made.paper->asset));
        forget_library();
        render::brushes::clear_custom();
        render::paper::clear();
        const auto keys = studio.window->import_brushes(file);
        QCOMPARE(keys.size(), std::size_t(1));
        const core::Brush back = render::brushes::brush(keys[0]);
        QVERIFY(back.paper.has_value());
        QVERIFY(back.paper == made.paper);
        QVERIFY(render::paper::bytes(back.paper->asset) != nullptr);
        QVERIFY(render::brushes::load_own_brushes(app::config_dir())[keys[0]].contains("paper"));
    }

    // The book on another computer: closed, the process knowing no brush and no picture, no library; opened again, the
    // page draws the same pixels and the book's brush draws more lines with its paper.
    void anotherComputer() {
        fs::path book;
        render::Image before;
        std::string key = "my_carried";
        QTemporaryDir keep;
        {
            Studio studio;
            book = gui_test::path_of(keep.path() + "/持ち出し.genko");
            render::brushes::define_brush(key, Json{{"label", "持ち出し"}, {"base", "fill_pen"}, {"width_mm", 5},
                                                    {"paper", render::brushes::with_paper_taken_in(Json{{"paper", Json{{"png", core::b64encode(grain_png())},
                                                                                                                        {"density", 1}, {"rotation", 20}}}})["paper"]}});
            render::brushes::save_to_library(app::config_dir(), key, core::brush_to_dict(render::brushes::brush(key)));
            studio.brush().reload_kinds(key);
            studio.draw({QPointF(8, 20), QPointF(52, 30)});
            QVERIFY(studio.session->document().brush_custom.contains(key));
            studio.session->save_as(book);
            QVERIFY(gui_test::wait_for([&] { return studio.session->path() == book && !studio.session->unsaved(); }));
            before = studio.page(studio.session->document());
        }
        forget_library();
        render::brushes::clear_custom();
        render::paper::clear();
        Studio there(book);
        QVERIFY(there.session->read_only_reason().empty());
        QVERIFY(there.page(there.session->document()).tobytes() == before.tobytes());
        there.choose(key);
        QCOMPARE(there.brush().kind(), key);
        there.draw({QPointF(8, 45), QPointF(52, 50)});
        QCOMPARE(gui_test::ink_strokes(there.session->document()), std::size_t(2));
        QVERIFY(render::brushes::brush(key).paper.has_value());
    }
};

QTEST_MAIN(TestGuiPaper)
#include "test_gui_paper.moc"
