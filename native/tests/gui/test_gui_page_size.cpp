// 判型 in the window (PC3 C-e, SPEC.md COLOR-07「判型」): ページ → 画像解像度…, カンバスサイズ… (Ctrl+Alt+C), 画像の回転 ▸,
// 切り抜き, トリミング…, すべての領域を表示 and ツール → 切り抜きツール (Shift+C) — where they are and their keys; each dialog's op
// as the window sends it; a book's covers left out of 画像解像度… and the person told; a page's own paper shown on the
// canvas as the book draws it, undone; the finished size that would
// not fit the new paper fitted to it once the person agrees; the crop tool's box drawn, turned, cropped to with Enter and
// let go with Esc, asked about when another tool is taken and let go on another page; the book preview drawing each page
// of a spread as big as its own paper.

#include <QAction>
#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QMenu>
#include <QMenuBar>
#include <QSpinBox>
#include <QTemporaryDir>
#include <QtTest>

#include <cmath>
#include <functional>

#include "app/bookview.hpp"
#include "app/canvas.hpp"
#include "app/main_window.hpp"
#include "app/page_size_dialogs.hpp"
#include "core/covers.hpp"
#include "core/pyconv.hpp"
#include "core/vector.hpp"
#include "gui_support.hpp"
#include "render/page.hpp"

using namespace genko;
using core::Json;
namespace fs = std::filesystem;

namespace {

// A book of `pages` A4 pages (210 × 297, its finished size 204 × 291) on disk in a window; every op it applies recorded.
struct Window {
    QTemporaryDir tmp;
    fs::path path;
    std::shared_ptr<app::Session> session;
    std::unique_ptr<app::MainWindow> window;
    gui_test::Answers answers;
    std::vector<Json> ops;
    QMetaObject::Connection recording;

    explicit Window(int pages = 3) {
        (void)gui_test::config_folder();
        path = gui_test::path_of(tmp.path() + QStringLiteral("/book.genko"));
        gui_test::write_book(path, gui_test::new_doc(pages));
        session = app::Session::open(path, gui_test::quick(gui_test::path_of(tmp.path() + QStringLiteral("/recovery"))));
        recording = QObject::connect(session.get(), &app::Session::changed, session.get(), [this](const app::BookChange& c) {
            if (c.why != app::BookChange::Why::Edit) return;
            for (Json op : c.ops) {
                if (op.is_object()) op.erase("_report");
                ops.push_back(op);
            }
        });
        window = std::make_unique<app::MainWindow>(session);
        window->resize(1300, 900);
        window->show();
        (void)QTest::qWaitForWindowExposed(window.get());
        window->canvas()->fit_page();
    }
    ~Window() {
        window.reset();
        QObject::disconnect(recording);
    }
    const core::Document& book() const { return window->book(); }
    bool trigger(const char* name) const {
        QAction* act = window->action(QString::fromLatin1(name));
        if (act == nullptr) return false;
        act->trigger();
        return true;
    }
    std::string since(std::size_t from) const {
        Json out = Json::array();
        for (std::size_t i = from; i < ops.size(); ++i) out.push_back(ops[i]);
        return core::dump_python(out);
    }
    // The canvas drawn as render_page draws the page in front now.
    bool drawn_as_book() const {
        app::PageCanvas* canvas = window->canvas();
        if (!canvas->wait_rendered(60000)) return false;
        const QImage now = canvas->renderer().compose(canvas->base_dpi());
        render::RenderOptions options;
        options.mode = "proof";
        options.skip_unported = true;
        const auto image = render::render_page(book().page(static_cast<std::size_t>(window->page_index())), canvas->base_dpi(), options, &book()).image;
        const auto bytes = image.tobytes();
        const QImage wanted = QImage(reinterpret_cast<const uchar*>(bytes.data()), image.width(), image.height(), image.width() * 3, QImage::Format_RGB888)
                                  .convertToFormat(QImage::Format_RGB32);
        return !now.isNull() && now == wanted;
    }
    // A drag on the canvas from one place of the page to another (mm), with these keys held.
    void drag(const QPointF& from, const QPointF& to, Qt::KeyboardModifiers keys = {}) const {
        app::PageCanvas* canvas = window->canvas();
        const QPoint a = canvas->to_widget(from).toPoint(), b = canvas->to_widget(to).toPoint();
        QTest::mousePress(canvas, Qt::LeftButton, keys, a);
        for (int k = 1; k <= 4; ++k) QTest::mouseMove(canvas, a + (b - a) * k / 4);
        QTest::mouseRelease(canvas, Qt::LeftButton, keys, b);
    }
};

QStringList menus_of(app::MainWindow* window, QAction* wanted) {
    QStringList found;
    std::function<void(QMenu*, const QString&)> walk = [&](QMenu* menu, const QString& at) {
        for (QAction* a : menu->actions()) {
            if (a == wanted) found << at;
            if (a->menu() != nullptr) walk(a->menu(), at + QStringLiteral("/") + a->menu()->title());
        }
    };
    for (QAction* top : window->menuBar()->actions()) {
        if (top->menu() != nullptr) walk(top->menu(), top->menu()->title());
    }
    return found;
}

bool near(double a, double b, double eps = 1e-6) { return std::fabs(a - b) < eps; }

}  // namespace

class TestGuiPageSize : public QObject {
    Q_OBJECT

private slots:
    // The commands where Photoshop has them (ページ is Genko's イメージ), Photoshop's names and keys (画像解像度's Ctrl+Alt+I
    // is 選択範囲を反転 here: none).
    void theCommandsAndTheirKeys() {
        Window w;
        const std::vector<std::tuple<const char*, const char*, const char*, const char*>> commands{
            {"act_image_size", "画像解像度…", "Ctrl+Alt+I", "ページ"},  // (PC5 H-0: the Photoshop key set)
            {"act_canvas_size", "カンバスサイズ…", "Ctrl+Alt+C", "ページ"},
            {"act_rotate_180", "180°", "", "ページ/画像の回転"},
            {"act_rotate_90cw", "90°（時計回り）", "", "ページ/画像の回転"},
            {"act_rotate_90ccw", "90°（反時計回り）", "", "ページ/画像の回転"},
            {"act_rotate_angle", "角度入力…", "", "ページ/画像の回転"},
            {"act_flip_canvas_h", "カンバスを左右反転", "", "ページ/画像の回転"},
            {"act_flip_canvas_v", "カンバスを上下反転", "", "ページ/画像の回転"},
            {"act_crop", "切り抜き", "", "ページ"},
            {"act_trim", "トリミング…", "", "ページ"},
            {"act_reveal_all", "すべての領域を表示", "", "ページ"},
            {"act_crop_tool", "切り抜きツール", "C", "ツール"}  /* (PC5 H-0: the Photoshop key set) */};
        for (const auto& [name, title, key, menu] : commands) {
            QAction* act = w.window->action(QString::fromLatin1(name));
            QVERIFY2(act != nullptr, name);
            QCOMPARE(act->text(), QString::fromUtf8(title));
            QCOMPARE(act->shortcut().toString(QKeySequence::PortableText), QString::fromLatin1(key));
            QCOMPARE(menus_of(w.window.get(), act), QStringList({QString::fromUtf8(menu)}));
        }
        QVERIFY(w.window->action(QStringLiteral("act_crop_tool"))->isCheckable());
    }

    // 画像解像度…: every page by default, the size kept in shape; the book's paper changed (no page of its own), the canvas
    // as the book; 再サンプル off keeps the pixels and changes the resolution. カンバスサイズ… for this page: its own paper,
    // shown, undone.
    void imageSizeAndCanvasSize() {
        Window w;
        w.answers.responder->exec = [](QDialog* d) {
            if (auto* size = qobject_cast<app::ImageSizeDialog*>(d)) {
                size->width->setValue(105);
                return int(QDialog::Accepted);
            }
            return int(QDialog::Rejected);
        };
        std::size_t n = w.ops.size();
        QVERIFY(w.trigger("act_image_size"));
        QCOMPARE(w.since(n), std::string(R"([{"op": "resize_pages", "pages": "all", "resample": "bicubic", "width_mm": 105.0}])"));
        QVERIFY(near(w.book().spec.width_mm.value(), 105) && near(w.book().spec.height_mm.value(), 148.5));
        QVERIFY(!core::vector::uses_page_spec(w.book()));
        QVERIFY(w.drawn_as_book());
        w.answers.responder->exec = [](QDialog* d) {
            if (auto* size = qobject_cast<app::ImageSizeDialog*>(d)) {
                size->resample->setChecked(false);
                size->dpi->setValue(300);
                return int(QDialog::Accepted);
            }
            return int(QDialog::Rejected);
        };
        n = w.ops.size();
        QVERIFY(w.trigger("act_image_size"));
        QCOMPARE(w.since(n), std::string(R"([{"op": "resize_pages", "pages": "all", "resample": "none", "dpi": 300}])"));
        QVERIFY(near(w.book().spec.width_mm.value(), 210) && near(w.book().spec.dpi.value(), 300));
        // カンバスサイズ… on this page: 20 mm more on the left
        w.answers.responder->exec = [](QDialog* d) {
            if (auto* canvas = qobject_cast<app::CanvasSizeDialog*>(d)) {
                canvas->scope->setCurrentIndex(canvas->scope->findData(QStringLiteral("page")));
                canvas->relative->setChecked(true);
                canvas->width->setValue(20);
                canvas->choose_anchor(QStringLiteral("right"));
                return int(QDialog::Accepted);
            }
            return int(QDialog::Rejected);
        };
        n = w.ops.size();
        QVERIFY(w.trigger("act_canvas_size"));
        QCOMPARE(w.since(n),
                 std::string(R"([{"op": "set_canvas_size", "page": 1, "width_mm": 20.0, "height_mm": 0.0, "relative": true, "anchor": "right", "extension": "transparent"}])"));
        QVERIFY(near(w.book().page(0).spec.width_mm.value(), 230) && w.book().page(0).own_spec);
        QVERIFY(!w.book().page(1).own_spec);
        QVERIFY(w.drawn_as_book());
        QVERIFY(w.trigger("act_undo"));
        QVERIFY(near(w.book().page(0).spec.width_mm.value(), 210) && !w.book().page(0).own_spec);
        QVERIFY(w.drawn_as_book());
    }

    // 画像解像度… on a book with a cover (its default, the whole book): done on the other pages, the cover as it was, and
    // the person told which (the coordinator's decision after the PC3 review: it was refused every time).
    void imageSizeLeavesTheCoversOut() {
        Window w(2);
        QVERIFY(w.window->apply_ops(Json::array({Json{{"op", "add_cover"}, {"kind", "front"}}})));
        const std::size_t at_cover = w.book().pages.size() - 1;
        QVERIFY(core::is_cover(w.book().page(at_cover)));
        const QString cover = QString::fromStdString(w.book().page(at_cover).index.repr());
        w.answers.responder->exec = [](QDialog* d) {
            if (auto* size = qobject_cast<app::ImageSizeDialog*>(d)) {
                size->width->setValue(105);
                return int(QDialog::Accepted);
            }
            return int(QDialog::Rejected);
        };
        const std::size_t n = w.ops.size();
        QVERIFY(w.trigger("act_image_size"));
        QCOMPARE(w.since(n), std::string(R"([{"op": "resize_pages", "pages": "all", "resample": "bicubic", "width_mm": 105.0}])"));
        QVERIFY(near(w.book().page(0).spec.width_mm.value(), 105) && near(w.book().page(at_cover).spec.width_mm.value(), 210));
        QVERIFY2(w.window->last_notice().contains(QStringLiteral("表紙・カバー・帯（%1 ページ）").arg(cover)), qPrintable(w.window->last_notice()));
    }

    // 画像の回転: the page in front turned (its paper on its side), flipped, turned by an angle; a canvas that would cut the
    // finished size: the person asked, and it is fitted to the new paper when they agree (nothing done when they do not).
    void rotationAndTheFinishedSize() {
        Window w;
        std::size_t n = w.ops.size();
        QVERIFY(w.trigger("act_rotate_90cw"));
        QCOMPARE(w.since(n), std::string(R"([{"op": "rotate_pages", "page": 1, "turn": "90cw", "text": "upright"}])"));
        QVERIFY(near(w.book().page(0).spec.width_mm.value(), 297) && near(w.book().page(0).spec.height_mm.value(), 210));
        QVERIFY(w.drawn_as_book());
        n = w.ops.size();
        QVERIFY(w.trigger("act_flip_canvas_h"));
        QCOMPARE(w.since(n), std::string(R"([{"op": "rotate_pages", "page": 1, "turn": "flip_h", "text": "upright"}])"));
        w.answers.responder->exec = [](QDialog* d) {
            if (auto* turn = qobject_cast<app::RotateAngleDialog*>(d)) {
                turn->angle->setValue(15);
                turn->direction->setCurrentIndex(turn->direction->findData(QStringLiteral("ccw")));
                return int(QDialog::Accepted);
            }
            return int(QDialog::Rejected);
        };
        n = w.ops.size();
        QVERIFY(w.trigger("act_rotate_angle"));
        QCOMPARE(w.since(n), std::string(R"([{"op": "rotate_pages", "page": 1, "angle": -15.0, "interp": "bicubic", "text": "upright"}])"));
        QVERIFY(w.book().page(0).frames.at(0).poly.has_value());
        // a smaller canvas: refused as it is, then (asked) fitted
        w.answers.responder->exec = [](QDialog* d) {
            if (auto* canvas = qobject_cast<app::CanvasSizeDialog*>(d)) {
                canvas->scope->setCurrentIndex(canvas->scope->findData(QStringLiteral("page")));
                canvas->width->setValue(150);
                canvas->height->setValue(200);
                return int(QDialog::Accepted);
            }
            return int(QDialog::Rejected);
        };
        w.window->go_to_page(2);
        n = w.ops.size();
        QVERIFY(w.trigger("act_canvas_size"));
        QCOMPARE(w.ops.size(), n);  // (the person said no)
        QVERIFY2(w.answers.asked.last().startsWith(QStringLiteral("仕上がり: ")), qPrintable(w.answers.asked.join(QStringLiteral(" / "))));
        w.answers.responder->question = [](const QString&, const QString&) { return true; };
        QVERIFY(w.trigger("act_canvas_size"));
        QCOMPARE(w.since(n),
                 std::string(R"([{"op": "set_canvas_size", "page": 2, "width_mm": 150.0, "height_mm": 200.0, "anchor": "center", "extension": "transparent", "trim": "fit"}])"));
        const core::Page& page = w.book().page(1);
        QVERIFY(near(page.spec.trim_size().first.value(), 144) && near(page.spec.width_mm.value(), 150));
        QVERIFY(w.drawn_as_book());
    }

    // 切り抜きツール: a box dragged on the page, Enter crops to it; a drag outside it turns it (the op's angle); Esc lets
    // it go; 切り抜き crops to the selection's box.
    void theCropTool() {
        Window w;
        w.answers.responder->question = [](const QString&, const QString&) { return true; };
        QVERIFY(w.trigger("act_crop_tool"));
        app::PageCanvas* canvas = w.window->canvas();
        QCOMPARE(canvas->tool(), QStringLiteral("crop"));
        w.drag(QPointF(20, 30), QPointF(120, 200));
        QVERIFY(canvas->crop().has_value());
        const QRectF box = canvas->crop()->rect();
        QVERIFY2(std::fabs(box.x() - 20) < 1 && std::fabs(box.width() - 100) < 1, qPrintable(QStringLiteral("%1 %2").arg(box.x()).arg(box.width())));
        std::size_t n = w.ops.size();
        QTest::keyClick(canvas, Qt::Key_Return);
        QCOMPARE(w.ops.size(), n + 1);
        QCOMPARE(w.ops.back()["op"], Json("crop_page"));
        QCOMPARE(w.ops.back()["trim"], Json("fit"));
        QVERIFY(!canvas->crop().has_value());
        QVERIFY(std::fabs(w.book().page(0).spec.width_mm.value() - 100) < 1);
        QVERIFY(w.drawn_as_book());
        // turned by a drag outside it
        w.window->go_to_page(2);
        w.drag(QPointF(30, 40), QPointF(150, 220));
        const QPointF middle = canvas->crop()->centre;
        w.drag(QPointF(middle.x() + 100, middle.y()), QPointF(middle.x() + 100, middle.y() + 30));
        QVERIFY2(canvas->crop()->angle > 5, qPrintable(QString::number(canvas->crop()->angle)));
        n = w.ops.size();
        QTest::keyClick(canvas, Qt::Key_Return);
        QCOMPARE(w.ops.size(), n + 1);
        QVERIFY(w.ops.back().contains("angle"));
        // Esc
        w.window->go_to_page(3);
        w.drag(QPointF(30, 40), QPointF(150, 220));
        QTest::keyClick(canvas, Qt::Key_Escape);
        QVERIFY(!canvas->crop().has_value());
        // 切り抜き: the selection's box
        canvas->set_selection(Json{{"poly", Json::array({Json::array({10, 10}), Json::array({110, 10}), Json::array({110, 160}), Json::array({10, 160})})}});
        n = w.ops.size();
        QVERIFY(w.trigger("act_crop"));
        QCOMPARE(w.ops.size(), n + 1);
        QCOMPARE(w.ops.back()["op"], Json("crop_page"));
        QVERIFY(near(w.book().page(2).spec.width_mm.value(), 100, 0.2));
        QVERIFY(!canvas->selection().has_value());
    }

    // The crop tool's box when another tool is taken: asked about, as Photoshop asks (画像を切り抜きますか？: yes crops to it,
    // no lets it go); when another page is shown: let go (it was the page's before), nothing asked or cropped.
    void theCropBoxEndsWithTheToolOrPage() {
        Window w;
        QStringList asked;
        bool yes = false;
        w.answers.responder->question = [&](const QString&, const QString& text) {
            asked << text;
            return yes;
        };
        app::PageCanvas* canvas = w.window->canvas();
        QVERIFY(w.trigger("act_crop_tool"));
        w.drag(QPointF(20, 30), QPointF(120, 200));
        QVERIFY(canvas->crop().has_value());
        std::size_t n = w.ops.size();
        QVERIFY(w.trigger("act_move"));
        QCOMPARE(asked, QStringList{QStringLiteral("画像を切り抜きますか？")});
        QVERIFY(!canvas->crop().has_value());
        QCOMPARE(w.ops.size(), n);
        QVERIFY(w.trigger("act_crop_tool"));
        QVERIFY(!canvas->crop().has_value());  // (let go: not back with the tool)
        // yes: cropped to it
        w.drag(QPointF(20, 30), QPointF(120, 200));
        yes = true;
        asked.clear();
        QVERIFY(w.trigger("act_move"));
        QVERIFY2(!asked.isEmpty() && asked.front() == QStringLiteral("画像を切り抜きますか？"), qPrintable(asked.join(QLatin1Char('|'))));
        QCOMPARE(w.ops.size(), n + 1);
        QCOMPARE(w.ops.back()["op"], Json("crop_page"));
        QVERIFY(near(w.book().page(0).spec.width_mm.value(), 100, 1));
        // another page: let go
        QVERIFY(w.trigger("act_crop_tool"));
        w.drag(QPointF(20, 30), QPointF(120, 200));
        QVERIFY(canvas->crop().has_value());
        asked.clear();
        n = w.ops.size();
        w.window->go_to_page(2);
        QVERIFY(!canvas->crop().has_value());
        QVERIFY(asked.isEmpty());
        QTest::keyClick(canvas, Qt::Key_Return);
        QCOMPARE(w.ops.size(), n);
    }

    // 本の形でプレビュー: the pages of a spread, each drawn as big as its own paper (one cut smaller is smaller).
    void bookPreviewOfPagesOfTheirOwnSize() {
        Window w(3);
        w.answers.responder->question = [](const QString&, const QString&) { return true; };
        w.window->go_to_page(2);
        QVERIFY(w.window->apply_ops(Json::array({Json{{"op", "crop_page"}, {"page", 2}, {"rect", Json::array({0, 0, 210, 200})}, {"trim", "fit"}}})));
        app::bookview::BookPreview preview(w.window.get());
        preview.resize(1000, 700);
        preview.show();
        (void)QTest::qWaitForWindowExposed(&preview);
        int spread = -1;
        for (int k = 0; k < 4; ++k) {
            const auto [l, r] = preview.sides(k);
            if ((l && *l == 1) || (r && *r == 1)) spread = k;
        }
        QVERIFY(spread >= 0);
        preview.go(spread);
        const auto [left, right] = preview.view->page_rects();
        QVERIFY(!left.isEmpty() && !right.isEmpty());
        // (page 2 is 194 mm tall to its trim, page 3 291: right-bound, page 2 on the right)
        const double ratio = right.height() / left.height();
        QVERIFY2(std::fabs(ratio - 194.0 / 291.0) < 0.02, qPrintable(QString::number(ratio)));
        QVERIFY(std::fabs(left.bottom() - right.bottom()) < 1 || std::fabs(left.center().y() - right.center().y()) < 1);
    }
};

QTEST_MAIN(TestGuiPageSize)
#include "test_gui_page_size.moc"
