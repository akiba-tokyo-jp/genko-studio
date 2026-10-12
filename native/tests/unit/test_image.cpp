// render::Image (Pillow's Image / ImageChops / ImageOps / ImageFilter layer over libImaging) against pixels made by
// Pillow 12.3.0: tools/migration/render_harness.py unit-tables writes each operation's inputs and result.

#include <QtTest>

#include <functional>
#include <map>
#include <string>
#include <thread>
#include <vector>

#include "core/base64.hpp"
#include "core/command_bus.hpp"
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

// A picture of noise (every byte from a fixed random sequence) in `mode`, made from L, LA, RGB or RGBA pixels.
Image noisy(const std::string& mode, int w, int h, unsigned seed = 7) {
    const std::string from = mode == "L" || mode == "1" || mode == "I" || mode == "F" ? "L" : mode == "LA" || mode == "La" ? "LA"
                             : mode == "RGB" || mode == "P" || mode == "CMYK"                                            ? "RGB"
                                                                                                                         : "RGBA";
    const int n = from == "L" ? 1 : from == "LA" ? 2 : from == "RGB" ? 3 : 4;
    std::string data;
    for (int i = 0; i < w * h * n; ++i) {
        seed = seed * 1103515245U + 12345U;
        // (blocks of a colour with noise over them: edges for the blurs to soften, and noise everywhere)
        const int block = ((i / n % w) / 9 + (i / n / w) / 7) % 3 * 90;
        data.push_back(static_cast<char>((block + static_cast<int>((seed >> 16) % 61)) % 256));
    }
    const Image made = Image::frombytes(from, {w, h}, data);
    return mode == from ? made : made.convert(mode);
}

// The bytes of a picture, its mode and size, or the error making it threw.
std::string outcome(const std::function<Image()>& make) {
    try {
        const Image im = make();
        return std::string(im.mode()) + " " + std::to_string(im.width()) + "x" + std::to_string(im.height()) + " " + im.tobytes();
    } catch (const genko::core::Error& e) {
        return "error " + e.code() + ": " + e.what();
    }
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

    void convertBudgetError() {
        render::ImageAllocationBudget budget(100);
        Image a = Image::create("L", {10, 10});
        QVERIFY_THROWS_EXCEPTION(genko::core::OpError, a.convert("RGB"));
        QCOMPARE(budget.live(), std::uint64_t{100});
        a = Image{};
        QCOMPARE(budget.live(), std::uint64_t{0});
        QVERIFY(!Image::create("L", {10, 10}).empty());
    }

    void liveAllocationBudget() {
        Image escaped;
        {
            render::ImageAllocationBudget budget(300);
            Image a = Image::create("L", {10, 10});
            Image b = a.copy();
            QCOMPARE(budget.live(), std::uint64_t(200));
            {
                const Image c = render::chops::lighter(a, b);
                QCOMPARE(budget.live(), std::uint64_t(300));
                QVERIFY_THROWS_EXCEPTION(genko::core::OpError, c.copy());
                QCOMPARE(budget.live(), std::uint64_t(300));
            }
            QCOMPARE(budget.live(), std::uint64_t(200));
            QVERIFY_THROWS_EXCEPTION(genko::core::OpError, a.filter(render::Filter::gaussian_blur(1)));
            QCOMPARE(budget.live(), std::uint64_t(200));
            QCOMPARE(budget.peak(), std::uint64_t(300));
            escaped = std::move(a);
        }
        {
            render::ImageAllocationBudget budget(400);
            const Image rgb = Image::create("RGB", {10, 10});
            QCOMPARE(budget.live(), std::uint64_t(400)); // libImaging stores RGB as four bytes.
            escaped = Image{}; // old budget stays alive independently of this scope.
            QCOMPARE(budget.live(), std::uint64_t(400));
            QVERIFY_THROWS_EXCEPTION(genko::core::OpError, rgb.getchannel(0));
        }
        QVERIFY(!Image::create("L", {20, 20}).empty()); // exception cleanup restores the normal allocator.
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

    void imagesWithoutPixelsCopy() {
        // (as a PSD's layer of no width: Pillow's row copy would read a null row, which the ASan build stops on)
        for (const render::Size size : {render::Size{0, 5}, render::Size{5, 0}, render::Size{0, 0}}) {
            for (const char* mode : {"L", "RGB", "RGBA"}) {
                const Image empty = Image::create(mode, size);
                const Image copied(empty);  // NOLINT(performance-unnecessary-copy-initialization): the copy is the test
                QCOMPARE(copied.size().width, size.width);
                QCOMPARE(copied.size().height, size.height);
                QCOMPARE(std::string(copied.mode()), std::string(mode));
                QCOMPARE(empty.copy().size().width, size.width);
                QCOMPARE(Image::merge("RGB", {empty.convert("L"), empty.convert("L"), empty.convert("L")}).size().height, size.height);
            }
        }
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

    // resize_in_bands and filter_in_bands (render/image.hpp): the same pixels (or the same error) as resize and filter
    // for any number of bands — every mode, enlarging and making smaller in one direction or both, every filter, the
    // blurs with one radius or two, none, and inside an allocation budget (one band there); a picture over 100 times
    // taller than wide.
    void bandsMakeTheSamePixels() {
        const render::BandCounts start = render::band_counts();
        const std::vector<int> counts{2, 3, 5, 8, 33};
        for (const char* mode : {"L", "RGB", "RGBA", "LA", "La", "RGBa", "CMYK", "1", "I", "F"}) {
            const Image src = noisy(mode, 203, 157);
            for (const render::Size size : {render::Size{613, 481}, render::Size{97, 71}, render::Size{203, 400}, render::Size{500, 157},
                                            render::Size{1, 300}, render::Size{203, 157}}) {
                for (const auto resample : {render::Resample::Lanczos, render::Resample::Bicubic, render::Resample::Bilinear, render::Resample::Box,
                                            render::Resample::Hamming, render::Resample::Nearest}) {
                    const std::string whole = outcome([&] { return src.resize(size, resample); });
                    for (const int threads : counts) {
                        QVERIFY2(outcome([&] { return src.resize_in_bands(size, resample, std::nullopt, threads); }) == whole,
                                 qPrintable(QStringLiteral("%1 to %2x%3, filter %4, %5 bands").arg(mode).arg(size.width).arg(size.height)
                                                .arg(static_cast<int>(resample)).arg(threads)));
                    }
                }
            }
            for (const render::Filter& f : {render::Filter::gaussian_blur(0), render::Filter::gaussian_blur(0.5), render::Filter::gaussian_blur(2.59),
                                            render::Filter::gaussian_blur(7.3), render::Filter::gaussian_blur(3, 0), render::Filter::gaussian_blur(0, 4),
                                            render::Filter::gaussian_blur(2, 5), render::Filter::box_blur(1), render::Filter::box_blur(2.5, 0.7),
                                            render::Filter::unsharp_mask(2.59, 90, 2), render::Filter::unsharp_mask(1, 150, 3),
                                            render::Filter::unsharp_mask(9.2, 50, 0), render::Filter::unsharp_mask(0, 90, 2),
                                            render::Filter::min_filter(3)}) {
                const std::string whole = outcome([&] { return src.filter(f); });
                for (const int threads : counts) {
                    QVERIFY2(outcome([&] { return src.filter_in_bands(f, threads); }) == whole,
                             qPrintable(QStringLiteral("%1 filter %2 (%3, %4), %5 bands").arg(mode).arg(static_cast<int>(f.kind)).arg(f.radius_x)
                                            .arg(f.radius_y).arg(threads)));
                }
            }
        }
        {
            render::ImageAllocationBudget budget(std::uint64_t{1} << 30);
            const Image src = noisy("RGB", 120, 90);
            QVERIFY(src.resize_in_bands({360, 270}, render::Resample::Lanczos, std::nullopt, 4).tobytes() ==
                    src.resize({360, 270}, render::Resample::Lanczos).tobytes());
            QVERIFY(src.filter_in_bands(render::Filter::unsharp_mask(2, 90, 2), 4).tobytes() == src.filter(render::Filter::unsharp_mask(2, 90, 2)).tobytes());
        }
        const Image tall = noisy("L", 3, 400);
        QVERIFY(tall.resize_in_bands({3, 200}, render::Resample::Lanczos, std::nullopt, 4).tobytes() == tall.resize({3, 200}, render::Resample::Lanczos).tobytes());
        // (the work was done in bands — not by resize and filter after a band failed — in most of the cases above)
        const render::BandCounts end = render::band_counts();
        QCOMPARE(end.failed - start.failed, std::uint64_t{0});
        qInfo("worked in bands %llu times", static_cast<unsigned long long>(end.banded - start.banded));
        QVERIFY2(end.banded - start.banded > 1000, qPrintable(QString::number(end.banded - start.banded)));
    }

    // The same with a box (a part of the picture resized: what the drawing does with a placed picture) and for a
    // region of the result (resize_region): boxes in fractions of a pixel, the whole picture, a plain crop, boxes the
    // picture cannot give (the same error), regions anywhere in the size, the whole size, and outside it.
    void bandsMakeTheSamePixelsOfABoxAndARegion() {
        const render::BandCounts start = render::band_counts();
        const std::vector<render::BoxF> boxes{{2.5, 1.25, 180.0, 150.5}, {0, 0, 203, 157}, {10, 10, 110, 90}, {0.3, 40.7, 203, 41.2},
                                              {-1, 0, 100, 100}, {0, 0, 300, 157}, {50, 50, 40, 60}};
        const std::vector<render::Box> regions{{0, 0, 1, 1}, {3, 17, 140, 190}, {0, 70, 150, 200}, {50, 0, 51, 200}, {0, 0, 150, 200}, {-1, 0, 5, 5},
                                               {0, 0, 151, 200}, {5, 5, 5, 9}};
        for (const char* mode : {"L", "RGB", "RGBA", "LA", "I", "F", "1"}) {
            const Image src = noisy(mode, 203, 157);
            for (const render::BoxF& box : boxes) {
                for (const auto resample : {render::Resample::Lanczos, render::Resample::Bicubic, render::Resample::Box, render::Resample::Nearest}) {
                    for (const render::Size size : {render::Size{613, 481}, render::Size{100, 80}, render::Size{1, 300}, render::Size{97, 71}}) {
                        const std::string whole = outcome([&] { return src.resize(size, resample, box); });
                        for (const int threads : {2, 3, 8}) {
                            QVERIFY2(outcome([&] { return src.resize_in_bands(size, resample, box, threads); }) == whole,
                                     qPrintable(QStringLiteral("%1 box (%2, %3, %4, %5) to %6x%7, filter %8, %9 bands").arg(mode).arg(box.x0).arg(box.y0)
                                                    .arg(box.x1).arg(box.y1).arg(size.width).arg(size.height).arg(static_cast<int>(resample)).arg(threads)));
                        }
                    }
                    const render::Size size{150, 200};
                    for (const render::Box& region : regions) {
                        const std::string part = outcome([&] { return src.resize_region(size, region, resample, box); });
                        for (const int threads : {2, 3, 8}) {
                            QVERIFY2(outcome([&] { return src.resize_region_in_bands(size, region, resample, box, threads); }) == part,
                                     qPrintable(QStringLiteral("%1 box (%2, %3, %4, %5), region (%6, %7, %8, %9), filter %10, %11 bands").arg(mode)
                                                    .arg(box.x0).arg(box.y0).arg(box.x1).arg(box.y1).arg(region.x0).arg(region.y0).arg(region.x1)
                                                    .arg(region.y1).arg(static_cast<int>(resample)).arg(threads)));
                        }
                    }
                }
            }
        }
        // (a large picture made small, as a placed picture is drawn small: in bands too)
        const Image big = noisy("RGBA", 1500, 1100);
        const render::BoxF part{12.5, 7.25, 1490.0, 1093.5};
        QVERIFY(big.resize_in_bands({301, 220}, render::Resample::Lanczos, part, 4).tobytes() == big.resize({301, 220}, render::Resample::Lanczos, part).tobytes());
        QVERIFY(big.resize_region_in_bands({301, 220}, {20, 33, 290, 200}, render::Resample::Lanczos, part, 4).tobytes() ==
                big.resize_region({301, 220}, {20, 33, 290, 200}, render::Resample::Lanczos, part).tobytes());
        const render::BandCounts end = render::band_counts();
        QCOMPARE(end.failed - start.failed, std::uint64_t{0});
        QVERIFY2(end.banded - start.banded > 500, qPrintable(QString::number(end.banded - start.banded)));
    }

    // side_by_side (render/image.hpp): both done, the side one on a thread of its own; an exception of either thrown
    // (the main one's first); inside an allocation budget one after the other on this thread, the side one first.
    void sideBySide() {
        const std::thread::id here = std::this_thread::get_id();
        std::thread::id side_on;
        std::string made;
        Image a;
        Image b;
        const Image src = noisy("RGB", 300, 200);
        render::side_by_side([&] { side_on = std::this_thread::get_id(); a = src.convert("L"); },
                             [&] { b = src.resize({150, 100}, render::Resample::Lanczos); });
        QVERIFY(side_on != here);
        QVERIFY(a.tobytes() == src.convert("L").tobytes());
        QVERIFY(b.tobytes() == src.resize({150, 100}, render::Resample::Lanczos).tobytes());
        const auto thrown = [](const std::function<void()>& side, const std::function<void()>& main) -> std::string {
            try {
                render::side_by_side(side, main);
            } catch (const genko::core::Error& e) {
                return e.what();
            }
            return "nothing";
        };
        bool other_done = false;
        QCOMPARE(thrown([&] { other_done = true; }, [] { throw genko::core::Error("value", "main"); }), std::string("main"));
        QVERIFY(other_done);
        other_done = false;
        QCOMPARE(thrown([] { throw genko::core::Error("value", "side"); }, [&] { other_done = true; }), std::string("side"));
        QVERIFY(other_done);
        QCOMPARE(thrown([] { throw genko::core::Error("value", "side"); }, [] { throw genko::core::Error("value", "main"); }), std::string("main"));
        {
            render::ImageAllocationBudget budget(std::uint64_t{1} << 30);
            std::vector<std::string> order;
            render::side_by_side([&] { side_on = std::this_thread::get_id(); order.push_back("side"); }, [&] { order.push_back("main"); });
            QVERIFY(side_on == here);
            QCOMPARE(order, (std::vector<std::string>{"side", "main"}));
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
