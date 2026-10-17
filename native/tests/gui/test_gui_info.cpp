// PC2d (SPEC.md COLOR-04: window.panel.histogram, window.panel.info, edit.profileInfo) in the window, on the offscreen
// platform: ヒストグラム counts the layer drawn on — its own pixels, each counted once — and the page as drawn, with
// the numbers of each channel; 情報 shows under the pointer what `genko inspect --pixel` gives (the screen's colour,
// the page's values, an RGB page's and a CMYK page's), the proof's values and the selection's size; 編集 →
// プロファイルの情報… shows what a profile says of itself.
#include <QtTest>
#include <QAction>
#include <QComboBox>
#include <QDialog>
#include <QDockWidget>
#include <QLabel>
#include "gui_support.hpp"
#include "app/canvas.hpp"
#include "app/config.hpp"
#include "app/display_color.hpp"
#include "app/info_panels.hpp"
#include "app/main_window.hpp"
#include "app/tiles.hpp"
#include "core/base64.hpp"
#include "core/color_model.hpp"
#include "render/inspect.hpp"
#include "render/png.hpp"
#include "render/proof.hpp"
using namespace genko;
using core::Json;

namespace {

// A book of two colour pages (60 × 60 mm, no panels): page 1 a paint layer of 60 × 40 pixels — a grey (128) on its
// left half, a green (alpha 200) on its right —, page 2 the same made a CMYK page.
core::Document book_doc() {
    auto doc = core::new_episode("情報", core::Num(1), 2, core::PageSpec::custom(60, 60, 50, 50, 1, 2, 2, 2, 2, 72, "color"));
    for (std::size_t i = 0; i < doc.pages.size(); ++i) doc.edit_page(i).frames.clear();
    render::Image picture = render::Image::create("RGBA", render::Size{60, 40}, render::Ink{0, 0, 0, 0});
    picture.paste(render::Ink{128, 128, 128, 255}, render::Box{0, 0, 30, 40});
    picture.paste(render::Ink{30, 160, 90, 200}, render::Box{30, 0, 60, 40});
    core::Layer paint;
    paint.id = "paint0000001";
    paint.kind = core::LayerKind::Raster;
    paint.title = "絵";
    paint.panel_clip = false;
    paint.raster_png = std::make_shared<const std::string>(render::write_png(picture));
    doc.edit_page(0).layers.push_back(paint);
    paint.id = "paint0000002";
    doc.edit_page(1).layers.push_back(std::move(paint));
    return doc;
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
        window->canvas()->fit_page();
        window->set_target_layer("paint0000001");
    }
    const core::Document& book() const { return session->document(); }
};

}  // namespace

class TestGuiInfo : public QObject {
    Q_OBJECT
private slots:
    void histogramCountsTheLayersPixels() {
        Studio s;
        QDockWidget* dock = s.window->histogram_dock();
        QVERIFY(dock != nullptr);
        QVERIFY(!dock->isVisible());
        dock->toggleViewAction()->trigger();  // (ウィンドウ → ヒストグラム)
        QVERIFY(dock->isVisible());
        app::HistogramPanel* panel = s.window->histogram_panel();
        panel->source()->setCurrentIndex(1);  // (the layer drawn on: its own 60 × 40 pixels)
        const app::Histogram& h = panel->histogram();
        QCOMPARE(h.pixels, std::uint64_t{2400});
        QCOMPARE(h.bins[app::Histogram::kRed][128], std::uint64_t{1200});
        QCOMPARE(h.bins[app::Histogram::kRed][30], std::uint64_t{1200});
        QCOMPARE(h.bins[app::Histogram::kGreen][160], std::uint64_t{1200});
        QCOMPARE(h.bins[app::Histogram::kBlue][90], std::uint64_t{1200});
        QCOMPARE(h.bins[app::Histogram::kRgb][(30 + 160 + 90) / 3], std::uint64_t{1200});
        QCOMPARE(h.bins[app::Histogram::kLuminosity][(30 * 299 + 160 * 587 + 90 * 114 + 500) / 1000], std::uint64_t{1200});
        std::uint64_t all = 0;
        for (const std::uint64_t v : h.bins[app::Histogram::kRgb]) all += v;
        QCOMPARE(all, h.pixels);
        // the numbers of the channel shown (red: half 128, half 30)
        panel->channel()->setCurrentIndex(1);
        QCOMPARE(panel->figure(0)->text(), QStringLiteral("79.00"));
        QCOMPARE(panel->figure(1)->text(), QStringLiteral("49.00"));
        QCOMPARE(panel->figure(2)->text(), QStringLiteral("30"));
        QCOMPARE(panel->figure(3)->text(), QStringLiteral("2400"));
        const app::HistogramStats stats = app::stats_of(h.bins[app::Histogram::kRed]);
        QCOMPARE(stats.pixels, std::uint64_t{2400});
        // the page as drawn: every pixel of its picture (opaque), the paper's white the most
        panel->source()->setCurrentIndex(0);
        const std::uint64_t page_pixels = panel->histogram().pixels;
        QVERIFY(page_pixels > 2400);
        QVERIFY(panel->histogram().bins[app::Histogram::kRed][128] > page_pixels / 3);  // (the grey half of the page)
        // followed while shown: the layer's new pixels counted a moment after the op
        panel->source()->setCurrentIndex(1);
        const render::Image small = render::Image::create("RGBA", render::Size{10, 10}, render::Ink{1, 2, 3, 255});
        QVERIFY(s.window->apply_ops(Json::array({Json{{"op", "put_raster"}, {"page", 1}, {"id", "paint0000001"}, {"png_base64", core::b64encode(render::write_png(small))}}})));
        QTRY_COMPARE_WITH_TIMEOUT(panel->histogram().pixels, std::uint64_t{100}, 3000);
        QCOMPARE(panel->histogram().bins[app::Histogram::kBlue][3], std::uint64_t{100});
    }

    void infoShowsWhatInspectGives() {
        Studio s;
        QDockWidget* dock = s.window->info_dock();
        QVERIFY(dock != nullptr);
        QCOMPARE(dock->toggleViewAction()->shortcut(), QKeySequence(QStringLiteral("F8")));
        dock->toggleViewAction()->trigger();
        QVERIFY(dock->isVisible());
        app::InfoPanel* panel = s.window->info_panel();
        // the pointer over the grey (the picture stretched over the page: its left half)
        emit s.window->canvas()->pointerOver(10.0, 30.0);
        QVERIFY(panel->at().has_value());
        const Json want = render::pixel_at(s.book().page(0), &s.book(), 10.0, 30.0, std::nullopt, 72);
        QCOMPARE((*panel->at())["srgb"], want["srgb"]);
        QCOMPARE((*panel->at())["values"], want["values"]);
        QVERIFY2(panel->screen()->text().contains(QStringLiteral("R 128")), qPrintable(panel->screen()->text()));
        QVERIFY(panel->place()->text().contains(QStringLiteral("X 10.0 mm")));
        // the proof's values: Genko's own CMYK for print (書き出しの CMYK), its K for a grey
        const Json proof = render::proof::proof_values(s.window->canvas()->renderer().proof_setup(), 128, 128, 128);
        QCOMPARE(proof["space"], Json("cmyk"));
        QVERIFY2(panel->proof()->text().startsWith(QStringLiteral("C ")), qPrintable(panel->proof()->text()));
        // off the page: nothing
        emit s.window->canvas()->pointerOver(-5.0, 30.0);
        QVERIFY(!panel->at().has_value());
        QCOMPARE(panel->values()->text(), QStringLiteral("—"));
        // a CMYK page: its inks, as the status bar and --pixel say them
        QVERIFY(s.window->apply_ops(Json::array({Json{{"op", "set_color_mode"}, {"page", 2}, {"mode", "cmyk"}, {"profile", "coated-cmyk"}}})));
        s.window->go_to_page(2);  // (by its number)
        emit s.window->canvas()->pointerOver(10.0, 30.0);
        QVERIFY(panel->at().has_value());
        const Json inks = render::pixel_at(s.book().page(1), &s.book(), 10.0, 30.0, std::nullopt, 72);
        QVERIFY2((*panel->at())["values"] == inks["values"], (panel->at()->dump() + " " + inks.dump()).c_str());
        QVERIFY2(panel->values()->text().contains(QStringLiteral("K ")), qPrintable(panel->values()->text()));
        // the selection's size
        s.window->canvas()->set_selection(Json{{"poly", Json::array({Json::array({5, 5}), Json::array({25, 5}), Json::array({25, 15}), Json::array({5, 15})})}},
                                          std::vector<QPointF>{{5, 5}, {25, 5}, {25, 15}, {5, 15}});
        panel->show_selection();
        QVERIFY2(panel->selection()->text().startsWith(QStringLiteral("幅 20.0 mm（57 px）  高さ 10.0 mm（28 px）")), qPrintable(panel->selection()->text()));
        s.window->canvas()->set_selection(std::nullopt);
        panel->show_selection();
        QCOMPARE(panel->selection()->text(), QStringLiteral("なし"));
    }

    // モニターのプロファイル (render:ICC-display, PC1b's, checked in PC2d): カラー設定's monitor profile takes the page on screen
    // to the monitor's colours (relative, black point compensated) — the screen's picture alone: the page's own pixels,
    // the eyedropper's, as they were —; sRGB does nothing.
    void theMonitorsProfileShowsThePage() {
        Studio s;
        const core::Bytes p3 = core::builtin_profile("display-p3")->bytes;
        QVERIFY(!app::display_color::monitor_profile(QStringLiteral("srgb")));
        QVERIFY(app::display_color::monitor_profile(QStringLiteral("display-p3")) && *app::display_color::monitor_profile(QStringLiteral("display-p3")) == *p3);
        const auto monitor = app::display_color::Monitor::to(p3);
        QVERIFY(monitor != nullptr);
        QVERIFY(QString::fromStdString(monitor->description()).contains(QStringLiteral("P3")));
        app::settings()->setValue(QStringLiteral("color/monitorProfile"), QStringLiteral("display-p3"));
        s.window->action("act_proof_export")->trigger();  // (the view's settings taken again: the monitor's too)
        QVERIFY(s.window->canvas()->wait_rendered(10000));
        const int dpi = s.window->canvas()->renderer().shown_dpi();
        const QImage page = s.window->canvas()->renderer().compose(dpi).convertToFormat(QImage::Format_RGB32);
        const QImage shown = s.window->canvas()->renderer().compose_shown(dpi).convertToFormat(QImage::Format_RGB32);
        QImage want = page;
        monitor->apply(want);
        QCOMPARE(shown, want);
        QVERIFY(shown != page);  // (the green's numbers on a P3 monitor are not sRGB's)
        app::settings()->setValue(QStringLiteral("color/monitorProfile"), QStringLiteral("srgb"));
        s.window->action("act_proof_export")->trigger();
        QVERIFY(s.window->canvas()->wait_rendered(10000));
        QCOMPARE(s.window->canvas()->renderer().compose_shown(dpi).convertToFormat(QImage::Format_RGB32),
                 s.window->canvas()->renderer().compose(dpi).convertToFormat(QImage::Format_RGB32));
        app::settings()->remove(QStringLiteral("color/monitorProfile"));
    }

    void profileInfoDialog() {
        Studio s;
        QAction* act = s.window->action("act_profile_info");
        QVERIFY(act != nullptr);
        QStringList seen;
        s.answers.responder->exec = [&seen](QDialog* d) {
            if (d->objectName() != QStringLiteral("profile_info_dialog")) return 0;
            auto* choice = d->findChild<QComboBox*>(QStringLiteral("profile_choice"));
            auto* shown = d->findChild<QLabel*>(QStringLiteral("profile_info"));
            seen << shown->text();  // (the page's: sRGB, Little CMS's own)
            choice->setCurrentIndex(choice->findData(QStringLiteral("coated-cmyk")));
            seen << shown->text();
            return static_cast<int>(QDialog::Rejected);
        };
        act->trigger();
        QCOMPARE(seen.size(), 2);
        QVERIFY2(seen[0].contains(QStringLiteral("色空間: RGB")) && seen[0].contains(QStringLiteral("Little CMS 内蔵")), qPrintable(seen[0]));
        QVERIFY2(seen[1].contains(QStringLiteral("色空間: CMYK")) && seen[1].contains(QStringLiteral("種類: 出力（印刷）")), qPrintable(seen[1]));
        const Json info = render::proof::profile_info(core::builtin_profile("coated-cmyk")->bytes);
        QVERIFY(seen[1].contains(QString::fromStdString(info["description"].get<std::string>())));
    }
};

QTEST_MAIN(TestGuiInfo)
#include "test_gui_info.moc"
