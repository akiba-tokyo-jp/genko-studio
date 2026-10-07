#include <QtTest/QtTest>
#include <cmath>
#include <array>
#include "core/color_raster.hpp"
#include "core/command_bus.hpp"
#include "core/strokes.hpp"
#include "core/error.hpp"
#include "core/stroke_geom.hpp"
#include "storage/writer.hpp"
#include "storage/reader.hpp"
#include "storage/snapshot.hpp"
#include <QTemporaryDir>
#include "render/ops_registry.hpp"
#include "render/page.hpp"
#include "render/raster.hpp"
#include "render/selection.hpp"
using namespace genko;
using core::Json;
namespace {
double linear(double v) { return v <= .04045 ? v / 12.92 : std::pow((v + .055) / 1.055, 2.4); }
double encoded(double v) { return v <= .0031308 ? v * 12.92 : 1.055 * std::pow(v, 1.0 / 2.4) - .055; }
}
class TestPrecisionMergeControls : public QObject {
    Q_OBJECT
private slots:
    void a4HighPrecision600dpi_data(){QTest::addColumn<QString>("precision");QTest::newRow("u16")<<QString("u16");QTest::newRow("f32")<<QString("f32");}
    void a4HighPrecision600dpi(){
        QFETCH(QString,precision);
        auto seed=core::new_episode("A4 color",core::Num(1),1,core::PageSpec::custom(210,297,176,253,10,10,10,10,10,600,"color"));
        seed.edit_page(0).layers.clear();seed.edit_page(0).frames.clear();seed.edit_page(0).numero=false;
        Json put={{"op","put_color_raster"},{"page",1},{"width",1},{"height",1},{"precision",precision.toStdString()}};
        put["pixels"]=precision=="u16"?Json::array({13107,26214,45874,65535}):Json::array({.2,.4,.7,1});
        const auto doc=core::CommandBus().apply(seed,Json::array({put}),core::Actor("human:test")).doc;
        bool rendered=false;std::string error;
        try{
            const auto image=render::render_page(doc.page(0),600,render::proof_options(),&doc).image;
            QCOMPARE(image.width(),render::mm_to_px(210,600));QCOMPARE(image.height(),render::mm_to_px(297,600));
            const std::vector<double> expected={51,102,178};
            for(const auto& xy:std::array<std::pair<int,int>,5>{{{0,0},{511,511},{512,512},{image.width()/2,image.height()/2},{image.width()-1,image.height()-1}}})QCOMPARE(image.getpixel(xy.first,xy.second),expected);
            rendered=true;
        }catch(const std::exception& e){error=e.what();}
        QVERIFY2(rendered,("A4 high precision 600dpi render: "+error).c_str());
    }
    void tiledPrecisionMatchesUntiledRegion(){
        auto seed=core::new_episode("tile seam",core::Num(1),1,core::PageSpec::custom(26,27,22,23,1,1,1,1,1,1200,"color"));
        seed.edit_page(0).layers.clear();seed.edit_page(0).frames.clear();seed.edit_page(0).numero=false;
        Json put={{"op","put_color_raster"},{"page",1},{"width",3},{"height",2},{"precision","f32"},{"pixels",Json::array({.2,.4,.7,1.,.7,.1,.3,.4,.1,.6,.8,0.,.5,.2,.9,.7,.9,.3,.1,1.,.3,.7,.5,.3})}};
        auto doc=core::CommandBus().apply(seed,Json::array({put}),core::Actor("human:test")).doc;
        put["box_mm"]=Json::array({2,3,21,22});put["pixels"]=Json::array({.1,.8,.2,.4,.6,.2,.4,.9,.8,.7,.3,.5,.2,.5,.9,.8,.4,.3,.8,.2,.7,.9,.1,.6});
        doc=core::CommandBus().apply(doc,Json::array({put}),core::Actor("human:test")).doc;
        doc=core::CommandBus().apply(doc,Json::array({Json{{"op","set_layer"},{"page",1},{"id",doc.page(0).layers.back().id},{"blend","multiply"},{"clip",true},{"opacity",.7}}}),core::Actor("human:test")).doc;
        const auto whole=render::render_page(doc.page(0),1200,render::proof_options(),&doc).image;
        auto options=render::proof_options();options.region=render::RenderRegion{350,350,800,800};
        const auto region=render::render_page(doc.page(0),1200,options,&doc).image;
        QCOMPARE(whole.crop(render::Box{350,350,1150,1150}).tobytes(),region.tobytes());
    }
    void b4HighPrecisionMerge_data(){QTest::addColumn<QString>("precision");QTest::newRow("u16")<<QString("u16");QTest::newRow("f32")<<QString("f32");}
    void b4HighPrecisionMerge(){
        QFETCH(QString,precision);auto seed=core::new_episode("B4 merge",core::Num(1),1,core::PageSpec::custom(257,364,220,310,10,10,10,10,10,600,"color"));
        seed.edit_page(0).layers.clear();seed.edit_page(0).frames.clear();seed.edit_page(0).numero=false;
        Json put={{"op","put_color_raster"},{"page",1},{"width",1},{"height",1},{"precision",precision.toStdString()}};
        put["pixels"]=precision=="u16"?Json::array({13107,26214,45874,65535}):Json::array({.2,.4,.7,1});
        const core::CommandBus bus(render::ops_registry());auto doc=bus.apply(seed,Json::array({put,put}),core::Actor("human:test")).doc;
        const auto expected=core::ColorRasterView(*doc.page(0).layers.back().color_raster).pixel(0);
        bool merged=false;std::string error;
        try{const auto result=bus.apply(doc,Json::array({Json{{"op","merge_layers"},{"page",1},{"ids",Json::array({doc.page(0).layers[0].id,doc.page(0).layers[1].id})}}}),core::Actor("human:test")).doc;
            QCOMPARE(result.page(0).layers.size(),std::size_t(1));const core::ColorRasterView pixels(*result.page(0).layers[0].color_raster);
            QCOMPARE(pixels.metadata("")["precision"],Json(precision.toStdString()));QCOMPARE(pixels.width(),std::uint32_t(render::mm_to_px(257,200)));QCOMPARE(pixels.height(),std::uint32_t(render::mm_to_px(364,200)));
            QCOMPARE(pixels.pixel(0),expected);QCOMPARE(pixels.pixel(std::size_t(pixels.width())*pixels.height()-1),expected);merged=true;
        }catch(const std::exception& e){error=e.what();}
        QVERIFY2(merged,("B4 precision merge must retain high bits: "+error).c_str());
    }
    void precisionRasterToPen_data(){QTest::addColumn<QString>("precision");QTest::newRow("u16")<<QString("u16");QTest::newRow("f32")<<QString("f32");}
    void precisionRasterToPen(){
        QFETCH(QString,precision);
        auto seed=core::new_episode("高精度ペン変換",core::Num(1),1,core::PageSpec::custom(20,20,16,16,1,2,2,2,2,200,"color"));
        seed.edit_page(0).layers.clear();seed.edit_page(0).frames.clear();seed.edit_page(0).numero=false;
        Json samples=Json::array();
        for(int y=0;y<7;++y)for(int x=0;x<7;++x){
            if(precision=="u16")for(const auto value:{40123,1031,60001,y==3?65535:0})samples.push_back(value);
            else for(const auto value:{1.25,.125,.500001,y==3?1.:0.})samples.push_back(value);
        }
        const core::CommandBus bus(render::ops_registry());
        auto doc=bus.apply(seed,Json::array({Json{{"op","put_color_raster"},{"page",1},{"width",7},{"height",7},{"box_mm",Json::array({3,3,14,14})},{"precision",precision.toStdString()},{"pixels",samples}}}),core::Actor("human:test")).doc;
        QCOMPARE(doc.page(0).layers.size(),std::size_t(1));QVERIFY(doc.page(0).layers[0].color_raster);
        const auto id=doc.page(0).layers[0].id;
        const auto before=doc.pages[0];bool accepted=false;std::string error;
        try{
            const auto result=bus.apply(doc,Json::array({Json{{"op","convert_layer"},{"page",1},{"id",id},{"to","pen"},{"min_mm",.2},{"preserve_precision",true}}}),core::Actor("human:test")).doc;
            QVERIFY(result.page(0).layers[0].kind==core::LayerKind::Strokes);
            QVERIFY(!result.page(0).layers[0].color_raster);
            const auto& layer=result.page(0).layers[0];
            QVERIFY(layer.stroke_count()>0);
            for (const auto& stroke : layer.strokes->items) {
                QVERIFY(stroke->color_rgb);QCOMPARE((*stroke->color_rgb)["precision"],Json(precision.toStdString()));
                const auto expected=precision=="u16"?std::array<double,3>{40123/65535.,1031/65535.,60001/65535.}:std::array<double,3>{1.25,.125,double(float(.500001))};
                for (unsigned c=0;c<3;++c) QVERIFY(std::abs((*stroke->color_rgb)["values"][c].get<double>()-expected[c])<1e-7);
            }
            QTemporaryDir temp;QVERIFY(temp.isValid());
            const auto path=std::filesystem::path(temp.path().toStdString());storage::AssetStore assets(path);
            const auto payload=storage::project_payload_v4(result,assets);
            QVERIFY(std::find(result.features.begin(),result.features.end(),core::kColorStrokeFeature)!=result.features.end());
            const auto loaded=storage::load_document_payload(payload,path);QVERIFY2(loaded.report.clean(),loaded.report.to_json().dump().c_str());
            QCOMPARE(core::strokes_blob(*loaded.document.page(0).layers[0].strokes),core::strokes_blob(*layer.strokes));
            for (const bool print : {false,true}) {
                auto options=render::proof_options();options.mode=print?"print":"proof";
                const auto picture=render::render_page(result.page(0),150,options,&result).image;
                QCOMPARE(picture.tobytes(),render::render_page(loaded.document.page(0),150,options,&loaded.document).image.tobytes());
                // The converted colour is not silently rendered as the default black pen.
                const auto rgba=picture.convert("RGBA").tobytes();bool red=false;
                for (std::size_t i=0;i<rgba.size();i+=4) if (static_cast<unsigned char>(rgba[i])>static_cast<unsigned char>(rgba[i+1])+30) { red=true;break; }
                QVERIFY(red);
            }
            const auto painted=bus.apply(result,Json::array({Json{{"op","convert_layer"},{"page",1},{"id",id},{"to","paint"}}}),core::Actor("human:test")).doc;
            QVERIFY2(painted.page(0).layers[0].color_raster,"precise pen to paint must preserve a native high-precision asset");
            const core::ColorRasterView px(*painted.page(0).layers[0].color_raster);
            QCOMPARE(px.metadata("")["precision"],Json(precision.toStdString()));
            for (const bool print : {false,true}) {auto options=render::proof_options();options.mode=print?"print":"proof";
                QCOMPARE(render::render_page(painted.page(0),render::raster::kWorkingDpi,options,&painted).image.tobytes(),render::render_page(result.page(0),render::raster::kWorkingDpi,options,&result).image.tobytes());}
            accepted=true;
        }catch(const std::exception& e){error=e.what();}
        QVERIFY2(accepted,("high precision raster must become editable precise-colour pen strokes: "+error).c_str());
        QCOMPARE(doc.pages[0],before);
    }

    void preciseStrokeEdits_data() {
        QTest::addColumn<QString>("operation");for(const auto* op:{"transform","edit_stroke","simplify_stroke","recolor"})QTest::newRow(op)<<QString(op);
    }
    void preciseStrokeEdits() {
        QFETCH(QString,operation);auto seed=core::new_episode("精度付き線の編集",core::Num(1),1,core::PageSpec::custom(20,20,16,16,1,2,2,2,2,200,"color"));
        seed.edit_page(0).layers.clear();seed.edit_page(0).frames.clear();seed.edit_page(0).numero=false;
        auto stroke=core::coerce_stroke(Json{{"id","precise"},{"points",Json::array({Json::array({2.,2.}),Json::array({10.,2.}),Json::array({17.,2.})})}});
        stroke.color_rgb=Json{{"precision","f32"},{"values",Json::array({1.25,.125,.500001})}};stroke.width_mm=.8;stroke.opacity=.7;
        core::Layer layer;layer.id="native";layer.role=core::LayerRole::User;layer.panel_clip=false;
        layer.strokes=core::make_strokes({std::make_shared<const core::Stroke>(stroke)});seed.edit_page(0).layers.push_back(layer);
        const auto before=core::strokes_blob(*seed.page(0).layers[0].strokes);
        if(operation=="transform") {
            const auto moved=render::selection::transform_stroke(stroke,{1,0,0,1,1,2});
            QVERIFY2(moved.color_rgb,"precise colour must survive a geometric stroke transform");QCOMPARE(*moved.color_rgb,*stroke.color_rgb);
            QCOMPARE(moved.points[0].x,3.);QCOMPARE(moved.points[0].y,4.);
        } else {
            Json op{{"op",operation=="recolor"?"set_stroke_width":operation.toStdString()},{"page",1},{"layer","user"},{"layer_id","native"},{"index",0}};
            if(operation=="recolor") {op["ids"]=Json::array({"precise"});op["rgb"]=Json::array({0,200,20});}
            else if(operation=="edit_stroke")op["points"]=Json::array({Json::array({3.,3.}),Json::array({17.,3.})});
            else op["epsilon_mm"]=.8;
            const auto edited=core::CommandBus(render::ops_registry()).apply(seed,Json::array({op}),core::Actor("human:test")).doc;
            const auto& result=*edited.page(0).layers[0].strokes->items[0];
            if(operation=="recolor") {QVERIFY2(!result.color_rgb,"explicit RGB recolour must replace the old precise colour");QCOMPARE(*result.rgb,std::vector<std::int64_t>({0,200,20}));}
            else {QVERIFY2(result.color_rgb,"precise colour must survive editing or simplifying line points");QCOMPARE(*result.color_rgb,*stroke.color_rgb);QCOMPARE(result.width_mm,stroke.width_mm);QCOMPARE(result.opacity,stroke.opacity);}
        }
        QCOMPARE(core::strokes_blob(*seed.page(0).layers[0].strokes),before);
    }
    void preciseCrossingEraser() {
        auto stroke=core::coerce_stroke(Json{{"id","precise"},{"points",Json::array({Json::array({2.,2.}),Json::array({17.,2.})})}});
        stroke.color_rgb=Json{{"precision","f32"},{"values",Json::array({1.25,.125,.500001})}};stroke.width_mm=.8;stroke.opacity=.7;
        const auto ptr=std::make_shared<const core::Stroke>(stroke);
        const auto crossing=std::make_shared<const core::Stroke>(core::coerce_stroke(Json{{"points",Json::array({Json::array({9.,0.}),Json::array({9.,4.})})}}));
        const std::vector<core::StrokePtr> lines{ptr,crossing};const auto result=core::erase_to_crossing(lines,core::PenPoints{{4.,2.,std::nullopt}},.1);
        QVERIFY(result.size()>=2);const auto& kept=*result.front();QVERIFY(kept.points.front().x>=9.);
        QVERIFY2(kept.color_rgb,"precise colour must survive vector erasing to a crossing");QCOMPARE(*kept.color_rgb,*stroke.color_rgb);QCOMPARE(kept.opacity,stroke.opacity);QCOMPARE(kept.width_mm,stroke.width_mm);
    }
    void preciseConsumerBoundaries_data(){
        QTest::addColumn<QString>("operation");QTest::addColumn<QString>("precision");
        for(const QString& precision:{QString("u16"),QString("f32")})
            for(const char* mode:{"warp","mesh","warpMove","reconvert","reconvertDefault","filter0","filter2","pixelErase","pixelDelete","pixelMove"})
                QTest::newRow((precision+"-"+mode).toUtf8().constData())<<QString(mode)<<precision;
    }
    void preciseConsumerBoundaries(){
        QFETCH(QString,operation);QFETCH(QString,precision);
        auto doc=core::new_episode("precise consumers",core::Num(1),1,core::PageSpec::custom(20,20,18,18,0,0,1,1,1,600,"color"));
        doc.edit_page(0).layers.clear();doc.edit_page(0).frames.clear();doc.edit_page(0).numero=false;
        core::Layer layer;layer.id="native";layer.role=core::LayerRole::User;layer.kind=core::LayerKind::Strokes;layer.panel_clip=false;
        auto stroke=core::coerce_stroke(Json{{"id","line"},{"width_mm",.6},{"color_rgb",Json{{"precision",precision.toStdString()},{"values",Json::array({precision=="f32"?1.25:40123./65535,.23,.16})}}},{"points",Json::array({Json::array({2.,6.}),Json::array({18.,6.})})}});
        layer.strokes=core::make_strokes(std::vector<core::StrokePtr>{std::make_shared<const core::Stroke>(stroke)});doc.edit_page(0).layers.push_back(layer);
        if(operation.startsWith("pixel"))doc=core::CommandBus(render::ops_registry()).apply(doc,Json::array({Json{{"op","convert_layer"},{"page",1},{"id","native"},{"to","paint"}}}),core::Actor("human:test")).doc;
        QTemporaryDir temporary;QVERIFY(temporary.isValid());storage::AssetStore store(temporary.path().toStdString());
        const auto payload=storage::project_json_v4(doc,store);const auto pixels=doc.page(0).layers[0].color_raster;
        const auto before=storage::snapshot(doc,true);const Json area={{"rect",Json::array({0.,0.,20.,20.})}};
        Json op={{"page",1},{"id","native"},{"layer_id","native"}};
        if(operation=="warp"||operation=="warpMove"||operation=="mesh") {
            op["op"]="transform_area";op["area"]=area;
            const double dx=operation=="warpMove"?1.:0.;
            op["warp"]=Json{{"perspective",Json::array({Json::array({dx,0.}),Json::array({20.+dx,0.}),Json::array({20.+dx,20.}),Json::array({dx,20.})})}};
            if(operation=="mesh")op["warp"]=Json{{"grid",Json::array({2,2})},{"mesh",Json::array({Json::array({0.,0.}),Json::array({20.,0.}),Json::array({0.,20.}),Json::array({20.,20.})})}};
        }
        else if(operation.startsWith("reconvert"))op.update(Json{{"op","convert_layer"},{"to","pen"},{"preserve_precision",true}});
        else if(operation.startsWith("filter"))op.update(Json{{"op","filter_raster"},{"kind","blur"},{"radius",operation=="filter0"?0:2}});
        else if(operation=="pixelErase")op.update(Json{{"op","erase"},{"mode","cut"},{"radius_mm",1.},{"points",Json::array({Json::array({10.,0.}),Json::array({10.,20.})})}});
        else op.update(Json{{"op",operation=="pixelDelete"?"delete_area":"transform_area"},{"area",area},{"matrix",Json::array({1.,0.,0.,1.,2.,0.})}});
        if(operation=="reconvertDefault")op.erase("preserve_precision");
        if(operation=="warp"||operation=="warpMove"||operation=="mesh"||operation.startsWith("reconvert")){
            const auto changed=core::CommandBus(render::ops_registry()).apply(doc,Json::array({op}),core::Actor("human:test")).doc;
            QVERIFY(changed.page(0).layers[0].stroke_count()>0);
            for(const auto& item:changed.page(0).layers[0].strokes->items)QCOMPARE(item->color_rgb,stroke.color_rgb);
            if(operation.startsWith("reconvert"))QCOMPARE(storage::project_json_v4(changed,store),payload);
            if(operation=="warpMove")QCOMPARE(changed.page(0).layers[0].strokes->items[0]->points[0].x,3.);
            const auto path=std::filesystem::path(temporary.path().toStdString())/"book";
            storage::AssetStore output(path);const auto text=storage::project_json_v4(changed,output);
            const auto loaded=storage::load_document_text(text,path);QVERIFY(loaded.report.clean());const auto& reopened=loaded.document;
            QCOMPARE(core::strokes_blob(*reopened.page(0).layers[0].strokes),core::strokes_blob(*changed.page(0).layers[0].strokes));
        }else{
            const Json precursor{{"op","set_note"},{"page",1},{"note","successful precursor"}};
            QCOMPARE(core::CommandBus(render::ops_registry()).apply(doc,Json::array({precursor}),core::Actor("human:test")).doc.page(0).note,std::string("successful precursor"));
            bool rejected=false;try{core::CommandBus(render::ops_registry()).apply(doc,Json::array({precursor,op}),core::Actor("human:test"));}catch(const core::Error& e){
                rejected=e.code()=="not_yet_ported"&&std::string(e.what()).find("ops[1]")!=std::string::npos;
            }
            QVERIFY2(rejected,"unsupported precision consumer must reject, never silently bake or no-op");
        }
        QCOMPARE(storage::snapshot(doc,true),before);
        QCOMPARE(storage::project_json_v4(doc,store),payload);QCOMPARE(doc.page(0).layers[0].color_raster,pixels);
    }
    void preciseStrokeColorRefusals() {
        const Json good{{"precision","f32"},{"values",Json::array({1.25,.125,.500001})}};
        std::vector<Json> invalid{Json(),Json::array(),Json::object(),Json{{"precision","u8"},{"values",Json::array({1.,0.,0.})}},Json{{"precision","f32"},{"values",Json::array({1.,0.})}},Json{{"precision","f32"},{"values",Json::array({true,0.,0.})}},Json{{"precision","f32"},{"values",Json::array({std::numeric_limits<double>::infinity(),0.,0.})}},Json{{"precision","f32"},{"values",Json::array({1e100,0.,0.})}},Json{{"precision","u16"},{"values",Json::array({1.25,0.,0.})}}};
        auto unknown=good;unknown["unknown"]=true;invalid.push_back(unknown);
        for(const auto& bad:invalid)QVERIFY_EXCEPTION_THROWN(core::coerce_stroke(Json{{"color_rgb",bad},{"points",Json::array()}}),core::Error);
        const auto accepted=core::coerce_stroke(Json{{"color_rgb",good},{"points",Json::array()}});QCOMPARE(*accepted.color_rgb,good);
    }
    void blendMergeRetainsPrecision_data() {
        QTest::addColumn<QString>("precision");QTest::addColumn<QString>("mode");QTest::addColumn<QString>("expectedJson");
        // Fixed analytical double-precision oracle: PhotoCraft blend.rs, SHA256 6fb224e746bcb2f2debdf0579e836dabd6eb49ab709adab47b66bfafb01e3280.
        // This is a source-formula translation, not a claim of executing the Rust reference.
        const Json cases=core::parse_python_json(R"oracle([{"precision":"u16","mode":"normal","expected":[0.4868512122810971,0.4938597006956364,0.6804820874651879,1]},{"precision":"u16","mode":"multiply","expected":[0.17052426212992366,0.4711861588812379,0.6558377585281884,1]},{"precision":"u16","mode":"screen","expected":[0.49563452189389806,0.6121768604809253,0.812877235113674,1]},{"precision":"u16","mode":"add","expected":[0.5021130069362809,0.6197461451128621,0.8400747666677391,1]},{"precision":"u16","mode":"overlay","expected":[0.1918825522648993,0.48167681439280324,0.7253645240821057,1]},{"precision":"u16","mode":"darken","expected":[0.1983825436789502,0.4938597006956364,0.6804820874651879,1]},{"precision":"u16","mode":"lighten","expected":[0.4868512122810971,0.5951171129930571,0.7934843976501106,1]},{"precision":"u16","mode":"color_burn","expected":[0.14587441058755587,0.4603941940484701,0.6176540857789272,1]},{"precision":"u16","mode":"color_dodge","expected":[0.22957542471087922,0.6035324170235017,0.8279180043020645,1]},{"precision":"u16","mode":"linear_burn","expected":[0.14587441058755587,0.4603941940484701,0.6176540857789272,1]},{"precision":"u16","mode":"soft_light","expected":[0.19209825912052722,0.5205727626550911,0.7540714552063262,1]},{"precision":"u16","mode":"hard_light","expected":[0.1918825522648993,0.48167681439280324,0.6913462004869811,1]},{"precision":"u16","mode":"difference","expected":[0.47096253620780243,0.569107150639675,0.743004796125345,1]},{"precision":"u16","mode":"exclusion","expected":[0.4890475275619091,0.6044853988638509,0.7844307945765594,1]},{"precision":"u16","mode":"subtract","expected":[0.14587441058755587,0.569107150639675,0.743004796125345,1]},{"precision":"u16","mode":"divide","expected":[0.25062276780259274,0.8015510037401222,0.8895060576718167,1]},{"precision":"u16","mode":"hue","expected":[0.5733071703206011,0.4949848174578995,0.7017025257898631,1]},{"precision":"u16","mode":"saturation","expected":[0.2873123404195423,0.5880494275615085,0.7624811972111966,1]},{"precision":"u16","mode":"color","expected":[0.5187500429869709,0.5252265742453125,0.7017124143462056,1]},{"precision":"u16","mode":"luminosity","expected":[0.14587441058755587,0.565202184297698,0.7610498055890093,1]},{"precision":"f32","mode":"normal","expected":[0.4927103613657717,0.5015918690677189,0.6944440986169442,1]},{"precision":"f32","mode":"multiply","expected":[0.17346021023801111,0.4783604764335861,0.6685516523535653,1]},{"precision":"f32","mode":"screen","expected":[0.5010680794703394,0.6176259119974988,0.8206616560930406,1]},{"precision":"f32","mode":"add","expected":[0.5077237654895116,0.6256474972795317,0.8509003486873121,1]},{"precision":"f32","mode":"overlay","expected":[0.19529741059361821,0.48939287679322346,0.7410351274918491,1]},{"precision":"f32","mode":"darken","expected":[0.20000000298023224,0.5015918690677189,0.6944440986169442,1]},{"precision":"f32","mode":"lighten","expected":[0.4927103613657717,0.6000000238418579,0.800000011920929,1]},{"precision":"f32","mode":"color_burn","expected":[0.1482209657777894,0.4669989642115267,0.6263879456152875,1]},{"precision":"f32","mode":"color_dodge","expected":[0.23314809381366647,0.6089722829273886,0.8394646673699627,1]},{"precision":"f32","mode":"linear_burn","expected":[0.1482209657777894,0.4669989642115267,0.6263879456152875,1]},{"precision":"f32","mode":"soft_light","expected":[0.19545507183265537,0.5278498952300148,0.7651069043079405,1]},{"precision":"f32","mode":"hard_light","expected":[0.19529741059361821,0.48939287679322346,0.7075286976511849,1]},{"precision":"f32","mode":"difference","expected":[0.47709748353851583,0.5728632971062876,0.7444582845044624,1]},{"precision":"f32","mode":"exclusion","expected":[0.4942989528368274,0.6094681099606278,0.7888854190568982,1]},{"precision":"f32","mode":"subtract","expected":[0.1482209657777894,0.5728632971062876,0.7444582845044624,1]},{"precision":"f32","mode":"divide","expected":[0.24839846230797175,0.8005483350787643,0.8912558847704295,1]},{"precision":"f32","mode":"hue","expected":[0.5731394271745766,0.49989414244128355,0.714877585468418,1]},{"precision":"f32","mode":"saturation","expected":[0.28589160852355444,0.5932480268287965,0.7703960600988848,1]},{"precision":"f32","mode":"color","expected":[0.5208770789119147,0.5291669185995305,0.7129398356202655,1]},{"precision":"f32","mode":"luminosity","expected":[0.1482209657777894,0.5740251578216251,0.7728198219827924,1]}])oracle");
        QCOMPARE(cases.size(),std::size_t(40));
        for(const auto& row:cases){const QString p=QString::fromStdString(row.at("precision").get<std::string>()),mode=QString::fromStdString(row.at("mode").get<std::string>());
            QTest::newRow((p+"-"+mode).toUtf8().constData())<<p<<mode<<QString::fromStdString(row.at("expected").dump());}
    }
    void blendMergeRetainsPrecision() {
        QFETCH(QString,precision);QFETCH(QString,mode);QFETCH(QString,expectedJson);
        auto seed=core::new_episode("blend",core::Num(1),1,core::PageSpec::custom(20,20,16,16,1,2,2,2,2,72,"color"));
        seed.edit_page(0).layers.clear();seed.edit_page(0).frames.clear();seed.edit_page(0).numero=false;
        Json put={{"op","put_color_raster"},{"page",1},{"width",1},{"height",1},{"precision",precision.toStdString()}};
        put["pixels"]=precision=="u16"?Json::array({13001,39001,52001,65535}):Json::array({.2,.6,.8,1});
        auto doc=core::CommandBus().apply(seed,Json::array({put}),core::Actor("human:test")).doc;
        put["pixels"]=precision=="u16"?Json::array({45001,19001,31001,40001}):Json::array({.7,.3,.5,.6});
        doc=core::CommandBus().apply(doc,Json::array({put}),core::Actor("human:test")).doc;
        bool styled=false;std::string styleError;
        try{doc=core::CommandBus().apply(doc,Json::array({Json{{"op","set_layer"},{"page",1},{"id",doc.page(0).layers.back().id},{"blend",mode.toStdString()},{"opacity",.7}}}),core::Actor("human:test")).doc;styled=true;}
        catch(const std::exception& e){styleError=e.what();}
        QVERIFY2(styled,("supported high-precision blend must accept formal set_layer: "+styleError).c_str());
        const auto& layers=doc.page(0).layers;
        const auto expected=core::parse_python_json(expectedJson.toStdString());
        core::Document merged;render::Image before;bool accepted=false;std::string error;
        try {before=render::render_page(doc.page(0),72,render::proof_options(),&doc).image;
             merged=core::CommandBus(render::ops_registry()).apply(doc,Json::array({Json{{"op","merge_down"},{"page",1},{"id",layers[1].id}}}),core::Actor("human:test")).doc;accepted=true;}
        catch(const std::exception& e){error=e.what();}
        QVERIFY2(accepted,("high-precision blend must render and merge: "+error).c_str());
        QVERIFY(merged.page(0).layers[0].color_raster);const core::ColorRasterView actual(*merged.page(0).layers[0].color_raster);
        QCOMPARE(actual.metadata("")["precision"],Json(precision.toStdString()));
        for(unsigned c=0;c<4;++c)QVERIFY(std::abs(actual.pixel(0)[c]-expected[c].get<double>())<=(precision=="u16"?.500001/65535:3e-7));
        QCOMPARE(before.tobytes(),render::render_page(merged.page(0),72,render::proof_options(),&merged).image.tobytes());
    }

    void finiteHdrDoesNotProduceInvalidSuccess_data(){QTest::addColumn<double>("opacity");QTest::newRow("overflow")<<1.0;QTest::newRow("zero-weight")<<0.0;}
    void finiteHdrDoesNotProduceInvalidSuccess(){
        QFETCH(double,opacity);
        auto build=[&](bool multiply){
            auto doc=core::new_episode("HDR合成保護",core::Num(1),1,core::PageSpec::custom(20,20,16,16,1,2,2,2,2,72,"color"));doc.edit_page(0).numero=false;doc.edit_page(0).frames.clear();doc.edit_page(0).layers.clear();
            Json put={{"op","put_color_raster"},{"page",1},{"width",1},{"height",1},{"precision","f32"},{"pixels",Json::array({1e38,1e38,1e38,1})}};
            doc=core::CommandBus().apply(doc,Json::array({put,put,put,put}),core::Actor("human:test")).doc;
            if(multiply){Json ops=Json::array();for(std::size_t i=1;i<4;++i)ops.push_back(Json{{"op","set_layer"},{"page",1},{"id",doc.page(0).layers[i].id},{"blend","multiply"},{"opacity",i==3?opacity:1.0}});doc=core::CommandBus().apply(doc,ops,core::Actor("human:test")).doc;}
            put["pixels"]=Json::array({1,0,0,1});return core::CommandBus().apply(doc,Json::array({put}),core::Actor("human:test")).doc;
        };
        const auto normal=build(false);const auto expected=std::vector<double>{255,0,0};QCOMPARE(render::render_page(normal.page(0),72,render::proof_options(),&normal).image.getpixel(0,0),expected);
        const auto doc=build(true);const auto before=doc.pages[0];bool refused=false,correct=false;std::string error;
        try{const auto image=render::render_page(doc.page(0),72,render::proof_options(),&doc).image;correct=image.getpixel(0,0)==expected;}
        catch(const core::Error&e){error=e.what();refused=error.find("finite working range")!=std::string::npos;}
        QVERIFY2(correct || (opacity==1 && refused),"finite HDR composite must produce correct opaque cover or refuse explicitly, never return invalid pixels");QCOMPARE(doc.pages[0],before);
        if(opacity==0)QVERIFY(correct);
        else{
            bool mergeRefused=false;
            try{core::CommandBus(render::ops_registry()).apply(doc,Json::array({Json{{"op","set_note"},{"page",1},{"note","前置変更"}},Json{{"op","merge_visible"},{"page",1},{"copy",false},{"flatten",true}}}),core::Actor("human:test"));}
            catch(const core::ApplyError&e){mergeRefused=std::string(e.what()).find("finite working range")!=std::string::npos;}
            QVERIFY(mergeRefused);QCOMPARE(doc.pages[0],before);QVERIFY(doc.page(0).note.empty());QCOMPARE(doc.page(0).layers.size(),std::size_t(5));
        }
    }
    void blendWithExternalBackdropRefuses_data() {
        QTest::addColumn<QString>("precision");for(const auto& p:{QString("u16"),QString("f32")})QTest::newRow(p.toUtf8().constData())<<p;
    }
    void blendWithExternalBackdropRefuses() {
        QFETCH(QString,precision);
        auto seed=core::new_episode("外部背景",core::Num(1),1,core::PageSpec::custom(20,20,16,16,1,2,2,2,2,72,"color"));
        seed.edit_page(0).layers.clear();seed.edit_page(0).frames.clear();seed.edit_page(0).numero=false;
        Json put={{"op","put_color_raster"},{"page",1},{"width",1},{"height",1},{"precision",precision.toStdString()},{"id","lower"},
            {"pixels",precision=="u16"?Json::array({13001,39001,52001,32768}):Json::array({.2,.6,.8,.5})}};
        auto doc=core::CommandBus().apply(seed,Json::array({put}),core::Actor("human:test")).doc;put["id"]="upper";
        doc=core::CommandBus().apply(doc,Json::array({put}),core::Actor("human:test")).doc;
        const auto upperId=doc.page(0).layers.back().id;
        doc=core::CommandBus().apply(doc,Json::array({Json{{"op","set_layer"},{"page",1},{"id",upperId},{"blend","multiply"}}}),core::Actor("human:test")).doc;
        const auto before=doc.pages[0];const auto raw=doc.page(0).layers[0].color_raster;
        bool refused=false;try {core::CommandBus(render::ops_registry()).apply(doc,Json::array({Json{{"op","merge_down"},{"page",1},{"id",upperId}}}),core::Actor("human:test"));}
        catch(const core::ApplyError& e){refused=std::string(e.what()).find("depends on external background")!=std::string::npos;}
        QVERIFY2(refused,"non-normal merge must not lose the external backdrop contribution");QCOMPARE(doc.pages[0],before);QCOMPARE(doc.page(0).layers[0].color_raster,raw);
        const auto image=render::render_page(doc.page(0),72,render::proof_options(),&doc).image;
        const auto flat=core::CommandBus(render::ops_registry()).apply(doc,Json::array({Json{{"op","merge_visible"},{"page",1},{"flatten",true},{"copy",false}}}),core::Actor("human:test")).doc;
        QCOMPARE(render::render_page(flat.page(0),72,render::proof_options(),&flat).image.tobytes(),image.tobytes());
        QVERIFY(flat.page(0).layers.front().color_raster);QCOMPARE(core::ColorRasterView(*flat.page(0).layers.front().color_raster).pixel(0)[3],1.0);
    }

    void alphaAndInternalClipping_data() {
        QTest::addColumn<QString>("precision");QTest::addColumn<QString>("operation");QTest::addColumn<bool>("clip");
        for (const auto& precision : {QString("u16"),QString("f32")})
            for (const auto& operation : {QString("merge_down"),QString("merge_layers"),QString("merge_visible")})
                for (bool clip : {false,true})
                    QTest::newRow((precision+"-"+operation+(clip ? "-clip" : "-normal")).toUtf8().constData()) << precision << operation << clip;
    }
    void alphaAndInternalClipping() {
        QFETCH(QString,precision);QFETCH(QString,operation);QFETCH(bool,clip);
        auto seed=core::new_episode("alpha",core::Num(1),2,core::PageSpec::custom(20,20,16,16,1,2,2,2,2,72,"color"));
        seed.edit_page(0).layers.clear();seed.edit_page(0).frames.clear();seed.edit_page(0).numero=false;
        Json put={{"op","put_color_raster"},{"page",1},{"width",1},{"height",1},{"precision",precision.toStdString()}};
        put["pixels"]=precision=="u16" ? Json::array({1,1025,30003,49151}) : Json::array({-.125,1.25,2.0,.75});
        auto base=core::CommandBus().apply(seed,Json::array({put}),core::Actor("human:test")).doc;
        put["pixels"]=precision=="u16" ? Json::array({51111,17,19340,26214}) : Json::array({.0625,-.125,.25,.4});
        base=core::CommandBus().apply(base,Json::array({put}),core::Actor("human:test")).doc;
        auto& layers=base.edit_page(0).layers;layers[0].opacity=.3;layers[1].opacity=.7;layers[1].clip=clip;
        const auto bottom=layers[0].color_raster,top=layers[1].color_raster;
        const auto b=core::ColorRasterView(*bottom).pixel(0),u=core::ColorRasterView(*top).pixel(0);
        const auto lowerID=layers[0].id,upperID=layers[1].id;
        // Genko clips to the preceding source alpha before layer opacity (Python render.py and page.cpp).
        const double ba=b[3]*.3,ua=u[3]*.7*(clip ? b[3] : 1),a=ua+ba*(1-ua);
        std::array<double,4> expected{};
        for (unsigned c=0;c<3;++c) expected[c]=encoded((linear(u[c])*ua+linear(b[c])*ba*(1-ua))/a);
        expected[3]=a;
        Json op={{"op",operation.toStdString()},{"page",1}};
        if (operation=="merge_down") op["id"]=upperID;
        if (operation=="merge_layers") op["ids"]=Json::array({lowerID,upperID});
        if (operation=="merge_visible") op["copy"]=false;
        const auto result=core::CommandBus(render::ops_registry()).apply(base,Json::array({op}),core::Actor("human:test"));
        QCOMPARE(result.doc.page(0).layers.size(),std::size_t(1));
        const auto raw=result.doc.page(0).layers.front().color_raster;QVERIFY(raw);
        const auto actual=core::ColorRasterView(*raw).pixel(0);
        const double tolerance=precision=="u16" ? .500001/65535 : 3e-7;
        for (unsigned c=0;c<4;++c) QVERIFY2(std::abs(actual[c]-expected[c])<=tolerance,qPrintable(QStringLiteral("straight-alpha formula: c%1 actual=%2 expected=%3 ba=%4 ua=%5").arg(c).arg(actual[c],0,'g',17).arg(expected[c],0,'g',17).arg(ba,0,'g',17).arg(ua,0,'g',17)));
        QCOMPARE(base.page(0).layers[0].color_raster,bottom);QCOMPARE(base.page(0).layers[1].color_raster,top);
        render::RenderOptions options;options.mode="proof";
        const auto before=render::render_page(base.page(0),72,options,&base).image;
        const auto after=render::render_page(result.doc.page(0),72,options,&result.doc).image;
        QCOMPARE(after.tobytes(),before.tobytes());
    }
};
QTEST_GUILESS_MAIN(TestPrecisionMergeControls)
#include "test_precision_merge_controls.moc"
