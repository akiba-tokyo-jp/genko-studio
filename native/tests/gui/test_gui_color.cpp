#include <QtTest>
#include <QAction>
#include <QApplication>
#include <QDialog>
#include <QDir>
#include <QDoubleSpinBox>
#include <QTimer>
#include "gui_support.hpp"
#include "app/main_window.hpp"
#include "app/canvas.hpp"
#include "app/tiles.hpp"
#include "app/thumbs.hpp"
#include "core/color_raster.hpp"
#include "render/page.hpp"
using namespace genko;
using core::Json;
class TestGuiColor : public QObject {
    Q_OBJECT
private slots:
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
