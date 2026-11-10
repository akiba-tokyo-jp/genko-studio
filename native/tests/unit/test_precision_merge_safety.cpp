#include <QtTest/QtTest>
#include <cmath>
#include "core/color_raster.hpp"
#include "core/exposure.hpp"
#include "core/command_bus.hpp"
#include "render/ops_registry.hpp"
#include "render/page.hpp"
#include "render/png.hpp"
#include <QTemporaryDir>
#include "storage/asset_store.hpp"
#include "storage/writer.hpp"
using namespace genko;
using core::Json;
class TestPrecisionMergeSafety : public QObject {
    Q_OBJECT
    static core::Document scene(bool fp) {
        auto doc=core::new_episode("精度結合安全対照",core::Num(1),2,core::PageSpec::custom(20,20,16,16,1,2,2,2,2,72,"color"));
        auto& page=doc.edit_page(0);page.layers.clear();page.frames.clear();page.numero=false;
        return core::CommandBus().apply(doc,Json::array({{{"op","put_color_raster"},{"page",1},{"precision",fp?"f32":"u16"},{"width",1},{"height",1},{"pixels",{0,0,0,0}}}}),core::Actor("human:test")).doc;
    }
    static core::Layer raster(std::string id,render::Ink rgb) {
        core::Layer l;l.id=std::move(id);l.role=core::LayerRole::User;l.kind=core::LayerKind::Raster;l.exportable=true;
        l.raster_png=std::make_shared<const std::string>(render::write_png(render::Image::create("RGBA",{1,1},rgb)));return l;
    }
private slots:
    void reviewedSafety_data() {
        QTest::addColumn<bool>("fp");QTest::addColumn<QString>("issue");QTest::addColumn<QString>("operation");
        for(bool fp:{false,true})for(const QString issue:{"L1-tint","L1-proof-print","L2-exposure","L2-opaque","L2-core","L3-tail-clip","L4-colour-space","L5-guide","L6-gate","S1-fill","S2-adjust-tail","S3-empty","F1-empty-fill","F2-fill-gap"})
            for(const QString operation:{"merge_down","merge_layers","merge_visible"}) {
                if(issue=="L6-gate"&&operation!="merge_down")continue;
                auto tag=QString("%1-%2-%3").arg(fp?"f32":"u16",issue,operation).toLatin1();
                QTest::newRow(tag.constData())<<fp<<issue<<operation;
            }
    }
    void reviewedSafety() {
        QFETCH(bool,fp);QFETCH(QString,issue);QFETCH(QString,operation);
        auto base=scene(fp);auto& page=base.edit_page(0);const auto native=page.layers.front();
        bool reject=true;std::vector<std::string> selected;
        if(issue.startsWith("F")) {
            page.layers.front().color_raster=std::make_shared<const std::string>(core::encode_color_raster(Json{{"width",1},{"height",1},{"precision",fp?"f32":"u16"},{"pixels",fp?Json::array({1.,0.,0.,1.}):Json::array({65535,0,0,65535})}}));
            const auto red_id=page.layers.front().id;
            std::string transparent_id;
            if(issue=="F2-fill-gap") { auto transparent=native;transparent.id="transparent";page.layers.push_back(transparent);transparent_id=transparent.id; }
            base=core::CommandBus().apply(base,Json::array({{{"op","add_layer"},{"page",1},{"kind","fill"},{"id","cleared-fill"},{"rgb",{0,255,0}}},{{"op","set_layer"},{"page",1},{"id","cleared-fill"},{"fill",nullptr}}}),core::Actor("human:test")).doc;
            if(issue=="F2-fill-gap") {
                auto adjust=raster("clipped-adjust",{0,0,0,0});adjust.kind=core::LayerKind::Adjust;adjust.raster_png.reset();
                adjust.adjust=Json{{"kind","exposure"},{"exposure",-1}};adjust.clip=true;base.edit_page(0).layers.push_back(adjust);
                base.features.push_back(std::string(core::kExposureFeature));
                selected={red_id,transparent_id};reject=operation!="merge_visible";
            } else { selected={red_id,"cleared-fill"};reject=false; }
        } else if(issue.startsWith("L1")) {
            auto top=raster("tinted",{0,0,0,255});top.color=Json::array({255,0,0});top.color_prints=issue=="L1-tint";
            page.layers.push_back(top);selected={native.id,top.id};reject=!top.color_prints;
        } else if(issue=="S1-fill") {
            auto top=raster("filled",{0,0,0,0});top.kind=core::LayerKind::Fill;top.raster_png.reset();
            top.fill=Json{{"rgb",{0,0,0}}};top.color=Json::array({255,0,0});top.color_prints=true;
            page.layers.push_back(top);selected={native.id,top.id};reject=false;
        } else if(issue=="S2-adjust-tail") {
            page.layers.front().color_raster=std::make_shared<const std::string>(core::encode_color_raster(Json{{"width",1},{"height",1},{"precision",fp?"f32":"u16"},{"pixels",fp?Json::array({1.,0.,0.,1.}):Json::array({65535,0,0,65535})}}));
            const auto opaque_id=page.layers.front().id;
            auto transparent=native;transparent.id="transparent";page.layers.push_back(transparent);
            auto adjust=raster("clipped-adjust",{0,0,0,0});adjust.kind=core::LayerKind::Adjust;adjust.raster_png.reset();
            adjust.adjust=Json{{"kind","exposure"},{"exposure",-1}};adjust.clip=true;page.layers.push_back(adjust);
            base.features.push_back(std::string(core::kExposureFeature));
            selected={opaque_id,transparent.id};reject=operation!="merge_visible";
        } else if(issue=="S3-empty") {
            page.layers.front().color_raster=std::make_shared<const std::string>(core::encode_color_raster(Json{{"width",1},{"height",1},{"precision",fp?"f32":"u16"},{"pixels",fp?Json::array({1.,0.,0.,1.}):Json::array({65535,0,0,65535})}}));
            const auto opaque_id=page.layers.front().id;
            auto empty=raster("empty",{0,0,0,0});empty.raster_png.reset();empty.kind=core::LayerKind::Strokes;page.layers.push_back(empty);
            auto filled=raster("clipped-fill",{0,0,0,0});filled.raster_png.reset();filled.kind=core::LayerKind::Fill;
            filled.fill=Json{{"rgb",{0,255,0}}};filled.clip=true;page.layers.push_back(filled);
            selected=operation=="merge_down"?std::vector<std::string>{empty.id,filled.id}:std::vector<std::string>{opaque_id,empty.id,filled.id};reject=false;
        } else if(issue.startsWith("L2")) {
            if(issue!="L2-exposure") {
                Json rgb=fp?Json::array({.5,.25,.125,1}):Json::array({32768,16384,8192,65535});
                page.layers.front().color_raster=std::make_shared<const std::string>(core::encode_color_raster({{"precision",fp?"f32":"u16"},{"width",1},{"height",1},{"pixels",rgb}}));
                reject=false;
            }
            core::Layer top;top.id="exposure";top.role=issue=="L2-core"?core::LayerRole::Finish:core::LayerRole::User;top.kind=core::LayerKind::Adjust;top.exportable=true;
            top.adjust=Json{{"kind","exposure"},{"exposure",-1}};page.layers.push_back(top);
            base.features.push_back(std::string(core::kExposureFeature));selected={native.id,top.id};
        } else if(issue=="L3-tail-clip") {
            auto bottom=raster("red",{255,0,0,255});auto top=raster("clipped-green",{0,255,0,255});top.clip=true;
            page.layers={bottom,native,top};selected={bottom.id,native.id};reject=operation!="merge_visible";
        } else if(issue=="L4-colour-space") {
            auto bottom=raster("red",{255,0,0,255});auto top=raster("blue",{0,0,255,255});top.opacity=.5;
            page.layers={bottom,top,native};selected={bottom.id,top.id};reject=false;
        } else if(issue=="L5-guide") {
            auto guide=raster("name",{0,0,0,0});guide.role=core::LayerRole::Name;
            auto colour=native;Json pixels=fp?Json::array({1,0,0,1}):Json::array({65535,0,0,65535});
            colour.color_raster=std::make_shared<const std::string>(core::encode_color_raster({{"precision",fp?"f32":"u16"},{"width",1},{"height",1},{"pixels",pixels}}));
            page.layers={guide,colour};selected={guide.id,colour.id};reject=operation!="merge_visible";
        } else {
            auto bottom=raster("finish",{0,0,0,0});bottom.role=core::LayerRole::Finish;
            auto top=native;top.exportable=false;page.layers={bottom,top};page.name_ok=false;
            base=core::CommandBus().apply(base,Json::array({{{"op","set_meta"},{"strict_gates",true}}}),core::Actor("human:test")).doc;
            selected={bottom.id,top.id};
        }
        Json op={{"op",operation.toStdString()},{"page",1}};
        if(operation=="merge_down")op["id"]=selected.back();
        else if(operation=="merge_layers")op["ids"]=selected;
        else op["copy"]=false;
        QTemporaryDir tmp;QVERIFY(tmp.isValid());storage::AssetStore assets(std::filesystem::path(tmp.path().toStdString()));
        const auto initial=storage::project_payload_v4(base,assets);
        const auto proof=render::render_page(base.page(0),72,render::proof_options(),&base).image.tobytes();
        render::RenderOptions opts;opts.mode="print";opts.crop_marks=false;
        const auto printed=render::render_page(base.page(0),72,opts,&base).image.tobytes();
        const auto batch=Json::array({{{"op","set_note"},{"page",2},{"note","前置変更"}},op});
        bool refused=false;std::optional<core::Document> out;
        try {out=core::CommandBus(render::ops_registry()).apply(base,batch,core::Actor("human:test")).doc;}
        // (L6-gate: strict_gates is Python's, which looks at the named layer only; this merge is refused all the same,
        // before the book changes)
        catch(const core::ApplyError&) {refused=true;}
        catch(const render::NotYetPorted&) {refused=true;}
        if(reject)QVERIFY2(refused,"reviewed unsafe merge must be rejected before document mutation");
        else {
            QVERIFY2(!refused,"representable colour merge must remain supported");QVERIFY(out.has_value());
            QVERIFY2(render::render_page(out->page(0),72,render::proof_options(),&*out).image.tobytes()==proof,"reviewed colour merge changed proof pixels");
            QVERIFY2(render::render_page(out->page(0),72,opts,&*out).image.tobytes()==printed,"reviewed colour merge changed print pixels");
        }
        QVERIFY(storage::project_payload_v4(base,assets)==initial);
    }
};
QTEST_GUILESS_MAIN(TestPrecisionMergeSafety)
#include "test_precision_merge_safety.moc"
