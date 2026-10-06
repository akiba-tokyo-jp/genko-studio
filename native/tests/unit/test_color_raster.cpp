#include <QtTest/QtTest>
#include <algorithm>
#include <QTemporaryDir>
#include "core/command_bus.hpp"
#include "core/model.hpp"
#include "core/color_raster.hpp"
#include "storage/asset_store.hpp"
#include "storage/reader.hpp"
#include "storage/transaction.hpp"
#include "storage/writer.hpp"
#include "render/page.hpp"
#include "render/ops_registry.hpp"
using namespace genko;
using core::Json;
class TestColorRaster : public QObject {
    Q_OBJECT
private slots:
    void repeatedColorAssetSharesBytes_data() {
        QTest::addColumn<QString>("precision");
        QTest::newRow("u16") << QStringLiteral("u16");
        QTest::newRow("f32") << QStringLiteral("f32");
    }
    void repeatedColorAssetSharesBytes() {
        QFETCH(QString,precision);
        QTemporaryDir tmp; QVERIFY(tmp.isValid());
        const std::filesystem::path dir(tmp.path().toStdString());
        auto doc=core::new_episode("素材の構造共有",core::Num(1),2,
            core::PageSpec::custom(20,20,16,16,1,2,2,2,2,72,"color"));
        const Json put={{"op","put_color_raster"},{"page",1},{"width",1},{"height",1},
            {"precision",precision.toStdString()},{"pixels",precision=="u16" ?
                Json::array({1000,1001,1002,65535}) : Json::array({0.1,-0.125,2.0,1.0})}};
        doc=core::CommandBus().apply(doc,Json::array({put}),core::Actor("human:test")).doc;
        const auto original=doc.page(0).layers.back().color_raster;
        auto copy=doc.page(0).layers.back(); copy.id="shared-second";
        doc.edit_page(0).layers.push_back(copy);
        storage::AssetStore assets(dir);
        const auto payload=storage::project_payload_v4(doc,assets);
        const auto loaded=storage::load_document_payload(payload,dir);
        QVERIFY2(loaded.document.read_only_reason.empty(),loaded.document.read_only_reason.c_str());
        const auto& layers=loaded.document.page(0).layers;
        const auto a=layers.at(layers.size()-2).color_raster;
        const auto b=layers.back().color_raster;
        QVERIFY(a && b);
        QCOMPARE(*a,*original); QCOMPARE(*b,*original);
        QVERIFY2(a==b,"one immutable high-precision asset must not be allocated once per layer");
    }
    void sharedColorAssetBudget() {
        QTemporaryDir tmp; QVERIFY(tmp.isValid());
        const std::filesystem::path dir(tmp.path().toStdString());
        auto doc=core::new_episode("共有素材の予算",core::Num(1),2,
            core::PageSpec::custom(20,20,16,16,1,2,2,2,2,72,"color"));
        const Json put={{"op","put_color_raster"},{"page",1},{"width",1},{"height",1},
            {"precision","f32"},{"pixels",Json::array({0.1,-0.125,2.0,1.0})}};
        doc=core::CommandBus().apply(doc,Json::array({put}),core::Actor("human:test")).doc;
        storage::AssetStore assets(dir);
        auto payload=storage::project_payload_v4(doc,assets);
        // A valid 2048x2048 f32 fixture: one HDR pixel and transparent zero padding.
        // Four references exceed 256MiB only if the same allocation is charged four times.
        auto bytes=*doc.page(0).layers.back().color_raster;
        bytes.resize(16+2048*2048*16,0);
        for (const std::size_t offset : {std::size_t(8),std::size_t(12)}) {
            bytes[offset]=0; bytes[offset+1]=8; bytes[offset+2]=0; bytes[offset+3]=0;
        }
        const auto ref=assets.put_bytes(bytes,core::kColorRasterSuffix);
        auto layer=payload["pages"][0]["layers"].back();
        layer["color_raster"]=core::ColorRasterView(bytes).metadata(ref);
        for (int i=0;i<4;++i) {
            layer["id"]="shared-large-"+std::to_string(i);
            if (i==0) payload["pages"][0]["layers"].back()=layer;
            else payload["pages"][i%2]["layers"].push_back(layer);
        }
        const auto loaded=storage::load_document_payload(payload,dir);
        QVERIFY2(loaded.document.read_only_reason.empty(),loaded.document.read_only_reason.c_str());
        core::Bytes shared;
        int references=0;
        for (const auto& page : loaded.document.pages) for (const auto& l : page->layers) {
            if (!l.color_raster) continue;
            if (!shared) shared=l.color_raster;
            QVERIFY(l.color_raster==shared); ++references;
        }
        QCOMPARE(references,4); QCOMPARE(*shared,bytes);
        auto independent=loaded.document;
        for (std::size_t i=0;i<independent.pages.size();++i) {
            auto& page=independent.edit_page(i);
            for (auto& l : page.layers) if (l.color_raster)
                l.color_raster=std::make_shared<const std::string>(*shared);
        }
        // Same content in separate live allocations still exceeds the unchanged cap.
        QVERIFY_EXCEPTION_THROWN(core::validate_color_document(independent),core::Error);
        const auto saved=storage::project_payload_v4(loaded.document,assets);
        const auto reloaded=storage::load_document_payload(saved,dir);
        QVERIFY(reloaded.document.read_only_reason.empty());
        QCOMPARE(*reloaded.document.page(0).layers.back().color_raster,bytes);
    }
    void sharedColorAssetStillChecksEachLayer_data() {
        QTest::addColumn<QString>("precision"); QTest::addColumn<QString>("badField");
        for (const auto& precision : {QStringLiteral("u16"),QStringLiteral("f32")})
            for (const auto& bad : {QStringLiteral("width"),QStringLiteral("precision"),QStringLiteral("role")})
                QTest::newRow((precision+"-"+bad).toUtf8().constData()) << precision << bad;
    }
    void sharedColorAssetStillChecksEachLayer() {
        QFETCH(QString,precision); QFETCH(QString,badField);
        QTemporaryDir tmp; QVERIFY(tmp.isValid());
        const std::filesystem::path dir(tmp.path().toStdString());
        auto doc=core::new_episode("共有素材の層検証",core::Num(1),2,
            core::PageSpec::custom(20,20,16,16,1,2,2,2,2,72,"color"));
        const Json put={{"op","put_color_raster"},{"page",1},{"width",1},{"height",1},
            {"precision",precision.toStdString()},{"pixels",precision=="u16" ?
                Json::array({1000,1001,1002,65535}) : Json::array({0.1,-0.125,2.0,1.0})}};
        doc=core::CommandBus().apply(doc,Json::array({put}),core::Actor("human:test")).doc;
        const auto original=doc.page(0).layers.back().color_raster;
        auto copy=doc.page(0).layers.back(); copy.id="shared-invalid-second";
        doc.edit_page(0).layers.push_back(copy);
        storage::AssetStore assets(dir);
        auto payload=storage::project_payload_v4(doc,assets);
        auto& bad=payload["pages"][0]["layers"].back();
        if (badField=="role") bad["role"]="tone";
        else if (badField=="width") bad["color_raster"]["width"]=2;
        else bad["color_raster"]["precision"]=precision=="u16" ? "f32" : "u16";
        const auto loaded=storage::load_document_payload(payload,dir);
        QVERIFY(!loaded.document.read_only_reason.empty());
        const auto& layers=loaded.document.page(0).layers;
        const auto first=layers.at(layers.size()-2).color_raster;
        QVERIFY(first); QCOMPARE(*first,*original);
        if (badField=="role") {
            QVERIFY(first==layers.back().color_raster);
            QVERIFY(loaded.document.read_only_reason.find("high-precision color layer style")!=std::string::npos);
        } else {
            QVERIFY(!layers.back().color_raster);
            QVERIFY(std::any_of(loaded.report.issues.begin(),loaded.report.issues.end(),
                [](const storage::LoadIssue& issue){return issue.kind=="broken_asset";}));
        }
    }
    void inactiveHighPrecisionDoesNotChangeLegacyPixels_data() {
        QTest::addColumn<QString>("precisionName"); QTest::addColumn<QString>("modeName");
        QTest::addColumn<bool>("visible"); QTest::addColumn<bool>("exportable");
        for (const QString& precision : {QStringLiteral("u16"),QStringLiteral("f32")}) {
            for (const QString& mode : {QStringLiteral("print"),QStringLiteral("proof"),QStringLiteral("name")}) {
                const auto label=precision+"-hidden-"+mode;
                QTest::newRow(label.toUtf8().constData()) << precision << mode << false << true;
            }
            const auto label=precision+"-non-exported-print";
            QTest::newRow(label.toUtf8().constData()) << precision << QStringLiteral("print") << true << false;
        }
    }
    void inactiveHighPrecisionDoesNotChangeLegacyPixels() {
        QFETCH(QString,precisionName); QFETCH(QString,modeName); QFETCH(bool,visible); QFETCH(bool,exportable);
        auto doc=core::new_episode("非表示素材の画素対照",core::Num(1),2,core::PageSpec::custom(20,20,16,16,1,2,2,2,2,72,"color"));
        auto& page=doc.edit_page(0); page.numero=false; page.frames.clear(); page.layers.clear();
        core::Layer red; red.id="red"; red.kind=core::LayerKind::Fill; red.panel_clip=false;
        red.fill=Json{{"rgb",Json::array({255,0,0})}}; red.opacity=0.5;
        core::Layer blue=red; blue.id="blue"; blue.fill=Json{{"rgb",Json::array({0,0,255})}};
        page.layers={red,blue};
        render::RenderOptions options; options.mode=modeName.toStdString();
        const auto baseline=render::render_page(page,72,options).image;
        QCOMPARE(baseline.getpixel(baseline.width()/2,baseline.height()/2),(std::vector<double>{128,64,191}));
        const Json put={{"op","put_color_raster"},{"page",1},{"width",1},{"height",1},
            {"precision",precisionName.toStdString()},{"pixels",precisionName=="u16" ? Json::array({1000,1001,1002,65535}) : Json::array({0.1,0.2,0.3,1.0})}};
        auto withColor=core::CommandBus().apply(doc,Json::array({put}),core::Actor("human:test")).doc;
        auto& layer=withColor.edit_page(0).layers.back(); layer.visible=visible; layer.exportable=exportable;
        const auto originalBytes=layer.color_raster;
        const auto actual=render::render_page(withColor.page(0),72,options).image;
        QCOMPARE(actual.mode(),baseline.mode()); QCOMPARE(actual.width(),baseline.width()); QCOMPARE(actual.height(),baseline.height());
        for (int y=0;y<baseline.height();++y) for (int x=0;x<baseline.width();++x)
            QCOMPARE(actual.getpixel(x,y),baseline.getpixel(x,y));
        QCOMPARE(withColor.page(0).layers.back().color_raster,originalBytes);
    }
    void invalidExposureOnOrdinaryBookBecomesReadOnly() {
        QTemporaryDir tmp;
        auto doc=core::new_episode("露光量保護",core::Num(1),2,core::PageSpec::b5_doujin());
        const auto valid=core::CommandBus().apply(doc,Json::array({Json{{"op","add_layer"},{"page",1},{"layer","finish"},
                            {"kind","adjust"},{"adjust",Json{{"kind","exposure"}}}}}),core::Actor("human:test")).doc;
        storage::AssetStore assets(std::filesystem::path(tmp.path().toStdString()));
        const auto original=storage::project_payload_v4(valid,assets);
        QVERIFY(storage::load_document_payload(original,std::filesystem::path(tmp.path().toStdString())).document.read_only_reason.empty());
        for (const Json& bad : {Json{{"kind","exposure"},{"gamma","nan"}},Json{{"kind","exposure"},{"gamma",Json::array({1})}},
                               Json{{"kind","exposure"},{"unknown",1}}}) {
            auto payload=original;
            payload["pages"][0]["layers"].back()["adjust"]=bad;
            payload["pages"][0]["layers"].back()["visible"]=false;
            const auto loaded=storage::load_document_payload(payload,std::filesystem::path(tmp.path().toStdString()));
            QVERIFY2(!loaded.document.read_only_reason.empty(),core::dump_canonical(bad).c_str());
        }
    }
    void canonicalSnapshotRetainsHighPrecisionMetadata() {
        QTemporaryDir tmp;
        auto doc=core::new_episode("履歴",core::Num(1),2,core::PageSpec::custom(20,20,16,16,1,2,2,2,2,72,"color"));
        const Json put={{"op","put_color_raster"},{"page",1},{"width",1},{"height",1},
                        {"precision","u16"},{"pixels",Json::array({1,2,3,65535})}};
        const auto changed=core::CommandBus().apply(doc,Json::array({put}),core::Actor("human:test")).doc;
        storage::AssetStore assets(std::filesystem::path(tmp.path().toStdString()));
        const auto payload=storage::project_payload_v4(changed,assets);
        const auto canonical=core::parse_python_json(core::dump_canonical(payload));
        const auto loaded=storage::load_document_payload(canonical,std::filesystem::path(tmp.path().toStdString()));
        QVERIFY2(loaded.document.read_only_reason.empty(),loaded.document.read_only_reason.c_str());
        QCOMPARE(*loaded.document.page(0).layers.back().color_raster,*changed.page(0).layers.back().color_raster);
    }
    void boundedAssetReadEnforcesOpenedBytes() {
        QTemporaryDir tmp;
        storage::AssetStore assets(std::filesystem::path(tmp.path().toStdString()));
        const auto ref=assets.put_bytes("0123456789abcdef", ".colorrgba");
        QCOMPARE(*assets.get_bytes(ref,".colorrgba",16),std::string("0123456789abcdef"));
        bool refused=false;
        try { (void)assets.get_bytes(ref,".colorrgba",15); }
        catch(const core::Error&) { refused=true; }
        QVERIFY(refused);
    }
    void toneRoleCannotHideUnsupportedPrecisionStyle() {
        QTemporaryDir tmp;
        auto doc=core::new_episode("トーン拒否",core::Num(1),2,core::PageSpec::custom(20,20,16,16,1,2,2,2,2,72,"color"));
        const Json put={{"op","put_color_raster"},{"page",1},{"width",1},{"height",1},
                        {"precision","u16"},{"pixels",Json::array({1000,1001,1002,65535})}};
        auto base=core::CommandBus().apply(doc,Json::array({put}),core::Actor("human:test")).doc;
        storage::AssetStore assets(std::filesystem::path(tmp.path().toStdString()));
        auto payload=storage::project_payload_v4(base,assets);
        payload["pages"][0]["layers"].back()["role"]="tone";
        const auto loaded=storage::load_document_payload(payload,std::filesystem::path(tmp.path().toStdString()));
        QVERIFY(!loaded.document.read_only_reason.empty());
        base.edit_page(0).layers.back().role=core::LayerRole::Tone;
        bool saved=false, rendered=false;
        try { (void)storage::project_payload_v4(base,assets); saved=true; } catch(const core::Error&) {}
        try { (void)render::render_page(base.page(0),72); rendered=true; } catch(const core::Error&) {}
        QVERIFY(!saved);
        QVERIFY(!rendered);
    }
    void finiteHdrExposureStaysWhite() {
        auto doc = core::new_episode("HDR",core::Num(1),2,core::PageSpec::custom(20,20,16,16,1,2,2,2,2,72,"color"));
        doc.edit_page(0).numero=false;
        const Json put={{"op","put_color_raster"},{"page",1},{"width",1},{"height",1},
                        {"precision","f32"},{"pixels",Json::array({1e10,1e10,1e10,1.0})}};
        const Json adjust={{"op","add_layer"},{"page",1},{"layer","finish"},{"kind","adjust"},
                           {"adjust",Json{{"kind","exposure"}}}};
        const auto changed=core::CommandBus().apply(doc,Json::array({put,adjust}),core::Actor("human:test")).doc;
        const auto image=render::render_page(changed.page(0),72).image;
        QCOMPARE(image.getpixel(image.width()/2,image.height()/2),(std::vector<double>{255,255,255}));
    }
    void mergeCannotEraseHighPrecisionSource() {
        auto doc = core::new_episode("精度喪失拒否", core::Num(1), 2,
            core::PageSpec::custom(20,20,16,16,1,2,2,2,2,72,"color"));
        const Json put = {{"op","put_color_raster"},{"page",1},{"width",1},{"height",1},
                         {"precision","u16"},{"pixels",Json::array({1000,1001,1002,65535})}};
        const auto base = core::CommandBus().apply(doc,Json::array({put}),core::Actor("human:test")).doc;
        const auto bytes = base.page(0).layers.back().color_raster;
        bool rejected = false;
        try {
            (void)core::CommandBus(render::ops_registry()).apply(base,Json::array({
                Json{{"op","set_note"},{"page",2},{"note","前置変更"}},
                Json{{"op","merge_down"},{"page",1},{"id",base.page(0).layers.back().id}}}),core::Actor("human:test"));
        } catch (const core::ApplyError& e) {
            rejected = std::string(e.what()).find("high-precision") != std::string::npos;
        }
        QVERIFY(rejected);
        QVERIFY(base.page(1).note.empty());
        QCOMPARE(base.page(0).layers.back().color_raster,bytes);
    }
    void exposureOnlyBookAdvertisesRequiredFeature() {
        QTemporaryDir tmp;
        auto doc = core::new_episode("８ビット露光量", core::Num(1), 2, core::PageSpec::b4_comic());
        const Json op = {{"op", "add_layer"}, {"page", 1}, {"layer", "finish"}, {"kind", "adjust"},
                         {"adjust", Json{{"kind", "exposure"}, {"exposure", 1.0}}}};
        const auto result = core::CommandBus().apply(doc, Json::array({op}), core::Actor("human:test"));
        storage::AssetStore assets(std::filesystem::path(tmp.path().toStdString()));
        const auto payload = storage::project_payload_v4(result.doc, assets);
        QVERIFY(std::find(payload.at("features").begin(), payload.at("features").end(), "native.exposure_v1") != payload.at("features").end());
    }
    void unsupportedColorStyleRollsBackWholeBatch() {
        auto doc = core::new_episode("拒否試験", core::Num(1), 2, core::PageSpec::b4_comic());
        const Json put = {{"op", "put_color_raster"}, {"page", 1}, {"width", 1}, {"height", 1},
                         {"precision", "u16"}, {"pixels", Json::array({1000, 1001, 1002, 65535})}};
        const auto base = core::CommandBus().apply(doc, Json::array({put}), core::Actor("human:test")).doc;
        const auto bytes = base.page(0).layers.back().color_raster;
        bool rejected = false;
        try {
            (void)core::CommandBus().apply(base, Json::array({
                Json{{"op", "set_note"}, {"page", 2}, {"note", "前置変更"}},
                Json{{"op", "set_layer"}, {"page", 1}, {"id", base.page(0).layers.back().id}, {"blend", "multiply"}}}),
                core::Actor("human:test"));
        } catch (const core::ApplyError& e) {
            rejected = std::string(e.what()).find("high-precision") != std::string::npos;
        }
        QVERIFY(rejected);
        QVERIFY(base.page(1).note.empty());
        QCOMPARE(base.page(0).layers.back().blend, std::string("normal"));
        QCOMPARE(base.page(0).layers.back().color_raster, bytes);
    }
    void exposureAdjustmentSeesAllSixteenBits() {
        auto doc = core::new_episode("露光量試験", core::Num(1), 2,
            core::PageSpec::custom(20, 20, 16, 16, 1, 2, 2, 2, 2, 72, "color"));
        doc.edit_page(0).numero = false;
        const Json put = {{"op", "put_color_raster"}, {"page", 1}, {"width", 1}, {"height", 1},
                         {"precision", "u16"}, {"pixels", Json::array({1, 2, 3, 65535})}};
        const Json add = {{"op", "add_layer"}, {"page", 1}, {"layer", "finish"}, {"kind", "adjust"},
                         {"adjust", Json{{"kind", "exposure"}, {"exposure", 10.0}}}};
        std::optional<core::ApplyResult> changed;
        try { changed = core::CommandBus().apply(doc, Json::array({put, add}), core::Actor("human:test")); }
        catch (const std::exception& e) { QFAIL(e.what()); }
        const auto raw = changed->doc.page(0).layers[changed->doc.page(0).layers.size()-2].color_raster;
        const auto output = render::render_page(changed->doc.page(0), 72).image;
        const auto rgb = output.getpixel(output.width()/2, output.height()/2);
        QCOMPARE(rgb, (std::vector<double>{4, 8, 12}));
        QCOMPARE(core::ColorRasterView(*raw).pixel(0)[0], 1.0/65535.0);
    }
    void rgba32RetainsNegativeHdrAndLowBits() {
        auto doc = core::new_episode("カラー32試験", core::Num(1), 2, core::PageSpec::b4_comic());
        const Json op = {{"op", "put_color_raster"}, {"page", 1}, {"width", 1}, {"height", 1},
                         {"precision", "f32"}, {"pixels", Json::array({1.00000011920928955078125, -0.125, 2.0, 0.125})}};
        std::optional<core::ApplyResult> changed;
        try { changed = core::CommandBus().apply(doc, Json::array({op}), core::Actor("human:test")); }
        catch (const std::exception& e) { QFAIL(e.what()); }
        const auto& bytes = changed->doc.page(0).layers.back().color_raster;
        QVERIFY(bytes != nullptr);
        const core::ColorRasterView view(*bytes);
        const auto pixel = view.pixel(0);
        QCOMPARE(pixel[0], 1.00000011920928955078125);
        QCOMPARE(pixel[1], -0.125);
        QCOMPARE(pixel[2], 2.0);
        QCOMPARE(pixel[3], 0.125);
        QCOMPARE(view.metadata("x").at("precision").get<std::string>(), std::string("f32"));
        QCOMPARE(bytes->size(), std::size_t(32));
        QCOMPARE(static_cast<unsigned char>((*bytes)[16]), static_cast<unsigned char>(1));
        QCOMPARE(doc.page(0).layers.size()+1, changed->doc.page(0).layers.size());
    }
    void rgba16SurvivesNormalSaveAndReload() {
        QTemporaryDir tmp;
        QVERIFY(tmp.isValid());
        const std::filesystem::path dir(tmp.path().toStdString());
        auto doc = core::new_episode("カラー試験", core::Num(1), 2, core::PageSpec::b4_comic());
        core::CommandBus bus;
        const Json pixels = Json::array({1000, 1001, 1002, 65535, 65535, 30000, 12000, 40000});
        const Json op = {{"op", "put_color_raster"}, {"page", 1}, {"width", 2}, {"height", 1},
                         {"precision", "u16"}, {"pixels", pixels}};
        std::optional<core::ApplyResult> changed;
        try { changed = bus.apply(doc, Json::array({op}), core::Actor("human:test")); }
        catch (const std::exception& e) { QFAIL(e.what()); }
        storage::ProjectLock lock(dir, "human:test");
        lock.try_acquire();
        storage::Saver saver(lock);
        storage::SaveRequest req;
        req.actor = "human:test";
        req.ops = Json::array({op});
        const auto saved = saver.save(changed->doc, req);
        QCOMPARE(saved.revision, std::int64_t(1));
        const auto payload = storage::read_disk_state(dir).payload;
        const auto& features = payload.at("features");
        QVERIFY(std::find(features.begin(), features.end(), "native.color_raster_v1") != features.end());
        const auto& layers = payload.at("pages").at(0).at("layers");
        const Json* encoded = nullptr;
        for (const auto& layer : layers) if (layer.contains("color_raster")) encoded = &layer.at("color_raster");
        QVERIFY(encoded != nullptr);
        QCOMPARE(encoded->at("precision").get<std::string>(), std::string("u16"));
        const auto ref = encoded->at("asset").get<std::string>();
        storage::AssetStore assets(dir);
        const auto raw = *assets.get_bytes(ref, ".colorrgba");
        QCOMPARE(raw.size(), std::size_t(32)); // 16-byte header + eight little-endian u16 samples
        QCOMPARE(static_cast<unsigned char>(raw[16]), static_cast<unsigned char>(0xe8));
        QCOMPARE(static_cast<unsigned char>(raw[18]), static_cast<unsigned char>(0xe9));
        QCOMPARE(static_cast<unsigned char>(raw[20]), static_cast<unsigned char>(0xea));
        const auto loaded = storage::load_document(dir);
        QVERIFY(loaded.document.read_only_reason.empty());
        storage::AssetStore other(dir / "copy");
        const auto rewritten = storage::project_payload_v4(loaded.document, other);
        const Json* second = nullptr;
        for (const auto& layer : rewritten.at("pages").at(0).at("layers"))
            if (layer.contains("color_raster")) second = &layer.at("color_raster");
        QVERIFY(second != nullptr);
        QCOMPARE(*encoded, *second);
        QCOMPARE(*other.get_bytes(ref, ".colorrgba"), raw);
    }
};
QTEST_GUILESS_MAIN(TestColorRaster)
#include "test_color_raster.moc"
