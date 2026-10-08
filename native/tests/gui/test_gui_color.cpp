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
#include "app/main_window.hpp"
#include "app/canvas.hpp"
#include "app/inject.hpp"
#include "app/tiles.hpp"
#include "app/thumbs.hpp"
#include "core/color_raster.hpp"
#include "core/strokes.hpp"
#include "render/colour.hpp"
#include "render/page.hpp"
using namespace genko;
using core::Json;
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
};
QTEST_MAIN(TestGuiColor)
#include "test_gui_color.moc"
