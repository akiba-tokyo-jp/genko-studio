#include <QtTest/QtTest>
#include <QComboBox>
#include <QCheckBox>
#include <QDoubleSpinBox>
#include <QDialog>
#include <QTimer>
#include <QDockWidget>
#include <QFile>
#include <QDockWidget>
#include "render/page.hpp"
#include "render/png.hpp"
#include <QBuffer>
#include <QImageReader>
#include <QDir>
#include <QTemporaryDir>
#include "app/material_panel.hpp"
#include "app/inject.hpp"
#include <QCryptographicHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <set>

#include "gui_support.hpp"
#include "app/main_window.hpp"
#include "core/json.hpp"

using namespace genko;

class TestGuiMaterials : public QObject {
    Q_OBJECT
private slots:
    void nombreDialogSaveReopenOutput() {
        QTemporaryDir tmp;QVERIFY(tmp.isValid());
        auto doc=core::new_episode("ノンブル制作",core::Num(1),2,core::PageSpec::custom(70,95,60,85,3,8,8,7,6));
        doc.start_side="right"; // Same page-side setting as the fixed Python font/hidden-number oracle.
        for(int n=0;n<2;++n){doc.edit_page(n).frames.clear();doc.edit_page(n).layers.clear();}
        const auto folder=gui_test::path_of(tmp.path()+"/制作.genko");gui_test::write_book(folder,doc);
        auto session=app::Session::open(folder,gui_test::quick(gui_test::path_of(tmp.path()+"/recovery")));
        app::MainWindow window(session);window.resize(1000,760);window.show();QVERIFY(QTest::qWaitForWindowExposed(&window));
        QVERIFY(window.action("act_nombre"));const auto before=session->snapshot();bool seen=false;
        QTimer::singleShot(0,&window,[&]{
            auto* dialog=window.findChild<QDialog*>("nombre_dialog");if(!dialog)return;seen=true;
            auto* font=dialog->findChild<QComboBox*>("nombre_font");auto* start=dialog->findChild<QLineEdit*>("nombre_start");
            if(!font||!start){dialog->reject();return;}font->setCurrentIndex(font->findData("hand"));start->setText("12");dialog->findChild<QCheckBox*>("nombre_hidden")->setChecked(true);
            const QString screenshots=qEnvironmentVariable("GENKO_GUI_SCREENSHOTS");if(!screenshots.isEmpty()){QVERIFY(QDir().mkpath(screenshots));QVERIFY(dialog->grab().save(screenshots+"/nombre-dialog.png"));}
            dialog->accept();
        });window.action("act_nombre")->trigger();QVERIFY(seen);
        QCOMPARE(window.book().nombre.at("font"),core::Json("hand"));QCOMPARE(window.book().nombre.at("start"),core::Json("12"));
        const auto pixels=[&](const core::Document& d){render::RenderOptions ro;ro.mode="print";ro.finish=false;return render::render_page(d.page(0),110,ro,&d).image;};
        const auto image=pixels(window.book());QVERIFY(image.tobytes()!=pixels(*before).tobytes());
        QFile cases(QStringLiteral(GENKO_REPO_ROOT)+"/native/tests/fixtures/render/nombre-cases.json");QVERIFY(cases.open(QIODevice::ReadOnly));
        const auto rows=core::Json::parse(cases.readAll().toStdString());std::string ref;
        for(const auto& c:rows.at("cases"))if(c["config"].value("font","")=="hand"&&c["config"].value("position","")=="bottom_center"&&c["paper"]==core::Json::array({255,255,255}))ref=c["png"].get<std::string>();
        QVERIFY(!ref.empty());QFile expected(QStringLiteral(GENKO_REPO_ROOT)+"/native/tests/fixtures/render/"+QString::fromStdString(ref));QVERIFY(expected.open(QIODevice::ReadOnly));
        const auto oracle=render::read_png(expected.readAll().toStdString()).convert("RGB");
        if(image.tobytes()!=oracle.tobytes()) {
            const auto out=std::filesystem::path(GENKO_REPO_ROOT)/"build/text-diagnostics";std::filesystem::create_directories(out);
            render::save_png(image,out/"nombre-gui-native.png");render::save_png(oracle,out/"nombre-gui-expected.png");
            qWarning()<<"nombre GUI geometry"<<window.book().nombre.dump()<<window.book().page(0).index.repr()<<window.book().start_side.value_or("null")<<ref;
        }
        QCOMPARE(image.tobytes(),oracle.tobytes());
        session->save_now();QVERIFY(session->wait_saved(std::chrono::seconds(10)));const auto saved=session->snapshot();const auto saved_revision=gui_test::disk_revision(folder);
        window.action("act_undo")->trigger();QCOMPARE(window.book().nombre,before->nombre);session->save_now();QVERIFY(session->wait_saved(std::chrono::seconds(10)));const auto undo_revision=gui_test::disk_revision(folder);QVERIFY(undo_revision>saved_revision);QCOMPARE(gui_test::read_book(folder).nombre,before->nombre);
        window.action("act_redo")->trigger();QCOMPARE(window.book().nombre,saved->nombre);
        session->save_now();QVERIFY(session->wait_saved(std::chrono::seconds(10)));QVERIFY(gui_test::disk_revision(folder)>undo_revision);
        auto reopened=app::Session::open(folder,gui_test::quick(gui_test::path_of(tmp.path()+"/reopened")));QCOMPARE(reopened->document().nombre.size(),saved->nombre.size());for(const auto& [key,value]:saved->nombre.items())QCOMPARE(reopened->document().nombre.at(key),value);QCOMPARE(pixels(reopened->document()).tobytes(),image.tobytes());
        const auto output=gui_test::path_of(tmp.path()+"/出力.png");render::save_png(image,output);QFile png(gui_test::qpath(output));QVERIFY(png.open(QIODevice::ReadOnly));QCOMPARE(render::read_png(png.readAll().toStdString()).tobytes(),image.tobytes());
        const QString screenshots=qEnvironmentVariable("GENKO_GUI_SCREENSHOTS");if(!screenshots.isEmpty()){QCoreApplication::processEvents();QVERIFY(window.grab().save(screenshots+"/nombre-window.png"));render::save_png(image,gui_test::path_of(screenshots+"/nombre-print.png"));}
    }
    void nombreModalKeepsTarget_data(){QTest::addColumn<QString>("change");for(const char* name:{"cancel","invalid","page","document","snapshot","stored-number"})QTest::newRow(name)<<QString(name);}
    void nombreDialogContractBounds() {
        auto doc=gui_test::new_doc(1);app::Session::Options options;options.autosave=false;
        auto session=std::make_shared<app::Session>(doc,std::nullopt,options);app::MainWindow window(session);const auto before=session->snapshot();bool seen=false;
        QTimer::singleShot(0,&window,[&]{auto* dialog=window.findChild<QDialog*>("nombre_dialog");if(!dialog)return;seen=true;
            auto* visible=dialog->findChild<QDoubleSpinBox*>("nombre_size");auto* hidden=dialog->findChild<QDoubleSpinBox*>("nombre_hidden_size");
            QCOMPARE(visible->minimum(),1.0);QCOMPARE(visible->maximum(),20.0);QCOMPARE(hidden->minimum(),1.0);QCOMPARE(hidden->maximum(),10.0);
            dialog->findChild<QCheckBox*>("nombre_numero")->setChecked(false);dialog->findChild<QLineEdit*>("nombre_start")->setText("-1");dialog->accept();
        });window.action("act_nombre")->trigger();QVERIFY(seen);QCOMPARE(session->snapshot(),before);
        QTimer::singleShot(0,&window,[&]{auto* dialog=window.findChild<QDialog*>("nombre_dialog");if(!dialog)return;
            dialog->findChild<QCheckBox*>("nombre_numero")->setChecked(false);auto* font=dialog->findChild<QComboBox*>("nombre_font");font->addItem("invalid","not-a-font");font->setCurrentIndex(font->count()-1);dialog->accept();
        });window.action("act_nombre")->trigger();QCOMPARE(session->snapshot(),before);
    }
    void nombreModalKeepsTarget(){
        QFETCH(QString,change);auto doc=core::new_episode("ノンブル対象",core::Num(1),2,core::PageSpec::custom(70,95,60,85,3,8,8,7,6));
        doc.edit_page(0).index=core::Num(10);doc.edit_page(1).index=core::Num(11);
        app::Session::Options options;options.autosave=false;auto session=std::make_shared<app::Session>(doc,std::nullopt,options);app::MainWindow window(session);
        window.go_to_page(10);const auto before=session->snapshot();bool seen=false;
        QTimer::singleShot(0,&window,[&]{auto* dialog=window.findChild<QDialog*>("nombre_dialog");if(!dialog)return;seen=true;
            dialog->findChild<QCheckBox*>("nombre_numero")->setChecked(false);dialog->findChild<QLineEdit*>("nombre_start")->setText(change=="invalid"?"bad":"12");
            if(change=="page")window.go_to_page(11);
            if(change=="document")window.add_document(std::make_shared<app::Session>(doc,std::nullopt,options));
            if(change=="snapshot")window.apply_ops(core::Json::array({core::Json{{"op","set_note"},{"page",11},{"note","変更"}}}));
            if(change=="cancel")dialog->reject();else dialog->accept();
        });window.action("act_nombre")->trigger();QVERIFY(seen);
        if(change=="stored-number"){QVERIFY(!session->document().page(0).numero);QVERIFY(session->document().page(1).numero);QCOMPARE(session->document().nombre.at("start"),core::Json("12"));}
        else{QCOMPARE(session->document().nombre,before->nombre);QVERIFY(session->document().page(0).numero);QVERIFY(session->document().page(1).numero);if(change!="snapshot")QCOMPARE(session->snapshot(),before);else QCOMPARE(session->document().page(1).note,std::string("変更"));}
    }
    void kindSearchAliasesMatchLegacy_data() {
        QTest::addColumn<QString>("query");
        QTest::addColumn<QStringList>("ids");
        const core::Json cases = core::parse_python_json(R"oracle([{"query":"効果音","ids":["sfx-ドーン","sfx-ゴゴゴ","sfx-バーン","sfx-ザワ…","sfx-シーン","sfx-ドキドキ","sfx-ガシャーン","sfx-ヒュー","sfx-パチパチ","sfx-キラッ"]},{"query":"効果音 ドーン","ids":["sfx-ドーン"]},{"query":"トーン","ids":["dot-60-30","dot-40-80","sand-50","dot-60-10","dot-60-20","dot-60-40","dot-85-20","dot-42-50","line-50-30","cross-50-40","sand-20","grad-v","grad-v-up","grad-radial","grad-sand"]},{"query":"描き文字","ids":["sfx-ドーン","sfx-ゴゴゴ","sfx-バーン","sfx-ザワ…","sfx-シーン","sfx-ドキドキ","sfx-ガシャーン","sfx-ヒュー","sfx-パチパチ","sfx-キラッ"]},{"query":"ブラシ","ids":["brush-雨ブラシ","brush-雪ブラシ","brush-落ち葉ブラシ","brush-星空ブラシ","brush-レース飾りブラシ","brush-カケアミ風ブラシ"]},{"query":"パーツ 線","ids":["mark-汗","mark-怒りマーク","mark-驚き線","mark-ハート","mark-星","mark-キラキラ","mark-音符","mark-ぐるぐる","mark-びっくりマーク","mark-はてなマーク","mark-ひらめき","mark-花","mark-湯気","mark-動きの線","mark-光の線","prop-窓","prop-ドア","prop-椅子","prop-机","prop-本棚","prop-木","prop-雲","prop-月","prop-太陽","prop-家","prop-山","prop-草","prop-街灯","prop-カップ","bg-room","bg-classroom","bg-corridor","bg-street"]},{"query":"効果線","ids":["speed-h","focus","focus-dense","speed-v","speed-d","speed-curve","uni","beta"]},{"query":"3D","ids":["3d-デッサン人形","3d-棒人形","3d-頭部","3d-手（ピース）","3d-手（握る）","3d-手（開く）","3d-箱","3d-円柱","3d-階段","3d-床の格子","3d-部屋（3D）","3d-教室（3D）","3d-廊下（3D）","3d-街並み（3D）"]},{"query":"3d","ids":["3d-デッサン人形","3d-棒人形","3d-頭部","3d-手（ピース）","3d-手（握る）","3d-手（開く）","3d-箱","3d-円柱","3d-階段","3d-床の格子","3d-部屋（3D）","3d-教室（3D）","3d-廊下（3D）","3d-街並み（3D）"]},{"query":"tone","ids":[]},{"query":"効果音\tドーン","ids":["sfx-ドーン"]},{"query":"　効果音　ドーン　","ids":["sfx-ドーン"]}])oracle");
        int row = 0;
        for (const auto& item : cases) {
            QStringList ids;
            for (const auto& id : item.at("ids")) ids.push_back(QString::fromStdString(id.get<std::string>()));
            const auto name = QStringLiteral("legacy-query-%1").arg(row++).toUtf8();
            QTest::newRow(name.constData()) << QString::fromStdString(item.at("query").get<std::string>()) << ids;
        }
    }

    void kindSearchAliasesMatchLegacy() {
        QFETCH(QString, query); QFETCH(QStringList, ids);
        auto doc = gui_test::new_doc(1);
        doc.edit_page(0).numero = false;
        auto session = std::make_shared<app::Session>(doc, std::nullopt);
        app::MainWindow window(session);
        auto* search = window.findChild<QLineEdit*>(QStringLiteral("materialSearch"));
        auto* list = window.findChild<QListWidget*>(QStringLiteral("materialList"));
        QVERIFY(search != nullptr); QVERIFY(list != nullptr);
        search->setText(query);
        QStringList actual;
        for (int row = 0; row < list->count(); ++row) actual.push_back(list->item(row)->data(Qt::UserRole).toString());
        QCOMPARE(actual, ids);
    }

    void initTestCase() { QVERIFY(gui_test::config_folder().isValid()); }

    void packagedBuiltinsAreVisibleAndSearchable() {
        auto doc = gui_test::new_doc(1);
        doc.edit_page(0).numero = false;
        auto session = std::make_shared<app::Session>(doc, std::nullopt);
        const auto before = session->snapshot();
        app::MainWindow window(session);
        auto* list = window.findChild<QListWidget*>(QStringLiteral("materialList"));
        QVERIFY2(list != nullptr, "built-in material list is missing");
        auto* dock = window.findChild<QDockWidget*>(QStringLiteral("素材"));
        QVERIFY(dock != nullptr);
        dock->show();
        window.show();
        auto* search = window.findChild<QLineEdit*>(QStringLiteral("materialSearch"));
        QVERIFY(search != nullptr);
        auto* folder = window.findChild<QComboBox*>(QStringLiteral("materialFolder"));
        QVERIFY(folder != nullptr);
        QFile catalog(QStringLiteral(":/genko/materials/catalog.json"));
        QVERIFY(catalog.open(QIODevice::ReadOnly));
        const auto entries = core::parse_python_json(catalog.readAll().toStdString());
        QVERIFY(entries.is_array());
        QVERIFY(entries.size() > 50);
        QCOMPARE(list->count(), static_cast<int>(entries.size()));
        std::set<std::string> wanted, shown;
        for (const auto& entry : entries) {
            wanted.insert(entry.at("id").get<std::string>());
            const auto path = QString::fromStdString(entry.at("preview").get<std::string>());
            const QImage preview(path);
            QVERIFY2(!preview.isNull(), qPrintable(path));
            QCOMPARE(preview.size(), QSize(56, 56));
        }
        for (int row = 0; row < list->count(); ++row) {
            const auto* item = list->item(row);
            shown.insert(item->data(Qt::UserRole).toString().toStdString());
            QVERIFY(!item->icon().isNull());
        }
        QVERIFY(shown == wanted);
        search->setText(QStringLiteral("　ハート　"));
        QCOMPARE(list->count(), 1);
        QCOMPARE(list->item(0)->data(Qt::UserRole).toString(), QStringLiteral("mark-ハート"));
        search->setText(QStringLiteral("存在しない素材検索語"));
        QCOMPARE(list->count(), 0);
        search->clear();
        QCOMPARE(list->count(), static_cast<int>(entries.size()));
        const int marks = folder->findText(QStringLiteral("漫符"));
        QVERIFY(marks >= 0);
        folder->setCurrentIndex(marks);
        QVERIFY(list->count() > 1);
        QVERIFY(list->count() < static_cast<int>(entries.size()));
        for (int row = 0; row < list->count(); ++row)
            QVERIFY(list->item(row)->data(Qt::UserRole).toString().startsWith(QStringLiteral("mark-")));
        // Browsing and searching are previews only; no document mutation or Undo entry.
        QCOMPARE(session->snapshot(), before);
    }
    void userLibraryImagesUseLazyPersistentCache() {
        QTemporaryDir config;QVERIFY(config.isValid());const QByteArray previous=qgetenv("GENKO_CONFIG_DIR");
        struct Restore { QByteArray value;~Restore(){if(value.isNull())qunsetenv("GENKO_CONFIG_DIR");else qputenv("GENKO_CONFIG_DIR",value);} } restore{previous};
        qputenv("GENKO_CONFIG_DIR",config.path().toUtf8());
        const QString root=config.path()+"/materials";QVERIFY(QDir().mkpath(root));
        QImage original(160,80,QImage::Format_ARGB32);original.fill(QColor(12,123,234));QVERIFY(original.save(root+"/u-test.png","PNG"));
        QFile imageFile(root+"/u-test.png");QVERIFY(imageFile.open(QIODevice::ReadOnly));const QByteArray originalBytes=imageFile.readAll();imageFile.close();
        QJsonArray library{QJsonObject{{"id","u-test"},{"name",QStringLiteral("私の青い画像")},{"folder",QStringLiteral("私の素材")},{"kind","image"},{"file","u-test.png"},{"width_mm",60},{"tags",QJsonArray{QStringLiteral("青")}}}};
        const QByteArray libraryBytes=QJsonDocument(library).toJson();QFile manifest(root+"/library.json");QVERIFY(manifest.open(QIODevice::WriteOnly));QCOMPARE(manifest.write(libraryBytes),qint64(libraryBytes.size()));manifest.close();
        auto visit=[&](int generated){
            auto panel=std::unique_ptr<QWidget>(app::make_builtin_material_panel());
            auto* list=panel->findChild<QListWidget*>("materialList");auto* folders=panel->findChild<QComboBox*>("materialFolder");QVERIFY(list&&folders);
            auto* search=panel->findChild<QLineEdit*>("materialSearch");QVERIFY(search);QCOMPARE(list->count(),87);
            QCOMPARE(panel->property("userPreviewGenerations").toInt(),0);
            QVERIFY(!QDir(config.path()+"/cache/material-previews").exists() || generated==0);
            panel->resize(1000,600);panel->show();QVERIFY(QTest::qWaitForWindowExposed(panel.get()));
            QTest::qWait(20);QCOMPARE(panel->property("userPreviewGenerations").toInt(),0);
            bool selected=false;for(int n=0;n<folders->count();++n)if(folders->itemText(n)==QStringLiteral("私の素材")){folders->setCurrentIndex(n);selected=true;break;}
            QVERIFY2(selected,"existing Python library folders must be visible");QCOMPARE(list->count(),1);
            QTRY_VERIFY_WITH_TIMEOUT(list->item(0)->data(Qt::UserRole+3).toBool(),5000);
            QCOMPARE(list->item(0)->data(Qt::UserRole).toString(),QStringLiteral("u-test"));QVERIFY(!list->item(0)->icon().isNull());
            const QImage icon=list->item(0)->icon().pixmap(56,56).toImage();
            QCOMPARE(icon.pixelColor(28,28),QColor(12,123,234));QCOMPARE(icon.pixelColor(28,3),QColor(255,255,255));
            QCOMPARE(panel->property("userPreviewGenerations").toInt(),generated);
            QCoreApplication::processEvents();
            const QImage shown=list->viewport()->grab().toImage().convertToFormat(QImage::Format_RGB32);
            const qreal dpr=shown.devicePixelRatio();const QRect itemRect=list->visualItemRect(list->item(0));
            const QRect painted(qRound(itemRect.x()*dpr),qRound(itemRect.y()*dpr),qRound(itemRect.width()*dpr),qRound(itemRect.height()*dpr));
            const QImage tile=shown.copy(painted.intersected(shown.rect()));int bluePixels=0;
            for(int y=0;y<tile.height();++y)for(int x=0;x<tile.width();++x)if(tile.pixelColor(x,y)==QColor(12,123,234))++bluePixels;
            QVERIFY2(bluePixels>=1000,"real viewport must paint the user's image, not only retain an icon object");
            const QString screenshots=qEnvironmentVariable("GENKO_GUI_SCREENSHOTS");
            if(!screenshots.isEmpty()){QVERIFY(QDir().mkpath(screenshots));QVERIFY(panel->grab().save(screenshots+QString("/user-library-%1.png").arg(generated)));}
            search->setText(QStringLiteral("青 画像"));QCOMPARE(list->count(),1);
            search->setText(QStringLiteral("存在しない"));QCOMPARE(list->count(),0);
        };
        visit(1);visit(0);
        QVERIFY(manifest.open(QIODevice::ReadOnly));QCOMPARE(manifest.readAll(),libraryBytes);manifest.close();
        QVERIFY(imageFile.open(QIODevice::ReadOnly));QCOMPARE(imageFile.readAll(),originalBytes);
        QCOMPARE(QDir(config.path()+"/cache/material-previews").entryList({"*.png"},QDir::Files).size(),1);
    }

    void doubleClickBrushDrawSaveReopen_data() {
        QTest::addColumn<bool>("refused");QTest::newRow("brush-draw")<<false;QTest::newRow("locked-refusal")<<true;
    }
    void doubleClickBrushDrawSaveReopen() {
        QFETCH(bool,refused);QTemporaryDir temp;QVERIFY(temp.isValid());
        auto doc=core::new_episode("ブラシ素材",core::Num(1),1,core::PageSpec::custom(20,20,16,16,1,2,2,2,2,72,"mono"));doc.edit_page(0).numero=false;
        if(refused)doc.page_locks[doc.page(0).id]="human:other";
        app::Session::Options options;options.autosave=false;
        auto session=std::make_shared<app::Session>(doc,std::nullopt,options);app::MainWindow window(session);
        window.pen()=app::PenSettings::for_kind("gpen");window.pen_changed();window.choose_tool("eraser");
        const auto before=session->snapshot();const auto penBefore=window.pen().stroke_fields();
        auto* dock=window.findChild<QDockWidget*>(QStringLiteral("素材"));QVERIFY(dock);window.resize(1200,800);window.show();dock->show();QVERIFY(QTest::qWaitForWindowExposed(&window));
        auto* search=dock->findChild<QLineEdit*>("materialSearch");auto* list=dock->findChild<QListWidget*>("materialList");QVERIFY(search&&list);
        search->setText(QStringLiteral("雨ブラシ"));QCOMPARE(list->count(),1);list->setCurrentRow(0);QCoreApplication::processEvents();
        const auto p=list->visualItemRect(list->item(0)).center();QTest::mouseClick(list->viewport(),Qt::LeftButton,Qt::NoModifier,p);QTest::mouseDClick(list->viewport(),Qt::LeftButton,Qt::NoModifier,p);
        if(refused){QCOMPARE(session->snapshot(),before);QCOMPARE(window.pen().stroke_fields(),penBefore);QCOMPARE(window.canvas()->tool(),QString("eraser"));return;}
        const std::string key="my_"+QCryptographicHash::hash(QStringLiteral("brush-雨ブラシ").toUtf8(),QCryptographicHash::Sha1).toHex().left(10).toStdString();
        QVERIFY(window.book().brush_custom.contains(key));
        QVERIFY2(window.pen().kind==key,"double-clicked brush must become the active pen before drawing");
        QCOMPARE(window.pen().width_mm,3.0);QCOMPARE(window.canvas()->tool(),QString("pen"));QCOMPARE(window.canvas()->live_pen().at("kind"),core::Json(key));
        app::inject::mouse_stroke(window.canvas(),{QPointF(6,8),QPointF(8,9),QPointF(10,10),QPointF(12,11)});
        const auto* ink=gui_test::ink_of(window.book().page(0));QVERIFY(ink);QCOMPARE(ink->stroke_count(),std::size_t(1));QCOMPARE(ink->strokes->items[0]->kind,key);QCOMPARE(ink->strokes->items[0]->width_mm,3.0);
        const auto proof=render::render_page(window.book().page(0),72,render::proof_options());QVERIFY(proof.image.tobytes()!=render::render_page(doc.page(0),72,render::proof_options()).image.tobytes());
        window.action("act_undo")->trigger();QCOMPARE(gui_test::ink_of(window.book().page(0))->stroke_count(),std::size_t(0));window.action("act_redo")->trigger();QCOMPARE(gui_test::ink_of(window.book().page(0))->stroke_count(),std::size_t(1));
        const auto folder=gui_test::path_of(temp.path()+"/ブラシ.genko");session->save_as(folder);QVERIFY(session->wait_saved(std::chrono::seconds(10)));const auto reopened=app::Session::open(folder,options);
        QVERIFY(reopened->document().brush_custom.contains(key));QCOMPARE(gui_test::ink_of(reopened->document().page(0))->strokes->items[0]->kind,key);
        const auto output=render::render_page(reopened->document().page(0),72,render::proof_options());QCOMPARE(output.image.tobytes(),proof.image.tobytes());
        const auto pngPath=gui_test::path_of(temp.path()+"/ブラシ.png");render::save_png(output.image,pngPath);QFile file(gui_test::qpath(pngPath));QVERIFY(file.open(QIODevice::ReadOnly));const auto bytes=file.readAll();QCOMPARE(render::read_png(std::string_view(bytes.constData(),static_cast<std::size_t>(bytes.size()))).tobytes(),proof.image.tobytes());
        const QString screenshots=qEnvironmentVariable("GENKO_GUI_SCREENSHOTS");if(!screenshots.isEmpty()){QVERIFY(QDir().mkpath(screenshots));QVERIFY(window.grab().save(screenshots+"/material-brush.png"));}
    }

    void doubleClickUserImageSaveReopenOutput(){
        QTemporaryDir cfg,bookRoot;QVERIFY(cfg.isValid());QVERIFY(bookRoot.isValid());const auto previous=qgetenv("GENKO_CONFIG_DIR");
        struct Restore{QByteArray value;~Restore(){if(value.isNull())qunsetenv("GENKO_CONFIG_DIR");else qputenv("GENKO_CONFIG_DIR",value);}}restore{previous};qputenv("GENKO_CONFIG_DIR",cfg.path().toUtf8());
        const auto root=cfg.path()+"/materials";QVERIFY(QDir().mkpath(root));QImage image(2,1,QImage::Format_ARGB32);image.fill(qRgb(12,123,234));QVERIFY(image.save(root+"/gui-blue.png","PNG"));QFile source(root+"/gui-blue.png");QVERIFY(source.open(QIODevice::ReadOnly));const auto png=source.readAll();source.close();
        const auto library=core::Json::array({core::Json{{"id","u-gui-blue"},{"kind","image"},{"name","配置用の青"},{"file","gui-blue.png"},{"width_mm",12},{"aspect",.5}}}).dump();QFile manifest(root+"/library.json");QVERIFY(manifest.open(QIODevice::WriteOnly));QCOMPARE(manifest.write(QByteArray::fromStdString(library)),qint64(library.size()));manifest.close();
        auto doc=core::new_episode("画像配置",core::Num(1),1,core::PageSpec::custom(20,20,16,16,1,2,2,2,2,72,"mono"));doc.edit_page(0).numero=false;
        const auto book=gui_test::path_of(bookRoot.path()+"/画像.genko");gui_test::write_book(book,doc);app::Session::Options options;options.autosave=false;auto session=app::Session::open(book,options);app::MainWindow window(session);
        auto* dock=window.findChild<QDockWidget*>(QStringLiteral("素材"));QVERIFY(dock);window.resize(1200,800);window.show();dock->show();QVERIFY(QTest::qWaitForWindowExposed(&window));
        auto* search=dock->findChild<QLineEdit*>("materialSearch");auto* list=dock->findChild<QListWidget*>("materialList");QVERIFY(search&&list);search->setText(QStringLiteral("配置用の青"));QCOMPARE(list->count(),1);QCOMPARE(list->item(0)->data(Qt::UserRole).toString(),QString("u-gui-blue"));
        list->setCurrentRow(0);list->scrollToItem(list->item(0));QCoreApplication::processEvents();const QPoint point=list->visualItemRect(list->item(0)).center();QTest::mouseClick(list->viewport(),Qt::LeftButton,Qt::NoModifier,point);QTest::mouseDClick(list->viewport(),Qt::LeftButton,Qt::NoModifier,point);
        const auto* ink=gui_test::ink_of(window.book().page(0));QVERIFY(ink);QCOMPARE(ink->patches.size(),std::size_t(1));QCOMPARE(*ink->patches[0].png,png.toStdString());
        const auto proof=render::render_page(window.book().page(0),72,render::proof_options()).image;QCOMPARE(proof.getpixel(proof.width()/2,proof.height()/2),(std::vector<double>{12,123,234}));QVERIFY(proof.tobytes()!=render::render_page(doc.page(0),72,render::proof_options()).image.tobytes());
        session->save_now();QVERIFY(session->wait_saved(std::chrono::seconds(10)));const auto revision=gui_test::disk_revision(book);
        window.action("act_undo")->trigger();QVERIFY(session->wait_saved(std::chrono::seconds(10)));QVERIFY(gui_test::disk_revision(book)>revision);QCOMPARE(gui_test::ink_of(window.book().page(0))->patches.size(),std::size_t(0));QCOMPARE(render::render_page(window.book().page(0),72,render::proof_options()).image.tobytes(),render::render_page(doc.page(0),72,render::proof_options()).image.tobytes());
        const auto undoRevision=gui_test::disk_revision(book);window.action("act_redo")->trigger();QVERIFY(session->wait_saved(std::chrono::seconds(10)));QVERIFY(gui_test::disk_revision(book)>undoRevision);
        const auto reopened=app::Session::open(book,options);const auto* restored=gui_test::ink_of(reopened->document().page(0));QVERIFY(restored);QCOMPARE(restored->patches.size(),std::size_t(1));QCOMPARE(*restored->patches[0].png,png.toStdString());const auto output=render::render_page(reopened->document().page(0),72,render::proof_options()).image;QCOMPARE(output.tobytes(),proof.tobytes());
        const auto file=gui_test::path_of(bookRoot.path()+"/画像.png");render::save_png(output,file);QFile exported(gui_test::qpath(file));QVERIFY(exported.open(QIODevice::ReadOnly));const auto exportedBytes=exported.readAll();QCOMPARE(render::read_png(std::string_view(exportedBytes.constData(),static_cast<std::size_t>(exportedBytes.size()))).tobytes(),proof.tobytes());
        window.canvas()->fit_page();QVERIFY(window.canvas()->wait_rendered(10000));const auto screen=window.canvas()->grab().toImage();const auto center=window.canvas()->to_widget(QPointF(10,10));const auto ratio=screen.devicePixelRatio();QCOMPARE(screen.pixelColor(qRound(center.x()*ratio),qRound(center.y()*ratio)),QColor(12,123,234));
        const QString screenshots=qEnvironmentVariable("GENKO_GUI_SCREENSHOTS");if(!screenshots.isEmpty()){QVERIFY(QDir().mkpath(screenshots));QVERIFY(window.grab().save(screenshots+"/material-user-image.png"));}
        QVERIFY(manifest.open(QIODevice::ReadOnly));QCOMPARE(manifest.readAll(),QByteArray::fromStdString(library));QVERIFY(source.open(QIODevice::ReadOnly));QCOMPARE(source.readAll(),png);
    }
    void doubleClickLinesSaveReopenOutput(){
        QTemporaryDir temp;QVERIFY(temp.isValid());auto doc=core::new_episode("線画素材",core::Num(1),1,core::PageSpec::custom(20,20,16,16,1,2,2,2,2,72,"mono"));doc.edit_page(0).numero=false;
        app::Session::Options options;options.autosave=false;auto session=std::make_shared<app::Session>(doc,std::nullopt,options);app::MainWindow window(session);const auto before=session->snapshot();
        auto* dock=window.findChild<QDockWidget*>(QStringLiteral("素材"));QVERIFY(dock);window.resize(1200,800);window.show();dock->show();QVERIFY(QTest::qWaitForWindowExposed(&window));
        auto* search=dock->findChild<QLineEdit*>("materialSearch");auto* list=dock->findChild<QListWidget*>("materialList");QVERIFY(search&&list);search->setText(QStringLiteral("汗"));QListWidgetItem* selected=nullptr;
        for(int n=0;n<list->count();++n){if(list->item(n)->data(Qt::UserRole).toString()=="mark-汗")selected=list->item(n);}QVERIFY(selected);list->setCurrentItem(selected);QCoreApplication::processEvents();
        const auto pos=list->visualItemRect(selected).center();QTest::mouseClick(list->viewport(),Qt::LeftButton,Qt::NoModifier,pos);QTest::mouseDClick(list->viewport(),Qt::LeftButton,Qt::NoModifier,pos);
        const auto* ink=gui_test::ink_of(window.book().page(0));QVERIFY(ink);QCOMPARE(ink->stroke_count(),std::size_t(2));
        const auto output=render::render_page(window.book().page(0),72,render::proof_options());QVERIFY(output.image.tobytes()!=render::render_page(doc.page(0),72,render::proof_options()).image.tobytes());
        window.action("act_undo")->trigger();QCOMPARE(session->snapshot(),before);window.action("act_redo")->trigger();QCOMPARE(gui_test::ink_of(window.book().page(0))->stroke_count(),std::size_t(2));
        const auto folder=gui_test::path_of(temp.path()+"/線画.genko");session->save_as(folder);QVERIFY(session->wait_saved(std::chrono::seconds(10)));const auto reopened=app::Session::open(folder,options);QCOMPARE(gui_test::ink_of(reopened->document().page(0))->stroke_count(),std::size_t(2));
        const auto after=render::render_page(reopened->document().page(0),72,render::proof_options());QCOMPARE(after.image.tobytes(),output.image.tobytes());const auto file=gui_test::path_of(temp.path()+"/線画.png");render::save_png(after.image,file);QFile png(gui_test::qpath(file));QVERIFY(png.open(QIODevice::ReadOnly));const auto bytes=png.readAll();QCOMPARE(render::read_png(std::string_view(bytes.constData(),static_cast<std::size_t>(bytes.size()))).tobytes(),output.image.tobytes());
        const QString screenshots=qEnvironmentVariable("GENKO_GUI_SCREENSHOTS");if(!screenshots.isEmpty()){QVERIFY(QDir().mkpath(screenshots));QVERIFY(window.grab().save(screenshots+"/material-lines.png"));}
    }
    void doubleClickToneSaveReopenOutput() {
        QTemporaryDir temp;QVERIFY(temp.isValid());
        auto doc=core::new_episode("素材制作",core::Num(1),2,core::PageSpec::custom(20,20,16,16,1,2,2,2,2,72,"mono"));
        // This new fixture isolates material placement; native nombre remains a separate M4 dependency.
        doc.edit_page(0).numero=false;doc.edit_page(1).numero=false;
        app::Session::Options options;options.autosave=false;
        auto session=std::make_shared<app::Session>(doc,std::nullopt,options);app::MainWindow window(session);
        auto* dock=window.findChild<QDockWidget*>(QStringLiteral("素材"));auto* panel=dock?dock->widget():nullptr;
        QVERIFY(panel&&dock);window.resize(1200,800);window.show();dock->show();QVERIFY(QTest::qWaitForWindowExposed(&window));
        auto* search=panel->findChild<QLineEdit*>("materialSearch");auto* list=panel->findChild<QListWidget*>("materialList");QVERIFY(search&&list);
        search->setText(QStringLiteral("網点 60 線 30%"));QCOMPARE(list->count(),1);
        list->setCurrentRow(0);list->scrollToItem(list->item(0));QCoreApplication::processEvents();
        const QPoint point=list->visualItemRect(list->item(0)).center();
        QTest::mouseClick(list->viewport(),Qt::LeftButton,Qt::NoModifier,point);QTest::mouseDClick(list->viewport(),Qt::LeftButton,Qt::NoModifier,point);
        QTRY_VERIFY_WITH_TIMEOUT(window.book().page(0).layers.size()==doc.page(0).layers.size()+1,3000);
        const auto found=std::find_if(window.book().page(0).layers.begin(),window.book().page(0).layers.end(),[](const core::Layer& l){return l.kind==core::LayerKind::Tone;});
        QVERIFY(found!=window.book().page(0).layers.end());const std::string toneId=found->id;const auto& tone=*found;QCOMPARE(tone.material_id.value_or(""),std::string("dot-60-30"));QCOMPARE(tone.density.value(),0.3);
        const auto proof=render::render_page(window.book().page(0),72,render::proof_options());
        const auto baseline=render::render_page(doc.page(0),72,render::proof_options());QVERIFY(proof.image.tobytes()!=baseline.image.tobytes());
        QVERIFY(window.action("act_undo"));window.action("act_undo")->trigger();QCOMPARE(window.book().page(0).layers.size(),doc.page(0).layers.size());QVERIFY(window.action("act_redo"));window.action("act_redo")->trigger();QCOMPARE(window.book().page(0).layers.size(),doc.page(0).layers.size()+1);
        const auto folder=gui_test::path_of(temp.path()+"/制作.genko");session->save_as(folder);QVERIFY(session->wait_saved(std::chrono::seconds(10)));
        const auto reopened=app::Session::open(folder,options);const auto restored=std::find_if(reopened->document().page(0).layers.begin(),reopened->document().page(0).layers.end(),[&](const core::Layer& l){return l.id==toneId;});
        QVERIFY(restored!=reopened->document().page(0).layers.end());QCOMPARE(restored->material_id.value_or(""),std::string("dot-60-30"));
        const auto output=render::render_page(reopened->document().page(0),72,render::proof_options());QCOMPARE(output.image.tobytes(),proof.image.tobytes());
        const auto file=gui_test::path_of(temp.path()+"/出力.png");render::save_png(output.image,file);QFile png(gui_test::qpath(file));QVERIFY(png.open(QIODevice::ReadOnly));const QByteArray bytes=png.readAll();QCOMPARE(render::read_png(std::string_view(bytes.constData(),static_cast<std::size_t>(bytes.size()))).tobytes(),proof.image.tobytes());
        const QString screenshots=qEnvironmentVariable("GENKO_GUI_SCREENSHOTS");
        if(!screenshots.isEmpty()){QVERIFY(QDir().mkpath(screenshots));QVERIFY(window.grab().save(screenshots+"/material-stamp-tone.png"));render::save_png(output.image,gui_test::path_of(screenshots+"/material-stamp-proof.png"));}
        QCOMPARE(reopened->document().page(1).layers.size(),doc.page(1).layers.size());
        QCOMPARE(render::render_page(reopened->document().page(1),72,render::proof_options()).image.tobytes(),render::render_page(doc.page(1),72,render::proof_options()).image.tobytes());
    }

    void corruptUserImageDoesNotStopHealthyPreview() {
        QTemporaryDir config;QVERIFY(config.isValid());const QByteArray previous=qgetenv("GENKO_CONFIG_DIR");
        struct Restore { QByteArray value;~Restore(){if(value.isNull())qunsetenv("GENKO_CONFIG_DIR");else qputenv("GENKO_CONFIG_DIR",value);} } restore{previous};
        qputenv("GENKO_CONFIG_DIR",config.path().toUtf8());
        const QString root=config.path()+"/materials";QVERIFY(QDir().mkpath(root));
        QImage image(32,16,QImage::Format_ARGB32);image.fill(QColor(12,123,234));QVERIFY(image.save(root+"/u-good.png","PNG"));
        QFile good(root+"/u-good.png");QVERIFY(good.open(QIODevice::ReadOnly));const auto goodBytes=good.readAll();good.close();
        QByteArray brokenBytes=goodBytes;const qsizetype idat=brokenBytes.indexOf("IDAT");QVERIFY(idat>=0);
        brokenBytes[idat+5]=char(static_cast<unsigned char>(brokenBytes[idat+5])^0x55);
        QBuffer probe(&brokenBytes);QVERIFY(probe.open(QIODevice::ReadOnly));QImageReader reader(&probe,"PNG");
        QCOMPARE(reader.size(),QSize(32,16));QVERIFY(reader.read().isNull());
        QFile broken(root+"/u-broken.png");QVERIFY(broken.open(QIODevice::WriteOnly));QCOMPARE(broken.write(brokenBytes),qint64(brokenBytes.size()));broken.close();
        QJsonArray entries;
        for(const char* id:{"u-broken","u-good"})entries.append(QJsonObject{{"id",id},{"name",id},{"kind","image"},{"folder","corrupt-test"},{"file",QString::fromLatin1(id)+".png"}});
        QFile manifest(root+"/library.json");QVERIFY(manifest.open(QIODevice::WriteOnly));const auto libraryBytes=QJsonDocument(entries).toJson();QCOMPARE(manifest.write(libraryBytes),qint64(libraryBytes.size()));manifest.close();
        auto panel=std::unique_ptr<QWidget>(app::make_builtin_material_panel());
        auto* list=panel->findChild<QListWidget*>("materialList");auto* search=panel->findChild<QLineEdit*>("materialSearch");QVERIFY(list&&search);
        search->setText("corrupt-test");QCOMPARE(list->count(),2);panel->resize(700,500);panel->show();QVERIFY(QTest::qWaitForWindowExposed(panel.get()));
        QTRY_VERIFY_WITH_TIMEOUT(list->item(0)->data(Qt::UserRole+3).toBool(),5000);
        QVERIFY(list->item(0)->icon().isNull());QVERIFY(list->item(0)->toolTip().contains(QStringLiteral("読み込めません")));
        QTRY_VERIFY_WITH_TIMEOUT(list->item(1)->data(Qt::UserRole+3).toBool(),5000);
        QVERIFY(!list->item(1)->icon().isNull());QCOMPARE(list->item(1)->icon().pixmap(56,56).toImage().pixelColor(28,28),QColor(12,123,234));
        QVERIFY(broken.open(QIODevice::ReadOnly));QCOMPARE(broken.readAll(),brokenBytes);broken.close();
        QVERIFY(good.open(QIODevice::ReadOnly));QCOMPARE(good.readAll(),goodBytes);good.close();
        QVERIFY(manifest.open(QIODevice::ReadOnly));QCOMPARE(manifest.readAll(),libraryBytes);
        QCOMPARE(QDir(config.path()+"/cache/material-previews").entryList({"*.png"},QDir::Files).size(),1);
    }

    void userLibraryRejectsEscapingPaths_data() {
        QTest::addColumn<QString>("kind");
        for(const auto& kind:{QString("parent"),QString("absolute"),QString("image-symlink"),QString("root-symlink")})QTest::newRow(kind.toUtf8().constData())<<kind;
    }
    void userLibraryRejectsEscapingPaths() {
        QFETCH(QString,kind);QTemporaryDir config;QVERIFY(config.isValid());const QByteArray previous=qgetenv("GENKO_CONFIG_DIR");
        struct Restore { QByteArray value;~Restore(){if(value.isNull())qunsetenv("GENKO_CONFIG_DIR");else qputenv("GENKO_CONFIG_DIR",value);} } restore{previous};
        qputenv("GENKO_CONFIG_DIR",config.path().toUtf8());
        const QString root=config.path()+"/materials";QVERIFY(QDir().mkpath(root));
        QImage original(8,8,QImage::Format_ARGB32);original.fill(Qt::red);const QString outside=config.path()+"/outside.png";QVERIFY(original.save(outside,"PNG"));
        QFile untouched(outside);QVERIFY(untouched.open(QIODevice::ReadOnly));const QByteArray before=untouched.readAll();untouched.close();
        QString filename="../outside.png";
        if(kind=="absolute")filename=outside;
        if(kind=="image-symlink"){filename="u-link.png";QVERIFY(QFile::link(outside,root+"/"+filename));}
        QJsonArray library{QJsonObject{{"id","u-rejected"},{"name",QStringLiteral("安全拒否の素材")},{"kind","image"},{"folder","test"},{"file",filename}}};
        QFile manifest(root+"/library.json");QVERIFY(manifest.open(QIODevice::WriteOnly));const QByteArray data=QJsonDocument(library).toJson();QCOMPARE(manifest.write(data),qint64(data.size()));manifest.close();
        if(kind=="root-symlink"){QVERIFY(QDir().rename(root,config.path()+"/outside-library"));QVERIFY(QFile::link(config.path()+"/outside-library",root));}
        auto panel=std::unique_ptr<QWidget>(app::make_builtin_material_panel());auto* list=panel->findChild<QListWidget*>("materialList");auto* search=panel->findChild<QLineEdit*>("materialSearch");QVERIFY(list&&search);
        panel->resize(700,500);panel->show();QVERIFY(QTest::qWaitForWindowExposed(panel.get()));search->setText(QStringLiteral("安全拒否"));
        if(kind=="root-symlink")QCOMPARE(list->count(),0);
        else {QCOMPARE(list->count(),1);QTRY_VERIFY_WITH_TIMEOUT(list->item(0)->data(Qt::UserRole+3).toBool(),5000);QVERIFY(list->item(0)->icon().isNull());}
        QCOMPARE(panel->property("userPreviewGenerations").toInt(),0);QVERIFY(!QDir(config.path()+"/cache/material-previews").exists());
        QVERIFY(untouched.open(QIODevice::ReadOnly));QCOMPARE(untouched.readAll(),before);
    }

};

QTEST_MAIN(TestGuiMaterials)
#include "test_gui_materials.moc"
