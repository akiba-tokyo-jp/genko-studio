// PC3 B3 (SPEC.md COLOR-06「賢い選択」「選択とマスク」「マッティング」, the user's decisions 4, 5 and 6) on the offscreen
// platform: the commands with their words, keys, tips and places; クイック選択ツール (a stroke grows over the disc, Alt takes
// part away) and オブジェクト選択ツール (a rectangle round it); 被写体を選択, 空を選択 (none here: the selection left alone) and
// フォーカス領域…; 選択とマスク… with its preview and its four outputs — the selection, a layer mask, a new layer, a new layer
// with a mask (and カラーの除去) — each one Undo; すべてのオブジェクトをマスク; マッティング; the worker thread stopped (nothing
// changes) and the tool's settings.
#include <QtTest>
#include <QAction>
#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QDoubleSpinBox>
#include <QLabel>
#include <QMenu>
#include <QMenuBar>
#include <QSpinBox>
#include <cmath>
#include "gui_support.hpp"
#include "app/canvas.hpp"
#include "app/inject.hpp"
#include "app/main_window.hpp"
#include "core/base64.hpp"
#include "core/command_bus.hpp"
#include "render/ops_registry.hpp"
#include "render/png.hpp"
#include "render/selection.hpp"
using namespace genko;
using core::Json;
namespace inject = genko::app::inject;

namespace {

constexpr int kSide = 236;  // (the picture's pixels over the 60 mm page: 100 dpi)

QKeySequence s(const char* text) { return QKeySequence(QString::fromLatin1(text)); }

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

bool in_disc(double x_mm, double y_mm) { return std::hypot(x_mm - 30, y_mm - 30) <= 12; }

// A textured red disc (centre (30, 30) mm, radius 12) on a noisy blue ground, opaque, over the whole page.
render::Image scene() {
    std::string bytes;
    for (int y = 0; y < kSide; ++y) {
        for (int x = 0; x < kSide; ++x) {
            const double mx = (x + 0.5) * 60.0 / kSide, my = (y + 0.5) * 60.0 / kSide;
            const double n = 0.05 * std::sin(x * 12.9898 + y * 78.233) * std::cos(x * 4.1 - y * 2.7);
            double r = 0.25 + n, g = 0.45 + n, b = 0.7 + n;
            if (in_disc(mx, my)) {
                const double t = std::sin((x + y) * 0.35) * 0.5 + 0.5;
                r = 0.85 + 0.05 * t + n;
                g = 0.2 + 0.3 * t + n;
                b = 0.15 + n;
            }
            for (const double v : {r, g, b, 1.0}) bytes.push_back(static_cast<char>(std::lround(std::clamp(v, 0.0, 1.0) * 255)));
        }
    }
    return render::Image::frombytes("RGBA", render::Size{kSide, kSide}, bytes);
}

core::Document book_doc() {
    auto doc = core::new_episode("選択とマスク", core::Num(1), 1, core::PageSpec::custom(60, 60, 50, 50, 1, 2, 2, 2, 2, 72, "color"));
    return core::CommandBus(render::ops_registry())
        .apply(doc,
               Json::array({Json{{"op", "add_layer"}, {"page", 1}, {"kind", "paint"}, {"id", "photo"}, {"title", "写真"}},
                            Json{{"op", "set_layer"}, {"page", 1}, {"id", "photo"}, {"panel_each", false}, {"panel_clip", false}},
                            Json{{"op", "put_raster"}, {"page", 1}, {"id", "photo"}, {"png_base64", core::b64encode(render::write_png(scene()))}}}),
               core::Actor("human:tester"))
        .doc;
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
        window->resize(1200, 860);
        window->show();
        canvas()->fit_page();
        window->set_target_layer("photo");
    }
    app::PageCanvas* canvas() const { return window->canvas(); }
    const core::Page& page() const { return session->document().page(0); }
    const core::Layer* layer(const std::string& id) const {
        for (const auto& l : page().layers)
            if (l.id == id) return &l;
        return nullptr;
    }
    void trigger(const char* name) const { window->action(QString::fromLatin1(name))->trigger(); }
    std::optional<Json> area() const { return canvas()->selection() ? std::optional<Json>(canvas()->selection()->area) : std::nullopt; }
    render::Image mask() const { return render::selection::to_mask(*area(), page(), nullptr, 100); }
    // the selection's IoU with the disc (counted at 100 dpi)
    double iou() const {
        if (!area()) return 0;
        const render::Image m = mask();
        std::size_t inter = 0, uni = 0;
        for (int y = 0; y < m.height(); ++y) {
            for (int x = 0; x < m.width(); ++x) {
                const bool a = m.getpixel(x, y)[0] >= 128, b = in_disc((x + 0.5) * 60.0 / m.width(), (y + 0.5) * 60.0 / m.height());
                inter += a && b ? 1 : 0;
                uni += a || b ? 1 : 0;
            }
        }
        return static_cast<double>(inter) / static_cast<double>(std::max<std::size_t>(uni, 1));
    }
    // the worker thread done
    bool settled() const { return !window->smart_select_busy(); }
    // the changes in the book's history (one Undo each)
    std::size_t steps() const { return session->history().done.size(); }
};

}  // namespace

class TestGuiSelectMask : public QObject {
    Q_OBJECT
private slots:
    void initTestCase() { qRegisterMetaType<genko::app::StrokeInput>(); }

    // The commands: words, keys, tips, places (Photoshop's names, the user's decision 5); the tools in their ways.
    void theCommands() {
        Studio studio;
        struct Expected {
            const char* attribute;
            const char* text;
            QList<QKeySequence> keys;
            const char* tip;
            bool checkable;
            const char* place;
        };
        const std::vector<Expected> list{
            {"act_object_select", "オブジェクト選択ツール", {}, "ドラッグで囲んだ中の物を選びます（Shift で追加・Alt で一部削除）", true, "ツール"},
            {"act_quick_select", "クイック選択ツール", {}, "なぞった所から、似た色の所を縁まで選びます（続けてなぞると追加、Alt で一部削除）", true, "ツール"},
            {"act_focus_area", "フォーカス領域…", {}, "ピントの合った（くっきりした）所を選びます", false, "選択範囲"},
            {"act_select_subject", "被写体を選択", {}, "ページでいちばん目立つ物を選びます（写真向け。線画では外れることがあります）", false, "選択範囲"},
            {"act_select_sky", "空を選択", {}, "写真の空を選びます", false, "選択範囲"},
            {"act_select_mask", "選択とマスク…", {s("Ctrl+Alt+R")},
             "選択範囲の縁を、絵の縁（髪の毛・ぼけ）に合わせて整え、選択範囲・マスク・新しいレイヤーに出します", false, "選択範囲"},
            {"act_mask_all_objects", "すべてのオブジェクトをマスク", {}, "描く先のレイヤーで目立つ物を見つけ、それ以外を隠すマスクを付けます", false, "レイヤー"},
            {"act_matting_decontaminate", "カラーの除去…", {}, "半透明の縁に残った背景の色を、中の色に置き換えます", false, "レイヤー/マッティング"},
            {"act_matting_defringe", "フリンジ削除…", {}, "縁から決めた幅の色を、内側の色に置き換えます", false, "レイヤー/マッティング"},
            {"act_matting_black", "黒マット削除", {}, "黒の上で切り抜いた縁の暗さを取ります", false, "レイヤー/マッティング"},
            {"act_matting_white", "白マット削除", {}, "白の上で切り抜いた縁の白さを取ります", false, "レイヤー/マッティング"},
        };
        for (const Expected& e : list) {
            QAction* a = studio.window->action(QString::fromLatin1(e.attribute));
            QVERIFY2(a != nullptr, e.attribute);
            QCOMPARE(a->text(), QString::fromUtf8(e.text));
            QCOMPARE(a->shortcuts(), e.keys);
            QCOMPARE(a->statusTip(), QString::fromUtf8(e.tip));
            QCOMPARE(a->isCheckable(), e.checkable);
            QMenu* menu = menu_at(studio.window.get(), QString::fromUtf8(e.place));
            QVERIFY2(menu != nullptr && menu->actions().contains(a), e.attribute);
        }
        for (const auto& [name, way] : {std::pair{"act_quick_select", "quick"}, {"act_object_select", "object"}}) {
            studio.trigger(name);
            QCOMPARE(studio.canvas()->tool(), QStringLiteral("marquee"));
            QCOMPARE(studio.canvas()->marquee, QString::fromLatin1(way));
            QVERIFY(studio.window->action(QString::fromLatin1(name))->isChecked());
            auto* mode = studio.window->findChild<QComboBox*>(QStringLiteral("marquee_mode"));
            QCOMPARE(mode->currentData().toString(), QString::fromLatin1(way));
        }
        // the settings: the brush's size, エッジを強調, 全レイヤーを対象
        studio.window->findChild<QDoubleSpinBox*>(QStringLiteral("quick_size"))->setValue(3);
        QCOMPARE(studio.canvas()->quick_size_mm, 3.0);
        studio.window->findChild<QCheckBox*>(QStringLiteral("quick_enhance_edge"))->setChecked(true);
        QVERIFY(studio.window->smart_select_settings.enhance_edge);
        // (全レイヤーを対象 off by default: the drawing layer, not the page with its panels' borders and nombre)
        auto* all = studio.window->findChild<QCheckBox*>(QStringLiteral("smart_all_layers"));
        QVERIFY(!all->isChecked() && !studio.window->smart_select_settings.all_layers);
        all->setChecked(true);
        QVERIFY(studio.window->smart_select_settings.all_layers);
        // the selection's right-click menu has 選択とマスク…
        studio.trigger("act_marquee");
        inject::mouse_stroke(studio.canvas(), {QPointF(10, 10), QPointF(40, 40)});
        QMenu menu;
        QVERIFY(studio.window->fill_selection_menu(menu, QPointF(20, 20)));
        QVERIFY(menu.actions().contains(studio.window->action("act_select_mask")));
    }

    // クイック選択ツール: a stroke in the disc grows over it; Alt takes the top away; オブジェクト選択ツール: a rectangle round it.
    void quickAndObjectSelection() {
        Studio studio;
        auto* canvas = studio.canvas();
        const std::size_t steps = studio.steps();
        studio.trigger("act_quick_select");
        studio.window->findChild<QDoubleSpinBox*>(QStringLiteral("quick_size"))->setValue(3);
        inject::mouse_stroke(canvas, {QPointF(26, 30), QPointF(30, 30), QPointF(34, 30)});
        QTRY_VERIFY_WITH_TIMEOUT(studio.settled(), 60000);
        QVERIFY(studio.area());
        QVERIFY2(studio.iou() > 0.85, qPrintable(QString::number(studio.iou())));
        const double whole = studio.iou();
        // a stroke on the ground added (Shift: the ground joins), then taken away again (Alt)
        inject::mouse_stroke(canvas, {QPointF(20, 50), QPointF(24, 50)}, Qt::ShiftModifier);
        QTRY_VERIFY_WITH_TIMEOUT(studio.settled(), 60000);
        QVERIFY2(studio.iou() < 0.6, qPrintable(QString::number(studio.iou())));
        inject::mouse_stroke(canvas, {QPointF(20, 50), QPointF(24, 50)}, Qt::AltModifier);
        QTRY_VERIFY_WITH_TIMEOUT(studio.settled(), 60000);
        QVERIFY2(std::abs(studio.iou() - whole) < 0.1, qPrintable(QStringLiteral("%1 %2").arg(studio.iou()).arg(whole)));
        // オブジェクト選択ツール: a new selection of the disc in the rectangle
        studio.trigger("act_deselect");
        studio.trigger("act_object_select");
        inject::mouse_stroke(canvas, {QPointF(14, 14), QPointF(30, 30), QPointF(46, 46)});
        QTRY_VERIFY_WITH_TIMEOUT(studio.settled(), 60000);
        QVERIFY2(studio.iou() > 0.85, qPrintable(QString::number(studio.iou())));
        // nothing in the book changed (the selection is the window's)
        QCOMPARE(studio.steps(), steps);
    }

    // 被写体を選択 (the disc), 空を選択 (no sky: said, the selection left as it was), フォーカス領域….
    void subjectSkyFocus() {
        Studio studio;
        studio.trigger("act_select_subject");
        QTRY_VERIFY_WITH_TIMEOUT(studio.settled(), 60000);
        QVERIFY2(studio.iou() > 0.85, qPrintable(QString::number(studio.iou())));
        const Json before = *studio.area();
        studio.trigger("act_select_sky");
        QTRY_VERIFY_WITH_TIMEOUT(studio.settled(), 60000);
        QVERIFY2(studio.window->last_notice() == QStringLiteral("空が見つかりませんでした") || studio.area() != before,
                 qPrintable(studio.window->last_notice()));
        bool asked = false;
        studio.answers.responder->exec = [&](QDialog* dialog) {
            if (dialog->objectName() != QStringLiteral("focus_area_dialog")) return int(QDialog::Rejected);
            asked = true;
            dialog->findChild<QSpinBox*>(QStringLiteral("focus_range"))->setValue(80);
            return int(QDialog::Accepted);
        };
        studio.trigger("act_focus_area");
        QVERIFY(asked);
        QTRY_VERIFY_WITH_TIMEOUT(studio.settled(), 60000);
    }

    // 選択とマスク…: its preview; its four outputs, each one Undo; カラーの除去 makes the new layer with a mask.
    void selectAndMaskOutputs() {
        Studio studio;
        const auto hard = [&] {
            studio.trigger("act_deselect");
            studio.window->set_target_layer("photo");
            studio.trigger("act_sel_ellipse");
            inject::mouse_stroke(studio.canvas(), {QPointF(18, 18), QPointF(30, 30), QPointF(42, 42)});
            QVERIFY(studio.area());
        };
        QString output = QStringLiteral("selection");
        bool decontaminate = false;
        bool previewed = false;
        std::string switch_to;  // (another layer drawn on while the window is open)
        studio.answers.responder->exec = [&](QDialog* dialog) {
            if (dialog->objectName() != QStringLiteral("select_mask_dialog")) return int(QDialog::Rejected);
            if (!switch_to.empty()) studio.window->set_target_layer(switch_to);
            dialog->findChild<QDoubleSpinBox*>(QStringLiteral("sm_radius"))->setValue(1.5);
            dialog->findChild<QDoubleSpinBox*>(QStringLiteral("sm_feather"))->setValue(0.3);
            auto* out = dialog->findChild<QComboBox*>(QStringLiteral("sm_output"));
            out->setCurrentIndex(out->findData(output));
            dialog->findChild<QCheckBox*>(QStringLiteral("sm_decontaminate"))->setChecked(decontaminate);
            if (!previewed) {  // (the preview comes from its worker a moment after a change; each view mode shows it)
                auto* picture = dialog->findChild<QLabel*>(QStringLiteral("sm_preview"));
                for (int i = 0; i < 300 && picture->pixmap().isNull(); ++i) QTest::qWait(100);
                auto* view = dialog->findChild<QComboBox*>(QStringLiteral("sm_view"));
                for (int i = 0; i < view->count(); ++i) view->setCurrentIndex(i);
                previewed = !picture->pixmap().isNull();
            }
            return int(QDialog::Accepted);
        };
        const std::size_t steps = studio.steps();
        // 選択範囲: the edge soft now
        hard();
        studio.trigger("act_select_mask");
        QTRY_VERIFY_WITH_TIMEOUT(studio.settled(), 60000);
        QVERIFY(previewed);
        QVERIFY(studio.area() && studio.area()->contains("mask"));
        const render::Image m = studio.mask();
        int soft = 0;
        for (int x = 0; x < m.width(); ++x) soft += m.getpixel(x, m.height() / 2)[0] > 10 && m.getpixel(x, m.height() / 2)[0] < 245 ? 1 : 0;
        QVERIFY2(soft >= 2, qPrintable(QString::number(soft)));
        QVERIFY2(studio.iou() > 0.8, qPrintable(QString::number(studio.iou())));
        QCOMPARE(studio.steps(), steps);
        // レイヤーマスク: the drawing layer's mask (linked), the selection let go; one Undo takes it off
        const std::size_t layers = studio.page().layers.size();
        output = QStringLiteral("layer_mask");
        hard();
        studio.trigger("act_select_mask");
        QTRY_VERIFY_WITH_TIMEOUT(studio.settled(), 60000);
        QVERIFY(studio.layer("photo")->mask);
        QVERIFY(studio.layer("photo")->mask->linked);
        QVERIFY(!studio.area());
        studio.trigger("act_undo");
        QVERIFY(!studio.layer("photo")->mask);
        QCOMPARE(studio.steps(), steps);
        // 新規レイヤー: the selection's pixels on a new layer, the source hidden; one Undo
        output = QStringLiteral("new_layer");
        hard();
        studio.trigger("act_select_mask");
        QTRY_VERIFY_WITH_TIMEOUT(studio.settled(), 60000);
        QCOMPARE(studio.page().layers.size(), layers + 1);
        QVERIFY(!studio.layer("photo")->visible);
        QVERIFY(studio.window->target_layer()->id != "photo");
        QVERIFY(studio.window->target_layer()->raster_png || !studio.window->target_layer()->patches.empty());
        studio.trigger("act_undo");
        QCOMPARE(studio.page().layers.size(), layers);
        QVERIFY(studio.layer("photo")->visible);
        QCOMPARE(studio.steps(), steps);
        // (another layer drawn on while the window is open: the layer it was opened on copied, as the source hidden — the
        // coordinator's decision after the PC3 review)
        for (const core::Layer& l : studio.page().layers)
            if (l.id != "photo" && l.kind == core::LayerKind::Strokes && switch_to.empty()) switch_to = l.id;
        QVERIFY(!switch_to.empty());
        hard();
        studio.trigger("act_select_mask");
        QTRY_VERIFY_WITH_TIMEOUT(studio.settled(), 60000);
        QCOMPARE(studio.page().layers.size(), layers + 1);
        QVERIFY(!studio.layer("photo")->visible && studio.layer(switch_to)->visible);
        QVERIFY(studio.window->target_layer()->raster_png || !studio.window->target_layer()->patches.empty());
        studio.trigger("act_undo");
        QCOMPARE(studio.page().layers.size(), layers);
        QCOMPARE(studio.steps(), steps);
        switch_to.clear();
        // 新規レイヤー（レイヤーマスクあり）: a duplicate with the refined mask; one Undo
        output = QStringLiteral("new_layer_mask");
        hard();
        studio.trigger("act_select_mask");
        QTRY_VERIFY_WITH_TIMEOUT(studio.settled(), 60000);
        QCOMPARE(studio.page().layers.size(), layers + 1);
        QVERIFY(studio.window->target_layer()->mask);
        QVERIFY(!studio.layer("photo")->visible);
        studio.trigger("act_undo");
        QCOMPARE(studio.page().layers.size(), layers);
        QCOMPARE(studio.steps(), steps);
        // 不要なカラーの除去 with 選択範囲: the new layer with a mask, its fringe's colours changed; one Undo
        output = QStringLiteral("selection");
        decontaminate = true;
        hard();
        studio.trigger("act_select_mask");
        QTRY_VERIFY_WITH_TIMEOUT(studio.settled(), 60000);
        QCOMPARE(studio.page().layers.size(), layers + 1);
        const core::Layer* made = studio.window->target_layer();
        QVERIFY(made->mask);
        QVERIFY(*made->raster_png != *studio.layer("photo")->raster_png);
        studio.trigger("act_undo");
        QCOMPARE(studio.page().layers.size(), layers);
        QCOMPARE(studio.steps(), steps);
        // nothing chosen: asked to choose first
        studio.trigger("act_deselect");
        studio.trigger("act_select_mask");
        QVERIFY(studio.settled());
    }

    // すべてのオブジェクトをマスク (one Undo), マッティング (each an op), the worker stopped: nothing changes, said.
    void maskAllObjectsMattingAndStopping() {
        Studio studio;
        studio.trigger("act_mask_all_objects");
        QTRY_VERIFY_WITH_TIMEOUT(studio.settled(), 60000);
        QVERIFY(studio.layer("photo")->mask);
        QVERIFY(studio.layer("photo")->mask->linked);
        studio.trigger("act_undo");
        QVERIFY(!studio.layer("photo")->mask);
        // the matting: an opaque layer has no fringe (done, nothing to change); the asked ones asked
        studio.trigger("act_matting_black");
        studio.trigger("act_matting_white");
        int asked = 0;
        studio.answers.responder->get_double = [&](const QString&, const QString&, double, double, double, int) {
            ++asked;
            return std::optional<double>(0.5);
        };
        studio.trigger("act_matting_defringe");
        studio.trigger("act_matting_decontaminate");
        QCOMPARE(asked, 2);
        QVERIFY(studio.window->last_error().isEmpty());
        // stopped at once: the selection as it was, said
        QVERIFY(!studio.area());
        studio.trigger("act_select_subject");
        QVERIFY(!studio.settled());
        studio.window->cancel_smart_select();
        QTRY_VERIFY_WITH_TIMEOUT(studio.settled(), 60000);
        QVERIFY(!studio.area());
        QCOMPARE(studio.window->last_notice(), QStringLiteral("被写体を選択をやめました"));
    }
};

QTEST_MAIN(TestGuiSelectMask)
#include "test_gui_select_mask.moc"
