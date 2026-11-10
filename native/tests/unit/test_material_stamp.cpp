#include <QtTest/QtTest>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include "core/command_bus.hpp"
#include "core/paths.hpp"
#include "core/pynum.hpp"
#include "render/ops_registry.hpp"
#include "render/page.hpp"
#include "render/png.hpp"
using namespace genko;
using core::Json;
class TestMaterialStamp : public QObject {
    Q_OBJECT
private slots:
    void builtinEditableKinds_data() {
        QTest::addColumn<QString>("id");QTest::addColumn<QString>("kind");
        QTest::newRow("effect")<<QString("speed-h")<<QString("effect");
        QTest::newRow("brush")<<QString::fromUtf8("brush-雨ブラシ")<<QString("brush");
        QTest::newRow("prim")<<QString::fromUtf8("3d-箱")<<QString("prim");
    }
    void builtinEditableKinds() {
        QFETCH(QString,id);QFETCH(QString,kind);
        const auto doc=core::new_episode("素材配置",core::Num(1),2,core::PageSpec::custom(20,20,16,16,1,2,2,2,2,72,"mono"));
        core::Document result;bool accepted=false;std::string error;
        try {result=core::CommandBus(render::ops_registry()).apply(doc,Json::array({
            Json{{"op","stamp_material"},{"page",1},{"material_id",id.toStdString()},{"id","stamp-test"},{"x_mm",10},{"y_mm",12}}}),core::Actor("human:test")).doc;accepted=true;}
        catch(const core::ApplyError& e){error=e.what();}
        QVERIFY2(accepted,("editable material subtype must be implemented: "+error).c_str());
        if(kind=="effect") {QCOMPARE(result.page(0).effects.size(),std::size_t(1));const auto& e=result.page(0).effects.front();
            QCOMPARE(e.at("id").get<std::string>(),std::string("stamp-test"));QCOMPARE(e.at("kind").get<std::string>(),std::string("speed"));QCOMPARE(e.at("params").at("center"),Json::array({10.0,12.0}));}
        if(kind=="brush") {const std::string key="my_"+QCryptographicHash::hash(id.toUtf8(),QCryptographicHash::Sha1).toHex().left(10).toStdString();
            QVERIFY(result.brush_custom.contains(key));QCOMPARE(result.brush_custom.at(key).at("pattern").get<std::string>(),std::string("dash"));
            QCOMPARE(result.brush_custom.at(key).at("width_mm").get<double>(),3.0);
            QCOMPARE(result.brush_custom.at(key).at("count").get<int>(),2);}
        if(kind=="prim") {QCOMPARE(result.page(0).prims.size(),std::size_t(1));const auto& p=result.page(0).prims.front();QCOMPARE(p.at("id").get<std::string>(),std::string("stamp-test"));QCOMPARE(p.at("kind").get<std::string>(),std::string("box"));QCOMPARE(p.at("pos"),Json::array({10.0,12.0,0}));}
        QCOMPARE(doc.pages[1],result.pages[1]);
    }
    void refusedBatchAndBrushDefinitionKeepOriginal() {
        const auto doc=core::new_episode("素材拒否",core::Num(1),1,core::PageSpec::custom(20,20,16,16,1,2,2,2,2,72,"mono"));
        const auto initial=doc.pages[0];core::CommandBus bus(render::ops_registry());
        for(const Json& ops:Json::array({
            Json::array({Json{{"op","stamp_material"},{"page",1},{"material_id","unknown"}}}),
            Json::array({Json{{"op","stamp_material"},{"page",1},{"material_id","dot-60-30"}},Json{{"op","stamp_material"},{"page",1},{"material_id","unknown"}}}),
            Json::array({Json{{"op","define_brush"},{"key","gpen"}}}),
            Json::array({Json{{"op","define_brush"},{"key","my_bad"},{"opacity",2}}})})) {
            QVERIFY_EXCEPTION_THROWN(bus.apply(doc,ops,core::Actor("human:test")),core::ApplyError);
            QCOMPARE(doc.pages[0],initial);QVERIFY(doc.brush_custom.empty());
        }
        auto gated=doc;gated.strict_gates=true;gated.page_locks[gated.page(0).id]="human:other";
        QVERIFY_EXCEPTION_THROWN(bus.apply(gated,Json::array({Json{{"op","stamp_material"},{"page",1},{"material_id","dot-60-30"}}}),core::Actor("ai:test")),core::ApplyError);
        QCOMPARE(doc.pages[0],initial);
        QVERIFY_EXCEPTION_THROWN(bus.apply(doc,Json::array({Json{{"op","name_ok"},{"page",1}}}),core::Actor("ai:test")),core::ApplyError);
        const auto made=bus.apply(doc,Json::array({Json{{"op","define_brush"},{"key","my_valid"},{"base","dashline"},{"width_mm",3}}}),core::Actor("human:test")).doc;
        const auto gone=bus.apply(made,Json::array({Json{{"op","define_brush"},{"key","my_valid"},{"delete",true}}}),core::Actor("human:test")).doc;
        QVERIFY(gone.brush_custom.empty());QVERIFY(made.brush_custom.contains("my_valid"));
    }

    void sceneDuplicateIdRefusesWithoutChangingOriginal_data() {
        QTest::addColumn<QString>("existingId");QTest::addColumn<QString>("givenJson");
        QTest::newRow("string")<<QString("shared")<<QString("\"shared\"");
        QTest::newRow("integer")<<QString("42")<<QString("42");
        QTest::newRow("boolean")<<QString("True")<<QString("true");
    }
    void sceneDuplicateIdRefusesWithoutChangingOriginal() {
        QFETCH(QString,existingId);QFETCH(QString,givenJson);const auto given=core::parse_python_json(givenJson.toStdString());
        const auto doc=core::new_episode("scene拒否",core::Num(1),1,core::PageSpec::custom(20,20,16,16,1,2,2,2,2,72,"mono"));
        core::CommandBus bus(render::ops_registry());
        const auto existing=bus.apply(doc,Json::array({Json{{"op","add_prim3d"},{"page",1},{"kind","box"},{"id",existingId.toStdString()},{"pos",Json::array({10,12,0})}}}),core::Actor("human:test")).doc;
        const auto before=existing.page(0).prims;bool refused=false;
        try {bus.apply(existing,Json::array({Json{{"op","stamp_material"},{"page",1},{"material_id","3d-部屋（3D）"},{"id",given},{"x_mm",9},{"y_mm",8}}}),core::Actor("human:test"));}
        catch(const core::ApplyError& e){refused=std::string(e.what()).find("duplicate 3D id")!=std::string::npos;}
        QVERIFY2(refused,"duplicate scene ID must refuse before existing prim can be overwritten");
        QCOMPARE(existing.page(0).prims,before);
        const auto placed=bus.apply(existing,Json::array({Json{{"op","stamp_material"},{"page",1},{"material_id","3d-部屋（3D）"},{"id",17},{"x_mm",9},{"y_mm",8}}}),core::Actor("human:test")).doc;
        QCOMPARE(placed.page(0).prims.size(),std::size_t(2));QCOMPARE(placed.page(0).prims[0],before[0]);
        const auto& scene=placed.page(0).prims[1];QCOMPARE(scene.at("id"),Json("17"));QCOMPARE(scene.at("kind"),Json("scene"));
        QCOMPARE(scene.at("pos")[0],Json(9.0));QCOMPARE(scene.at("pos")[1],Json(8.0));QVERIFY(scene.at("pos")[2].get<double>()>0);
    }

    void invalidLibraryMustRefuseBeforeAdoption_data(){
        QTest::addColumn<QString>("failure");for(const QString& value:{QString("bad-patch-default"),QString("bad-patch-centered"),QString("missing-box"),QString("short-box"),QString("huge-box"),QString("overflow-width"),QString("overflow-aspect"),QString("nan-width"),QString("infinite-width")})QTest::newRow(value.toUtf8().constData())<<value;
    }
    void invalidLibraryMustRefuseBeforeAdoption(){
        QFETCH(QString,failure);QTemporaryDir cfg;QVERIFY(cfg.isValid());const auto previous=qgetenv("GENKO_CONFIG_DIR");
        struct Restore{QByteArray value;~Restore(){if(value.isNull())qunsetenv("GENKO_CONFIG_DIR");else qputenv("GENKO_CONFIG_DIR",value);}}restore{previous};qputenv("GENKO_CONFIG_DIR",cfg.path().toUtf8());
        const QString root=cfg.path()+"/materials";QVERIFY(QDir().mkpath(root));
        render::save_png(render::Image::create("RGBA",{2,1},render::Ink{12,123,234,255}),core::path_from_utf8((root+"/u-blue.png").toStdString()));
        QFile source(root+"/u-blue.png");QVERIFY(source.open(QIODevice::ReadOnly));const QByteArray original=source.readAll();source.close();
        const bool lines=failure.contains("patch")||failure.contains("box");
        Json entry={{"id","u-invalid"},{"kind",lines?"lines":"image"},{"name","拒否対照"},{"file","u-blue.png"},{"width_mm",12},{"aspect",.5}};
        if(lines){Json patch={{"box",Json::array({0,0,20,20})},{"mode","image"},{"opacity",1},{"png",failure.contains("bad-patch")?std::string("QQ=="):original.toBase64().toStdString()}};
            if(failure=="missing-box"){patch.erase("box");}if(failure=="short-box"){patch["box"]=Json::array({0,0,20});}if(failure=="huge-box"){patch["box"]=Json::array({0,0,1e308,20});}
            entry["items"]=Json{{"patches",Json::array({patch})}};
        }else entry[failure=="overflow-aspect"?"aspect":"width_mm"]="RAW_NONFINITE_NUMBER";
        QByteArray bytes=QByteArray::fromStdString(Json::array({entry}).dump());if(!lines)bytes.replace(QByteArray::fromStdString(Json("RAW_NONFINITE_NUMBER").dump()),failure=="nan-width"?"NaN":failure=="infinite-width"?"Infinity":"1e400");
        QFile library(root+"/library.json");QVERIFY(library.open(QIODevice::WriteOnly));QCOMPARE(library.write(bytes),qint64(bytes.size()));library.close();
        const auto doc=core::new_episode("素材拒否",core::Num(1),2,core::PageSpec::custom(20,20,16,16,1,2,2,2,2,72,"mono"));const auto pages=doc.pages;
        Json op={{"op","stamp_material"},{"page",1},{"material_id","u-invalid"}};if(failure=="bad-patch-centered"){op["x_mm"]=10;op["y_mm"]=10;}
        bool accepted=false;try{(void)core::CommandBus(render::ops_registry()).apply(doc,Json::array({Json{{"op","set_note"},{"page",2},{"note","前置変更"}},op}),core::Actor("human:test"));accepted=true;}catch(const core::ApplyError&){}
        QVERIFY2(!accepted,"invalid library must refuse before document/history/save adoption");QCOMPARE(doc.pages,pages);QVERIFY(doc.page(1).note.empty());for(const auto& layer:doc.page(0).layers)QVERIFY(layer.patches.empty());
        QVERIFY(source.open(QIODevice::ReadOnly));QCOMPARE(source.readAll(),original);QVERIFY(library.open(QIODevice::ReadOnly));QCOMPARE(library.readAll(),bytes);
    }
    void userImageLibraryPlacement_data(){QTest::addColumn<QString>("failure");for(const QString& kind:{QString("image"),QString("outside-path"),QString("absolute"),QString("image-symlink"),QString("root-symlink"),QString("broken-image"),QString("locked-layer"),QString("width-infinite"),QString("geometry-overflow")})QTest::newRow(kind.toUtf8().constData())<<kind;}
    void userImageLibraryPlacement(){
        QFETCH(QString,failure);const bool unsafe=failure!="image";QTemporaryDir cfg;QVERIFY(cfg.isValid());const auto previous=qgetenv("GENKO_CONFIG_DIR");
        struct Restore{QByteArray value;~Restore(){if(value.isNull())qunsetenv("GENKO_CONFIG_DIR");else qputenv("GENKO_CONFIG_DIR",value);}}restore{previous};qputenv("GENKO_CONFIG_DIR",cfg.path().toUtf8());
        const auto root=cfg.path()+"/materials";QVERIFY(QDir().mkpath(root));const auto source=core::path_from_utf8((root+"/u-blue.png").toStdString());render::save_png(render::Image::create("RGBA",{2,1},render::Ink{12,123,234,255}),source);
        QFile picture(root+"/u-blue.png");QVERIFY(picture.open(QIODevice::ReadOnly));auto original=picture.readAll();picture.close();
        if(failure=="broken-image"){original=original.left(33);QVERIFY(picture.open(QIODevice::WriteOnly));QCOMPARE(picture.write(original),qint64(original.size()));picture.close();}
        const auto outside=cfg.path()+"/outside.png";QFile external(outside);QVERIFY(external.open(QIODevice::WriteOnly));QCOMPARE(external.write(original),qint64(original.size()));external.close();
        QString filename="u-blue.png";if(failure=="outside-path")filename="../outside.png";if(failure=="absolute")filename=outside;
        if(failure=="image-symlink"){filename="u-link.png";QVERIFY(QFile::link(outside,root+"/"+filename));}
        const auto library=Json::array({Json{{"id","u-blue"},{"kind","image"},{"name","青い画像"},{"file",filename.toStdString()},{"width_mm",failure=="geometry-overflow"?Json(1e308):Json(12)},{"aspect",failure=="geometry-overflow"?2.0:.5}}}).dump();QFile manifest(root+"/library.json");QVERIFY(manifest.open(QIODevice::WriteOnly));QCOMPARE(manifest.write(QByteArray::fromStdString(library)),qint64(library.size()));manifest.close();
        auto doc=core::new_episode("画像素材",core::Num(1),2,core::PageSpec::custom(20,20,16,16,1,2,2,2,2,72,"mono"));doc.edit_page(0).numero=false;if(failure=="locked-layer")for(auto& layer:doc.edit_page(0).layers){if(layer.role==core::LayerRole::Ink)layer.locked=true;}
        if(failure=="root-symlink"){QVERIFY(QDir().rename(root,cfg.path()+"/kept-original-library"));QVERIFY(QFile::link(cfg.path()+"/kept-original-library",root));}
        auto result=doc;bool accepted=false;
        try{result=core::CommandBus(render::ops_registry()).apply(doc,Json::array({Json{{"op","set_note"},{"page",2},{"note","前置変更"}},Json{{"op","stamp_material"},{"page",1},{"material_id","u-blue"},{"x_mm",10},{"y_mm",10},{"width_mm",failure=="width-infinite"?Json("inf"):Json()}}}),core::Actor("human:test")).doc;accepted=true;}catch(const core::ApplyError&){}
        if(unsafe){QVERIFY(!accepted);QVERIFY(doc.page(1).note.empty());}
        else{QVERIFY2(accepted,"user image must be placed from the existing read-only library");const auto& layers=result.page(0).layers;const auto& target=*std::find_if(layers.begin(),layers.end(),[](const auto&l){return l.role==core::LayerRole::Ink;});QCOMPARE(target.patches.size(),std::size_t(1));QCOMPARE(*target.patches[0].png,original.toStdString());QCOMPARE(target.patches[0].attrs["box"],Json::array({4.0,7.0,12.0,6.0}));const auto image=render::render_page(result.page(0),72,render::proof_options()).image;QCOMPARE(image.getpixel(image.width()/2,image.height()/2),(std::vector<double>{12,123,234}));}
        QVERIFY(manifest.open(QIODevice::ReadOnly));QCOMPARE(manifest.readAll(),QByteArray::fromStdString(library));QVERIFY(picture.open(QIODevice::ReadOnly));QCOMPARE(picture.readAll(),original);QVERIFY(external.open(QIODevice::ReadOnly));QCOMPARE(external.readAll(),original);
    }
    void drawnBuiltinKinds_data(){QTest::addColumn<QString>("material");QTest::newRow("lines")<<QString("mark-汗");QTest::newRow("lettering")<<QString("sfx-ドーン");}
    void letteringPutsALineAsTheMaterialHasIt() {
        // 描き文字 (ops._apply_one): a line set as the material has it, centred where asked and as wide as asked (the
        // page's middle and the material's size otherwise), in the panel and with the id given; the page itself is not
        // touched
        const auto doc = core::new_episode("描き文字", core::Num(1), 2, core::PageSpec::b4_comic());
        const auto before = doc.pages[0];
        const std::string panel = doc.page(0).frames[0].id;
        const auto result = core::CommandBus(render::ops_registry()).apply(doc, Json::array({
            Json{{"op", "set_note"}, {"page", 2}, {"note", "前置変更"}},
            Json{{"op", "stamp_material"}, {"page", 1}, {"material_id", "sfx-ゴゴゴ"}, {"x_mm", 50}, {"y_mm", 60}, {"width_mm", 20},
                 {"frame_id", panel}, {"id", "sfx-1"}},
            Json{{"op", "stamp_material"}, {"page", 1}, {"material_id", "sfx-ヒュー"}}}), core::Actor("human:test")).doc;
        QCOMPARE(result.story.size(), std::size_t(2));
        const core::StoryLine& down = result.story[0];
        QCOMPARE(down.id, std::string("sfx-1"));
        QCOMPARE(down.text, std::string("ゴゴゴゴ"));
        QCOMPARE(down.wrap, std::string("vertical"));
        QCOMPARE(down.balloon, std::string("sfx"));
        QVERIFY(down.frame_id == panel);
        QCOMPARE(down.x_mm.json(), Json(40.0));
        QCOMPARE(down.y_mm.json(), Json(32.0));
        QCOMPARE(down.w_mm.json(), Json(20.0));
        QCOMPARE(down.h_mm.json(), Json(56.0));
        QCOMPARE(down.style, (Json{{"outline_mm", 1.0}, {"spike_jitter", 0.0}, {"skew_deg", 10.0}}));
        const core::StoryLine& across = result.story[1];
        const double middle = doc.page(0).spec.width_mm.value() / 2;
        QCOMPARE(across.x_mm.json(), Json(core::py_round(middle - 30, 2)));
        QCOMPARE(across.w_mm.json(), Json(60.0));
        QCOMPARE(across.h_mm.json(), Json(25.0));
        QCOMPARE(across.balloon, std::string("none"));
        QCOMPARE(across.wrap, std::string("horizontal"));
        QVERIFY(!across.frame_id);
        QCOMPARE(across.style.at("font"), Json("sfx"));
        const Json path = Json::array({Json::array({0.0, 20.0}), Json::array({20.0, 5.0}), Json::array({40.0, 15.0}), Json::array({60.0, 0.0})});
        QCOMPARE(across.style.at("text_path"), path);
        QCOMPARE(result.pages[0], before);
        QCOMPARE(result.page(1).note, std::string("前置変更"));
        const auto drawn = render::render_page(result.page(0), 72, render::proof_options(), &result).image;
        QVERIFY(drawn.tobytes() != render::render_page(doc.page(0), 72, render::proof_options(), &doc).image.tobytes());
    }
    void pictureBalloonFromTheLibrary_data() {
        QTest::addColumn<QString>("material");
        QTest::addColumn<QString>("line");
        QTest::addColumn<QString>("refusal");
        QTest::newRow("balloon") << QString("u-blue") << QString("l1") << QString();
        QTest::newRow("no line") << QString("u-blue") << QString("nope") << QString("no line nope");
        QTest::newRow("missing") << QString("u-gone") << QString("l1") << QString("the picture file of this material is missing");
        QTest::newRow("missing, no line") << QString("u-gone") << QString("nope") << QString("the picture file of this material is missing");
        QTest::newRow("missing in a folder") << QString("u-sub-gone") << QString("l1") << QString("the picture file of this material is missing");
        QTest::newRow("missing in a folder, no line") << QString("u-sub-gone") << QString("nope") << QString("the picture file of this material is missing");
        // (a name this build does not read — a whole path, a way out of the library, a link — is refused before the file
        // system is asked anything about it: before Python's own errors, its missing picture and its line)
        QTest::newRow("outside") << QString("u-out") << QString("l1") << QString("unsafe material image source");
        QTest::newRow("outside, no line") << QString("u-out") << QString("nope") << QString("unsafe material image source");
        QTest::newRow("outside and missing") << QString("u-out-gone") << QString("l1") << QString("unsafe material image source");
        QTest::newRow("whole path and missing") << QString("u-whole-gone") << QString("l1") << QString("material image must be inside the material library");
        QTest::newRow("whole path, no line") << QString("u-whole") << QString("nope") << QString("material image must be inside the material library");
#ifndef Q_OS_WIN
        QTest::newRow("a link") << QString("u-link") << QString("l1") << QString("unsafe material image source");
        QTest::newRow("a link to nothing") << QString("u-dangling") << QString("l1") << QString("unsafe material image source");
        QTest::newRow("through a linked folder") << QString("u-through") << QString("l1") << QString("unsafe material image source");
        QTest::newRow("missing through a linked folder") << QString("u-through-gone") << QString("l1") << QString("unsafe material image source");
#endif
        QTest::newRow("not a picture") << QString("u-text") << QString("l1") << QString("not a readable image");
    }
    void pictureBalloonFromTheLibrary() {
        // 画像のフキダシ: an image material with a line_id becomes that line's balloon (its picture, base64, in the line's
        // style). A name this build does not read (a whole path, a way out of the library, a link) is refused first,
        // before the file system is asked anything about it; for the others Python's errors come first (the picture
        // missing, then the line), then what this build refuses (one it cannot draw); the library never written
        QFETCH(QString, material);
        QFETCH(QString, line);
        QFETCH(QString, refusal);
        QTemporaryDir cfg;
        QVERIFY(cfg.isValid());
        struct Restore {
            QByteArray value;
            ~Restore() {
                if (value.isNull()) qunsetenv("GENKO_CONFIG_DIR");
                else qputenv("GENKO_CONFIG_DIR", value);
            }
        } restore{qgetenv("GENKO_CONFIG_DIR")};
        qputenv("GENKO_CONFIG_DIR", cfg.path().toUtf8());
        const QString root = cfg.path() + "/materials";
        QVERIFY(QDir().mkpath(root));
        const auto blue = render::Image::create("RGBA", {2, 1}, render::Ink{12, 123, 234, 255});
        render::save_png(blue, core::path_from_utf8((root + "/u-blue.png").toStdString()));
        render::save_png(blue, core::path_from_utf8((cfg.path() + "/outside.png").toStdString()));
        QFile text(root + "/notes.png");
        QVERIFY(text.open(QIODevice::WriteOnly));
        text.write("not a picture");
        text.close();
        QVERIFY(QDir().mkpath(root + "/sub"));
#ifndef Q_OS_WIN
        QVERIFY(QFile::link(cfg.path() + "/outside.png", root + "/u-link.png"));
        QVERIFY(QFile::link(cfg.path() + "/nowhere.png", root + "/u-dangling.png"));
        QVERIFY(QFile::link(cfg.path(), root + "/linked"));
#endif
        const std::string whole = (cfg.path() + "/outside.png").toStdString();
        const std::string whole_gone = (cfg.path() + "/gone.png").toStdString();
        const std::string library = Json::array({Json{{"id", "u-blue"}, {"kind", "image"}, {"name", "青"}, {"file", "u-blue.png"}},
                                                 Json{{"id", "u-gone"}, {"kind", "image"}, {"name", "ない"}, {"file", "gone.png"}},
                                                 Json{{"id", "u-sub-gone"}, {"kind", "image"}, {"name", "中にない"}, {"file", "sub/gone.png"}},
                                                 Json{{"id", "u-out"}, {"kind", "image"}, {"name", "外"}, {"file", "../outside.png"}},
                                                 Json{{"id", "u-out-gone"}, {"kind", "image"}, {"name", "外にない"}, {"file", "../gone.png"}},
                                                 Json{{"id", "u-whole"}, {"kind", "image"}, {"name", "全体"}, {"file", whole}},
                                                 Json{{"id", "u-whole-gone"}, {"kind", "image"}, {"name", "全体にない"}, {"file", whole_gone}},
                                                 Json{{"id", "u-link"}, {"kind", "image"}, {"name", "リンク"}, {"file", "u-link.png"}},
                                                 Json{{"id", "u-dangling"}, {"kind", "image"}, {"name", "先のないリンク"}, {"file", "u-dangling.png"}},
                                                 Json{{"id", "u-through"}, {"kind", "image"}, {"name", "リンクのフォルダ"}, {"file", "linked/outside.png"}},
                                                 Json{{"id", "u-through-gone"}, {"kind", "image"}, {"name", "リンクのフォルダにない"}, {"file", "linked/gone.png"}},
                                                 Json{{"id", "u-text"}, {"kind", "image"}, {"name", "文字"}, {"file", "notes.png"}}}).dump();
        QFile manifest(root + "/library.json");
        QVERIFY(manifest.open(QIODevice::WriteOnly));
        manifest.write(QByteArray::fromStdString(library));
        manifest.close();
        QFile picture(root + "/u-blue.png");
        QVERIFY(picture.open(QIODevice::ReadOnly));
        const QByteArray original = picture.readAll();
        picture.close();
        core::CommandBus bus(render::ops_registry());
        auto doc = core::new_episode("画像のフキダシ", core::Num(1), 2, core::PageSpec::custom(40, 40, 36, 36, 1, 2, 2, 2, 2, 72, "color"));
        doc.edit_page(0).numero = false;
        doc = bus.apply(doc, Json::array({Json{{"op", "add_line"}, {"page", 1}, {"text", "台詞"}, {"id", "l1"}, {"x_mm", 4}, {"y_mm", 4},
                                                {"w_mm", 30}, {"h_mm", 20}, {"style", Json{{"font", "gothic"}}}}}), core::Actor("human:test")).doc;
        const Json ops = Json::array({Json{{"op", "set_note"}, {"page", 2}, {"note", "前置変更"}},
                                      Json{{"op", "stamp_material"}, {"page", 1}, {"material_id", material.toStdString()}, {"line_id", line.toStdString()}}});
        if (refusal.isEmpty()) {
            const auto result = bus.apply(doc, ops, core::Actor("human:test")).doc;
            const core::StoryLine& balloon = result.story.front();
            QCOMPARE(balloon.balloon, std::string("picture"));
            QCOMPARE(balloon.style, (Json{{"font", "gothic"}, {"picture", original.toBase64().toStdString()}}));
            QCOMPARE(result.pages[0], doc.pages[0]);
            const auto image = render::render_page(result.page(0), 72, render::proof_options(), &result).image;
            const auto corner = image.getpixel(render::mm_to_px(6, 72), render::mm_to_px(6, 72));  // (the picture stretched over the box)
            QVERIFY(corner[0] < 40 && corner[2] > 200);
        } else {
            std::string error;
            try {
                (void)bus.apply(doc, ops, core::Actor("human:test"));
            } catch (const core::ApplyError& e) {
                error = e.what();
            }
            QVERIFY2(error.find(refusal.toStdString()) != std::string::npos, error.c_str());
            QCOMPARE(doc.story.front().balloon, std::string("speech"));
            QVERIFY(doc.page(1).note.empty());
        }
        QVERIFY(manifest.open(QIODevice::ReadOnly));
        QCOMPARE(manifest.readAll(), QByteArray::fromStdString(library));
        QVERIFY(picture.open(QIODevice::ReadOnly));
        QCOMPARE(picture.readAll(), original);
    }
    void drawnBuiltinKinds(){
        QFETCH(QString,material);auto initial=core::new_episode("描画素材",core::Num(1),2,core::PageSpec::custom(20,20,16,16,1,2,2,2,2,72,"mono"));
        initial.edit_page(0).layers.clear();initial.edit_page(0).numero=false;auto bus=core::CommandBus(render::ops_registry());
        const Json op={{"op","stamp_material"},{"page",1},{"material_id",material.toStdString()},{"x_mm",10},{"y_mm",10},{"width_mm",12}};
        auto stamped=initial;bool accepted=false;std::string error;try{stamped=bus.apply(initial,Json::array({op}),core::Actor("human:test")).doc;accepted=true;}catch(const std::exception&e){error=e.what();}
        QVERIFY2(accepted,("drawn material must be editable and render: "+error).c_str());
        if(material.startsWith("mark-")){
            QCOMPARE(stamped.page(0).layers.size(),std::size_t(1));QCOMPARE(stamped.page(0).layers[0].stroke_count(),std::size_t(2));
            const auto firstId=stamped.page(0).layers[0].strokes->items[0]->id;
            const auto twice=bus.apply(stamped,Json::array({op}),core::Actor("human:test")).doc;
            QCOMPARE(twice.page(0).layers[0].stroke_count(),std::size_t(4));QVERIFY(firstId!=twice.page(0).layers[0].strokes->items[2]->id);
        }else{QCOMPARE(stamped.story.size(),std::size_t(1));const auto&l=stamped.story[0];QCOMPARE(l.text,std::string("ドーン"));QCOMPARE(l.balloon,std::string("sfx"));QCOMPARE(l.wrap,std::string("horizontal"));QCOMPARE(l.style["outline_mm"],Json(1.2));QCOMPARE(l.x_mm.value(),4.0);QCOMPARE(l.w_mm.value(),12.0);}
        const auto before=render::render_page(initial.page(0),72,render::proof_options(),&initial).image;
        const auto after=render::render_page(stamped.page(0),72,render::proof_options(),&stamped).image;QVERIFY(before.tobytes()!=after.tobytes());
        QCOMPARE(initial.pages[0]->layers.size(),std::size_t(0));
    }
    void builtinToneCopiesEditableParameters() {
        const auto doc=core::new_episode("素材配置",core::Num(1),2,
            core::PageSpec::custom(20,20,16,16,1,2,2,2,2,72,"mono"));
        core::Document result;bool accepted=false;std::string error;
        try { result=core::CommandBus(render::ops_registry()).apply(doc,Json::array({
            Json{{"op","stamp_material"},{"page",1},{"material_id","dot-60-30"},{"id","test-tone"},
                 {"area",Json{{"poly",Json::array({Json::array({2,2}),Json::array({18,2}),Json::array({18,18}),Json::array({2,18})})}}}}}),core::Actor("human:test")).doc;accepted=true; }
        catch (const core::ApplyError& e) {error=e.what();}
        QVERIFY2(accepted,("stamp_material must be implemented: "+error).c_str());
        const auto& tone=result.page(0).layers.back();QCOMPARE(tone.id,std::string("test-tone"));
        QCOMPARE(tone.kind,core::LayerKind::Tone);QCOMPARE(tone.lpi.value(),60.0);QCOMPARE(tone.density.value(),0.3);
        QCOMPARE(tone.material_id.value_or(""),std::string("dot-60-30"));
        QVERIFY(!tone.patches.empty());QCOMPARE(doc.page(0).layers.size()+1,result.page(0).layers.size());QCOMPARE(doc.pages[1],result.pages[1]);
    }
};
QTEST_GUILESS_MAIN(TestMaterialStamp)
#include "test_material_stamp.moc"
