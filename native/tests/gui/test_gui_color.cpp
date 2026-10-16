#include <QtTest>
#include <QAction>
#include <QApplication>
#include <QDialog>
#include <QDir>
#include <QDoubleSpinBox>
#include <QListWidget>
#include <QMessageBox>
#include <QAbstractButton>
#include <QPushButton>
#include <QTimer>
#include "gui_support.hpp"
#include "app/config.hpp"
#include "app/main_window.hpp"
#include "app/canvas.hpp"
#include "app/inject.hpp"
#include "app/tiles.hpp"
#include "app/thumbs.hpp"
#include "core/color_raster.hpp"
#include "core/paths.hpp"
#include "core/strokes.hpp"
#include "render/colour.hpp"
#include "render/page.hpp"
#include "render/png.hpp"
#include "render/proof.hpp"
#include "render/ops_registry.hpp"
#include "app/color_settings_dialog.hpp"
#include "app/preferences.hpp"
#include "core/color_model.hpp"
#include "storage/asset_store.hpp"
#include <QComboBox>
#include <QCheckBox>
#include <QMenuBar>
#include <QLabel>
#include <zlib.h>
#include <tuple>
using namespace genko;
using core::Json;
namespace {
// A page of 20 × 20 mm: a saturated green (out of coated-cmyk's gamut) on its left, a grey on its right (u16 pixels).
core::Document coloured_book(const std::string& title) {
    auto doc=core::new_episode(title,core::Num(1),1,core::PageSpec::custom(20,20,16,16,1,2,2,2,2,72,"color"));
    doc.edit_page(0).numero=false;doc.edit_page(0).layers.clear();doc.edit_page(0).frames.clear();
    return core::CommandBus().apply(doc,Json::array({Json{{"op","put_color_raster"},{"page",1},{"width",2},{"height",1},{"precision","u16"},
        {"pixels",Json::array({0,65535,0,65535,32768,32768,32768,65535})}}}),core::Actor("human:tester")).doc;
}
}  // namespace
class TestGuiColor : public QObject {
    Q_OBJECT
private slots:
    void precisionLayerActionsKeepDiskUndo_data() {
        QTest::addColumn<QString>("precision"); QTest::addColumn<QString>("operation");QTest::addColumn<QString>("blendMode");QTest::addColumn<double>("expectedRed");
        for (const QString& precision : {QString("u16"),QString("f32")})
            for (const QString& operation : {QString("merge_down"),QString("merge_layers"),QString("merge_visible"),QString("flatten")}) {
                QTest::newRow((precision+"-"+operation).toUtf8().constData()) << precision << operation << QString("normal") << (precision=="u16"?1.0/65535:-.125);
                for(const QString& mode:{QString("multiply"),QString("soft_light"),QString("color")}) {
                    const double expected=precision=="u16"?(mode=="multiply"?0.17052426212992366:mode=="soft_light"?0.19209825912052722:0.5187500429869709):
                        (mode=="multiply"?0.17346021023801111:mode=="soft_light"?0.19545507183265537:0.5208770789119147);
                    QTest::newRow((precision+"-"+operation+"-"+mode).toUtf8().constData())<<precision<<operation<<mode<<expected;
                }
            }
    }
    void precisionLayerActionsKeepDiskUndo() {
        QFETCH(QString,precision);QFETCH(QString,operation);QFETCH(QString,blendMode);QFETCH(double,expectedRed);
        (void)gui_test::config_folder();QTemporaryDir tmp;
        auto seed=core::new_episode("高精度結合",core::Num(1),2,
            core::PageSpec::custom(20,20,16,16,1,2,2,2,2,72,"color"));
        seed.edit_page(0).index=core::Num(10);seed.edit_page(1).index=core::Num(11);
        seed.edit_page(0).numero=false;seed.edit_page(0).layers.clear();
        const Json values=blendMode=="normal"?(precision=="u16" ? Json::array({1,2,3,65535}) : Json::array({-0.125,1.0000001192092896,2.0,1.0})):
            (precision=="u16"?Json::array({13001,39001,52001,65535}):Json::array({.2,.6,.8,1}));
        Json put={{"op","put_color_raster"},{"page",10},{"width",1},{"height",1},
                  {"precision",precision.toStdString()},{"pixels",values},{"id","lower"}};
        const auto lower=core::CommandBus().apply(seed,Json::array({put}),core::Actor("human:tester")).doc;
        put["id"]="upper";
        if(blendMode!="normal")put["pixels"]=precision=="u16"?Json::array({45001,19001,31001,40001}):Json::array({.7,.3,.5,.6});
        const auto upper=core::CommandBus().apply(lower,Json::array({put}),core::Actor("human:tester")).doc;
        const auto original=blendMode=="normal"?upper:core::CommandBus().apply(upper,Json::array({Json{{"op","set_layer"},{"page",10},{"id",upper.page(0).layers.back().id},{"blend",blendMode.toStdString()},{"opacity",.7}}}),core::Actor("human:tester")).doc;
        const auto path=gui_test::path_of(tmp.path()+"/book");gui_test::write_book(path,original);
        auto session=app::Session::open(path,gui_test::quick(gui_test::path_of(tmp.path()+"/recovery")));
        QVERIFY(session->read_only_reason().empty());
        const auto before=session->snapshot();
        app::MainWindow window(session);window.go_to_page(10);
        window.resize(1000,720);window.show();
        const auto checkView = [&](const QString& stage) {
            QTRY_VERIFY_WITH_TIMEOUT(window.canvas()->doc()==session->snapshot(),10000);
            QVERIFY(window.canvas()->wait_rendered(10000));
            const int dpi=window.canvas()->renderer().shown_dpi();QVERIFY(dpi>0);
            render::RenderOptions options;options.mode="proof";options.skip_unported=true;
            const auto image=render::render_page(session->document().page(0),dpi,options,&session->document()).image;
            const auto bytes=image.tobytes();
            const QImage expected=QImage(reinterpret_cast<const uchar*>(bytes.data()),image.width(),image.height(),
                image.width()*3,QImage::Format_RGB888).convertToFormat(QImage::Format_RGB32);
            QCOMPARE(window.canvas()->renderer().compose(dpi),expected);
            // Inspect the actual widget paint path in the flat interior; frame/bleed guides stay outside.
            const QImage painted=window.canvas()->grab().toImage().convertToFormat(QImage::Format_RGB32);
            const double ratio=painted.devicePixelRatio();
            const QPointF lo=window.canvas()->to_widget(QPointF(8,8))*ratio;
            const QPointF hi=window.canvas()->to_widget(QPointF(12,12))*ratio;
            const QRect box(QPoint(int(std::ceil(lo.x())),int(std::ceil(lo.y()))),
                            QPoint(int(std::floor(hi.x())),int(std::floor(hi.y()))));
            QVERIFY(box.width()>4 && box.height()>4);QVERIFY(painted.rect().contains(box));
            QImage interior(box.size(),QImage::Format_RGB32);
            interior.fill(expected.pixelColor(expected.width()/2,expected.height()/2));
            interior.setDevicePixelRatio(painted.devicePixelRatio());
            QCOMPARE(painted.copy(box),interior);
            QImage missing=interior;missing.fill(Qt::white);
            QVERIFY2(missing!=interior,"a missing/white canvas must fail this painted-pixel oracle");
            const QString folder=QString::fromUtf8(qgetenv("GENKO_COLOR_SCREENSHOT_DIR"));
            if (!folder.isEmpty()) {
                QVERIFY(QDir().mkpath(folder));
                QVERIFY(window.grab().save(folder+"/"+precision+"-"+operation+"-"+blendMode+"-"+stage+".png"));
            }
        };
        checkView("before");
        std::vector<std::string> beforePixels;
        for (const char* mode : {"proof","print"}) {
            render::RenderOptions options;options.mode=mode;options.skip_unported=true;
            beforePixels.push_back(render::render_page(before->page(0),72,options,before.get()).image.tobytes());
        }
        const QByteArray actionName=("act_layer_"+operation).toUtf8();
        auto* action=window.action(actionName.constData());
        QVERIFY2(action,"precision layer GUI action is required");
        bool picked=operation=="merge_visible";
        if (operation=="flatten") QTimer::singleShot(0,&window,[&] {
            auto* dialog=window.findChild<QMessageBox*>("flatten_layer_confirm");if (!dialog) return;
            const QString folder=QString::fromUtf8(qgetenv("GENKO_COLOR_SCREENSHOT_DIR"));
            if (!folder.isEmpty()) QVERIFY(dialog->grab().save(folder+"/"+precision+"-flatten-confirm.png"));
            picked=true;dialog->done(QMessageBox::Yes);
        });
        else if (operation!="merge_visible") QTimer::singleShot(0,&window,[&] {
            auto* dialog=window.findChild<QDialog*>("merge_layer_dialog");if (!dialog) return;
            auto* choices=dialog->findChild<QListWidget*>("merge_layer_choices");
            if (!choices) { dialog->reject();return; }
            for (int i=0;i<choices->count();++i)
                choices->item(i)->setSelected(operation=="merge_layers" ||
                    choices->item(i)->data(Qt::UserRole).toString().toStdString()==original.page(0).layers.back().id);
            const QString folder=QString::fromUtf8(qgetenv("GENKO_COLOR_SCREENSHOT_DIR"));
            if (!folder.isEmpty()) {
                QVERIFY(dialog->isVisible());QVERIFY(choices->isVisible());
                QVERIFY(dialog->grab().save(folder+"/"+precision+"-"+operation+"-picker.png"));
            }
            picked=true;dialog->accept();
        });
        action->trigger();
        QVERIFY2(picked,"layer picker must select target through UI");
        const auto after=session->snapshot();
        checkView("merged");
        for (std::size_t i=0;i<beforePixels.size();++i) {
            render::RenderOptions options;options.mode=i==0 ? "proof" : "print";options.skip_unported=true;
            QCOMPARE(render::render_page(after->page(0),72,options,after.get()).image.tobytes(),beforePixels[i]);
        }
        QCOMPARE(after->page(0).layers.size(),operation=="merge_visible" ? 3U : 1U);
        QCOMPARE(after->page(1).index.value(),11.0);
        QCOMPARE(after->pages[1],before->pages[1]);
        const auto merged=operation=="merge_visible" ? after->page(0).layers.back().color_raster : after->page(0).layers.front().color_raster;
        QVERIFY(merged);
        if (operation=="flatten") QVERIFY(after->page(0).layers.front().lock_alpha);
        const auto rgba=core::ColorRasterView(*merged).pixel(0);
        QCOMPARE(core::ColorRasterView(*merged).metadata("")["precision"],Json(precision.toStdString()));
        QVERIFY(std::abs(rgba[0]-expectedRed)<=(blendMode=="normal"?1e-7:precision=="u16"?.500001/65535:3e-7));
        QVERIFY(session->can_undo());session->save_now();QVERIFY(session->wait_saved(std::chrono::milliseconds(10000)));
        auto saved=gui_test::read_book(path);
        QCOMPARE(*(operation=="merge_visible" ? saved.page(0).layers.back().color_raster : saved.page(0).layers.front().color_raster),*merged);
        session->undo();session->save_now();QVERIFY(session->wait_saved(std::chrono::milliseconds(10000)));
        checkView("undo");
        saved=gui_test::read_book(path);QCOMPARE(saved.page(0).layers.size(),2U);
        QCOMPARE(*saved.page(0).layers.back().color_raster,*original.page(0).layers.back().color_raster);
        session->redo();session->save_now();QVERIFY(session->wait_saved(std::chrono::milliseconds(10000)));
        checkView("redo");
        saved=gui_test::read_book(path);
        QCOMPARE(*(operation=="merge_visible" ? saved.page(0).layers.back().color_raster : saved.page(0).layers.front().color_raster),*merged);
        window.hide();
    }
    void eraserAndPenOnPrecisePixels_data() {
        QTest::addColumn<QString>("precision");
        QTest::newRow("u16")<<QString("u16");QTest::newRow("f32")<<QString("f32");
    }
    // The eraser and the pen, drawn on the canvas over a precise colour layer, go into its own samples (no 8-bit
    // picture, no lines kept on it); shown as the page renders, saved, undone and redone.
    void eraserAndPenOnPrecisePixels() {
        QFETCH(QString,precision);
        (void)gui_test::config_folder();QTemporaryDir tmp;
        auto seed=core::new_episode("高精度消しゴム",core::Num(1),1,core::PageSpec::custom(25.4,25.4,20,20,1,2,2,2,2,200,"color"));
        seed.edit_page(0).numero=false;seed.edit_page(0).layers.clear();seed.edit_page(0).frames.clear();
        Json pixels=Json::array();
        for(int i=0;i<200*200;++i)for(const double v:{.2,.6,.9,1.})pixels.push_back(precision=="u16"?Json(std::lround(v*65535)):Json(v));
        const auto doc=core::CommandBus().apply(seed,Json::array({Json{{"op","put_color_raster"},{"page",1},{"width",200},{"height",200},
            {"precision",precision.toStdString()},{"pixels",pixels}}}),core::Actor("human:tester")).doc;
        const auto id=doc.page(0).layers.back().id;const auto original=doc.page(0).layers.back().color_raster;
        const auto path=gui_test::path_of(tmp.path()+"/book");gui_test::write_book(path,doc);
        auto session=app::Session::open(path,gui_test::quick(gui_test::path_of(tmp.path()+"/recovery")));
        app::MainWindow window(session);window.resize(1000,720);window.show();QVERIFY(QTest::qWaitForWindowExposed(&window));
        window.set_target_layer(id);
        const auto precise=[&]{return core::ColorRasterView(*window.book().page(0).layers.back().color_raster);};
        window.choose_tool("eraser");
        app::inject::mouse_stroke(window.canvas(),{QPointF(3,12.7),QPointF(12,12.7),QPointF(22,12.7)});
        QVERIFY(window.book().page(0).layers.back().color_raster!=original);
        QCOMPARE(precise().metadata("")["precision"],Json(precision.toStdString()));
        QCOMPARE(precise().pixel(std::size_t(100)*200+100)[3],0.0);      // (under the eraser)
        QCOMPARE(precise().pixel(std::size_t(20)*200+100),core::ColorRasterView(*original).pixel(std::size_t(20)*200+100));
        window.pen()=app::PenSettings::for_kind("mili");window.pen().width_mm=1.0;window.pen_changed();window.choose_tool("pen");
        app::inject::mouse_stroke(window.canvas(),{QPointF(12.7,3),QPointF(12.7,8),QPointF(12.7,20)});
        QCOMPARE(window.book().page(0).layers.back().stroke_count(),std::size_t(0));  // (drawn into the pixels)
        QCOMPARE(precise().pixel(std::size_t(50)*200+100)[3],1.0);
        QVERIFY(precise().pixel(std::size_t(50)*200+100)[0]<.2);  // (the pen's dark ink over the light paint)
        QTRY_VERIFY_WITH_TIMEOUT(window.canvas()->doc()==session->snapshot(),10000);
        QVERIFY(window.canvas()->wait_rendered(10000));
        const int dpi=window.canvas()->renderer().shown_dpi();QVERIFY(dpi>0);
        render::RenderOptions options;options.mode="proof";options.skip_unported=true;
        const auto image=render::render_page(session->document().page(0),dpi,options,&session->document()).image;
        const auto bytes=image.tobytes();
        QCOMPARE(window.canvas()->renderer().compose(dpi),QImage(reinterpret_cast<const uchar*>(bytes.data()),image.width(),image.height(),
            image.width()*3,QImage::Format_RGB888).convertToFormat(QImage::Format_RGB32));
        session->save_now();QVERIFY(session->wait_saved(std::chrono::milliseconds(10000)));
        const auto drawn=*window.book().page(0).layers.back().color_raster;
        QCOMPARE(*gui_test::read_book(path).page(0).layers.back().color_raster,drawn);
        window.action("act_undo")->trigger();window.action("act_undo")->trigger();
        QCOMPARE(*window.book().page(0).layers.back().color_raster,*original);
        window.action("act_redo")->trigger();window.action("act_redo")->trigger();
        QCOMPARE(*window.book().page(0).layers.back().color_raster,drawn);
        session->save_now();QVERIFY(session->wait_saved(std::chrono::milliseconds(10000)));
        QCOMPARE(*gui_test::read_book(path).page(0).layers.back().color_raster,drawn);
        const QString folder=QString::fromUtf8(qgetenv("GENKO_COLOR_SCREENSHOT_DIR"));
        if (!folder.isEmpty()) {QVERIFY(QDir().mkpath(folder));QVERIFY(window.grab().save(folder+"/"+precision+"-eraser-pen.png"));}
        window.hide();
    }
    // 色校正: the canvas shows the page as render/colour::proof makes it print (the plain conversion: no profile
    // chosen on this computer), and as it is again once switched off.
    void cmykProofShowsThePageAsItPrints() {
        (void)gui_test::config_folder();QTemporaryDir tmp;
        // (PC1b: the working CMYK is coated-cmyk unless chosen — the user's decision; Python's 「CMYK で見る」 is the choice
        // 書き出しの ICC・Genko の変換, chosen here so that what this test sees is still Python's conversion)
        app::settings()->setValue(QStringLiteral("color/workingCmyk"),QStringLiteral("export"));
        auto doc=core::new_episode("色校正",core::Num(1),1,core::PageSpec::custom(20,20,16,16,1,2,2,2,2,72,"color"));
        doc.edit_page(0).numero=false;doc.edit_page(0).layers.clear();doc.edit_page(0).frames.clear();
        doc=core::CommandBus().apply(doc,Json::array({Json{{"op","put_color_raster"},{"page",1},{"width",2},{"height",1},{"precision","u16"},
            {"pixels",Json::array({0,65535,0,65535,65535,0,40000,65535})}}}),core::Actor("human:tester")).doc;
        const auto path=gui_test::path_of(tmp.path()+"/book");gui_test::write_book(path,doc);
        auto session=app::Session::open(path,gui_test::quick(gui_test::path_of(tmp.path()+"/recovery")));
        app::MainWindow window(session);window.resize(900,700);window.show();QVERIFY(QTest::qWaitForWindowExposed(&window));
        const auto shown=[&](bool proof) {
            QVERIFY(window.canvas()->wait_rendered(10000));
            const int dpi=window.canvas()->renderer().shown_dpi();QVERIFY(dpi>0);
            render::RenderOptions options;options.mode="proof";options.skip_unported=true;
            auto image=render::render_page(session->document().page(0),dpi,options,&session->document()).image;
            if (proof) image=render::colour::proof(image);
            const auto bytes=image.convert("RGB").tobytes();
            QCOMPARE(window.canvas()->renderer().compose(dpi),QImage(reinterpret_cast<const uchar*>(bytes.data()),image.width(),image.height(),
                image.width()*3,QImage::Format_RGB888).convertToFormat(QImage::Format_RGB32));
        };
        shown(false);
        window.action("act_cmyk_proof")->trigger();
        QVERIFY(window.canvas()->renderer().cmyk_proof());
        shown(true);
        const QString folder=QString::fromUtf8(qgetenv("GENKO_COLOR_SCREENSHOT_DIR"));
        if (!folder.isEmpty()) {QVERIFY(QDir().mkpath(folder));QVERIFY(window.grab().save(folder+"/cmyk-proof.png"));}
        window.action("act_cmyk_proof")->trigger();
        shown(false);
        // through the profile chosen for export (at a path of any name); its file gone: the plain conversion again
        const auto icc=std::filesystem::path(tmp.path().toStdString())/core::path_from_utf8("プロファイル.icc");
        std::filesystem::copy_file(std::filesystem::path(GENKO_TEST_DATA)/"colour"/"photocraft-coated-cmyk.icc",icc);
        app::settings()->setValue(QStringLiteral("color/icc"),QString::fromStdString(core::path_to_utf8(icc)));
        window.action("act_cmyk_proof")->trigger();
        const auto through=[&](const std::optional<std::filesystem::path>& profile) {
            QVERIFY(window.canvas()->wait_rendered(10000));
            const int dpi=window.canvas()->renderer().shown_dpi();QVERIFY(dpi>0);
            render::RenderOptions options;options.mode="proof";options.skip_unported=true;
            const auto image=render::colour::proof(render::render_page(session->document().page(0),dpi,options,&session->document()).image,profile);
            const auto bytes=image.convert("RGB").tobytes();
            QCOMPARE(window.canvas()->renderer().compose(dpi),QImage(reinterpret_cast<const uchar*>(bytes.data()),image.width(),image.height(),
                image.width()*3,QImage::Format_RGB888).convertToFormat(QImage::Format_RGB32));
        };
        through(icc);
        std::filesystem::remove(icc);
        window.canvas()->zoom_by(2.0);  // (tiles drawn again: the profile looked up again)
        through(std::nullopt);
        window.action("act_cmyk_proof")->trigger();
        app::settings()->remove(QStringLiteral("color/icc"));
        app::settings()->remove(QStringLiteral("color/workingCmyk"));
        window.hide();
    }
    void precisionPenConversionKeepsDiskUndo_data() {precisionPaintConversionKeepsSource_data();}
    void precisionPenConversionKeepsDiskUndo() {
        QFETCH(QString,precision);(void)gui_test::config_folder();QTemporaryDir tmp;
        auto seed=core::new_episode("高精度ペン変換",core::Num(1),2,core::PageSpec::custom(20,20,16,16,1,2,2,2,2,200,"color"));
        seed.edit_page(0).layers.clear();seed.edit_page(0).frames.clear();seed.edit_page(0).numero=false;
        seed.edit_page(1).numero=false;
        Json samples=Json::array();
        for(int y=0;y<7;++y)for(int x=0;x<7;++x) {
            if(precision=="u16")for(const auto v:{40123,1031,60001,y==3?65535:0})samples.push_back(v);
            else for(const auto v:{1.25,.125,.500001,y==3?1.:0.})samples.push_back(v);
        }
        const auto base=core::CommandBus().apply(seed,Json::array({Json{{"op","put_color_raster"},{"page",1},{"width",7},{"height",7},{"precision",precision.toStdString()},{"pixels",samples}}}),core::Actor("human:tester")).doc;
        const auto path=gui_test::path_of(tmp.path()+"/book");gui_test::write_book(path,base);
        auto session=app::Session::open(path,gui_test::quick(gui_test::path_of(tmp.path()+"/recovery")));
        app::MainWindow window(session);window.resize(1000,720);window.show();
        const auto original=session->snapshot();const auto id=original->page(0).layers.back().id;
        auto* action=window.action("act_layer_convert_pen");QVERIFY2(action,"GUI must offer editable precise pen conversion");
        bool picked=false;
        QTimer::singleShot(0,&window,[&]{
            auto* dialog=window.findChild<QDialog*>("merge_layer_dialog");if(!dialog)return;
            auto* choices=dialog->findChild<QListWidget*>("merge_layer_choices");
            for(int i=0;i<choices->count();++i)choices->item(i)->setSelected(choices->item(i)->data(Qt::UserRole).toString().toStdString()==id);
            picked=true;dialog->accept();
        });
        action->trigger();QVERIFY(picked);QVERIFY2(window.last_error().isEmpty(),qPrintable(window.last_error()));
        const auto pen=session->snapshot();const auto& layer=pen->page(0).layers.back();
        QCOMPARE(layer.kind,core::LayerKind::Strokes);QVERIFY(core::has_color_strokes(layer));QVERIFY(!layer.color_raster);
        QCOMPARE(pen->pages[1],original->pages[1]);const auto strokes=core::strokes_blob(*layer.strokes);
        const auto view=[&](const QString& stage){
            QTRY_VERIFY_WITH_TIMEOUT(window.canvas()->doc()==session->snapshot(),10000);QVERIFY(window.canvas()->wait_rendered(10000));
            const auto dpi=window.canvas()->renderer().shown_dpi();QVERIFY(dpi>0);
            const auto doc=session->snapshot();const auto image=render::render_page(doc->page(0),dpi,render::proof_options(),doc.get()).image;
            const auto bytes=image.tobytes();const QImage expected=QImage(reinterpret_cast<const uchar*>(bytes.data()),image.width(),image.height(),image.width()*3,QImage::Format_RGB888).convertToFormat(QImage::Format_RGB32);
            QCOMPARE(window.canvas()->renderer().compose(dpi),expected);
            const auto folder=QString::fromUtf8(qgetenv("GENKO_COLOR_SCREENSHOT_DIR"));
            if(!folder.isEmpty()){QVERIFY(QDir().mkpath(folder));QVERIFY(window.grab().save(folder+"/"+precision+"-pen-"+stage+".png"));}
        };
        view("converted");session->save_now();QVERIFY(session->wait_saved(std::chrono::milliseconds(10000)));
        auto saved=gui_test::read_book(path);QCOMPARE(core::strokes_blob(*saved.page(0).layers.back().strokes),strokes);
        for(const auto* mode:{"proof","print"}) {render::RenderOptions options;options.mode=mode;
            QCOMPARE(render::render_page(saved.page(0),200,options,&saved).image.tobytes(),render::render_page(pen->page(0),200,options,pen.get()).image.tobytes());}
        session->undo();session->save_now();QVERIFY(session->wait_saved(std::chrono::milliseconds(10000)));view("undo");
        saved=gui_test::read_book(path);QVERIFY(saved.page(0).layers.back().color_raster);QCOMPARE(*saved.page(0).layers.back().color_raster,*original->page(0).layers.back().color_raster);
        session->redo();session->save_now();QVERIFY(session->wait_saved(std::chrono::milliseconds(10000)));view("redo");
        saved=gui_test::read_book(path);QCOMPARE(core::strokes_blob(*saved.page(0).layers.back().strokes),strokes);
        // Sending the same GUI conversion again must not retrace precise strokes via RGB8.
        picked=false;QTimer::singleShot(0,&window,[&]{
            auto* dialog=window.findChild<QDialog*>("merge_layer_dialog");if(!dialog)return;
            auto* choices=dialog->findChild<QListWidget*>("merge_layer_choices");
            for(int i=0;i<choices->count();++i)choices->item(i)->setSelected(choices->item(i)->data(Qt::UserRole).toString().toStdString()==id);
            picked=true;dialog->accept();
        });
        action->trigger();QVERIFY(picked);QVERIFY2(window.last_error().isEmpty(),qPrintable(window.last_error()));
        QCOMPARE(core::strokes_blob(*session->document().page(0).layers.back().strokes),strokes);
        session->save_now();QVERIFY(session->wait_saved(std::chrono::milliseconds(10000)));view("reconverted");
        saved=gui_test::read_book(path);QCOMPARE(core::strokes_blob(*saved.page(0).layers.back().strokes),strokes);window.hide();
    }
    void precisionPaintConversionKeepsSource_data() {
        QTest::addColumn<QString>("precision");
        QTest::newRow("u16") << QString("u16");QTest::newRow("f32") << QString("f32");
    }
    void precisionPaintConversionKeepsSource() {
        QFETCH(QString,precision);(void)gui_test::config_folder();
        auto doc=core::new_episode("ペイント変換",core::Num(1),2,
            core::PageSpec::custom(20,20,16,16,1,2,2,2,2,72,"color"));
        const Json values=precision=="u16" ? Json::array({1000,1001,1002,65535}) : Json::array({-0.125,1.0000001192092896,2.0,1.0});
        const Json put={{"op","put_color_raster"},{"page",1},{"width",1},{"height",1},
                        {"precision",precision.toStdString()},{"pixels",values}};
        auto base=core::CommandBus().apply(doc,Json::array({put}),core::Actor("human:tester")).doc;
        const auto original=base.page(0).layers.back().color_raster;
        app::Session::Options options;options.autosave=false;options.actor="human:tester";
        auto session=std::make_shared<app::Session>(base,std::nullopt,options);
        app::MainWindow window(session);bool picked=false;
        QTimer::singleShot(0,&window,[&] {
            auto* dialog=window.findChild<QDialog*>("merge_layer_dialog");if (!dialog) return;
            auto* choices=dialog->findChild<QListWidget*>("merge_layer_choices");
            for (int i=0;i<choices->count();++i)
                choices->item(i)->setSelected(choices->item(i)->data(Qt::UserRole).toString().toStdString()==base.page(0).layers.back().id);
            picked=true;dialog->accept();
        });
        auto* action=window.action("act_layer_convert_paint");QVERIFY(action);action->trigger();
        QVERIFY2(picked,"layer picker must select target through UI");
        QVERIFY2(window.last_error().isEmpty(),"paint conversion must address the selected layer");
        QCOMPARE(session->document().page(0).layers.back().color_raster,original);
        QCOMPARE(session->document().page(0).layers.back().kind,core::LayerKind::Raster);
    }
    void modalLayerMergeKeepsOriginalTarget_data() {
        QTest::addColumn<bool>("switchBook");QTest::addColumn<QString>("operation");
        for(const QString& operation:{QString("merge_layers"),QString("flatten")}) {
            QTest::newRow((operation+"-switch-book").toUtf8().constData())<<true<<operation;
            QTest::newRow((operation+"-concurrent-edit").toUtf8().constData())<<false<<operation;
        }
    }
    void modalLayerMergeKeepsOriginalTarget() {
        QFETCH(bool,switchBook);QFETCH(QString,operation);(void)gui_test::config_folder();
        const auto spec=core::PageSpec::custom(20,20,16,16,1,2,2,2,2,72,"color");
        auto seed=core::new_episode("元原稿",core::Num(1),2,spec);seed.edit_page(0).layers.clear();
        const Json put={{"op","put_color_raster"},{"page",1},{"width",1},{"height",1},
                        {"precision","u16"},{"pixels",Json::array({1000,1001,1002,65535})}};
        const auto base=core::CommandBus().apply(seed,Json::array({put,put}),core::Actor("human:tester")).doc;
        auto otherDoc=core::new_episode("別原稿",core::Num(1),2,spec);
        otherDoc.edit_page(0)=base.page(0);otherDoc.features=base.features; // Colliding IDs must not retarget an operation.
        app::Session::Options options;options.autosave=false;options.actor="human:tester";
        auto origin=std::make_shared<app::Session>(base,std::nullopt,options);
        auto other=std::make_shared<app::Session>(otherDoc,std::nullopt,options);
        auto expectedOrigin=origin->snapshot();const auto expectedOther=other->snapshot();
        app::MainWindow window(origin);bool dialogSeen=false;
        QTimer::singleShot(0,&window,[&] {
            auto* dialog=window.findChild<QDialog*>(operation=="flatten"?"flatten_layer_confirm":"merge_layer_dialog");if (!dialog) return;
            dialogSeen=true;
            if(operation!="flatten") {
                auto* list=dialog->findChild<QListWidget*>("merge_layer_choices");QVERIFY(list);
                for (int i=0;i<list->count();++i) list->item(i)->setSelected(true);
            }
            if (switchBook) window.add_document(other);
            else {
                (void)origin->apply(Json::array({Json{{"op","set_note"},{"page",1},{"note","同時編集"}}}));
                expectedOrigin=origin->snapshot();
            }
            if(operation=="flatten") dialog->done(QMessageBox::Yes);else dialog->accept();
        });
        window.action(QStringLiteral("act_layer_")+operation)->trigger();QVERIFY(dialogSeen);
        QVERIFY2(origin->snapshot()==expectedOrigin && other->snapshot()==expectedOther,
                 "modal layer merge must keep original target");
        if (switchBook) QVERIFY(window.session_ptr()==other);
        else QCOMPARE(origin->document().page(0).note,std::string("同時編集"));
    }
    void flattenCancelledLeavesBookAndHistory() {
        (void)gui_test::config_folder();auto doc=gui_test::new_doc(2);
        app::Session::Options options;options.autosave=false;auto session=std::make_shared<app::Session>(doc,std::nullopt,options);
        const auto before=session->snapshot();app::MainWindow window(session);bool shown=false;
        QTimer::singleShot(0,&window,[&] {
            auto* dialog=window.findChild<QMessageBox*>("flatten_layer_confirm");if(!dialog)return;
            QVERIFY(dialog->defaultButton()==dialog->button(QMessageBox::No));shown=true;
            QTest::mouseClick(dialog->button(QMessageBox::No),Qt::LeftButton);
        });
        window.action("act_layer_flatten")->trigger();QVERIFY(shown);
        QCOMPARE(session->snapshot(),before);QVERIFY(!session->can_undo());
    }
    void exposureTargetsStoredPageNumber_data() {
        QTest::addColumn<int>("first"); QTest::addColumn<int>("second");
        QTest::addColumn<int>("selected"); QTest::addColumn<bool>("editing");
        for (const auto& numbers : {std::pair{2,1},std::pair{10,11}})
            for (int page=0;page<2;++page) for (bool edit : {false,true}) {
                const auto name=QStringLiteral("%1-%2-page%3-%4").arg(numbers.first).arg(numbers.second).arg(page).arg(edit ? "edit" : "add");
                QTest::newRow(name.toUtf8().constData()) << numbers.first << numbers.second << page << edit;
            }
    }
    void colorSourceChangesRefreshCaches_data() {
        QTest::addColumn<QString>("precision");
        QTest::addColumn<bool>("thumbnail");
        for (const QString& precision : {QString("u16"),QString("f32")}) {
            QTest::newRow((precision+"-canvas").toUtf8().constData()) << precision << false;
            QTest::newRow((precision+"-thumbnail").toUtf8().constData()) << precision << true;
        }
    }
    void colorSourceChangesRefreshCaches() {
        QFETCH(QString,precision);QFETCH(bool,thumbnail);
        QTemporaryDir tmp;QVERIFY(tmp.isValid());
        auto seed=core::new_episode("カラー更新",core::Num(1),2,
            core::PageSpec::custom(20,20,16,16,1,2,2,2,2,72,"color"));
        seed.edit_page(0).numero=false;
        const Json red=precision=="u16" ? Json::array({65535,0,0,65535}) : Json::array({1.0,0.0,0.0,1.0});
        const Json blue=precision=="u16" ? Json::array({0,0,65535,65535}) : Json::array({0.0,0.0,1.0,1.0});
        Json op={{"op","put_color_raster"},{"page",1},{"width",1},{"height",1},
                 {"precision",precision.toStdString()},{"pixels",red}};
        auto before=std::make_shared<core::Document>(core::CommandBus().apply(seed,Json::array({op}),core::Actor("human:tester")).doc);
        auto after=std::make_shared<core::Document>(*before);
        const auto originalBytes=before->page(0).layers.back().color_raster;
        op["pixels"]=blue;
        after->edit_page(0).layers.back().color_raster=std::make_shared<const std::string>(core::encode_color_raster(op));
        core::validate_color_document(*before);core::validate_color_document(*after);
        QCOMPARE(before->page(0).layers.back().id,after->page(0).layers.back().id);
        QCOMPARE(before->pages[1],after->pages[1]);
        const auto reference=[](const core::Document& doc) {
            render::RenderOptions options;options.mode="proof";options.skip_unported=true;
            const auto image=render::render_page(doc.page(0),72,options,&doc).image;
            const auto bytes=image.tobytes();
            return QImage(reinterpret_cast<const uchar*>(bytes.data()),image.width(),image.height(),image.width()*3,QImage::Format_RGB888).convertToFormat(QImage::Format_RGB32);
        };
        const QImage original=reference(*before),expected=reference(*after);
        QVERIFY(original!=expected);
        if (thumbnail) {
            const auto oldKey=app::ThumbCache::key(*before,before->page(0),60,"proof");
            const auto newKey=app::ThumbCache::key(*after,after->page(0),60,"proof");
            QVERIFY(oldKey!=newKey);
            QCOMPARE(app::ThumbCache::key(*before,before->page(1),60,"proof"),app::ThumbCache::key(*after,after->page(1),60,"proof"));
            const auto folder=gui_test::path_of(tmp.path()+"/thumbs");
            app::ThumbCache(folder).put(oldKey,original);
            const app::ThumbCache reopened(folder);
            QVERIFY(reopened.get(oldKey).has_value());
            QCOMPARE(*reopened.get(oldKey),original);
            QVERIFY(!reopened.get(newKey).has_value());
            reopened.put(newKey,expected);
            QCOMPARE(*app::ThumbCache(folder).get(newKey),expected);
            QCOMPARE(*reopened.get(oldKey),original);
        } else {
            app::PageRenderer renderer;renderer.show(before,0);renderer.want(72,72,QRectF(0,0,20,20));
            QTRY_VERIFY_WITH_TIMEOUT(renderer.settled(),10000);
            QCOMPARE(renderer.compose(72),original);
            renderer.show(after,0);
            QTRY_VERIFY_WITH_TIMEOUT(renderer.settled(),10000);
            QCOMPARE(renderer.compose(72),expected);
            renderer.show(before,0);
            QTRY_VERIFY_WITH_TIMEOUT(renderer.settled(),10000);
            QCOMPARE(renderer.compose(72),original);
        }
        QCOMPARE(before->page(0).layers.back().color_raster,originalBytes);
        op["pixels"]=red;
        QCOMPARE(*originalBytes,core::encode_color_raster(op));
    }
    void exposureTargetsStoredPageNumber() {
        QFETCH(int,first); QFETCH(int,second); QFETCH(int,selected); QFETCH(bool,editing);
        (void)gui_test::config_folder(); QTemporaryDir tmp;
        auto doc=core::new_episode("ページ番号対照",core::Num(1),2,core::PageSpec::custom(20,20,16,16,1,2,2,2,2,72,"color"));
        doc.edit_page(0).index=core::Num(first); doc.edit_page(1).index=core::Num(second);
        for (int i=0;i<2;++i) {
            core::Layer adjustment; adjustment.id="exposure-page-"+std::to_string(i);
            adjustment.kind=core::LayerKind::Adjust;
            adjustment.adjust=Json{{"kind","exposure"},{"exposure",0.0}};
            doc.edit_page(i).layers.push_back(adjustment);
        }
        gui_test::write_book(gui_test::path_of(tmp.path()+"/book"),doc);
        const auto opened=app::Session::open(gui_test::path_of(tmp.path()+"/book"),gui_test::quick(gui_test::path_of(tmp.path()+"/recovery")));
        QVERIFY(opened->read_only_reason().empty());
        app::MainWindow window(opened); window.go_to_page(selected ? second : first);
        QCOMPARE(window.current_page()->index.value(),double(selected ? second : first));
        if (editing) window.set_target_layer("exposure-page-"+std::to_string(selected));
        const auto before=opened->snapshot();
        QTimer::singleShot(0,&window,[&] {
            auto* dialog=window.findChild<QDialog*>("exposure_dialog"); if (!dialog) return;
            dialog->findChild<QDoubleSpinBox*>("exposure_ev")->setValue(2.0); dialog->accept();
        });
        window.action("act_exposure")->trigger();
        const auto& after=opened->document();
        QCOMPARE(after.page(selected).index.value(),double(selected ? second : first));
        QCOMPARE(after.pages[1-selected],before->pages[1-selected]);
        const auto& layers=after.page(selected).layers;
        QCOMPARE(layers.size(),before->page(selected).layers.size()+(editing ? 0U : 1U));
        QCOMPARE(layers.back().adjust->at("exposure").get<double>(),2.0);
        opened->save_now();
        QVERIFY(opened->wait_saved(std::chrono::milliseconds(10000)));
        const auto reloaded=gui_test::read_book(gui_test::path_of(tmp.path()+"/book"));
        QCOMPARE(reloaded.page(selected).layers.back().adjust->at("exposure").get<double>(),2.0);
        QCOMPARE(reloaded.page(1-selected).layers.back().adjust->at("exposure").get<double>(),0.0);
    }
    void malformedExistingExposureDoesNotEscapeAction() {
        (void)gui_test::config_folder();
        auto doc=core::new_episode("不正な露光量",core::Num(1),2,core::PageSpec::b5_doujin());
        doc.edit_page(0).numero=false;
        for (auto& layer : doc.edit_page(0).layers) if (layer.role==core::LayerRole::Ink) {
            layer.kind=core::LayerKind::Adjust;
            layer.adjust=Json{{"kind","exposure"},{"gamma","nan"}};
        }
        app::Session::Options options;
        options.autosave=false;
        options.actor="human:tester";
        auto session=std::make_shared<app::Session>(doc,std::nullopt,options);
        app::MainWindow window(session);
        const auto before=session->snapshot();
        bool escaped=false;
        try { window.action("act_exposure")->trigger(); } catch(const std::exception&) { escaped=true; }
        QVERIFY(!escaped);
        QCOMPARE(session->snapshot(),before);
        QVERIFY(!session->can_undo());
    }
    void exposureDialogEditsActualColorBook() {
        (void)gui_test::config_folder();
        auto doc = core::new_episode("高精度カラー試験", core::Num(1), 2,
            core::PageSpec::custom(20, 20, 16, 16, 1, 2, 2, 2, 2, 72, "color"));
        doc.edit_page(0).numero = false;
        const Json put = {{"op", "put_color_raster"}, {"page", 1}, {"width", 1}, {"height", 1},
                         {"precision", "u16"}, {"pixels", Json::array({1, 2, 3, 65535})}};
        doc = core::CommandBus().apply(doc, Json::array({put}), core::Actor("human:tester")).doc;
        const auto raw = doc.page(0).layers.back().color_raster;
        app::Session::Options options;
        options.autosave = false;
        options.actor = "human:tester";
        auto session = std::make_shared<app::Session>(doc, std::nullopt, options);
        app::MainWindow window(session);
        window.resize(900, 700);
        window.show();
        QAction* action = window.action("act_exposure");
        QVERIFY2(action, "露光量のメニューが必要");
        bool dialogSeen = false;
        QTimer::singleShot(0, [&] {
            auto* dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget());
            if (!dialog) return;
            auto* ev = dialog->findChild<QDoubleSpinBox*>("exposure_ev");
            if (!ev) { dialog->reject(); return; }
            dialogSeen = true;
            ev->setValue(10);
            const QString screenshots=QString::fromUtf8(qgetenv("GENKO_COLOR_SCREENSHOT_DIR"));
            if (!screenshots.isEmpty()) {
                QVERIFY(QDir().mkpath(screenshots));
                QVERIFY(dialog->grab().save(screenshots+QStringLiteral("/exposure-dialog.png")));
            }
            dialog->accept();
        });
        action->trigger();
        QVERIFY(dialogSeen);
        const auto& layers = session->document().page(0).layers;
        QCOMPARE(layers.back().kind, core::LayerKind::Adjust);
        QCOMPARE(layers.back().adjust->at("kind"), Json("exposure"));
        QCOMPARE(layers[layers.size()-2].color_raster, raw);
        const auto image = render::render_page(session->document().page(0), 72).image;
        QCOMPARE(image.getpixel(image.width()/2, image.height()/2), (std::vector<double>{4,8,12}));
        const QString screenshots=QString::fromUtf8(qgetenv("GENKO_COLOR_SCREENSHOT_DIR"));
        if (!screenshots.isEmpty()) {
            QCoreApplication::processEvents();
            QVERIFY(window.grab().save(screenshots+QStringLiteral("/color-window.png")));
        }
        session->undo();
        QCOMPARE(session->document().page(0).layers.back().color_raster, raw);
        session->redo();
        QCOMPARE(session->document().page(0).layers.back().adjust->at("exposure"), Json(10.0));
        window.hide();
    }

    // --- PC1b (SPEC COLOR-03): 色の校正・校正の設定・色域外警告・カラー設定・方針 ------------------------------------------

    // 表示 → 校正の設定: each kind shows the page as render/proof makes it (the working CMYK: coated-cmyk unless another is
    // chosen in カラー設定); choosing one turns 色の校正 on (Ctrl+Alt+Y), its mark exclusive; カスタム… a profile of its own;
    // the proof is the book's in this window (another book in front has its own).
    void proofSetupKindsShowThePage() {
        (void)gui_test::config_folder();QTemporaryDir tmp;
        for (const char* key : {"color/workingCmyk","color/icc"}) app::settings()->remove(QString::fromLatin1(key));
        const auto path=gui_test::path_of(tmp.path()+"/book");gui_test::write_book(path,coloured_book("校正の設定"));
        auto session=app::Session::open(path,gui_test::quick(gui_test::path_of(tmp.path()+"/recovery")));
        app::MainWindow window(session);window.resize(900,700);window.show();QVERIFY(QTest::qWaitForWindowExposed(&window));
        QCOMPARE(window.action("act_cmyk_proof")->text(),QStringLiteral("色の校正"));
        QCOMPARE(window.action("act_cmyk_proof")->shortcuts(),QList<QKeySequence>{QKeySequence(QStringLiteral("Ctrl+Alt+Y"))});
        QCOMPARE(window.action("act_gamut_warning")->shortcuts(),QList<QKeySequence>{QKeySequence(QStringLiteral("Ctrl+Shift+Y"))});
        QCOMPARE(window.action("act_color_settings")->shortcuts(),QList<QKeySequence>{QKeySequence(QStringLiteral("Ctrl+Shift+K"))});
        QStringList kinds;
        for (QAction* top : window.menuBar()->actions()) {
            if (top->menu()==nullptr || top->menu()->title()!=QStringLiteral("表示")) continue;
            for (QAction* a : top->menu()->actions())
                if (a->menu()!=nullptr && a->menu()->title()==QStringLiteral("校正の設定"))
                    for (QAction* k : a->menu()->actions()) if (!k->isSeparator()) kinds<<k->property("genko_attribute").toString();
        }
        QCOMPARE(kinds,(QStringList{"act_proof_custom","act_proof_working_cmyk","act_proof_export","act_proof_cyan","act_proof_magenta",
            "act_proof_yellow","act_proof_black","act_proof_cmy","act_proof_srgb","act_proof_monitor","act_proof_legacy_mac","act_proof_protan","act_proof_deutan"}));
        const auto shown=[&](const std::optional<render::proof::Setup>& setup) {
            QVERIFY(window.canvas()->wait_rendered(10000));
            const int dpi=window.canvas()->renderer().shown_dpi();QVERIFY(dpi>0);
            render::RenderOptions options;options.mode="proof";options.skip_unported=true;
            auto image=render::render_page(session->document().page(0),dpi,options,&session->document()).image;
            if (setup) image=render::proof::apply(image,*setup);
            const auto bytes=image.convert("RGB").tobytes();
            QCOMPARE(window.canvas()->renderer().compose(dpi),QImage(reinterpret_cast<const uchar*>(bytes.data()),image.width(),image.height(),
                image.width()*3,QImage::Format_RGB888).convertToFormat(QImage::Format_RGB32));
        };
        shown(std::nullopt);
        const core::Bytes coated=core::builtin_profile("coated-cmyk")->bytes;
        render::proof::Setup setup;
        setup.kind=render::proof::Kind::Protan;
        setup.icc=coated;
        window.action("act_proof_protan")->trigger();
        QVERIFY(window.canvas()->renderer().cmyk_proof());QVERIFY(window.action("act_cmyk_proof")->isChecked());
        QVERIFY(window.action("act_proof_protan")->isChecked());QVERIFY(!window.action("act_proof_working_cmyk")->isChecked());
        shown(setup);
        window.action("act_proof_working_cmyk")->trigger();
        setup=render::proof::Setup{};setup.icc=coated;
        QVERIFY(window.action("act_proof_working_cmyk")->isChecked() && !window.action("act_proof_protan")->isChecked());
        shown(setup);
        const QString folder=QString::fromUtf8(qgetenv("GENKO_COLOR_SCREENSHOT_DIR"));
        if (!folder.isEmpty()) {QVERIFY(QDir().mkpath(folder));QVERIFY(window.grab().save(folder+"/proof-working-cmyk.png"));}
        window.action("act_proof_black")->trigger();
        setup.kind=render::proof::Kind::Plate;setup.plate=3;
        shown(setup);
        window.action("act_proof_export")->trigger();  // (書き出しの CMYK: Python's 「CMYK で見る」, the plain conversion here)
        shown(render::proof::Setup{});
        window.action("act_proof_legacy_mac")->trigger();  // (the kind alone changes: the profile as it was)
        setup=render::proof::Setup{};setup.kind=render::proof::Kind::LegacyMac;
        shown(setup);
        // カスタム…: a grey profile of its own
        gui_test::Answers answers;
        answers.responder->exec=[](QDialog* d) {
            if (d->objectName()!=QStringLiteral("proof_custom_dialog")) return 0;
            auto* profile=d->findChild<QComboBox*>(QStringLiteral("proof_profile"));
            profile->setCurrentIndex(profile->findData(QStringLiteral("sgray")));
            return static_cast<int>(QDialog::Accepted);
        };
        window.action("act_proof_custom")->trigger();
        QVERIFY(window.action("act_proof_custom")->isChecked());
        setup=render::proof::Setup{};setup.icc=core::builtin_profile("sgray")->bytes;
        shown(setup);
        // the book's own: another book in front has none; back, it is there again
        const auto other=gui_test::path_of(tmp.path()+"/other");gui_test::write_book(other,coloured_book("別の本"));
        window.add_document(app::Session::open(other,gui_test::quick(gui_test::path_of(tmp.path()+"/recovery2"))));
        QCOMPARE(window.current_document(),1);
        QVERIFY(!window.canvas()->renderer().cmyk_proof());QVERIFY(!window.action("act_cmyk_proof")->isChecked());
        window.switch_document(0);
        QVERIFY(window.canvas()->renderer().cmyk_proof());QVERIFY(window.action("act_proof_custom")->isChecked());
        // 色の校正 off: the page as it is
        window.action("act_cmyk_proof")->trigger();
        QVERIFY(!window.canvas()->renderer().cmyk_proof());
        shown(std::nullopt);
        window.hide();
    }

    // 色の校正 as it comes (Ctrl+Alt+Y, nothing chosen in カラー設定): the working CMYK, coated-cmyk (the user's decision:
    // upstream's), its soft proof — not Python's 「CMYK で見る」, which is 書き出しの CMYK (cmykProofShowsThePageAsItPrints).
    void theProofAsItComesIsTheWorkingCoatedCmyk() {
        (void)gui_test::config_folder();QTemporaryDir tmp;
        for (const char* key : {"color/workingCmyk","color/icc"}) app::settings()->remove(QString::fromLatin1(key));
        const auto path=gui_test::path_of(tmp.path()+"/book");gui_test::write_book(path,coloured_book("既定の校正"));
        auto session=app::Session::open(path,gui_test::quick(gui_test::path_of(tmp.path()+"/recovery")));
        app::MainWindow window(session);window.resize(900,700);window.show();QVERIFY(QTest::qWaitForWindowExposed(&window));
        window.activateWindow();  // (a shortcut reaches the active window only: on a real xcb screen too)
        QVERIFY(QTest::qWaitForWindowActive(&window));
        QTest::keySequence(&window,QKeySequence(QStringLiteral("Ctrl+Alt+Y")));
        QVERIFY(window.canvas()->renderer().cmyk_proof());
        QVERIFY(window.action("act_proof_working_cmyk")->isChecked());
        QVERIFY(window.canvas()->wait_rendered(10000));
        const int dpi=window.canvas()->renderer().shown_dpi();QVERIFY(dpi>0);
        render::RenderOptions options;options.mode="proof";options.skip_unported=true;
        const auto image=render::render_page(session->document().page(0),dpi,options,&session->document()).image;
        render::proof::Setup coated;coated.icc=core::builtin_profile("coated-cmyk")->bytes;
        const auto qimage=[](const render::Image& i) {
            const auto bytes=i.convert("RGB").tobytes();
            return QImage(reinterpret_cast<const uchar*>(bytes.data()),i.width(),i.height(),i.width()*3,QImage::Format_RGB888).convertToFormat(QImage::Format_RGB32);
        };
        QCOMPARE(window.canvas()->renderer().compose(dpi),qimage(render::proof::apply(image,coated)));
        QVERIFY(window.canvas()->renderer().compose(dpi)!=qimage(render::colour::proof(image)));
        window.hide();
    }

    // The 校正の設定 mark is on the kind shown: a kind chosen after カスタム… (its profile kept for the gamut warning) is
    // marked, not カスタム….
    void theProofsMarkIsOnTheKindShown() {
        (void)gui_test::config_folder();QTemporaryDir tmp;
        const auto path=gui_test::path_of(tmp.path()+"/book");gui_test::write_book(path,coloured_book("印"));
        auto session=app::Session::open(path,gui_test::quick(gui_test::path_of(tmp.path()+"/recovery")));
        app::MainWindow window(session);window.resize(900,700);window.show();QVERIFY(QTest::qWaitForWindowExposed(&window));
        gui_test::Answers answers;
        answers.responder->exec=[](QDialog* d) {
            if (d->objectName()!=QStringLiteral("proof_custom_dialog")) return 0;
            auto* profile=d->findChild<QComboBox*>(QStringLiteral("proof_profile"));
            profile->setCurrentIndex(profile->findData(QStringLiteral("display-p3")));
            return static_cast<int>(QDialog::Accepted);
        };
        window.action("act_proof_custom")->trigger();
        QVERIFY(window.action("act_proof_custom")->isChecked());
        const auto marked=[&] {
            QStringList on;
            for (const char* name : {"act_proof_custom","act_proof_working_cmyk","act_proof_export","act_proof_black","act_proof_srgb","act_proof_monitor",
                                     "act_proof_legacy_mac","act_proof_protan","act_proof_deutan"})
                if (window.action(QString::fromLatin1(name))->isChecked()) on<<QString::fromLatin1(name);
            return on;
        };
        for (const char* kind : {"act_proof_protan","act_proof_legacy_mac","act_proof_srgb"}) {
            window.action(QString::fromLatin1(kind))->trigger();
            QCOMPARE(marked(),QStringList{QString::fromLatin1(kind)});
        }
        QCOMPARE(window.proof_view().profile,QStringLiteral("display-p3"));
        window.switch_document(0);
        QCOMPARE(marked(),QStringList{QStringLiteral("act_proof_srgb")});
        window.hide();
    }

    // カスタム…'s profile file gone: the proof is not silently another — it is turned off (the gamut warning too), and said.
    void aCustomProfileGoneTurnsTheProofOff() {
        (void)gui_test::config_folder();QTemporaryDir tmp;
        const auto path=gui_test::path_of(tmp.path()+"/book");gui_test::write_book(path,coloured_book("消えた校正"));
        auto session=app::Session::open(path,gui_test::quick(gui_test::path_of(tmp.path()+"/recovery")));
        app::MainWindow window(session);window.resize(900,700);window.show();QVERIFY(QTest::qWaitForWindowExposed(&window));
        const QString icc=tmp.path()+QStringLiteral("/校正.icc");
        QVERIFY(QFile::copy(QString::fromStdString(core::path_to_utf8(std::filesystem::path(GENKO_TEST_DATA)/"colour"/"photocraft-coated-cmyk.icc")),icc));
        gui_test::Answers answers;
        answers.responder->exec=[&](QDialog* d) {
            if (d->objectName()!=QStringLiteral("proof_custom_dialog")) return 0;
            auto* profile=d->findChild<QComboBox*>(QStringLiteral("proof_profile"));
            profile->addItem(QStringLiteral("校正.icc"),icc);
            profile->setCurrentIndex(profile->findData(icc));
            return static_cast<int>(QDialog::Accepted);
        };
        window.action("act_proof_custom")->trigger();
        QVERIFY(window.canvas()->renderer().cmyk_proof());
        QVERIFY(window.canvas()->renderer().proof_setup().icc!=nullptr);
        window.action("act_gamut_warning")->trigger();
        QVERIFY(window.canvas()->renderer().gamut().has_value());
        QVERIFY(QFile::remove(icc));
        window.action("act_cmyk_proof")->trigger();  // (off, and on again: the file looked for)
        window.action("act_cmyk_proof")->trigger();
        QVERIFY(!window.canvas()->renderer().cmyk_proof());
        QVERIFY(!window.action("act_cmyk_proof")->isChecked());
        QVERIFY(!window.canvas()->renderer().gamut().has_value());
        QVERIFY(!window.action("act_gamut_warning")->isChecked());
        QVERIFY2(window.last_notice().contains(QStringLiteral("校正.icc")),qPrintable(window.last_notice()));
        QVERIFY(window.canvas()->wait_rendered(10000));
        window.hide();
    }

    // カラー設定 and 環境設定 change every window, not only the one they were opened from.
    void theSettingsChangeEveryWindow() {
        (void)gui_test::config_folder();QTemporaryDir tmp;
        for (const char* key : {"color/workingCmyk","color/icc","view/gamut_color","view/gamut_opacity"}) app::settings()->remove(QString::fromLatin1(key));
        const auto path=gui_test::path_of(tmp.path()+"/book");gui_test::write_book(path,coloured_book("一つ目"));
        const auto other=gui_test::path_of(tmp.path()+"/other");gui_test::write_book(other,coloured_book("二つ目"));
        app::MainWindow window(app::Session::open(path,gui_test::quick(gui_test::path_of(tmp.path()+"/recovery"))));
        window.resize(900,700);window.show();QVERIFY(QTest::qWaitForWindowExposed(&window));
        app::MainWindow second(app::Session::open(other,gui_test::quick(gui_test::path_of(tmp.path()+"/recovery2"))));
        second.resize(900,700);second.show();QVERIFY(QTest::qWaitForWindowExposed(&second));
        second.action("act_proof_working_cmyk")->trigger();
        second.action("act_gamut_warning")->trigger();
        QVERIFY(second.canvas()->renderer().proof_setup().icc!=nullptr);
        gui_test::Answers answers;
        answers.responder->exec=[](QDialog* d) {
            if (auto* dialog=qobject_cast<app::color_settings::ColorSettingsDialog*>(d)) {
                dialog->working_cmyk->setCurrentIndex(dialog->working_cmyk->findData(QStringLiteral("export")));
                return static_cast<int>(QDialog::Accepted);
            }
            if (auto* prefs=qobject_cast<app::preferences::PreferencesDialog*>(d)) {
                prefs->gamut_percent->setValue(40);
                prefs->save();
                return static_cast<int>(QDialog::Accepted);
            }
            return 0;
        };
        window.action("act_color_settings")->trigger();
        QVERIFY(second.canvas()->renderer().proof_setup().icc==nullptr);  // (書き出しの CMYK: Genko's own conversion here)
        window.action("act_prefs")->trigger();
        QVERIFY(second.canvas()->renderer().gamut().has_value());
        QCOMPARE(second.canvas()->renderer().gamut()->opacity,0.4);
        for (const char* key : {"color/workingCmyk","view/gamut_opacity"}) app::settings()->remove(QString::fromLatin1(key));
        second.hide();window.hide();
    }

    // 表示 → 色域外警告 (Ctrl+Shift+Y): the colours coated-cmyk cannot print marked on screen (環境設定's colour and
    // opacity), counted in the status bar; the page's own pixels (the eyedropper's, compose) as they were.
    void gamutWarningMarksAndCounts() {
        (void)gui_test::config_folder();QTemporaryDir tmp;
        for (const char* key : {"color/workingCmyk","view/gamut_color","view/gamut_opacity"}) app::settings()->remove(QString::fromLatin1(key));
        const auto path=gui_test::path_of(tmp.path()+"/book");gui_test::write_book(path,coloured_book("色域外"));
        auto session=app::Session::open(path,gui_test::quick(gui_test::path_of(tmp.path()+"/recovery")));
        app::MainWindow window(session);window.resize(900,700);window.show();QVERIFY(QTest::qWaitForWindowExposed(&window));
        QVERIFY(!window.gamut_label()->isVisible());
        window.action("act_gamut_warning")->trigger();
        QVERIFY(window.canvas()->renderer().gamut().has_value());
        QVERIFY(window.canvas()->wait_rendered(10000));
        QTRY_VERIFY_WITH_TIMEOUT(window.gamut_label()->isVisible() && window.canvas()->renderer().gamut_count().second>0,10000);
        const auto [out,all]=window.canvas()->renderer().gamut_count();
        QVERIFY2(out>0 && out<all,qPrintable(QStringLiteral("%1 of %2").arg(out).arg(all)));
        QVERIFY(window.gamut_label()->text().startsWith(QStringLiteral("色域外 ")));
        const int dpi=window.canvas()->renderer().shown_dpi();QVERIFY(dpi>0);
        render::RenderOptions options;options.mode="proof";options.skip_unported=true;
        const auto image=render::render_page(session->document().page(0),dpi,options,&session->document()).image;
        const auto bytes=image.tobytes();
        const QImage plain=QImage(reinterpret_cast<const uchar*>(bytes.data()),image.width(),image.height(),image.width()*3,QImage::Format_RGB888)
            .convertToFormat(QImage::Format_RGB32);
        QCOMPARE(window.canvas()->renderer().compose(dpi),plain);
        const QPoint green(plain.width()/8,plain.height()/2), grey(plain.width()*7/8,plain.height()/2);
        QImage marked=window.canvas()->renderer().compose_shown(dpi);
        QCOMPARE(marked.pixelColor(green),QColor(128,128,128));
        QCOMPARE(marked.pixelColor(grey),plain.pixelColor(grey));
        const QString folder=QString::fromUtf8(qgetenv("GENKO_COLOR_SCREENSHOT_DIR"));
        if (!folder.isEmpty()) {QVERIFY(QDir().mkpath(folder));QVERIFY(window.grab().save(folder+"/gamut-warning.png"));}
        // 環境設定 → 透明部分・色域: magenta at half
        app::settings()->setValue(QStringLiteral("view/gamut_color"),QStringLiteral("#ff00ff"));
        app::settings()->setValue(QStringLiteral("view/gamut_opacity"),50);
        app::preferences::apply_all(&window);
        QVERIFY(window.canvas()->wait_rendered(10000));
        marked=window.canvas()->renderer().compose_shown(dpi);
        const QColor half=marked.pixelColor(green), was=plain.pixelColor(green);
        for (const auto& [a,b,c] : {std::tuple{half.red(),was.red(),255},std::tuple{half.green(),was.green(),0},std::tuple{half.blue(),was.blue(),255}})
            QVERIFY(std::abs(a-(b+c)/2)<=1);
        QCOMPARE(window.canvas()->renderer().compose(dpi),plain);
        window.action("act_gamut_warning")->trigger();
        QVERIFY(!window.canvas()->renderer().gamut());
        QVERIFY(window.canvas()->wait_rendered(10000));
        QVERIFY(!window.gamut_label()->isVisible());
        QCOMPARE(window.canvas()->renderer().compose_shown(dpi),plain);
        for (const char* key : {"view/gamut_color","view/gamut_opacity"}) app::settings()->remove(QString::fromLatin1(key));
        window.hide();
    }

    // A Display P3 page is checked and shown by its own values, not by the screen's sRGB they were cut to (upstream's
    // composite_profile): its P3 green is out of an sRGB proof's gamut (as sRGB it is sRGB's own green, in), and
    // インターネット標準の RGB shows its numbers (0, 1, 0) as sRGB's green.
    void aProfiledPageIsCheckedByItsOwnValues() {
        (void)gui_test::config_folder();QTemporaryDir tmp;
        for (const char* key : {"color/workingCmyk","view/gamut_color","view/gamut_opacity"}) app::settings()->remove(QString::fromLatin1(key));
        auto doc=coloured_book("P3 の色域");
        doc=core::CommandBus(render::ops_registry()).apply(doc,Json::array({Json{{"op","put_color_raster"},{"page",1},{"width",2},{"height",1},
            {"precision","u16"},{"pixels",Json::array({0,65535,0,65535,32768,32768,32768,65535})}},Json{{"op","assign_profile"},{"page",1},{"profile","display-p3"}}}),
            core::Actor("human:tester")).doc;
        QVERIFY(doc.page(0).rgb);
        const auto path=gui_test::path_of(tmp.path()+"/book");gui_test::write_book(path,doc);
        auto session=app::Session::open(path,gui_test::quick(gui_test::path_of(tmp.path()+"/recovery")));
        app::MainWindow window(session);window.resize(900,700);window.show();QVERIFY(QTest::qWaitForWindowExposed(&window));
        gui_test::Answers answers;
        answers.responder->exec=[](QDialog* d) {
            if (d->objectName()!=QStringLiteral("proof_custom_dialog")) return 0;
            auto* profile=d->findChild<QComboBox*>(QStringLiteral("proof_profile"));
            profile->setCurrentIndex(profile->findData(QStringLiteral("srgb")));
            return static_cast<int>(QDialog::Accepted);
        };
        window.action("act_proof_custom")->trigger();
        window.action("act_cmyk_proof")->trigger();  // (the proof off: the gamut warning alone)
        QVERIFY(!window.canvas()->renderer().cmyk_proof());
        window.action("act_gamut_warning")->trigger();
        QVERIFY(window.canvas()->wait_rendered(10000));
        QTRY_VERIFY_WITH_TIMEOUT(window.canvas()->renderer().gamut_count().second>0,10000);
        const int dpi=window.canvas()->renderer().shown_dpi();QVERIFY(dpi>0);
        const QImage plain=window.canvas()->renderer().compose(dpi);
        const QPoint green(plain.width()/8,plain.height()/2), grey(plain.width()*7/8,plain.height()/2);
        const QImage marked=window.canvas()->renderer().compose_shown(dpi);
        QCOMPARE(marked.pixelColor(green),QColor(128,128,128));
        QCOMPARE(marked.pixelColor(grey),plain.pixelColor(grey));
        const auto [out,all]=window.canvas()->renderer().gamut_count();
        QVERIFY2(out>0 && out<all,qPrintable(QStringLiteral("%1 of %2").arg(out).arg(all)));
        window.action("act_gamut_warning")->trigger();
        window.action("act_proof_srgb")->trigger();
        QVERIFY(window.canvas()->renderer().cmyk_proof());
        QVERIFY(window.canvas()->wait_rendered(10000));
        const QImage shown=window.canvas()->renderer().compose(dpi);
        QCOMPARE(shown.pixelColor(green),QColor(0,255,0));
        const QColor g=shown.pixelColor(grey);
        QVERIFY(std::abs(g.red()-128)<=1 && g.red()==g.green() && g.green()==g.blue());
        window.hide();
    }

    // 編集 → カラー設定…: kept on this computer (QSettings color/*), 初期設定に戻す; the book's own 文字のガンマ is an op
    // (set_text_gamma, undone as any) and the page is drawn with it as everywhere.
    void colorSettingsAreKeptAndTheBookGetsItsGamma() {
        (void)gui_test::config_folder();QTemporaryDir tmp;
        auto doc=core::new_episode("カラー設定",core::Num(1),1,core::PageSpec::custom(60,60,50,50,2,2,2,2,2,150,"color"));
        doc=core::CommandBus(render::ops_registry()).apply(doc,Json::array({Json{{"op","add_line"},{"page",1},{"text","ガンマ"},{"x_mm",10},{"y_mm",10},
            {"w_mm",40},{"h_mm",30}}}),core::Actor("human:tester")).doc;
        const auto path=gui_test::path_of(tmp.path()+"/book");gui_test::write_book(path,doc);
        auto session=app::Session::open(path,gui_test::quick(gui_test::path_of(tmp.path()+"/recovery")));
        app::MainWindow window(session);window.resize(900,700);window.show();QVERIFY(QTest::qWaitForWindowExposed(&window));
        gui_test::Answers answers;
        QStringList seen;  // (what the dialog showed when it opened)
        answers.responder->exec=[&seen](QDialog* d) {
            auto* dialog=qobject_cast<app::color_settings::ColorSettingsDialog*>(d);
            if (dialog==nullptr) return 0;
            seen<<dialog->working_cmyk->currentData().toString()<<(dialog->gamma_on->isChecked() ? QStringLiteral("gamma") : QStringLiteral("none"));
            dialog->working_cmyk->setCurrentIndex(dialog->working_cmyk->findData(QStringLiteral("export")));
            dialog->policy_rgb->setCurrentIndex(dialog->policy_rgb->findData(QStringLiteral("convert")));
            dialog->ask_mismatch->setChecked(false);
            dialog->gamma_on->setChecked(true);
            dialog->gamma->setValue(1.6);
            return static_cast<int>(QDialog::Accepted);
        };
        window.action("act_color_settings")->trigger();
        QCOMPARE(seen,(QStringList{QStringLiteral("coated-cmyk"),QStringLiteral("none")}));
        QCOMPARE(app::settings()->value(QStringLiteral("color/workingCmyk")).toString(),QStringLiteral("export"));
        QCOMPARE(app::settings()->value(QStringLiteral("color/policyRgb")).toString(),QStringLiteral("convert"));
        QCOMPARE(app::settings()->value(QStringLiteral("color/askOnMismatch")).toString(),QStringLiteral("false"));
        QCOMPARE(window.book().text_gamma.value_or(0),1.6);
        QVERIFY(window.canvas()->wait_rendered(10000));
        {
            const int dpi=window.canvas()->renderer().shown_dpi();
            render::RenderOptions options;options.mode="proof";options.skip_unported=true;
            const auto image=render::render_page(session->document().page(0),dpi,options,&session->document()).image;
            const auto bytes=image.tobytes();
            QCOMPARE(window.canvas()->renderer().compose(dpi),QImage(reinterpret_cast<const uchar*>(bytes.data()),image.width(),image.height(),
                image.width()*3,QImage::Format_RGB888).convertToFormat(QImage::Format_RGB32));
        }
        window.action("act_undo")->trigger();
        QVERIFY(!window.book().text_gamma);
        // shown again; 初期設定に戻す
        seen.clear();
        answers.responder->exec=[&seen](QDialog* d) {
            auto* dialog=qobject_cast<app::color_settings::ColorSettingsDialog*>(d);
            if (dialog==nullptr) return 0;
            seen<<dialog->working_cmyk->currentData().toString()<<dialog->policy_rgb->currentData().toString();
            dialog->reset->click();
            seen<<dialog->working_cmyk->currentData().toString()<<dialog->policy_rgb->currentData().toString()
                <<(dialog->ask_mismatch->isChecked() ? QStringLiteral("ask") : QStringLiteral("quiet"));
            return static_cast<int>(QDialog::Accepted);
        };
        window.action("act_color_settings")->trigger();
        QCOMPARE(seen,(QStringList{QStringLiteral("export"),QStringLiteral("convert"),QStringLiteral("coated-cmyk"),QStringLiteral("preserve"),
            QStringLiteral("ask")}));
        QCOMPARE(app::color_settings::load(),app::color_settings::Settings{});
        QVERIFY(!window.book().text_gamma);
        window.hide();
    }

    // The page list's small pictures are kept by what the page is drawn from: the book's text gamma and the page's RGB
    // profile change them (a book without either keeps its key).
    void smallPicturesFollowTheGammaAndTheProfile() {
        auto doc=core::new_episode("縮小図",core::Num(1),1,core::PageSpec::custom(60,60,50,50,2,2,2,2,2,150,"color"));
        const core::CommandBus bus(render::ops_registry());
        doc=bus.apply(doc,Json::array({Json{{"op","add_line"},{"page",1},{"text","ガンマ"},{"x_mm",10},{"y_mm",10},{"w_mm",40},{"h_mm",30}}}),
            core::Actor("human:tester")).doc;
        const auto key=[](const core::Document& d) {return app::ThumbCache::key(d,d.page(0),60,"proof");};
        const auto gamma=bus.apply(doc,Json::array({Json{{"op","set_text_gamma"},{"gamma",1.45}}}),core::Actor("human:tester")).doc;
        const auto none=bus.apply(gamma,Json::array({Json{{"op","set_text_gamma"},{"gamma",nullptr}}}),core::Actor("human:tester")).doc;
        const auto p3=bus.apply(doc,Json::array({Json{{"op","assign_profile"},{"page",1},{"profile","display-p3"}}}),core::Actor("human:tester")).doc;
        QVERIFY(key(gamma)!=key(doc));
        QCOMPARE(key(none),key(doc));
        QVERIFY(key(p3)!=key(doc));
    }

    // カラー設定's policy for a book opened: its page of another RGB profile converted to the working sRGB through an op
    // (undone as any); asked first when chosen, the answer taken (外す: the profile replaced, the numbers kept); a book
    // opened read-only is never converted nor asked about.
    void openingABookFollowsThePolicy() {
        (void)gui_test::config_folder();QTemporaryDir tmp;
        auto p3=core::new_episode("P3",core::Num(1),1,core::PageSpec::custom(20,20,16,16,1,2,2,2,2,72,"color"));
        p3.edit_page(0).numero=false;p3.edit_page(0).layers.clear();p3.edit_page(0).frames.clear();
        p3=core::CommandBus(render::ops_registry()).apply(p3,Json::array({Json{{"op","put_color_raster"},{"page",1},{"width",1},{"height",1},
            {"precision","u16"},{"pixels",Json::array({50000,20000,10000,65535})}},Json{{"op","assign_profile"},{"page",1},{"profile","display-p3"}}}),
            core::Actor("human:tester")).doc;
        QVERIFY(p3.page(0).rgb);
        const auto raster=*p3.page(0).layers.back().color_raster;
        const auto ref=core::builtin_profile("display-p3")->ref();
        for (const char* name : {"converted","asked","locked"}) gui_test::write_book(gui_test::path_of(tmp.path()+"/"+name),p3);
        {  // (a feature this build does not know: read-only)
            const auto project=gui_test::path_of(tmp.path()+"/locked")/"project.json";
            QFile in(gui_test::qpath(project));QVERIFY(in.open(QIODevice::ReadOnly));
            Json payload=Json::parse(in.readAll().toStdString());in.close();
            payload["features"].push_back("zz-future@1");
            QFile file(gui_test::qpath(project));QVERIFY(file.open(QIODevice::WriteOnly|QIODevice::Truncate));
            file.write(QByteArray::fromStdString(payload.dump()));
        }
        app::settings()->setValue(QStringLiteral("color/policyRgb"),QStringLiteral("convert"));
        app::settings()->setValue(QStringLiteral("color/askOnMismatch"),QStringLiteral("false"));
        const auto start=gui_test::path_of(tmp.path()+"/start");gui_test::write_book(start,gui_test::new_doc(1));
        auto session=app::Session::open(start,gui_test::quick(gui_test::path_of(tmp.path()+"/recovery")));
        app::MainWindow window(session);window.resize(900,700);window.show();QVERIFY(QTest::qWaitForWindowExposed(&window));
        gui_test::Answers answers;
        QStringList questions;
        QString answer;
        answers.responder->get_item=[&](const QString& title,const QString&,const QStringList& items,int,bool) {
            questions<<title;
            for (const QString& item : items) if (item.contains(answer)) return std::optional<QString>(item);
            return std::optional<QString>();
        };
        window.open_project(gui_test::path_of(tmp.path()+"/converted"));
        // (in the working sRGB: no profile; the page keeps the 16 bits its pixels are held in — an RGB mode of 16 bits —, PC1a E)
        const auto in_srgb=[&] { return !window.book().page(0).rgb || window.book().page(0).rgb->profile.empty(); };
        QTRY_VERIFY_WITH_TIMEOUT(window.documents().size()==2 && in_srgb(),10000);
        QVERIFY(questions.isEmpty());
        {  // (the values in sRGB: at the page's 8 bits, or kept precise)
            const core::Layer& converted=window.book().page(0).layers.back();
            QVERIFY(converted.color_raster ? *converted.color_raster!=raster : converted.raster_png!=nullptr);
        }
        QVERIFY(window.session().can_undo());
        window.action("act_undo")->trigger();
        QVERIFY(window.book().page(0).rgb && window.book().page(0).rgb->profile==ref);
        QVERIFY(window.book().page(0).layers.back().color_raster);
        QCOMPARE(*window.book().page(0).layers.back().color_raster,raster);
        // asked: 外す (the working sRGB taken for its numbers)
        app::settings()->setValue(QStringLiteral("color/askOnMismatch"),QStringLiteral("true"));
        answer=QStringLiteral("外して");
        window.open_project(gui_test::path_of(tmp.path()+"/asked"));
        QTRY_VERIFY_WITH_TIMEOUT(window.documents().size()==3 && in_srgb(),10000);
        QCOMPARE(questions,QStringList{QStringLiteral("プロファイルの不一致")});
        QVERIFY(window.book().page(0).layers.back().color_raster);
        QCOMPARE(*window.book().page(0).layers.back().color_raster,raster);
        // read-only: as it is, nothing asked
        window.open_project(gui_test::path_of(tmp.path()+"/locked"));
        QTRY_VERIFY_WITH_TIMEOUT(window.documents().size()==4,10000);
        QTest::qWait(200);
        QVERIFY(!window.session().read_only_reason().empty());
        QVERIFY(window.book().page(0).rgb && window.book().page(0).rgb->profile==ref);
        QCOMPARE(questions.size(),1);
        for (const char* key : {"color/policyRgb","color/askOnMismatch"}) app::settings()->remove(QString::fromLatin1(key));
        window.hide();
    }

    // The policy for a book opened takes the book's default (the pages made after it) with its pages of that profile, in the
    // same op (undone with them); the question names the book it is about.
    void thePolicyTakesTheBooksDefaultAndNamesTheBook() {
        (void)gui_test::config_folder();QTemporaryDir tmp;
        auto p3=core::new_episode("P3 の本",core::Num(1),2,core::PageSpec::custom(20,20,16,16,1,2,2,2,2,72,"color"));
        p3=core::CommandBus(render::ops_registry()).apply(p3,Json::array({Json{{"op","assign_profile"},{"profile","display-p3"}}}),core::Actor("human:tester")).doc;
        QVERIFY(p3.rgb && p3.page(0).rgb && p3.page(1).rgb);
        gui_test::write_book(gui_test::path_of(tmp.path()+"/p3"),p3);
        app::settings()->setValue(QStringLiteral("color/policyRgb"),QStringLiteral("convert"));
        app::settings()->setValue(QStringLiteral("color/askOnMismatch"),QStringLiteral("true"));
        app::settings()->setValue(QStringLiteral("color/intent"),QStringLiteral("perceptual"));
        const auto start=gui_test::path_of(tmp.path()+"/start");gui_test::write_book(start,gui_test::new_doc(1));
        auto session=app::Session::open(start,gui_test::quick(gui_test::path_of(tmp.path()+"/recovery")));
        app::MainWindow window(session);window.resize(900,700);window.show();QVERIFY(QTest::qWaitForWindowExposed(&window));
        gui_test::Answers answers;
        QStringList asked;
        answers.responder->get_item=[&](const QString&,const QString& label,const QStringList& items,int,bool) {
            asked<<label;
            for (const QString& item : items) if (item.contains(QStringLiteral("変換"))) return std::optional<QString>(item);
            return std::optional<QString>();
        };
        window.open_project(gui_test::path_of(tmp.path()+"/p3"));
        QTRY_VERIFY_WITH_TIMEOUT(window.documents().size()==2 && !window.book().page(0).rgb,10000);
        QCOMPARE(asked.size(),1);
        QVERIFY2(asked.front().contains(QStringLiteral("「P3 の本 第1話」")),qPrintable(asked.front()));
        QVERIFY(!window.book().rgb && !window.book().page(1).rgb);  // (the book's default too, the pages without a colour model)
        window.action("act_undo")->trigger();
        QVERIFY(window.book().rgb && window.book().page(0).rgb && window.book().page(1).rgb);
        for (const char* key : {"color/policyRgb","color/askOnMismatch","color/intent"}) app::settings()->remove(QString::fromLatin1(key));
        window.hide();
    }

    // A book read in parts (its later pages on the worker) is asked about once it is read — after the read is done with
    // (the saves it put off, the file watcher), not in the middle of it.
    void aBookReadInPartsIsAskedOnceRead() {
        (void)gui_test::config_folder();QTemporaryDir tmp;
        auto p3=core::new_episode("あとで読む本",core::Num(1),3,core::PageSpec::custom(20,20,16,16,1,2,2,2,2,72,"color"));
        p3=core::CommandBus(render::ops_registry()).apply(p3,Json::array({Json{{"op","assign_profile"},{"page",3},{"profile","display-p3"}}}),
            core::Actor("human:tester")).doc;
        const auto path=gui_test::path_of(tmp.path()+"/later");gui_test::write_book(path,p3);
        app::settings()->setValue(QStringLiteral("color/policyRgb"),QStringLiteral("convert"));
        app::settings()->setValue(QStringLiteral("color/askOnMismatch"),QStringLiteral("true"));
        const auto start=gui_test::path_of(tmp.path()+"/start");gui_test::write_book(start,gui_test::new_doc(1));
        app::MainWindow window(app::Session::open(start,gui_test::quick(gui_test::path_of(tmp.path()+"/recovery"))));
        window.resize(900,700);window.show();QVERIFY(QTest::qWaitForWindowExposed(&window));
        auto options=gui_test::quick(gui_test::path_of(tmp.path()+"/recovery2"));
        options.defer_pages=true;options.read_rest_now=false;
        auto session=app::Session::open(path,options);
        QVERIFY(session->loading());
        window.add_document(session);
        window.color_policy_on_open(session);
        bool told=false;  // (the read's change told to everyone listening, before the question)
        QObject::connect(session.get(),&app::Session::changed,&window,[&](const app::BookChange&) {if (!session->loading()) told=true;});
        gui_test::Answers answers;
        QList<bool> when;
        answers.responder->get_item=[&](const QString&,const QString&,const QStringList& items,int,bool) {
            when<<told;
            return std::optional<QString>(items.at(0));  // (as it is)
        };
        session->read_rest();
        QTRY_VERIFY_WITH_TIMEOUT(!session->loading() && when.size()==1,10000);
        QCOMPARE(when,QList<bool>{true});
        QTest::qWait(100);
        QCOMPARE(when.size(),1);
        for (const char* key : {"color/policyRgb","color/askOnMismatch"}) app::settings()->remove(QString::fromLatin1(key));
        window.hide();
    }

    // カラー設定's policy for a picture taken in (画像を読み込む): its own Display P3 profile — kept as its numbers
    // (Python's) when so answered, taken to sRGB when chosen; an sRGB one never asks.
    void importingAPictureFollowsThePolicy() {
        (void)gui_test::config_folder();QTemporaryDir tmp;
        const auto path=gui_test::path_of(tmp.path()+"/book");gui_test::write_book(path,gui_test::new_doc(1));
        auto session=app::Session::open(path,gui_test::quick(gui_test::path_of(tmp.path()+"/recovery")));
        app::MainWindow window(session);window.resize(900,700);window.show();QVERIFY(QTest::qWaitForWindowExposed(&window));
        const auto picture_with=[&](const std::string& icc,const QString& name) {
            const std::string png=render::write_png(render::Image::create("RGB",render::Size{4,4},render::Ink{128,64,32}));
            uLongf size=compressBound(static_cast<uLong>(icc.size()));
            std::string packed(size,'\0');
            compress(reinterpret_cast<Bytef*>(packed.data()),&size,reinterpret_cast<const Bytef*>(icc.data()),static_cast<uLong>(icc.size()));
            packed.resize(size);
            const std::string data=std::string("icc\0\0",5)+packed;
            std::string chunk;
            for (int i=3;i>=0;--i) chunk.push_back(static_cast<char>((data.size()>>(8*i))&0xff));
            chunk+="iCCP"+data;
            const uLong crc=crc32(crc32(0,reinterpret_cast<const Bytef*>("iCCP"),4),reinterpret_cast<const Bytef*>(data.data()),static_cast<uInt>(data.size()));
            for (int i=3;i>=0;--i) chunk.push_back(static_cast<char>((crc>>(8*i))&0xff));
            const std::string file=png.substr(0,33)+chunk+png.substr(33);  // (after the signature and IHDR)
            const QString where=tmp.path()+"/"+name;
            QFile out(where);if (!out.open(QIODevice::WriteOnly)) return QString();
            out.write(file.data(),static_cast<qint64>(file.size()));
            return where;
        };
        const QString p3=picture_with(*core::builtin_profile("display-p3")->bytes,QStringLiteral("p3.png"));
        const QString srgb=picture_with(render::colour::srgb_icc(),QStringLiteral("srgb.png"));
        gui_test::Answers answers;
        QStringList questions;
        int choice=0;
        answers.responder->get_item=[&](const QString& title,const QString&,const QStringList& items,int,bool) {
            questions<<title;
            return std::optional<QString>(items.at(choice));
        };
        const auto placed=[&]() -> std::vector<double> {
            const core::Layer* last=nullptr;
            for (const core::Layer& l : window.book().page(0).layers) if (l.kind==core::LayerKind::Placed) last=&l;
            if (last==nullptr || !last->asset) return {};
            const auto bytes=storage::AssetStore(path).get_bytes(*last->asset,".png");
            if (!bytes) return {};
            return render::read_png(*bytes).getpixel(0,0);
        };
        QVERIFY(window.import_image(p3));
        QCOMPARE(questions,QStringList{QStringLiteral("埋め込まれたプロファイル")});
        QCOMPARE(placed(),(std::vector<double>{128,64,32}));
        choice=1;
        QVERIFY(window.import_image(p3));
        QCOMPARE(questions.size(),2);
        const auto converted=placed();
        QVERIFY(converted.size()>=3 && converted!=(std::vector<double>{128,64,32}));
        QVERIFY(window.import_image(srgb));
        QCOMPARE(questions.size(),2);  // (an sRGB one: as it is, not asked)
        QCOMPARE(placed(),(std::vector<double>{128,64,32}));
        window.hide();
    }
};
QTEST_MAIN(TestGuiColor)
#include "test_gui_color.moc"
