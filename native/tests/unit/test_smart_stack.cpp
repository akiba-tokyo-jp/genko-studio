// smart-objects@1, PC3 A4 (SPEC.md COLOR-05「スタックモード」「台詞のラスタライズ」): a smart object's stack mode — the
// statistics as PhotoCraft's (algo/src/stack.rs's tests: the basic statistics, samples that do not show left out, the
// median taking an outlier out), its contents' layers combined (layer_menu_cmds/tests.rs:276-330: the median takes the
// passer-by out, the maximum keeps it, the range is nothing where all agree, none as before) in the three ways (8 bits;
// 16 bits, its own values; CMYK, plate by plate), a folder at the top, layers hidden, a part the whole cut there, a file one
// layer, rasterized as it looks, set by the op and written and read back; rasterize_lines drawing the lines as the page
// drew them (over every layer, under the layer they are set under, joined balloons together, the letters alone), refused
// as it should be; a tone layer rasterized on a page in precise colour and on a page of inks.

#include <QtTest>

#include <QTemporaryDir>

#include <algorithm>
#include <array>
#include <cmath>
#include <filesystem>
#include <string>
#include <vector>

#include "core/base64.hpp"
#include "core/color_raster.hpp"
#include "core/command_bus.hpp"
#include "core/ink.hpp"
#include "core/model.hpp"
#include "core/paths.hpp"
#include "core/smart.hpp"
#include "render/image.hpp"
#include "render/ops_registry.hpp"
#include "render/page.hpp"
#include "render/png.hpp"
#include "render/stack.hpp"
#include "storage/reader.hpp"
#include "testsupport.hpp"

namespace core = genko::core;
namespace render = genko::render;
namespace storage = genko::storage;
namespace stack = genko::render::stack;
namespace fs = std::filesystem;
using core::Actor;
using core::Document;
using core::Json;
using core::Num;

namespace {

const core::CommandBus& bus() {
    static const core::CommandBus b(render::ops_registry());
    return b;
}

Document run(const Document& doc, const Json& ops) { return bus().apply(doc, ops, Actor("human:作者")).doc; }
core::ApplyResult applied(const Document& doc, const Json& ops) { return bus().apply(doc, ops, Actor("human:作者")); }

// The op refused: what it says.
std::string refused(const Document& doc, const Json& op) {
    try {
        (void)bus().apply(doc, Json::array({op}), Actor("human:作者"));
    } catch (const std::exception& error) {
        return error.what();
    }
    return {};
}

core::PageSpec colour_page() {
    core::PageSpec spec = core::PageSpec::a5_doujin();
    spec.expression = "color";
    return spec;
}

const Json kSixteen{{"op", "set_color_mode"}, {"page", 1}, {"mode", "rgb"}, {"depth", 16}};
const Json kCmyk{{"op", "set_color_mode"}, {"page", 1}, {"mode", "cmyk"}, {"profile", "coated-cmyk"}};

// A picture of the whole page (stretched over it, 40 × 30): grey `v`, its block (10..20, 10..20) `block`.
std::string frame_png(int v, int block) {
    render::Image picture = render::Image::create("RGBA", render::Size{40, 30}, render::Ink{v, v, v, 255});
    picture.paste(render::Ink{block, block, block, 255}, render::Box{10, 10, 20, 20});
    return render::write_png(picture);
}

// Three shots of one place (paint layers f0, f1, f2, the bottom first): greys `greys`, f2's block bright (a passer-by)
// unless `quiet`; in the colour mode given (none: 8 bits) and, `convert`, made one smart object "so".
Document shots(const Json& mode = Json(), std::array<int, 3> greys = {102, 102, 102}, bool quiet = false, bool convert = true) {
    Document doc = core::new_episode("スタック", Num(1), 1, colour_page());
    Json ops = Json::array();
    for (int k = 0; k < 3; ++k) {
        const std::string id = "f" + std::to_string(k);
        ops.push_back(Json{{"op", "add_layer"}, {"page", 1}, {"kind", "paint"}, {"id", id}});
        ops.push_back(Json{{"op", "set_layer"}, {"page", 1}, {"id", id}, {"panel_each", false}, {"panel_clip", false}});
        ops.push_back(Json{{"op", "put_raster"}, {"page", 1}, {"id", id}, {"png_base64", core::b64encode(frame_png(greys[k], k == 2 && !quiet ? 255 : greys[k]))}});
    }
    doc = run(doc, ops);
    if (!mode.is_null()) doc = run(doc, Json::array({mode}));
    if (!convert) return doc;
    return run(doc, Json::array({Json{{"op", "smart_convert"}, {"page", 1}, {"ids", Json::array({"f0", "f1", "f2"})}, {"new_id", "so"}}}));
}

Json stacked(const char* mode, const char* id = "so") { return Json{{"op", "smart_stack"}, {"page", 1}, {"id", id}, {"mode", mode}}; }

render::RenderResult drawn(const Document& doc, int dpi = 60, std::optional<render::RenderRegion> region = std::nullopt, const std::string& mode = "proof") {
    render::RenderOptions options;
    options.mode = mode;
    options.inks = doc.page(0).color.has_value();
    options.precise = !doc.page(0).color && doc.page(0).rgb && doc.page(0).rgb->depth > 8;
    options.region = region;
    return render::render_page(doc.page(0), dpi, options, &doc);
}

// Where on the page (a share of its width and height) the shots' block is, and a place away from it.
constexpr std::array<double, 2> kBlock{15.0 / 40, 15.0 / 30};
constexpr std::array<double, 2> kAway{32.0 / 40, 24.0 / 30};

std::size_t at(const render::RenderResult& r, std::array<double, 2> where) {
    const int x = static_cast<int>(where[0] * r.image.width()), y = static_cast<int>(where[1] * r.image.height());
    return std::size_t(y) * r.image.width() + x;
}
// The screen's red there.
int shown(const render::RenderResult& r, std::array<double, 2> where) {
    const std::string bytes = r.image.convert("RGB").tobytes();
    return static_cast<unsigned char>(bytes[at(r, where) * 3]);
}
// The page's own red there (a page in precise colour).
double value(const render::RenderResult& r, std::array<double, 2> where) { return core::ColorRasterView(*r.precise).pixel(at(r, where))[0]; }
// The most ink there (a page of inks).
double most_ink(const render::RenderResult& r, std::array<double, 2> where) {
    double most = 0;
    for (unsigned c = 0; c < r.inks->colours(); ++c) most = std::max(most, r.inks->value(at(r, where), c));
    return most;
}

int most_apart(const render::Image& a, const render::Image& b) {
    const std::string x = a.convert("RGB").tobytes(), y = b.convert("RGB").tobytes();
    if (x.size() != y.size()) return 256;
    int most = 0;
    for (std::size_t i = 0; i < x.size(); ++i) most = std::max(most, std::abs(int(static_cast<unsigned char>(x[i])) - int(static_cast<unsigned char>(y[i]))));
    return most;
}

const core::Layer* layer_of(const Document& doc, const std::string& id) {
    for (const core::Layer& l : doc.page(0).layers)
        if (l.id == id) return &l;
    return nullptr;
}

std::vector<double> one_pixel(std::initializer_list<double> greys) {
    std::vector<double> out;
    for (const double g : greys) out.insert(out.end(), {g, g, g, 1.0});
    return out;
}

double combined(const std::vector<double>& values, stack::Stat stat, unsigned c = 0) {
    std::vector<double> out(4), scratch;
    stack::combine_pixel(values.data(), values.size() / 4, 3, stat, out.data(), scratch);
    return out[c];
}

// A page with a line in a speech balloon well inside its panel (and a paint layer "p1" under it), in the mode given.
Document with_lines(const core::PageSpec& spec, const Json& mode = Json()) {
    Document doc = core::new_episode("台詞", Num(1), 2, spec);
    if (!mode.is_null()) doc = run(doc, Json::array({mode}));
    return run(doc, Json::array({Json{{"op", "add_layer"}, {"page", 1}, {"kind", "paint"}, {"id", "p1"}},
                                 Json{{"op", "set_layer"}, {"page", 1}, {"id", "p1"}, {"panel_each", false}, {"panel_clip", false}},
                                 Json{{"op", "put_raster"}, {"page", 1}, {"id", "p1"}, {"png_base64", core::b64encode(frame_png(180, 60))}},
                                 Json{{"op", "add_line"}, {"page", 1}, {"id", "l1"}, {"text", "こんにちは。\nいい天気"}, {"x_mm", 40}, {"y_mm", 60},
                                      {"w_mm", 34}, {"h_mm", 46}, {"balloon", "speech"}, {"tail", Json::array({70, 120})}}}));
}

}  // namespace

class TestSmartStack : public QObject {
    Q_OBJECT

private slots:
    // algo/src/stack.rs's tests: the basic statistics of five samples, samples that do not show left out, the median
    // taking an outlier out; the names of the modes; what one sample makes.
    void statisticsAsPhotoCrafts() {
        const std::vector<double> five = one_pixel({0.1, 0.9, 0.2, 0.3, 0.1});
        QVERIFY(std::abs(combined(five, stack::Stat::Mean) - 0.32) < 1e-9);
        QVERIFY(std::abs(combined(five, stack::Stat::Median) - 0.2) < 1e-9);
        QCOMPARE(combined(five, stack::Stat::Minimum), 0.1);
        QCOMPARE(combined(five, stack::Stat::Maximum), 0.9);
        QVERIFY(std::abs(combined(five, stack::Stat::Range) - 0.8) < 1e-9);
        QCOMPARE(combined(five, stack::Stat::Summation), 1.0);  // (taken into 0..1)
        QVERIFY(std::abs(combined(five, stack::Stat::StandardDeviation) - std::sqrt(combined(five, stack::Stat::Variance))) < 1e-9);
        QVERIFY2(combined(five, stack::Stat::Skewness) > 0.5, "right-skewed");
        QVERIFY(combined(five, stack::Stat::Entropy) > 0 && combined(five, stack::Stat::Entropy) <= 1);
        QVERIFY(combined(five, stack::Stat::Kurtosis) > 0);
        // (a sample that does not show is left out; nothing shows: nothing)
        const std::vector<double> two{0.5, 0.5, 0.5, 1.0, 1.0, 1.0, 1.0, 0.0};
        for (unsigned c = 0; c < 4; ++c) QCOMPARE(combined(two, stack::Stat::Mean, c), c == 3 ? 1.0 : 0.5);
        QCOMPARE(combined(std::vector<double>{1.0, 1.0, 1.0, 0.0}, stack::Stat::Mean, 3), 0.0);
        // (the alpha the largest)
        QCOMPARE(combined(std::vector<double>{0.2, 0.2, 0.2, 0.4, 0.6, 0.6, 0.6, 0.7}, stack::Stat::Minimum, 3), 0.7);
        QCOMPARE(combined(one_pixel({0.5, 0.5, 1.0}), stack::Stat::Median), 0.5);
        // the names: the book's (core::smart::kStackModes), none other
        for (const std::string_view mode : core::smart::kStackModes) QVERIFY2(stack::stat_of(mode).has_value(), std::string(mode).c_str());
        for (const char* other : {"none", "standardDeviation", "Median", ""}) QVERIFY2(!stack::stat_of(other), other);
        // one sample: itself, or a value of its own
        for (const stack::Stat s : {stack::Stat::Mean, stack::Stat::Median, stack::Stat::Minimum, stack::Stat::Maximum, stack::Stat::Summation}) {
            QVERIFY(!stack::of_one(s));
            QCOMPARE(combined(one_pixel({0.37}), s), 0.37);
        }
        for (const stack::Stat s : {stack::Stat::Range, stack::Stat::Variance, stack::Stat::StandardDeviation, stack::Stat::Skewness, stack::Stat::Kurtosis,
                                    stack::Stat::Entropy}) {
            QVERIFY(stack::of_one(s).has_value());
            QCOMPARE(combined(one_pixel({0.37}), s), *stack::of_one(s));
            QCOMPARE(combined(one_pixel({0.37, 0.37, 0.37}), s), *stack::of_one(s));
        }
    }

    // smart_stack: set (in its place among the settings), changed, taken out by none or null; any smart object; refused
    // for another name, none given, a layer that is not a smart object, a locked one; written and read back.
    void theOpSetsTheMode() {
        const Document doc = shots();
        const Document median = run(doc, Json::array({stacked("median")}));
        const Json& s = *layer_of(median, "so")->smart;
        QCOMPARE(s["stack"], Json("median"));
        std::vector<std::string> keys;
        for (const auto& [key, v] : s.items()) keys.push_back(key);
        QVERIFY(keys == (std::vector<std::string>{"contents", "matrix", "interp", "stack"}));
        QCOMPARE(applied(doc, Json::array({stacked("standard_deviation")})).results[0]["stack"], Json("standard_deviation"));
        QVERIFY(!layer_of(run(median, Json::array({stacked("none")})), "so")->smart->contains("stack"));
        QCOMPARE(*layer_of(run(median, Json::array({Json{{"op", "smart_stack"}, {"page", 1}, {"id", "so"}, {"mode", nullptr}}})), "so")->smart,
                 *layer_of(doc, "so")->smart);
        QVERIFY(refused(doc, stacked("standardDeviation")).find("mode is a stack mode") != std::string::npos);
        QVERIFY(refused(doc, Json{{"op", "smart_stack"}, {"page", 1}, {"id", "so"}}).find("mode is a stack mode") != std::string::npos);
        const Document painted = shots(Json(), {102, 102, 102}, false, false);
        QVERIFY(refused(painted, stacked("mean", "f0")).find("is not a smart object") != std::string::npos);
        const Document locked = run(doc, Json::array({Json{{"op", "set_layer"}, {"page", 1}, {"id", "so"}, {"locked", true}}}));
        QVERIFY(refused(locked, stacked("mean")).find("locked") != std::string::npos);
        // (written and read back: the same setting, the same pixels)
        QTemporaryDir tmp;
        const fs::path dir = core::path_from_utf8((tmp.path() + "/book").toStdString());
        genko::test::write_project(median, dir);
        const auto loaded = storage::load_document(dir);
        QVERIFY2(loaded.report.clean(), loaded.report.to_json().dump().c_str());
        QCOMPARE(loaded.document.read_only_reason, std::string());
        QCOMPARE(*layer_of(loaded.document, "so")->smart, s);
        QCOMPARE(drawn(loaded.document).image.tobytes(), drawn(median).image.tobytes());
    }

    // The contents' three shots combined (layer_menu_cmds/tests.rs:276-330): the top shot shows as they are; the median
    // takes the passer-by out, the maximum keeps it, the range is nothing where the shots agree, none draws as before —
    // on an 8-bit page, a page of 16 bits and a CMYK page.
    void stackedInTheThreeWays_data() {
        QTest::addColumn<QString>("mode");
        QTest::newRow("8 bits") << QString();
        QTest::newRow("16 bits") << QString::fromStdString(kSixteen.dump());
        QTest::newRow("CMYK") << QString::fromStdString(kCmyk.dump());
    }
    void stackedInTheThreeWays() {
        QFETCH(QString, mode);
        const Document doc = shots(mode.isEmpty() ? Json() : Json::parse(mode.toStdString()));
        const render::RenderResult plain = drawn(doc);
        const int grey = shown(plain, kAway);
        QVERIFY2(shown(plain, kBlock) > grey + 60, "the top shot shows its passer-by");
        const render::RenderResult median = drawn(run(doc, Json::array({stacked("median")})));
        QVERIFY2(std::abs(shown(median, kBlock) - grey) <= 1, qPrintable(QStringLiteral("%1 %2").arg(shown(median, kBlock)).arg(grey)));
        QVERIFY(std::abs(shown(median, kAway) - grey) <= 1);
        QCOMPARE(shown(drawn(run(doc, Json::array({stacked("maximum")}))), kBlock), shown(plain, kBlock));
        const render::RenderResult range = drawn(run(doc, Json::array({stacked("range")})));
        const render::RenderResult mean = drawn(run(doc, Json::array({stacked("mean")})));
        if (doc.page(0).color) {
            // (plate by plate on the inverted plates, as the page's blend modes: where the shots agree every plate full —
            // not the profile's black an 8-bit black would be taken to —, the mean's ink between the grey's and none)
            for (unsigned c = 0; c < range.inks->colours(); ++c) QVERIFY2(range.inks->value(at(range, kAway), c) > 0.99, "every plate full where they agree");
            QVERIFY(most_ink(range, kBlock) < 0.99);
            QVERIFY(most_ink(mean, kBlock) > 0.01 && most_ink(mean, kBlock) < most_ink(mean, kAway));
        } else {
            QVERIFY2(shown(range, kAway) <= 1, "nothing where the shots agree");
            QVERIFY(shown(range, kBlock) > 100);
            QVERIFY(std::abs(shown(mean, kBlock) - (2 * grey + shown(plain, kBlock)) / 3) <= 2);
        }
        if (doc.page(0).rgb && doc.page(0).rgb->depth > 8) {
            QVERIFY(median.precise.has_value());
            QVERIFY(std::abs(value(median, kBlock) - 102 / 255.0) < 1e-3);
        }
        const Document none = run(run(doc, Json::array({stacked("mean")})), Json::array({stacked("none")}));
        QCOMPARE(drawn(none).image.tobytes(), plain.image.tobytes());
    }

    // On a page of 16 bits the statistic is of the page's own values (the mean of 100, 100 and 101 not an 8-bit step).
    void sixteenBitsKeepTheirValues() {
        const Document doc = run(shots(kSixteen, {100, 100, 101}, true), Json::array({stacked("mean")}));
        const render::RenderResult out = drawn(doc);
        QVERIFY(out.precise.has_value());
        const double v = value(out, kAway);
        QVERIFY2(std::abs(v - (100 + 100 + 101) / 765.0) < 2e-4, qPrintable(QString::number(v * 255)));
        QVERIFY(std::abs(v - 100 / 255.0) > 1e-3);
    }

    // The layers in a folder at the top of the contents are the stack; a hidden layer is not one of them.
    void aFolderAndHiddenLayers() {
        Document doc = shots(Json(), {102, 102, 102}, false, false);
        doc = run(doc, Json::array({Json{{"op", "add_layer"}, {"page", 1}, {"folder", true}, {"id", "fd"}},
                                    Json{{"op", "move_layers"}, {"page", 1}, {"ids", Json::array({"f0", "f1", "f2"})}, {"parent", "fd"}}}));
        for (const char* id : {"f0", "f1", "f2"}) QCOMPARE(layer_of(doc, id)->parent_id, Json("fd"));
        doc = run(doc, Json::array({Json{{"op", "smart_convert"}, {"page", 1}, {"id", "fd"}, {"new_id", "so"}}}));
        const int grey = shown(drawn(doc), kAway);
        QVERIFY(std::abs(shown(drawn(run(doc, Json::array({stacked("median")}))), kBlock) - grey) <= 1);
        // (the passer-by's shot hidden: the maximum is the others')
        Document hidden = shots(Json(), {102, 102, 102}, false, false);
        hidden = run(hidden, Json::array({Json{{"op", "set_layer"}, {"page", 1}, {"id", "f2"}, {"visible", false}},
                                          Json{{"op", "smart_convert"}, {"page", 1}, {"ids", Json::array({"f0", "f1", "f2"})}, {"new_id", "so"}},
                                          stacked("maximum")}));
        QVERIFY(std::abs(shown(drawn(hidden), kBlock) - grey) <= 1);
    }

    // A part of the page is the whole cut there: moved by whole pixels, scaled and turned.
    void aPartIsTheWholeCutThere() {
        const Document base = run(shots(), Json::array({stacked("median")}));
        const double turn = 12 * 3.14159265358979323846 / 180;
        for (const Json& m : {Json::array({1, 0, 0, 1, 3, 4}), Json::array({0.7, 0, 0, 0.6, 12.5, 7.25}),
                              Json::array({std::cos(turn), std::sin(turn), -std::sin(turn), std::cos(turn), 20, -5})}) {
            const Document moved = run(base, Json::array({Json{{"op", "smart_transform"}, {"page", 1}, {"id", "so"}, {"set_matrix", m}}}));
            const render::Image whole = drawn(moved, 100).image;
            for (const render::RenderRegion part : {render::RenderRegion{0, 0, 64, 64}, render::RenderRegion{150, 210, 90, 60}}) {
                const render::Image piece = drawn(moved, 100, part).image;
                QVERIFY2(piece.tobytes() == whole.crop(render::Box{part.x, part.y, part.x + part.w, part.y + part.h}).tobytes(), m.dump().c_str());
            }
        }
    }

    // A file is one layer: the mean of it is itself, its variance nothing (black where it shows).
    void aFileIsOneLayer() {
        Document doc = core::new_episode("ファイル", Num(1), 1, colour_page());
        const core::Rect b = doc.page(0).bleed_rect_mm();
        doc = run(doc, Json::array({Json{{"op", "smart_place"}, {"page", 1}, {"data", core::b64encode(frame_png(102, 255))}, {"name", "f.png"},
                                         {"placement_mm", Json::array({b.x.json(), b.y.json(), b.width.json(), b.height.json()})}, {"new_id", "so"}}}));
        const render::Image plain = drawn(doc).image;
        QCOMPARE(drawn(run(doc, Json::array({stacked("mean")}))).image.tobytes(), plain.tobytes());
        const render::RenderResult variance = drawn(run(doc, Json::array({stacked("variance")})));
        QVERIFY(shown(variance, kAway) <= 1 && shown(variance, kBlock) <= 1);
        QCOMPARE(shown(drawn(run(doc, Json::array({stacked("skewness")}))), kAway), 128);
        // (on a page of 16 bits too)
        const Document deep = run(doc, Json::array({kSixteen}));
        QVERIFY(shown(drawn(run(deep, Json::array({stacked("variance")}))), kAway) <= 1);
        QCOMPARE(drawn(run(deep, Json::array({stacked("median")}))).image.tobytes(), drawn(deep).image.tobytes());
    }

    // Rasterized, a stacked smart object is the paint layer it looks like.
    void rasterizedAsItLooks() {
        for (const Json& mode : {Json(), kSixteen, kCmyk}) {
            const Document doc = run(shots(mode), Json::array({stacked("median")}));
            const render::Image before = drawn(doc).image;
            const Document flat = run(doc, Json::array({Json{{"op", "rasterize_layer"}, {"page", 1}, {"id", "so"}}}));
            QCOMPARE(layer_of(flat, "so")->kind, core::LayerKind::Raster);
            QVERIFY2(most_apart(drawn(flat).image, before) <= 2, mode.dump().c_str());
        }
    }

    // rasterize_lines: the line's balloon and letters as a paint layer over every layer, the same pixels as the page drew
    // them (at the paint layers' 200 dpi, in print and in proof; a monochrome page too), the line gone; undone, as it was.
    void linesRasterizedAsDrawn() {
        for (const core::PageSpec& spec : {colour_page(), core::PageSpec::a4_mono()}) {
            const Document doc = with_lines(spec);
            const auto result = applied(doc, Json::array({Json{{"op", "rasterize_lines"}, {"page", 1}, {"ids", Json::array({"l1"})}, {"new_id", "tx"}}}));
            const Document& after = result.doc;
            QCOMPARE(result.results[0]["id"], Json("tx"));
            QCOMPARE(result.results[0]["lines"], Json::array({"l1"}));
            QVERIFY(std::none_of(after.story.begin(), after.story.end(), [](const core::StoryLine& l) { return l.id == "l1"; }));
            const core::Layer& made = after.page(0).layers.back();
            QCOMPARE(made.id, std::string("tx"));
            QCOMPARE(made.kind, core::LayerKind::Raster);
            QCOMPARE(made.role, core::LayerRole::User);
            QVERIFY(made.raster_png && !made.panel_clip);
            QVERIFY(made.title.starts_with("台詞 こんにちは。"));
            for (const char* how : spec.expression == "color" ? std::vector<const char*>{"print", "proof"} : std::vector<const char*>{"proof"}) {
                const render::Image was = drawn(doc, 200, std::nullopt, how).image, now = drawn(after, 200, std::nullopt, how).image;
                QVERIFY2(was.tobytes() != drawn(run(doc, Json::array({Json{{"op", "delete_line"}, {"id", "l1"}}})), 200, std::nullopt, how).image.tobytes(),
                         "the line is drawn");
                QVERIFY2(most_apart(was, now) <= 2, qPrintable(QStringLiteral("%1 %2: %3").arg(QString::fromStdString(spec.expression), QString::fromLatin1(how)).arg(most_apart(was, now))));
            }
        }
    }

    // Set under a layer, the paint layer goes under it; lines joined into one balloon come together; the letters alone
    // ("balloon": false) leave the balloon a line without words; what is refused.
    void underJoinedAndTheLettersAlone() {
        Document doc = with_lines(colour_page());
        doc = run(doc, Json::array({Json{{"op", "edit_line"}, {"id", "l1"}, {"style", Json{{"below_layer", "p1"}, {"group", "g"}}}},
                                    Json{{"op", "add_line"}, {"page", 1}, {"id", "l2"}, {"text", "はい"}, {"x_mm", 80}, {"y_mm", 62}, {"w_mm", 24},
                                         {"h_mm", 30}, {"style", Json{{"below_layer", "p1"}, {"group", "g"}}}}}));
        const render::Image was = drawn(doc, 200).image;
        const auto result = applied(doc, Json::array({Json{{"op", "rasterize_lines"}, {"page", 1}, {"id", "l2"}, {"new_id", "tx"}}}));
        const Document& after = result.doc;
        QCOMPARE(result.results[0]["lines"], Json::array({"l1", "l2"}));
        QVERIFY(after.story.empty() || std::none_of(after.story.begin(), after.story.end(), [](const core::StoryLine& l) { return l.page_index == Num(1); }));
        const auto& layers = after.page(0).layers;
        const auto tx = std::find_if(layers.begin(), layers.end(), [](const core::Layer& l) { return l.id == "tx"; });
        QVERIFY(tx != layers.end() && tx + 1 != layers.end() && (tx + 1)->id == "p1");
        QVERIFY2(most_apart(was, drawn(after, 200).image) <= 2, "under the layer, as drawn");
        // (the letters alone: the balloon stays a line, without words)
        const Document letters = run(with_lines(colour_page()), Json::array({Json{{"op", "rasterize_lines"}, {"page", 1}, {"id", "l1"}, {"balloon", false}}}));
        const auto kept = std::find_if(letters.story.begin(), letters.story.end(), [](const core::StoryLine& l) { return l.id == "l1"; });
        QVERIFY(kept != letters.story.end() && kept->text.empty() && kept->balloon == "speech");
        QCOMPARE(letters.page(0).layers.back().kind, core::LayerKind::Raster);
        QCOMPARE(kept->style["below_layer"], Json(letters.page(0).layers.back().id));  // (the balloon just under its letters)
        QVERIFY2(most_apart(drawn(with_lines(colour_page()), 200).image, drawn(letters, 200).image) <= 2, "the letters over the balloon, as drawn");
        // refused: a line that is not there, one on another page, one without a balloon, lines at two places, no lines
        const Document base = with_lines(colour_page());
        QVERIFY(refused(base, Json{{"op", "rasterize_lines"}, {"page", 1}, {"id", "nope"}}).find("no line nope") != std::string::npos);
        const Document other = run(base, Json::array({Json{{"op", "add_line"}, {"page", 2}, {"id", "l9"}, {"text", "次"}, {"x_mm", 30}, {"y_mm", 30}}}));
        QVERIFY(refused(other, Json{{"op", "rasterize_lines"}, {"page", 1}, {"id", "l9"}}).find("is not on page 1") != std::string::npos);
        Document label = base;  // (a line of an older book with no place and no balloon: drawn as a label)
        label.add_line(Num(1), "ラベル", "", std::nullopt, "", Num(0), Num(0), Num(40), Num(20), "").id = "l3";
        QVERIFY2(refused(label, Json{{"op", "rasterize_lines"}, {"page", 1}, {"id", "l3"}}).find("has no balloon") != std::string::npos,
                 refused(label, Json{{"op", "rasterize_lines"}, {"page", 1}, {"id", "l3"}}).c_str());
        const Document two = run(base, Json::array({Json{{"op", "add_line"}, {"page", 1}, {"id", "l4"}, {"text", "下"}, {"x_mm", 90}, {"y_mm", 120},
                                                         {"style", Json{{"below_layer", "p1"}}}}}));
        QVERIFY(refused(two, Json{{"op", "rasterize_lines"}, {"page", 1}, {"ids", Json::array({"l1", "l4"})}}).find("different places") != std::string::npos);
        QVERIFY(refused(base, Json{{"op", "rasterize_lines"}, {"page", 1}}).find("names the lines") != std::string::npos);
    }

    // On a page in precise colour the layer holds the page's values, on a page of inks its inks: drawn as the lines were.
    void linesOnPagesOfPreciseColourAndInks() {
        for (const Json& mode : {kSixteen, kCmyk}) {
            const Document doc = with_lines(colour_page(), mode);
            const Document after = run(doc, Json::array({Json{{"op", "rasterize_lines"}, {"page", 1}, {"id", "l1"}}}));
            const core::Layer& made = after.page(0).layers.back();
            QVERIFY2(mode == kCmyk ? made.ink_raster != nullptr : made.color_raster != nullptr, mode.dump().c_str());
            QVERIFY(!made.raster_png);
            // (inside the balloon and its letters: as drawn; their edges are laid as the page lays a layer)
            const render::Image was = drawn(doc, 200).image, now = drawn(after, 200).image;
            const std::string a = was.convert("L").tobytes(), b = now.convert("L").tobytes();
            QCOMPARE(a.size(), b.size());
            std::size_t near = 0;
            for (std::size_t i = 0; i < a.size(); ++i) near += std::abs(int(static_cast<unsigned char>(a[i])) - int(static_cast<unsigned char>(b[i]))) <= 3 ? 1 : 0;
            QVERIFY2(near > a.size() * 995 / 1000, qPrintable(QStringLiteral("%1 of %2").arg(near).arg(a.size())));
        }
    }

    // A tone layer rasterized on a monochrome page of 16 bits and of CMYK: the black of its tone, its share the alpha —
    // screened as it printed where the page prints its tones in dots (16 bits: the same share of dots), flat where it prints
    // them flat (a page of inks, a colour book's page: no screen, its grey as the proof and print showed it). (The page of
    // 16 bits lays the black in light as it lays every layer: SPEC's known limits.)
    void aToneOnPagesOfPreciseColourAndInks() {
        {
            Document colour = run(core::new_episode("トーン", Num(1), 1, colour_page()),
                                  Json::array({Json{{"op", "add_tone"}, {"page", 1}, {"id", "tn"}, {"density", 0.3},
                                                    {"area", Json{{"poly", Json::array({Json::array({20, 30}), Json::array({100, 30}), Json::array({100, 120})})}}}}}));
            const render::Image before = drawn(colour, 100, std::nullopt, "print").image;
            const Document flat = run(colour, Json::array({Json{{"op", "rasterize_layer"}, {"page", 1}, {"id", "tn"}}}));
            QVERIFY(!layer_of(flat, "tn")->screen);
            // (inside the tone: its edge is drawn at 200 dpi, then as the page's resolution takes it)
            const render::Box inside{static_cast<int>(75 / 25.4 * 100), static_cast<int>(40 / 25.4 * 100), static_cast<int>(95 / 25.4 * 100),
                                     static_cast<int>(60 / 25.4 * 100)};
            QVERIFY2(most_apart(drawn(flat, 100, std::nullopt, "print").image.crop(inside), before.crop(inside)) <= 3, "a colour page prints its tone flat");
        }
        for (const Json& mode : {kSixteen, kCmyk}) {
            Document doc = run(core::new_episode("トーン", Num(1), 1, core::PageSpec::a4_mono()), Json::array({mode}));
            doc = run(doc, Json::array({Json{{"op", "add_tone"}, {"page", 1}, {"id", "tn"}, {"density", 0.3}, {"lpi", 60}, {"angle", 45},
                                             {"area", Json{{"poly", Json::array({Json::array({40, 120}), Json::array({160, 120}), Json::array({160, 240}),
                                                                                 Json::array({40, 240})})}}}}}));
            const render::Image proof = drawn(doc, 50).image;
            const render::Image print = drawn(doc, 300, std::nullopt, "print").image;
            const Document flat = run(doc, Json::array({Json{{"op", "rasterize_layer"}, {"page", 1}, {"id", "tn"}}}));
            const core::Layer* tn = layer_of(flat, "tn");
            QVERIFY(tn != nullptr && tn->kind == core::LayerKind::Raster && tn->role == core::LayerRole::User && !tn->tone);
            QVERIFY2(mode == kCmyk ? tn->ink_raster != nullptr : tn->color_raster != nullptr, mode.dump().c_str());
            const auto box = [](int dpi) {
                return render::Box{static_cast<int>(60 / 25.4 * dpi), static_cast<int>(140 / 25.4 * dpi), static_cast<int>(140 / 25.4 * dpi),
                                   static_cast<int>(220 / 25.4 * dpi)};
            };
            const auto dark = [&](const render::Image& image) {
                const std::string grey = image.crop(box(300)).convert("L").tobytes();
                return double(std::count_if(grey.begin(), grey.end(), [](char v) { return static_cast<unsigned char>(v) < 128; })) / double(grey.size());
            };
            if (mode == kCmyk) {  // (a page of inks prints its tones flat)
                QVERIFY(!tn->screen);
                const render::Image now = drawn(flat, 50).image;
                QVERIFY2(most_apart(now.crop(box(50)), proof.crop(box(50))) <= 3,
                         qPrintable(QStringLiteral("the grey of the tone: %1 (%2 → %3)")
                                        .arg(most_apart(now.crop(box(50)), proof.crop(box(50))))
                                        .arg(proof.getpixel(box(50).x0 + 5, box(50).y0 + 5)[0])
                                        .arg(now.getpixel(box(50).x0 + 5, box(50).y0 + 5)[0])));
                QVERIFY2(most_apart(drawn(flat, 300, std::nullopt, "print").image.crop(box(300)), print.crop(box(300))) <= 3, "printed flat");
                continue;
            }
            QVERIFY(tn->screen.has_value());
            QCOMPARE((*tn->screen)["pattern"], Json("dot"));
            QCOMPARE((*tn->screen)["lpi"], Json(60.0));
            QCOMPARE((*tn->screen)["angle"], Json(45.0));
            const double was = dark(print), now = dark(drawn(flat, 300, std::nullopt, "print").image);
            QVERIFY2(std::abs(was - 0.3) < 0.05 && std::abs(now - was) < 0.03, qPrintable(QStringLiteral("%1 %2").arg(was).arg(now)));
        }
    }
};

QTEST_MAIN(TestSmartStack)
#include "test_smart_stack.moc"
