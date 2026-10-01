// render::Image (Pillow's Image / ImageChops / ImageOps / ImageFilter layer over libImaging) against pixels made by
// Pillow 12.3.0: tools/migration/render_harness.py unit-tables writes each operation's inputs and result.

#include <QtTest>

#include <map>
#include <string>
#include <vector>

#include "core/base64.hpp"
#include "core/error.hpp"
#include "render/image.hpp"
#include "testsupport.hpp"

using genko::core::Json;
namespace render = genko::render;
using render::Image;

namespace {

const Json& tables() {
    static const Json data = genko::test::read_json(genko::test::test_data("pyref/render_unit_tables.json"));
    return data;
}

// render_harness.pattern: band c of pixel (x, y) is (x * 31 + y * 17 + c * 59) % 251
Image pattern(const std::string& mode, int w, int h) {
    const int n = mode == "L" ? 1 : mode == "LA" ? 2 : mode == "RGB" ? 3 : 4;
    std::string data;
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            for (int c = 0; c < n; ++c) data.push_back(static_cast<char>((x * 31 + y * 17 + c * 59) % 251));
        }
    }
    return Image::frombytes(mode, {w, h}, data);
}

// The stored input pictures (each is stored once, with an "id"; later cases give its "ref").
const Json& stored(const std::string& id) {
    static const std::map<std::string, Json> by_id = [] {
        std::map<std::string, Json> out;
        for (const Json& c : tables()["image"]) {
            for (const Json& j : c["inputs"]) {
                if (j.contains("id")) out[j["id"].get<std::string>()] = j;
            }
        }
        return out;
    }();
    return by_id.at(id);
}

Image input(const Json& j) {
    if (j.contains("pattern")) return pattern(j["pattern"].get<std::string>(), j["size"][0].get<int>(), j["size"][1].get<int>());
    const Json* s = &j;
    if (j.contains("ref")) s = &stored(j["ref"].get<std::string>());
    return Image::frombytes((*s)["mode"].get<std::string>(), {(*s)["size"][0].get<int>(), (*s)["size"][1].get<int>()},
                            genko::core::a2b_base64((*s)["data"].get<std::string>()));
}

render::Filter filter_named(const std::string& name) {
    using render::Filter;
    if (name == "gauss1.5") return Filter::gaussian_blur(1.5);
    if (name == "gauss0") return Filter::gaussian_blur(0);
    if (name == "box2") return Filter::box_blur(2);
    if (name == "min3") return Filter::min_filter(3);
    if (name == "max5") return Filter::max_filter(5);
    if (name == "median3") return Filter::median_filter(3);
    if (name == "mode3") return Filter::mode_filter(3);
    if (name == "smooth") return Filter::smooth();
    if (name == "sharpen") return Filter::sharpen();
    if (name == "blur") return Filter::blur();
    if (name == "unsharp") return Filter::unsharp_mask(2, 150, 3);
    if (name == "edge") return Filter::find_edges();
    throw std::runtime_error("unknown filter " + name);
}

render::Resample resample_of(int r) { return static_cast<render::Resample>(r); }

// Python's "(w, h)" in a case name
render::Size size_in(const std::string& text) {
    const auto open = text.find('(');
    const auto comma = text.find(',', open);
    const auto close = text.find(')', comma);
    return {std::stoi(text.substr(open + 1, comma - open - 1)), std::stoi(text.substr(comma + 1, close - comma - 1))};
}

Image run(const std::string& name, const std::vector<Image>& in) {
    using namespace render;
    const auto starts = [&](const char* p) { return name.rfind(p, 0) == 0; };
    if (name == "chops.screen") return chops::screen(in[0], in[1]);
    if (name == "chops.multiply") return chops::multiply(in[0], in[1]);
    if (name == "chops.add") return chops::add(in[0], in[1]);
    if (name == "chops.subtract") return chops::subtract(in[0], in[1]);
    if (name == "chops.lighter") return chops::lighter(in[0], in[1]);
    if (name == "chops.darker") return chops::darker(in[0], in[1]);
    if (name == "chops.difference") return chops::difference(in[0], in[1]);
    if (name == "chops.invert") return chops::invert(in[0]);
    if (name == "chops.overlay") return chops::overlay(in[0], in[1]);
    if (name == "chops.logical_and") return chops::logical_and(in[0], in[1]);
    if (name == "alpha_composite") return alpha_composite(in[0], in[1]);
    if (name == "alpha_composite_in_place") {
        Image dest = in[0].copy();
        dest.alpha_composite(in[1].crop(Box{2, 1, 9, 6}), Point{5, 3});
        return dest;
    }
    if (name == "convert.L.1.none") return in[0].convert("1", Dither::None);
    if (starts("convert.")) return in[0].convert(name.substr(name.rfind('.') + 1));
    if (name == "resize.default") return in[0].resize({16, 4});
    if (name == "resize.reducing_gap") return in[0].resize({3, 2}, Resample::Bicubic, std::nullopt, 2.0);
    if (name == "resize.tall") return in[0].resize({2, 37}, Resample::Bicubic);
    if (starts("resize.")) return in[0].resize(size_in(name), resample_of(std::stoi(name.substr(name.rfind('.') + 1))));
    if (name == "thumbnail") {
        Image t = in[0].copy();
        t.thumbnail({96, 96});
        return t;
    }
    if (name == "rotate.noexpand") return in[0].rotate(17.0, Resample::Bilinear);
    if (starts("rotate.L.")) return in[0].rotate(std::stod(name.substr(9)), Resample::Bicubic, true);
    if (name == "transform.affine") {
        const std::vector<double> m{0.9, 0.1, 1.0, -0.1, 1.1, 0.5};
        return in[0].transform({14, 9}, TransformMethod::Affine, m, Resample::Bilinear);
    }
    if (starts("filter.")) return in[0].filter(filter_named(name.substr(name.find('.', 7) + 1)));
    if (name == "crop.outside") return in[0].crop(Box{-3, -2, 20, 7});
    if (name == "crop.float") return in[0].crop(BoxF{0.5, 1.5, 7.5, 6.49});
    if (name == "paste.convert") {
        Image p = in[0].copy();
        p.paste(in[1].crop(Box{0, 0, 5, 4}), Point{3, 2});
        return p;
    }
    if (name == "paste.rgba_mask") {
        Image p = in[0].copy();
        p.paste(in[1], Point{0, 0}, &in[1]);
        return p;
    }
    if (name == "paste.colour_mask") {
        Image p = in[0].copy();
        const Image mask = in[1].crop(Box{0, 0, 6, 4});
        p.paste(Ink{10, 200, 30, 128}, Box{2, 2, 8, 6}, &mask);
        return p;
    }
    if (name == "putalpha.rgb") {
        Image p = in[0].copy();
        p.putalpha(in[1].crop(Box{0, 0, 11, 7}));
        return p;
    }
    if (name == "putalpha.l_const") {
        Image p = in[0].copy();
        p.putalpha(77);
        return p;
    }
    if (name == "point.l") return in[0].point([](int v) { return v * 45 / 100; });
    if (name == "point.rgba") return in[0].point([](int v) { return 255 - v; });
    if (name == "point.to1") return in[0].point([](int p) { return p > 128 ? 255 : 0; }, "1");
    if (name == "merge") return Image::merge("RGBA", {in[0], in[1], in[0], in[1]});
    if (name == "split.alpha") return in[0].split()[3];
    if (name == "getchannel.A") return in[0].getchannel("A");
    if (name == "composite") return composite(in[0], in[1], in[2].crop(Box{0, 0, 11, 7}));
    if (name == "blend") return blend(in[0], in[1].convert("RGB"), 0.3);
    if (name == "ops.invert.rgb") return ops::invert(in[0]);
    if (name == "ops.grayscale") return ops::grayscale(in[0]);
    if (name == "offset") return chops::offset(in[0], 3, -2);
    if (name == "transpose") return in[0].transpose(Transpose::Rotate90);
    if (name == "hsv_roundtrip") return in[0].convert("HSV").convert("RGB");
    throw std::runtime_error("unknown case " + name);
}

}  // namespace

class TestImage : public QObject {
    Q_OBJECT

private slots:
    void operations_data() {
        QTest::addColumn<int>("index");
        const Json& cases = tables()["image"];
        for (std::size_t i = 0; i < cases.size(); ++i) {
            QTest::newRow(cases[i]["name"].get<std::string>().c_str()) << static_cast<int>(i);
        }
    }

    void operations() {
        QFETCH(int, index);
        const Json& c = tables()["image"][static_cast<std::size_t>(index)];
        const std::string name = c["name"].get<std::string>();
        std::vector<Image> in;
        for (const Json& j : c["inputs"]) in.push_back(input(j));
        if (name == "getbbox") {
            const auto box = in[0].getbbox();
            std::vector<int> got;
            if (box) got = {box->x0, box->y0, box->x1, box->y1};
            QCOMPARE(got, c["result"].get<std::vector<int>>());
            return;
        }
        if (name == "getextrema") {
            const auto e = in[0].getextrema();
            QCOMPARE(e.size(), c["result"].size());
            for (std::size_t i = 0; i < e.size(); ++i) {
                QCOMPARE(e[i].first, c["result"][i][0].get<double>());
                QCOMPARE(e[i].second, c["result"][i][1].get<double>());
            }
            return;
        }
        const Image out = run(name, in);
        const Json& want = c["result"];
        QCOMPARE(std::string(out.mode()), want["mode"].get<std::string>());
        QCOMPARE(out.width(), want["size"][0].get<int>());
        QCOMPARE(out.height(), want["size"][1].get<int>());
        const std::string got = out.tobytes();
        const std::string expected = genko::core::a2b_base64(want["data"].get<std::string>());
        if (got != expected) {
            std::size_t at = 0;
            while (at < got.size() && at < expected.size() && got[at] == expected[at]) ++at;
            QFAIL(qPrintable(QStringLiteral("pixels differ at byte %1 of %2").arg(at).arg(expected.size())));
        }
    }

    void errors() {
        const Image l = Image::create("L", {4, 4});
        QVERIFY_THROWS_EXCEPTION(genko::core::Error, l.crop(render::Box{3, 0, 1, 2}));
        QVERIFY_THROWS_EXCEPTION(genko::core::Error, Image::create("L", {-1, 2}));
        QVERIFY_THROWS_EXCEPTION(genko::core::Error, render::Image::create("RGB", {2, 2}, render::Ink{1, 2}));
        QVERIFY_THROWS_EXCEPTION(genko::core::Error, render::alpha_composite(l, l));
        QVERIFY_THROWS_EXCEPTION(genko::core::Error, render::Filter::min_filter(4));
        // a crop beyond twice Pillow's MAX_IMAGE_PIXELS is a decompression bomb
        QVERIFY_THROWS_EXCEPTION(genko::core::Error, l.crop(render::Box{0, 0, 20000, 20000}));
    }

    void resize_region() {
        // a part of a resized picture is the same as cutting it from the whole one, for every filter and mode
        unsigned seed = 12345;
        const auto next = [&](int n) {
            seed = seed * 1103515245U + 12345U;
            return static_cast<int>((seed >> 8) % static_cast<unsigned>(n));
        };
        for (const char* mode : {"L", "RGB", "RGBA", "LA"}) {
            const Image src = pattern(mode == std::string("LA") ? "LA" : mode, 37, 23);
            for (const auto resample : {render::Resample::Bicubic, render::Resample::Bilinear, render::Resample::Lanczos,
                                        render::Resample::Box, render::Resample::Hamming, render::Resample::Nearest}) {
                for (const render::Size size : {render::Size{80, 50}, render::Size{11, 7}, render::Size{37, 60}, render::Size{90, 23},
                                                render::Size{37, 23}}) {
                    const Image whole = src.resize(size, resample);
                    for (int k = 0; k < 6; ++k) {
                        const int x0 = next(size.width);
                        const int y0 = next(size.height);
                        const render::Box region{x0, y0, x0 + 1 + next(size.width - x0), y0 + 1 + next(size.height - y0)};
                        const Image part = src.resize_region(size, region, resample);
                        QVERIFY2(part.tobytes() == whole.crop(region).tobytes(),
                                 qPrintable(QStringLiteral("%1 filter %2 %3x%4").arg(mode).arg(static_cast<int>(resample))
                                                .arg(size.width).arg(size.height)));
                    }
                }
            }
            const render::BoxF box{2.5, 1.25, 30.0, 20.5};
            const Image whole = src.resize({50, 40}, render::Resample::Bicubic, box);
            const render::Box region{7, 3, 44, 29};
            QVERIFY(src.resize_region({50, 40}, region, render::Resample::Bicubic, box).tobytes() == whole.crop(region).tobytes());
        }
    }

    void value_semantics() {
        Image a = Image::create("RGBA", {3, 2}, render::Ink{1, 2, 3, 4});
        Image b = a;  // a copy of the pixels
        b.paste(render::Ink{9, 9, 9, 9}, render::Box{0, 0, 3, 2});
        QCOMPARE(a.getpixel(0, 0), (std::vector<double>{1, 2, 3, 4}));
        QCOMPARE(b.getpixel(2, 1), (std::vector<double>{9, 9, 9, 9}));
        Image c = std::move(b);
        QVERIFY(b.empty());  // NOLINT(bugprone-use-after-move): moved from is empty
        QCOMPARE(c.getpixel(1, 1), (std::vector<double>{9, 9, 9, 9}));
    }
};

QTEST_GUILESS_MAIN(TestImage)
#include "test_image.moc"
