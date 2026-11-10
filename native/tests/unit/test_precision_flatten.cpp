#include <QtTest/QtTest>
#include <QTemporaryDir>
#include <algorithm>
#include "core/color_raster.hpp"
#include "core/command_bus.hpp"
#include "render/ops_registry.hpp"
#include "render/page.hpp"
#include "render/png.hpp"
#include "storage/asset_store.hpp"
#include "storage/reader.hpp"
#include "storage/writer.hpp"
using namespace genko;
using core::Json;
namespace {
core::Document sample(const QString& precision) {
    auto doc=core::new_episode("背景への統合",core::Num(1),2,
        core::PageSpec::custom(20,20,16,16,1,2,2,2,2,72,"color"));
    auto& page=doc.edit_page(0);page.numero=false;page.frames.clear();page.layers.clear();
    const Json put={{"op","put_color_raster"},{"page",1},{"width",1},{"height",1},{"precision",precision.toStdString()},
        {"pixels",precision=="u16" ? Json::array({1,1025,30003,32768}) : Json::array({-.125,1.0000001192092896,2.0,.5})}};
    auto changed=core::CommandBus().apply(doc,Json::array({put,put}),core::Actor("human:test")).doc;
    changed.edit_page(0).layers.back().visible=false;
    return changed;
}
Json flatten() { return Json{{"op","merge_visible"},{"page",1},{"copy",false},{"flatten",true}}; }
}
class TestPrecisionFlatten : public QObject {
    Q_OBJECT
private slots:
    void flattenKeepsPrecisionAndDropsHidden_data() {
        QTest::addColumn<QString>("precision");
        QTest::newRow("u16") << QString("u16");QTest::newRow("f32") << QString("f32");
    }
    void flattenKeepsPrecisionAndDropsHidden() {
        QFETCH(QString,precision);const auto base=sample(precision);
        const auto raw=base.page(0).layers.front().color_raster;
        std::vector<std::string> pixels;
        for (const char* mode : {"proof","print"}) {
            render::RenderOptions opts;opts.mode=mode;
            pixels.push_back(render::render_page(base.page(0),72,opts,&base).image.tobytes());
        }
        const auto result=core::CommandBus(render::ops_registry()).apply(base,Json::array({flatten()}),core::Actor("human:test")).doc;
        QCOMPARE(result.page(0).layers.size(),std::size_t(1));
        const auto& layer=result.page(0).layers.front();
        QVERIFY2(layer.lock_alpha,"flatten must create an opaque transparency-locked background");
        QVERIFY(layer.color_raster);QVERIFY(!layer.raster_png);
        const core::ColorRasterView view(*layer.color_raster);
        QCOMPARE(view.metadata("x")["precision"].get<std::string>(),precision.toStdString());
        for (std::size_t i=0;i<std::size_t(view.width())*view.height();++i) QCOMPARE(view.pixel(i)[3],1.0);
        for (std::size_t i=0;i<pixels.size();++i) {
            render::RenderOptions opts;opts.mode=i==0 ? "proof" : "print";
            QCOMPARE(render::render_page(result.page(0),72,opts,&result).image.tobytes(),pixels[i]);
        }
        QCOMPARE(result.pages[1],base.pages[1]);QCOMPARE(base.page(0).layers.front().color_raster,raw);
        QTemporaryDir tmp;const std::filesystem::path dir(tmp.path().toStdString());storage::AssetStore assets(dir);
        const auto payload=storage::project_payload_v4(result,assets);
        const auto loaded=storage::load_document_payload(payload,dir).document;
        QVERIFY(loaded.read_only_reason.empty());
        const auto found=std::find_if(loaded.page(0).layers.begin(),loaded.page(0).layers.end(),[&](const core::Layer& l){return l.id==layer.id;});
        QVERIFY(found!=loaded.page(0).layers.end());QVERIFY(found->lock_alpha);
        QCOMPARE(*found->color_raster,*layer.color_raster);
    }
    void rgb8KeepsExistingPixels_data() {
        QTest::addColumn<QString>("expression");QTest::newRow("color")<<QString("color");QTest::newRow("gray")<<QString("gray");
    }
    void rgb8KeepsExistingPixels() {
        QFETCH(QString,expression);auto doc=sample("u16");auto& page=doc.edit_page(0);page.spec.expression=expression.toStdString();
        page.layers[0].color_raster.reset();page.layers[0].kind=core::LayerKind::Raster;
        page.layers[0].raster_png=std::make_shared<const std::string>(render::write_png(render::Image::create("RGBA",render::Size{2,2},render::Ink{20,70,190,128})));
        std::vector<std::string> before;
        for(const char* mode:{"proof","print"}) {
            render::RenderOptions o;o.mode=mode;before.push_back(render::render_page(doc.page(0),72,o,&doc).image.tobytes());
        }
        const auto after=core::CommandBus(render::ops_registry()).apply(doc,Json::array({Json{{"op","merge_visible"},{"page",1},{"copy",false},{"flatten",true}}}),core::Actor("human:tester")).doc;
        for(std::size_t i=0;i<before.size();++i) {
            render::RenderOptions o;o.mode=i==0?"proof":"print";QCOMPARE(render::render_page(after.page(0),72,o,&after).image.tobytes(),before[i]);
        }
        QVERIFY(!after.page(0).layers.front().color_raster);QVERIFY(after.page(0).layers.front().raster_png);
        QVERIFY(after.page(0).layers.front().lock_alpha);
    }
    void flattenRefusesWithoutChangingBatch_data() {
        QTest::addColumn<QString>("reason");
        for (const char* row : {"copy","hidden-locked","gate","page-lock"}) QTest::newRow(row) << QString(row);
    }
    void flattenRefusesWithoutChangingBatch() {
        QFETCH(QString,reason);auto base=sample("u16");auto op=flatten();std::string expected;
        if (reason=="copy") {op["copy"]=true;expected="flatten requires copy=false";}
        if (reason=="hidden-locked") {base.edit_page(0).layers.back().locked=true;expected="the layer is locked";}
        if (reason=="gate") {base.strict_gates=true;expected="needs name_ok";}
        if (reason=="page-lock") {base.page_locks[base.page(0).id]="human:other";expected="locked by human:other";}
        QTemporaryDir tmp;storage::AssetStore assets(std::filesystem::path(tmp.path().toStdString()));
        const auto before=storage::project_payload_v4(base,assets);bool refused=false;
        try { (void)core::CommandBus(render::ops_registry()).apply(base,Json::array({
            Json{{"op","set_note"},{"page",2},{"note","前置変更"}},op}),core::Actor("human:test")); }
        catch (const core::ApplyError& e) {refused=std::string(e.what()).find(expected)!=std::string::npos;}
        QVERIFY2(refused,expected.c_str());QCOMPARE(storage::project_payload_v4(base,assets),before);
    }
    void backgroundMatchesRenderer_data() {
        QTest::addColumn<QString>("kind");
        for(const auto& kind:{QString("multiply-white"),QString("invert-white"),QString("bg-u16"),QString("bg-f32"),QString("bg-rgb8"),QString("name-diverges"),QString("colored-paper"),QString("first-clip")})QTest::newRow(kind.toUtf8().constData())<<kind;
    }
    void backgroundMatchesRenderer() {
        QFETCH(QString,kind);core::CommandBus bus(render::ops_registry());
        QTemporaryDir tmp;QVERIFY(tmp.isValid());const std::filesystem::path dir(tmp.path().toStdString());storage::AssetStore assets(dir);
        auto doc=sample("u16");doc.edit_page(0).layers.clear();
        doc=storage::load_document_payload(storage::project_payload_v4(doc,assets),dir).document;
        QVERIFY(doc.read_only_reason.empty());
        const bool plain=kind=="multiply-white"||kind=="invert-white"||kind=="bg-rgb8"||kind=="first-clip";
        if(plain){core::Layer layer;layer.id="flatten-background-source";layer.role=core::LayerRole::Ink;
            if(kind=="multiply-white"){layer.kind=core::LayerKind::Fill;layer.fill=Json{{"kind","solid"},{"rgb",Json::array({255,255,255})}};layer.blend="multiply";}
            else if(kind=="invert-white"){layer.kind=core::LayerKind::Adjust;layer.adjust=Json{{"kind","invert"}};}
            else {layer.kind=core::LayerKind::Raster;const render::Ink color=kind=="first-clip"?render::Ink{255,0,0,255}:render::Ink{0,0,0,0};
                layer.raster_png=std::make_shared<const std::string>(render::write_png(render::Image::create("RGBA",render::Size{1,1},color)));layer.clip=kind=="first-clip";}
            doc.edit_page(0).layers.push_back(std::move(layer));
        }else{
            doc=bus.apply(doc,Json::array({Json{{"op","put_color_raster"},{"page",1},{"width",1},{"height",1},{"precision",kind=="bg-f32"?"f32":"u16"},{"pixels",Json::array({0,0,0,0})}}}),core::Actor("human:test")).doc;
        }
        if(kind.startsWith("bg-"))doc.edit_page(0).fills={{core::LayerRole::Bg,core::NumList{0,0,0}}};
        if(kind=="name-diverges")doc.edit_page(0).fills={{core::LayerRole::Name,core::NumList{0,0,0}}};
        if(kind=="colored-paper")doc.edit_page(0).extra["paper_rgb"]=Json::array({12,123,234});
        doc=storage::load_document_payload(storage::project_payload_v4(doc,assets),dir).document;
        QVERIFY(doc.read_only_reason.empty());if(kind.startsWith("bg-"))QVERIFY(doc.page(0).fill_of(core::LayerRole::Bg));
        const auto before=storage::project_payload_v4(doc,assets);render::RenderOptions options;
        options.mode="proof";const auto proof=render::render_page(doc.page(0),72,options,&doc).image;
        options.mode="print";const auto print=render::render_page(doc.page(0),72,options,&doc).image;
        core::Document result;bool refused=false;
        try{result=bus.apply(doc,Json::array({flatten()}),core::Actor("human:test")).doc;}catch(const core::ApplyError&){refused=true;}
        if(kind=="name-diverges"){QVERIFY2(refused,"divergent proof/print background must be refused without hiding a Name fill");QCOMPARE(storage::project_payload_v4(doc,assets),before);return;}
        QVERIFY2(!refused,"supported background and legacy fills must render without refusal");
        options.mode="proof";QVERIFY2(render::render_page(result.page(0),72,options,&result).image.tobytes()==proof.tobytes(),"flatten must retain paper, legacy fills, background blend/adjustment and initial clip semantics");
        options.mode="print";QVERIFY(render::render_page(result.page(0),72,options,&result).image.tobytes()==print.tobytes());
    }

};
QTEST_GUILESS_MAIN(TestPrecisionFlatten)
#include "test_precision_flatten.moc"
