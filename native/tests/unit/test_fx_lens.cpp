// PC4 節 D6 (SPEC.md COLOR-08「フィルター」: レンズ補正・広角補正・消点) without Python — PhotoCraft's own tests of them
// (algo/src/exif.rs, lens.rs, wideangle.rs, vanishing.rs; engine/src/lens_cmds.rs, vp_cmds.rs) on Genko's surfaces and ops:
// the camera's EXIF read back and garbage survived; レンズ補正 the identity at its defaults on every depth, its distortion,
// edges, vignetting, fringes, perspective, 角度補正's angle, 自動拡大・縮小 and generic profiles, a CMYK page's ink lessened,
// through filter_raster on 8 bits, 16 and 32 (its corners cleared, the book before it unchanged, what it resolves in the
// report), the profile "auto" from a smart object's JPEG and from an opened JPEG's page (its EXIF's focal length), refused
// without one, as a smart filter; 広角補正's lens models, its constraints made straight and level, no constraint the
// identity, no gaps across tiles, through the op and as a smart filter; both the same at 200 and 600 dpi and on an opened
// picture's page; 消点's camera and metric aspect from one plane, a plane torn off perpendicular, pasting and the
// perspective stamp in perspective, the op on 8 bits, precise and CMYK pages and inside a selection, a new layer, the planes
// kept on the page (vanishing-point@1: written, read back, a broken one read-only) and used again.

#include <QTemporaryDir>
#include <QtTest/QtTest>

#include <algorithm>
#include <array>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <functional>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include "core/base64.hpp"
#include "core/color_raster.hpp"
#include "core/command_bus.hpp"
#include "core/error.hpp"
#include "core/exif.hpp"
#include "core/filter_kinds.hpp"
#include "core/ink.hpp"
#include "core/lens.hpp"
#include "core/vanishing.hpp"
#include "formats/open_file.hpp"
#include "formats/pillow_save.hpp"
#include "render/fx.hpp"
#include "render/image.hpp"
#include "render/ops_registry.hpp"
#include "render/page.hpp"
#include "render/png.hpp"
#include "render/raster.hpp"
#include "render/vanishing.hpp"
#include "storage/lock.hpp"
#include "storage/reader.hpp"
#include "storage/transaction.hpp"
#include "testsupport.hpp"

using namespace genko;
using core::Json;
using render::fx::Rect;

namespace {

constexpr double kInch = 25.4;
using Px = std::array<float, 4>;
using XY = std::array<double, 2>;

std::string precise(std::uint32_t w, std::uint32_t h, const std::vector<Px>& px, bool f32) {
    return core::encode_color_pixels(w, h, f32 ? "f32" : "u16", [&](std::size_t i) { return std::array<double, 4>{px[i][0], px[i][1], px[i][2], px[i][3]}; });
}

// PhotoCraft's lens test picture: a grid of lines every 16 (or 12) pixels over a darker ground.
std::vector<Px> grid_picture(std::uint32_t w, std::uint32_t h, std::uint32_t every = 16) {
    std::vector<Px> out;
    for (std::uint32_t y = 0; y < h; ++y)
        for (std::uint32_t x = 0; x < w; ++x) {
            const float v = (x % every < 2 || y % every < 2) ? 0.9f : 0.3f;
            out.push_back({v, v * 0.8f, v * 0.6f, 1.0f});
        }
    return out;
}

render::Image eight_of(std::uint32_t w, std::uint32_t h, const std::vector<Px>& px) {
    std::string bytes;
    for (const Px& p : px)
        for (const float v : p) bytes.push_back(static_cast<char>(std::lround(std::clamp(v, 0.0f, 1.0f) * 255.0f)));
    return render::Image::frombytes("RGBA", render::Size{static_cast<int>(w), static_cast<int>(h)}, bytes);
}

const render::fx::Kernel& kernel(const std::string& kind) {
    const render::fx::Kernel* k = render::fx::kernel(kind);
    if (k == nullptr) throw core::Error("value", "no kernel " + kind);
    return *k;
}

Json normal(const std::string& kind, const Json& given) { return core::normalize_filter(kind, given, nullptr); }

std::string run_precise(const std::string& raster, const std::string& kind, const Json& given, render::fx::Tiling tiling = {}, double s = 1,
                        const std::string* cover = nullptr) {
    const auto surface = render::fx::precise_surface(raster);
    const Rect grid{0, 0, int(surface->width()), int(surface->height())};
    render::fx::apply(*surface, kernel(kind), normal(kind, given), core::FilterInks::Plates, grid, cover, s, nullptr, tiling);
    return render::fx::take_bytes(*surface);
}

std::string run_eight(const render::Image& picture, const std::string& kind, const Json& given, double s = 1, render::fx::Tiling tiling = {}) {
    const auto surface = render::fx::eight_surface(picture);
    render::fx::apply(*surface, kernel(kind), normal(kind, given), core::FilterInks::Plates, Rect{0, 0, picture.width(), picture.height()}, nullptr, s,
                      nullptr, tiling);
    return render::fx::take_image(*surface).tobytes();
}

std::array<double, 4> pixel(const std::string& raster, std::uint32_t x, std::uint32_t y) {
    const core::ColorRasterView view(raster);
    return view.pixel(std::size_t(y) * view.width() + x);
}

std::array<int, 4> eight_px(const std::string& bytes, int w, int x, int y) {
    const std::size_t i = (std::size_t(y) * std::size_t(w) + std::size_t(x)) * 4;
    return {static_cast<unsigned char>(bytes[i]), static_cast<unsigned char>(bytes[i + 1]), static_cast<unsigned char>(bytes[i + 2]),
            static_cast<unsigned char>(bytes[i + 3])};
}

core::ApplyResult apply_ops(const core::Document& doc, const Json& ops) { return core::CommandBus(render::ops_registry()).apply(doc, ops, core::Actor("human:test")); }

std::string refusal(const core::Document& doc, const Json& op) {
    try {
        apply_ops(doc, Json::array({op}));
    } catch (const core::ApplyError& error) {
        return error.code() + ": " + error.what();
    }
    return {};
}

core::Document blank(double w_mm = kInch, double h_mm = kInch, double dpi = 200) {
    auto doc = core::new_episode("レンズ", core::Num(1), 1, core::PageSpec::custom(w_mm, h_mm, w_mm - 2, h_mm - 2, 1, 0.5, 0.5, 0.5, 0.5, dpi, "color"));
    auto& p = doc.edit_page(0);
    p.numero = false;
    p.frames.clear();
    p.layers.clear();
    return doc;
}

const core::Layer& layer_of(const core::Document& doc, std::string_view id, std::size_t page = 0) {
    for (const auto& layer : doc.page(page).layers)
        if (layer.id == id) return layer;
    throw core::Error("value", "no layer " + std::string(id));
}

// A page of w × h pixels at 200 dpi with 8-bit paint layers (id → picture; each the page's size).
core::Document eight_page(int w, int h, const std::vector<std::pair<std::string, render::Image>>& layers) {
    auto doc = blank(w * kInch / 200.0, h * kInch / 200.0);
    for (const auto& [id, picture] : layers) {
        core::Layer layer;
        layer.id = id;
        layer.kind = core::LayerKind::Raster;
        layer.panel_clip = false;
        render::raster::save_raster(doc.page(0), layer, picture);
        doc.edit_page(0).layers.push_back(layer);
    }
    return doc;
}

render::Image picture_of(const core::Document& doc, const std::string& id) { return render::raster::layer_pixels(doc.page(0), layer_of(doc, id)).convert("RGBA"); }

// A precise page of w × h pixels (at 200 dpi) with one paint layer "paint".
core::Document precise_page(std::uint32_t w, std::uint32_t h, const std::vector<Px>& px, bool f32) {
    auto doc = blank(w * kInch / 200.0, h * kInch / 200.0);
    Json pixels = Json::array();
    for (const auto& p : px)
        for (const float v : p) pixels.push_back(f32 ? Json(static_cast<double>(v)) : Json(static_cast<std::int64_t>(std::lround(std::clamp(v, 0.0f, 1.0f) * 65535))));
    doc = core::CommandBus().apply(doc, Json::array({Json{{"op", "put_color_raster"}, {"page", 1}, {"width", w}, {"height", h},
                                                          {"precision", f32 ? "f32" : "u16"}, {"pixels", pixels}}}), core::Actor("human:test")).doc;
    doc.edit_page(0).layers[0].id = "paint";
    return doc;
}

// A CMYK page (16 bits) of side × side with a paint layer "pic": `ink(x, y, c)` its plates.
core::Document cmyk_page(std::uint32_t side, const std::function<double(std::uint32_t, std::uint32_t, unsigned)>& ink) {
    auto doc = apply_ops(blank(side * kInch / 200.0, side * kInch / 200.0),
                         Json::array({Json{{"op", "set_color_mode"}, {"mode", "cmyk"}, {"depth", 16}, {"profile", "coated-cmyk"}},
                                      Json{{"op", "add_layer"}, {"page", 1}, {"kind", "paint"}, {"id", "pic"}}}))
                   .doc;
    const core::PageColor& color = *doc.page(0).color;
    core::InkPixels p = core::blank_ink_pixels(color.space, color.depth, side, side, 0);
    for (std::uint32_t y = 0; y < side; ++y)
        for (std::uint32_t x = 0; x < side; ++x) {
            for (unsigned c = 0; c < p.colours(); ++c) p.set(std::size_t(y) * side + x, c, ink(x, y, c));
            p.set(std::size_t(y) * side + x, p.colours(), 1.0);
        }
    doc.edit_page(0).layers.back().ink_raster = std::make_shared<const std::string>(core::encode_ink_raster(p));
    return doc;
}

double mm(double px) { return px * kInch / 200.0; }  // (a pixel of the working 200 dpi in mm)

// A JPEG of the picture with an APP1 "Exif" segment of `camera` after its SOI.
std::string jpeg_with_exif(const render::Image& rgb, const core::exif::Camera& camera) {
    const std::string jpeg = formats::jpeg_bytes(rgb.convert("RGB"), formats::JpegSave{95, false, {}});
    const std::string payload = std::string("Exif\0\0", 6) + core::exif::build(camera);
    const std::size_t length = payload.size() + 2;
    std::string app1{static_cast<char>(0xFF), static_cast<char>(0xE1), static_cast<char>((length >> 8) & 255), static_cast<char>(length & 255)};
    return jpeg.substr(0, 2) + app1 + payload + jpeg.substr(2);
}

// A floor seen by a camera of focal f (PhotoCraft's floor_quad): a 2 × 1 rectangle tilted and turned, four metres off.
render::vanishing::Quad floor_quad(double f, const XY& c) {
    using V3 = std::array<double, 3>;
    const auto project = [&](const V3& p) { return XY{c[0] + f * p[0] / p[2], c[1] + f * p[1] / p[2]}; };
    const double rx = 0.6, ry = 0.4;
    const auto rot = [&](const V3& p) {
        const double s = std::sin(rx), co = std::cos(rx);
        const V3 q{p[0], co * p[1] - s * p[2], s * p[1] + co * p[2]};
        const double s2 = std::sin(ry), c2 = std::cos(ry);
        return V3{c2 * q[0] + s2 * q[2], q[1], -s2 * q[0] + c2 * q[2]};
    };
    render::vanishing::Quad out{};
    const V3 corners[4] = {{-1.0, -0.5, 0.0}, {1.0, -0.5, 0.0}, {1.0, 0.5, 0.0}, {-1.0, 0.5, 0.0}};
    for (std::size_t i = 0; i < 4; ++i) {
        V3 p = rot(corners[i]);
        p[2] += 4.0;
        out[i] = project(p);
    }
    return out;
}

Json quad_json(const std::vector<XY>& px) {
    Json out = Json::array();
    for (const XY& p : px) out.push_back(Json::array({mm(p[0]), mm(p[1])}));
    return out;
}

}  // namespace

class TestFxLens : public QObject {
    Q_OBJECT

private slots:
    // --- the camera's EXIF (exif.rs) --------------------------------------------------------------------------------------

    void theCamerasExifReadBack() {
        core::exif::Camera info;
        info.make = "Generic";
        info.model = "Camera 1";
        info.lens = "24-70mm";
        info.exposure_time = 0.004;
        info.f_number = 8.0;
        info.iso = 200.0;
        info.focal_length = 35.0;
        info.focal_length_35mm = 52.0;
        const std::string b = core::exif::build(info);
        QVERIFY(core::exif::read(b) == info);
        QVERIFY(core::exif::read(std::string("Exif\0\0", 6) + b) == info);
        QVERIFY(core::exif::read("nonsense") == core::exif::Camera{});
        for (const std::size_t cut : {std::size_t(5), std::size_t(12), std::size_t(30), b.size() - 3}) (void)core::exif::read(b.substr(0, cut));
        // (a big-endian block too)
        std::string be("MM\0*\0\0\0\x08\0\x01\x92\x0a\0\x05\0\0\0\x01\0\0\0\x1a\0\0\0\0\0\0\0\x18\0\0\0\x01", 34);
        QCOMPARE(core::exif::read(be).focal_length.value_or(0), 24.0);
    }

    // --- レンズ補正 (lens.rs) -------------------------------------------------------------------------------------------------

    void lensIdentityIsLosslessAtEveryDepth() {
        const auto px = grid_picture(96, 64);
        for (const bool f32 : {false, true}) {
            const std::string raster = precise(96, 64, px, f32);
            const std::string out = run_precise(raster, "lens_correction", Json::object());
            double err = 0;
            for (std::uint32_t y = 0; y < 64; ++y)
                for (std::uint32_t x = 0; x < 96; ++x)
                    for (unsigned c = 0; c < 4; ++c) err = std::max(err, std::fabs(pixel(raster, x, y)[c] - pixel(out, x, y)[c]));
            QVERIFY2(err < 2.0 / 255.0, qPrintable(QString::number(err)));
        }
        const render::Image eight = eight_of(96, 64, px);
        const std::string out = run_eight(eight, "lens_correction", Json::object());
        int err = 0;
        const std::string in = eight.tobytes();
        for (std::size_t i = 0; i < in.size(); ++i) err = std::max(err, std::abs(int(static_cast<unsigned char>(in[i])) - int(static_cast<unsigned char>(out[i]))));
        QVERIFY2(err <= 2, qPrintable(QString::number(err)));
    }

    void lensDistortionRoundTripAndEdges() {
        core::lens::Correction lc;
        lc.distortion = 50;
        const core::lens::Map m(lc, 0, 0, 200, 100);
        const auto s = m.sample(100, 50);
        QVERIFY(s && std::fabs(s->p[0] - 100) < 1e-9 && std::fabs(s->p[1] - 50) < 1e-9);
        const auto c = m.sample(0, 0);
        QVERIFY(c && c->p[0] > 5.0 && c->p[1] > 2.0);
        // transparency leaves the corners clear; extension and a colour fill them
        const render::Image picture = eight_of(200, 100, grid_picture(200, 100));
        const std::string clear = run_eight(picture, "lens_correction", Json{{"distortion", -60}});
        QVERIFY(eight_px(clear, 200, 0, 0)[3] < 3);
        QVERIFY(eight_px(clear, 200, 100, 50)[3] > 252);
        const std::string extended = run_eight(picture, "lens_correction", Json{{"distortion", -60}, {"edge", "extension"}});
        QCOMPARE(eight_px(extended, 200, 0, 0)[3], 255);
        const std::string red = run_eight(picture, "lens_correction", Json{{"distortion", -60}, {"edge", "color"}, {"edge_color", Json::array({255, 0, 0})}});
        const auto r = eight_px(red, 200, 0, 0);
        QVERIFY2(r[0] > 252 && r[1] < 3 && r[3] == 255, qPrintable(QStringLiteral("%1 %2 %3 %4").arg(r[0]).arg(r[1]).arg(r[2]).arg(r[3])));
        const std::string white = run_eight(picture, "lens_correction", Json{{"distortion", -60}, {"edge", "white"}});
        QCOMPARE(eight_px(white, 200, 0, 0), (std::array<int, 4>{255, 255, 255, 255}));
    }

    void lensVignetteFringesPerspectiveAndStraighten() {
        core::lens::Correction lc;
        lc.vignette_amount = 100;
        const core::lens::Map m(lc, 0, 0, 300, 200);
        QVERIFY(std::fabs(m.sample(150, 100)->gain - 1.0f) < 1e-6f);
        QVERIFY(m.sample(1, 1)->gain > 2.0f);
        lc.vignette_amount = -100;
        QVERIFY(core::lens::Map(lc, 0, 0, 300, 200).sample(1, 1)->gain < 0.5f);
        core::lens::Correction ca;
        ca.red_cyan = -100;
        const auto f = core::lens::Map(ca, 0, 0, 300, 200).sample(10, 10);
        QVERIFY(f->pr[0] < f->p[0]);
        core::lens::Correction v;
        v.vertical = -50;
        const core::lens::Map vm(v, 0, 0, 300, 200);
        const auto tl = vm.sample(0, 0)->p, tr = vm.sample(300, 0)->p, bl = vm.sample(0, 200)->p, br = vm.sample(300, 200)->p;
        QVERIFY(tr[0] - tl[0] < br[0] - bl[0]);
        QVERIFY(std::fabs(core::lens::straighten_angle({0, 0}, {100, 100 * std::tan(5.0 * M_PI / 180.0)}) + 5.0) < 1e-9);
        QVERIFY(std::fabs(core::lens::straighten_angle({0, 0}, {3, 100}) - 1.718) < 0.01);
        core::lens::Correction turned;
        turned.angle = 5;
        const double scale = core::lens::auto_scale(turned, 0, 0, 300, 200);
        QVERIFY2(scale > 105 && scale < 130, qPrintable(QString::number(scale)));
        turned.scale = scale;
        const auto corner = core::lens::Map(turned, 0, 0, 300, 200).sample(0, 0)->p;
        QVERIFY(corner[0] >= 0 && corner[0] <= 300);
    }

    void genericProfilesAndAPageOfInks() {
        const auto wide = core::lens::generic_profile(16), tele = core::lens::generic_profile(135);
        QVERIFY(wide.k1 < -0.05 && tele.k1 > 0.0 && wide.vignette[0] < tele.vignette[0]);
        const auto mid = core::lens::generic_profile(30);
        QVERIFY(mid.k1 < -0.025 && mid.k1 > -0.06);
        QCOMPARE(QString::fromStdString(core::lens::generic_profile(24).name), QStringLiteral("Generic Lens (24 mm equivalent)"));
        // CMYK: lightening the corners takes ink away (each plate), the middle as it was
        const auto doc = cmyk_page(40, [](std::uint32_t, std::uint32_t, unsigned) { return 0.5; });
        const auto done = apply_ops(doc, Json::array({Json{{"op", "filter_raster"}, {"page", 1}, {"id", "pic"}, {"kind", "lens_correction"}, {"vignette_amount", 100},
                                                            {"edge", "extension"}}}))
                              .doc;
        const core::InkPixels inks = core::decode_ink_raster(*layer_of(done, "pic").ink_raster);
        QVERIFY2(inks.value(0, 0) < 0.4, qPrintable(QString::number(inks.value(0, 0))));
        QVERIFY(std::fabs(inks.value(20 * 40 + 20, 0) - 0.5) < 0.02);
        QVERIFY(std::fabs(inks.value(0, 4) - 1.0) < 1e-6);  // (opaque: extension)
    }

    // lens_cmds.rs: on 8, 16 and 32 bits through filter_raster — the corners cleared, the book before it as it was (one Undo),
    // what it resolved in its report; 角度補正's line, 自動拡大・縮小; edges and profiles it does not know refused.
    void lensCorrectionThroughTheOp() {
        const auto px = grid_picture(120, 80, 12);
        const std::vector<core::Document> books{eight_page(120, 80, {{"paint", eight_of(120, 80, px)}}), precise_page(120, 80, px, false), precise_page(120, 80, px, true)};
        for (const core::Document& doc : books) {
            const Json op{{"op", "filter_raster"}, {"page", 1}, {"id", "paint"}, {"kind", "lens_correction"}, {"distortion", -40}, {"vignette_amount", -50}};
            const core::ApplyResult done = apply_ops(doc, Json::array({op}));
            const core::Layer& after = layer_of(done.doc, "paint");
            if (after.color_raster) {
                QVERIFY2(pixel(*after.color_raster, 1, 1)[3] < 0.5, "a precise layer's corner cleared");
                QVERIFY(*layer_of(doc, "paint").color_raster != *after.color_raster);
            } else {
                QVERIFY(picture_of(done.doc, "paint").getpixel(1, 1)[3] < 128);
            }
            const Json report = done.results.at(0);
            QCOMPARE(report["filter"]["kind"], Json("lens_correction"));
            QCOMPARE(report["filter"]["scale"], Json(100));
            QVERIFY(report["filter"]["profile_name"].is_null());
        }
        const core::Document& doc = books[0];
        // 角度補正: a line 5 px up over 100 px (the page's mm): its angle
        const Json straighten{{"op", "filter_raster"}, {"page", 1}, {"id", "paint"}, {"kind", "lens_correction"},
                              {"straighten", Json::array({Json::array({0, 0}), Json::array({mm(100), mm(5)})})}};
        const Json turned = apply_ops(doc, Json::array({straighten})).results.at(0);
        QVERIFY2(std::fabs(turned["filter"]["angle"].get<double>() + 2.862) < 0.01, turned.dump().c_str());
        QVERIFY(!turned["filter"].contains("straighten"));
        // 自動拡大・縮小: the scale that covers the frame, said
        const Json fit{{"op", "filter_raster"}, {"page", 1}, {"id", "paint"}, {"kind", "lens_correction"}, {"angle", 5}, {"auto_scale", true}};
        const Json fitted = apply_ops(doc, Json::array({fit})).results.at(0);
        QVERIFY2(fitted["filter"]["scale"].get<double>() > 105, fitted.dump().c_str());
        QVERIFY2(picture_of(apply_ops(doc, Json::array({fit})).doc, "paint").getpixel(2, 2)[3] == 255, "the corners covered");
        // a generic profile: its name
        const Json generic{{"op", "filter_raster"}, {"page", 1}, {"id", "paint"}, {"kind", "lens_correction"}, {"profile", "generic"}, {"focal_length", 16}};
        QCOMPARE(apply_ops(doc, Json::array({generic})).results.at(0)["filter"]["profile_name"], Json("Generic Lens (16 mm equivalent)"));
        for (const Json& wrong : {Json{{"edge", "rainbow"}}, Json{{"profile", "zeiss"}}, Json{{"straighten", Json::array({Json::array({0, 0})})}}}) {
            Json op{{"op", "filter_raster"}, {"page", 1}, {"id", "paint"}, {"kind", "lens_correction"}};
            op.update(wrong);
            QVERIFY2(!refusal(doc, op).empty(), wrong.dump().c_str());
        }
        // the profile auto without the picture's EXIF and without a focal length: refused (the book as it was)
        const Json bare{{"op", "filter_raster"}, {"page", 1}, {"id", "paint"}, {"kind", "lens_correction"}, {"profile", "auto"}};
        QVERIFY2(refusal(doc, bare).find("needs the focal length") != std::string::npos, refusal(doc, bare).c_str());
    }

    // The profile auto takes the 35 mm-equivalent focal length the picture's EXIF says (lens_cmds.rs: generic of it, kept so):
    // a smart object's JPEG (its smart filter), a page opened from a JPEG (filter_raster), 広角補正 its focal length and crop.
    void theProfileAutoFromThePicturesExif() {
        core::exif::Camera camera;
        camera.focal_length = 16.0;
        camera.focal_length_35mm = 24.0;
        const render::Image rgb = eight_of(120, 80, grid_picture(120, 80, 12)).convert("RGB");
        const std::string jpeg = jpeg_with_exif(rgb, camera);
        // a smart object of the JPEG
        auto doc = blank(mm(120), mm(80));
        doc = apply_ops(doc, Json::array({Json{{"op", "smart_place"}, {"page", 1}, {"data", core::b64encode(jpeg)}, {"name", "shot.jpg"}, {"new_id", "so"}}})).doc;
        const auto smart = apply_ops(doc, Json::array({Json{{"op", "smart_filter_add"}, {"page", 1}, {"id", "so"}, {"kind", "lens_correction"},
                                                            {"profile", "auto"}, {"auto_scale", true}, {"edge", "extension"}}}))
                               .doc;
        const Json stored = (*layer_of(smart, "so").smart)["filters"][0]["params"];
        QCOMPARE(stored["profile"], Json("generic"));
        QCOMPARE(stored["focal_length"].get<double>(), 24.0);
        const auto wide = apply_ops(doc, Json::array({Json{{"op", "smart_filter_add"}, {"page", 1}, {"id", "so"}, {"kind", "adaptive_wide_angle"}}})).doc;
        const Json wide_stored = (*layer_of(wide, "so").smart)["filters"][0]["params"];
        QCOMPARE(wide_stored["focal_length"].get<double>(), 16.0);
        QCOMPARE(wide_stored["crop_factor"].get<double>(), 1.5);
        // a page opened from the JPEG: its EXIF kept with the book
        const core::Document opened = formats::open_file(jpeg, "shot.jpg").doc;
        QVERIFY(opened.page(0).native_source && !opened.page(0).native_source->exif.empty());
        const std::string id = opened.page(0).layers[0].id;
        const Json report = apply_ops(opened, Json::array({Json{{"op", "filter_raster"}, {"page", 1}, {"id", id}, {"kind", "lens_correction"}, {"profile", "auto"}}}))
                                .results.at(0);
        QCOMPARE(report["filter"]["profile"], Json("generic"));
        QCOMPARE(report["filter"]["focal_length"].get<double>(), 24.0);
        QCOMPARE(report["filter"]["profile_name"], Json("Generic Lens (24 mm equivalent)"));
        // (a focal length given wins over the EXIF)
        const Json given = apply_ops(opened, Json::array({Json{{"op", "filter_raster"}, {"page", 1}, {"id", id}, {"kind", "lens_correction"}, {"profile", "auto"},
                                                              {"focal_length", 50}}}))
                               .results.at(0);
        QCOMPARE(given["filter"]["focal_length"].get<double>(), 50.0);
    }

    void lensCorrectionAsASmartFilter() {
        const auto doc = apply_ops(eight_page(120, 80, {{"paint", eight_of(120, 80, grid_picture(120, 80, 12))}}),
                                   Json::array({Json{{"op", "smart_convert"}, {"page", 1}, {"ids", Json::array({"paint"})}, {"new_id", "so"}}}))
                             .doc;
        const auto done = apply_ops(doc, Json::array({Json{{"op", "smart_filter_add"}, {"page", 1}, {"id", "so"}, {"kind", "lens_correction"}, {"vignette_amount", 80},
                                                           {"edge", "extension"}}}))
                              .doc;
        const Json filters = (*layer_of(done, "so").smart)["filters"];
        QCOMPARE(filters.size(), std::size_t(1));
        QCOMPARE(filters[0]["kind"], Json("lens_correction"));
        QVERIFY(core::uses_filters(done));
        const render::Image before = render::layer_image(doc.page(0), layer_of(doc, "so"), 200, &doc);
        const render::Image after = render::layer_image(done.page(0), layer_of(done, "so"), 200, &done);
        QVERIFY2(after.getpixel(3, 3)[0] > before.getpixel(3, 3)[0], "the corner lightened");
    }

    // --- 広角補正 (wideangle.rs) ----------------------------------------------------------------------------------------------

    void wideAngleCameraModels() {
        for (const core::lens::Model model : {core::lens::Model::Fisheye, core::lens::Model::Perspective}) {
            core::lens::WideAngle wa;
            wa.model = model;
            wa.focal_length = 15;
            const auto cam = core::lens::Camera::of(wa, 0, 0, 600, 400);
            for (const XY p : {XY{10, 20}, XY{300, 200}, XY{590, 350}}) {
                const auto back = cam.pixel(cam.ray(p[0], p[1]));
                QVERIFY(back && std::fabs((*back)[0] - p[0]) < 1e-6 && std::fabs((*back)[1] - p[1]) < 1e-6);
            }
        }
        const auto sphere = core::lens::Camera::of(core::lens::WideAngle{}, 0, 0, 800, 400);
        QVERIFY(sphere.model == core::lens::Model::FullSpherical);
        const auto p = sphere.pixel(sphere.ray(123, 77));
        QVERIFY(std::fabs((*p)[0] - 123) < 1e-6 && std::fabs((*p)[1] - 77) < 1e-6);
        core::lens::WideAngle persp;
        persp.model = core::lens::Model::Perspective;
        const auto straight = core::lens::Camera::of(persp, 0, 0, 600, 400).arc({50, 50}, {550, 80}, 10);
        QVERIFY(std::fabs(straight[5][1] - 65) < 2.0);
        core::lens::WideAngle fish;
        fish.model = core::lens::Model::Fisheye;
        fish.focal_length = 8;
        const auto bent = core::lens::Camera::of(fish, 0, 0, 600, 400).arc({50, 60}, {550, 60}, 10);
        QVERIFY2(bent[5][1] < 50.0, "a line near the top bows away from the centre");
    }

    void wideAngleConstraintsBecomeStraightAndLevel() {
        core::lens::WideAngle p;
        p.model = core::lens::Model::Fisheye;
        p.focal_length = 8;
        p.constraints = {core::lens::Constraint{{60, 70}, {420, 70}, core::lens::Orientation::Horizontal},
                         core::lens::Constraint{{80, 60}, {80, 260}, core::lens::Orientation::Free}};
        const auto cam = core::lens::Camera::of(p, 0, 0, 480, 320);
        double before = 0;
        for (const XY& q : cam.arc({60, 70}, {420, 70}, 24)) before = std::max(before, std::fabs(q[1] - 70));
        QVERIFY(before > 10.0);
        const core::lens::Mesh mesh = core::lens::solve(p, 0, 0, 480, 320);
        QVERIFY2(mesh.residual < 2.0, qPrintable(QString::number(mesh.residual)));
        double lo = 1e9, hi = -1e9;
        for (const XY& q : mesh.curves[0]) {
            const XY at = mesh.map(0, 0, 480, 320, q);
            lo = std::min(lo, at[1]);
            hi = std::max(hi, at[1]);
        }
        QVERIFY2(hi - lo < 2.0, qPrintable(QString::number(hi - lo)));
        const XY far = mesh.map(0, 0, 480, 320, {300, 250});
        QVERIFY(std::fabs(far[0] - 300) < 80 && std::fabs(far[1] - 250) < 40);
    }

    void wideAngleWithoutConstraintsIsTheIdentity() {
        const core::lens::Mesh mesh = core::lens::solve(core::lens::WideAngle{}, 0, 0, 64, 48);
        for (const auto& [o, s] : mesh.verts) QVERIFY(std::fabs(o[0] - s[0]) < 1e-9 && std::fabs(o[1] - s[1]) < 1e-9);
        const render::Image flat = render::Image::create("RGBA", render::Size{64, 48}, render::Ink{51, 102, 153, 255});
        QCOMPARE(QString::fromStdString(run_eight(flat, "adaptive_wide_angle", Json::object()) == flat.tobytes() ? "same" : "changed"), QStringLiteral("same"));
        const std::string smaller = run_eight(flat, "adaptive_wide_angle", Json{{"scale", 80}, {"interpolation", "bilinear"}});
        QVERIFY(eight_px(smaller, 64, 32, 24)[3] > 252);
        QVERIFY(eight_px(smaller, 64, 1, 1)[3] < 3);
    }

    void wideAngleLeavesNoGapsAcrossTiles() {
        const render::Image flat = render::Image::create("RGBA", render::Size{772, 517}, render::Ink{51, 102, 153, 255});
        const Json fisheye{{"model", "fisheye"}, {"focal_length", 12}, {"interpolation", "bilinear"},
                           {"constraints", Json::array({Json{{"a", Json::array({mm(20), mm(30)})}, {"b", Json::array({mm(700), mm(35)})}, {"orientation", "horizontal"}}})}};
        for (const Json& params : {Json::object(), fisheye}) {
            const std::string out = run_eight(flat, "adaptive_wide_angle", params, 1, {128, 2});
            int holes = 0;
            for (int y = 80; y < 440; ++y)
                for (int x = 80; x < 690; ++x) holes += eight_px(out, 772, x, y)[3] < 128;
            QVERIFY2(holes == 0, qPrintable(QStringLiteral("%1 holes").arg(holes)));
            QCOMPARE(run_eight(flat, "adaptive_wide_angle", params, 1, {256, 1}), out);  // (the tiling changes nothing)
        }
    }

    void wideAngleThroughTheOp() {
        const auto doc = eight_page(120, 80, {{"paint", eight_of(120, 80, grid_picture(120, 80, 12))}});
        const Json op{{"op", "filter_raster"}, {"page", 1}, {"id", "paint"}, {"kind", "adaptive_wide_angle"}, {"model", "fisheye"}, {"focal_length", 10},
                      {"constraints", Json::array({Json{{"a", Json::array({mm(10), mm(15)})}, {"b", Json::array({mm(110), mm(15)})}, {"orientation", "horizontal"}}})}};
        const core::ApplyResult done = apply_ops(doc, Json::array({op}));
        const Json report = done.results.at(0)["filter"];
        QCOMPARE(report["model"], Json("fisheye"));
        QVERIFY2(report["residual"].get<double>() < 2.0, report.dump().c_str());
        QCOMPARE(report["constraints"], Json(1));
        QVERIFY(picture_of(done.doc, "paint").tobytes() != picture_of(doc, "paint").tobytes());
        Json wrong = op;
        wrong["constraints"] = Json::array({Json{{"a", Json::array({0, 0})}}});
        QVERIFY2(refusal(doc, wrong).find("constraints must be") != std::string::npos, refusal(doc, wrong).c_str());
        // as a smart filter on a 16-bit page: drawn again, its corners cleared by the scale
        const auto deep = apply_ops(precise_page(120, 80, grid_picture(120, 80, 12), false),
                                    Json::array({Json{{"op", "smart_convert"}, {"page", 1}, {"ids", Json::array({"paint"})}, {"new_id", "so"}},
                                                 Json{{"op", "smart_filter_add"}, {"page", 1}, {"id", "so"}, {"kind", "adaptive_wide_angle"}, {"model", "perspective"},
                                                      {"scale", 90}}}))
                              .doc;
        const render::Image drawn = render::layer_image(deep.page(0), layer_of(deep, "so"), 200, &deep);
        QVERIFY(drawn.getpixel(1, 1)[3] < 128);
        QVERIFY(drawn.getpixel(60, 40)[3] > 250);
    }

    // Both the same on the paper at 200 and 600 dpi (a 600 dpi layer's result shrunk by 3 near the 200 dpi one's), and on an
    // opened picture's page (its grid "finest").
    void theSameLookAtEveryResolution() {
        const auto fine = grid_picture(360, 240, 36);
        const auto coarse = grid_picture(120, 80, 12);
        const Json wide{{"model", "fisheye"}, {"focal_length", 10},
                        {"constraints", Json::array({Json{{"a", Json::array({mm(10), mm(15)})}, {"b", Json::array({mm(110), mm(15)})}, {"orientation", "horizontal"}}})}};
        for (const auto& [kind, params] : std::vector<std::pair<std::string, Json>>{{"lens_correction", Json{{"distortion", -40}, {"vignette_amount", 40}}},
                                                                                    {"adaptive_wide_angle", wide}}) {
            const std::string big = run_precise(precise(360, 240, fine, true), kind, params, {}, 3.0);
            const std::string small = run_precise(precise(120, 80, coarse, true), kind, params, {}, 1.0);
            double total = 0;
            for (std::uint32_t y = 0; y < 80; ++y)
                for (std::uint32_t x = 0; x < 120; ++x) {
                    double mean = 0;
                    for (std::uint32_t dy = 0; dy < 3; ++dy)
                        for (std::uint32_t dx = 0; dx < 3; ++dx) mean += pixel(big, x * 3 + dx, y * 3 + dy)[3] / 9.0;
                    total += std::fabs(mean - pixel(small, x, y)[3]);
                }
            QVERIFY2(total / (120.0 * 80.0) < 0.03, qPrintable(QStringLiteral("%1: %2").arg(QString::fromStdString(kind)).arg(total / 9600.0)));
        }
        // an opened picture (300 ppi, its own pixels)
        const render::Image picture = eight_of(300, 200, grid_picture(300, 200)).convert("RGB");
        const std::string png = formats::png_bytes(picture, formats::PngSave{std::array<double, 2>{300, 300}, {}, false});
        const core::Document opened = formats::open_file(png, "picture.png").doc;
        const std::string id = opened.page(0).layers[0].id;
        for (const Json& op : {Json{{"op", "filter_raster"}, {"page", 1}, {"id", id}, {"kind", "lens_correction"}, {"distortion", -30}},
                               Json{{"op", "filter_raster"}, {"page", 1}, {"id", id}, {"kind", "adaptive_wide_angle"}, {"scale", 80}}}) {
            const core::Document done = apply_ops(opened, Json::array({op})).doc;
            const render::Image after = render::raster::layer_pixels(done.page(0), done.page(0).layers[0]).convert("RGBA");
            QCOMPARE(after.size(), (render::Size{300, 200}));
            QVERIFY2(after.getpixel(0, 0)[3] < 128, op.dump().c_str());
        }
    }

    // A selection keeps the result to it, the frame still the picture's (its centre the lens's).
    void theFrameIsThePicturesInsideASelection() {
        const auto doc = eight_page(120, 80, {{"paint", eight_of(120, 80, grid_picture(120, 80, 12))}});
        const Json op{{"op", "filter_raster"}, {"page", 1}, {"id", "paint"}, {"kind", "lens_correction"}, {"distortion", -60},
                      {"area", Json{{"rect", Json::array({0, 0, mm(60), mm(80)})}}}};
        const render::Image after = picture_of(apply_ops(doc, Json::array({op})).doc, "paint");
        const render::Image whole = picture_of(apply_ops(doc, Json::array({Json{{"op", "filter_raster"}, {"page", 1}, {"id", "paint"}, {"kind", "lens_correction"},
                                                                               {"distortion", -60}}}))
                                                   .doc,
                                               "paint");
        QCOMPARE(after.getpixel(10, 40), whole.getpixel(10, 40));          // (inside: as the whole one)
        QCOMPARE(after.getpixel(110, 2), picture_of(doc, "paint").getpixel(110, 2));  // (outside: as it was)
    }

    // --- 消点 (vanishing.rs, vp_cmds.rs) ----------------------------------------------------------------------------------------

    void focalAndMetricAspectFromOnePlane() {
        const XY c{400, 300};
        const auto plane = floor_quad(500, c);
        const auto f = render::vanishing::focal(plane, c);
        QVERIFY(f && std::fabs(*f - 500) < 1.0);
        const auto [lu, lv] = render::vanishing::lift(plane, *f, c).lengths();
        QVERIFY2(std::fabs(lu / lv - 2.0) < 0.01, qPrintable(QString::number(lu / lv)));
    }

    void aPlaneTornOffIsPerpendicular() {
        const XY c{400, 300};
        auto scene = render::vanishing::Scene::make(floor_quad(500, c), c, std::nullopt, 800);
        const auto k = scene.tear_off(0, render::vanishing::Edge::Top, 90, 1);
        QVERIFY(k);
        const auto& p = scene.planes[*k];
        QVERIFY(std::fabs(p[3][0] - scene.planes[0][0][0]) < 1e-6 && std::fabs(p[2][0] - scene.planes[0][1][0]) < 1e-6);
        const auto a = scene.lifted(0), b = scene.lifted(*k);
        const auto normal = [](const render::vanishing::Lifted& l) {
            const std::array<double, 3> u{l.pts[1][0] - l.pts[0][0], l.pts[1][1] - l.pts[0][1], l.pts[1][2] - l.pts[0][2]};
            const std::array<double, 3> v{l.pts[3][0] - l.pts[0][0], l.pts[3][1] - l.pts[0][1], l.pts[3][2] - l.pts[0][2]};
            std::array<double, 3> n{u[1] * v[2] - u[2] * v[1], u[2] * v[0] - u[0] * v[2], u[0] * v[1] - u[1] * v[0]};
            const double len = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
            return std::array<double, 3>{n[0] / len, n[1] / len, n[2] / len};
        };
        const auto n1 = normal(a), n2 = normal(b);
        QVERIFY(std::fabs(n1[0] * n2[0] + n1[1] * n2[1] + n1[2] * n2[2]) < 0.02);
    }

    void pastingAndStampingInPerspective() {
        const XY c{400, 300};
        const auto scene = render::vanishing::Scene::make(floor_quad(500, c), c, std::nullopt, 800);
        const auto h = render::vanishing::homography(scene.planes[0]);
        QVERIFY(h);
        // a red picture pasted at (0.25, 0.25), half the plane wide: around the plane's middle, not its corner (a grid of a mm
        // a pixel)
        render::resample::Plane red(100, 50, 4, true);
        for (int y = 0; y < 50; ++y)
            for (int x = 0; x < 100; ++x) std::copy_n(std::array<float, 4>{1, 0, 0, 1}.data(), 4, red.at(x, y));
        const render::Box grid{0, 0, 800, 600};
        const render::Box out = render::vanishing::paste_box(scene, 0, 1.0, grid);
        const auto made = render::vanishing::pasted(scene, 0, red, {0.25, 0.25}, 0.5, 1.0, out);
        QVERIFY(made);
        const auto [mx, my] = h->apply(0.5, 0.5);
        QVERIFY(made->at(int(mx) - out.x0, int(my) - out.y0)[3] > 0.9f);
        const auto [cx, cy] = h->apply(0.1, 0.1);
        QVERIFY(made->at(int(cx) - out.x0, int(cy) - out.y0)[3] < 0.1f);
        // the stamp: a red dot near the far edge copied from one near the near edge
        const auto [sx, sy] = h->apply(0.5, 0.8);
        const auto [dx, dy] = h->apply(0.5, 0.3);
        const render::vanishing::Stamp st = render::vanishing::stamp_coverage(scene, {sx, sy}, {{dx, dy}}, 10.0 * 200.0 / 25.4, 0.8, 1.0, grid);
        QCOMPARE(st.dabs, std::size_t(1));
        const auto from = render::vanishing::stamp_source(scene, st, {dx, dy}, 1.0);
        QVERIFY(from && std::fabs((*from)[0] - sx) < 1.0 && std::fabs((*from)[1] - sy) < 1.0);
        QVERIFY(st.cov[std::size_t(int(dy) - st.box.y0) * std::size_t(st.box.width()) + std::size_t(int(dx) - st.box.x0)] > 0.9f);
    }

    // vp_cmds.rs's test on Genko's page (its places in mm): a floor's plane and one torn off its top; a layer pasted onto it
    // (its middle covered, its far corner not); the stamp copying a stripe; the planes kept on the page and used again; a new
    // layer; refused without planes and with a plane of two corners.
    void theVanishingPointOp() {
        const render::Image grey = render::Image::create("RGBA", render::Size{400, 300}, render::Ink{64, 64, 64, 255});
        render::Image sticker = render::Image::create("RGBA", render::Size{400, 300}, render::Ink{0, 0, 0, 0});
        sticker.paste(render::Ink{255, 0, 0, 255}, render::Box{0, 0, 40, 20});
        const auto doc = eight_page(400, 300, {{"floor", grey}, {"sticker", sticker}});
        const Json planes = Json::array({Json{{"corners_mm", quad_json({{120, 120}, {280, 120}, {360, 280}, {40, 280}})}}, Json{{"from", 0}, {"edge", "top"}, {"angle", 90}}});
        const core::ApplyResult pasted = apply_ops(doc, Json::array({Json{{"op", "vanishing_point"}, {"page", 1}, {"id", "floor"}, {"planes", planes},
                                                                          {"paste", Json::array({Json{{"plane", 0}, {"layer", "sticker"}, {"at", Json::array({0.3, 0.4})}, {"width", 0.4}}})}}}));
        const Json report = pasted.results.at(0);
        QCOMPARE(report["planes"].size(), std::size_t(2));
        QVERIFY2(report["focal_mm"].get<double>() > mm(50), report.dump().c_str());
        QCOMPARE(report["pasted"], Json(1));
        const render::Image floor = picture_of(pasted.doc, "floor");
        const auto h = render::vanishing::homography(render::vanishing::Quad{XY{120, 120}, XY{280, 120}, XY{360, 280}, XY{40, 280}});
        const auto [x, y] = h->apply(0.5, 0.41);
        QVERIFY2(floor.getpixel(int(x), int(y))[0] > 230, "the paste covers the plane's middle");
        QVERIFY(floor.getpixel(130, 125)[0] < 128);
        QCOMPARE(picture_of(doc, "floor").tobytes(), grey.tobytes());  // (the book before it: one Undo)
        // the planes kept on the page (vanishing-point@1), used again without being given
        const Json* kept = core::page_vanishing(pasted.doc.page(0));
        QVERIFY(kept != nullptr);
        QCOMPARE((*kept)["planes"].size(), std::size_t(2));
        QVERIFY(core::uses_vanishing(pasted.doc));
        QVERIFY(!core::uses_vanishing(doc));
        // the stamp: a green stripe near the bottom copied to the top of the floor
        render::Image striped = grey;
        striped.paste(render::Ink{0, 255, 0, 255}, render::Box{180, 250, 220, 265});
        const auto with_stripe = eight_page(400, 300, {{"floor", striped}});
        const core::ApplyResult cloned = apply_ops(
            with_stripe, Json::array({Json{{"op", "vanishing_point"}, {"page", 1}, {"id", "floor"}, {"planes", planes},
                                           {"clone", Json::array({Json{{"source", Json::array({mm(200), mm(257)})}, {"points", quad_json({{200, 160}, {205, 160}})},
                                                                       {"size_px", 12}, {"hardness", 80}}})}}}));
        QVERIFY2(cloned.results.at(0)["dabs"].get<int>() >= 1, cloned.results.at(0).dump().c_str());
        QVERIFY2(picture_of(cloned.doc, "floor").getpixel(200, 160)[1] > 200, "the stripe copied in perspective");
        // the page's planes used when none are given
        const auto again = apply_ops(pasted.doc, Json::array({Json{{"op", "vanishing_point"}, {"page", 1}, {"id", "floor"},
                                                                   {"paste", Json::array({Json{{"layer", "sticker"}, {"at", Json::array({0.1, 0.1})}, {"width", 0.2}}})}}}));
        QCOMPARE(again.results.at(0)["pasted"], Json(1));
        // refused: no planes on the page and none given; a plane of two corners; no layer
        QVERIFY2(refusal(doc, Json{{"op", "vanishing_point"}, {"page", 1}, {"id", "floor"}}).find("no planes") != std::string::npos,
                 refusal(doc, Json{{"op", "vanishing_point"}, {"page", 1}, {"id", "floor"}}).c_str());
        QVERIFY(!refusal(doc, Json{{"op", "vanishing_point"}, {"page", 1}, {"id", "floor"},
                                   {"planes", Json::array({Json{{"corners_mm", Json::array({Json::array({0, 0}), Json::array({1, 1})})}}})}})
                     .empty());
        QVERIFY(!refusal(doc, Json{{"op", "vanishing_point"}, {"page", 1}, {"planes", planes}}).empty());
        // a bow-tie (not convex): refused
        QVERIFY(refusal(doc, Json{{"op", "vanishing_point"}, {"page", 1}, {"id", "floor"},
                                  {"planes", Json::array({Json{{"corners_mm", quad_json({{120, 120}, {360, 280}, {280, 120}, {40, 280}})}}})}})
                    .find("convex") != std::string::npos);
        // a new layer above the one named
        const core::ApplyResult made = apply_ops(doc, Json::array({Json{{"op", "vanishing_point"}, {"page", 1}, {"id", "floor"}, {"planes", planes}, {"new_layer", true},
                                                                        {"paste", Json::array({Json{{"layer", "sticker"}}})}}}));
        const std::string id = made.results.at(0)["id"].get<std::string>();
        QVERIFY(id != "floor");
        QCOMPARE(made.doc.page(0).layers.size(), std::size_t(3));
        QCOMPARE(made.doc.page(0).layers[1].id, id);
        QCOMPARE(picture_of(made.doc, "floor").tobytes(), grey.tobytes());
        // keep_planes false: the page without them
        const auto unkept = apply_ops(doc, Json::array({Json{{"op", "vanishing_point"}, {"page", 1}, {"id", "floor"}, {"planes", planes}, {"keep_planes", false}}})).doc;
        QVERIFY(core::page_vanishing(unkept.page(0)) == nullptr);
    }

    // On every kind of a paint layer's pixels (precise 16 and 32 bits, CMYK), and inside a selection.
    void theVanishingPointOnEveryKind() {
        const Json planes = Json::array({Json{{"corners_mm", quad_json({{30, 30}, {90, 30}, {110, 110}, {10, 110}})}}});
        std::vector<Px> red(120 * 120, Px{0, 0, 0, 0});
        for (std::uint32_t y = 0; y < 10; ++y)
            for (std::uint32_t x = 0; x < 20; ++x) red[std::size_t(y) * 120 + x] = {1, 0, 0, 1};
        for (const bool f32 : {false, true}) {
            auto doc = precise_page(120, 120, std::vector<Px>(120 * 120, Px{0.25f, 0.25f, 0.25f, 1}), f32);
            core::Layer sticker = doc.page(0).layers[0];
            sticker.id = "sticker";
            sticker.color_raster = std::make_shared<const std::string>(precise(120, 120, red, f32));
            doc.edit_page(0).layers.push_back(sticker);
            const auto done = apply_ops(doc, Json::array({Json{{"op", "vanishing_point"}, {"page", 1}, {"id", "paint"}, {"planes", planes},
                                                               {"paste", Json::array({Json{{"layer", "sticker"}, {"at", Json::array({0, 0})}, {"width", 1}}})}}}))
                                  .doc;
            const std::string& after = *layer_of(done, "paint").color_raster;
            QVERIFY2(pixel(after, 60, 45)[0] > 0.9 && pixel(after, 60, 45)[1] < 0.1, f32 ? "f32" : "u16");
            QVERIFY(std::fabs(pixel(after, 2, 2)[0] - 0.25) < 1e-4);
        }
        auto cmyk = cmyk_page(120, [](std::uint32_t, std::uint32_t, unsigned c) { return c == 3 ? 0.2 : 0.0; });
        render::Image picture = render::Image::create("RGBA", render::Size{20, 10}, render::Ink{255, 0, 0, 255});
        const auto inked = apply_ops(cmyk, Json::array({Json{{"op", "vanishing_point"}, {"page", 1}, {"id", "pic"}, {"planes", planes},
                                                            {"paste", Json::array({Json{{"data", core::b64encode(render::write_png(picture))}, {"at", Json::array({0, 0})},
                                                                                        {"width", 1}}})}}}))
                               .doc;
        const core::InkPixels inks = core::decode_ink_raster(*layer_of(inked, "pic").ink_raster);
        QVERIFY2(inks.value(45 * 120 + 60, 1) > 0.5, "magenta for red");  // (the red pasted: magenta and yellow)
        QVERIFY(std::fabs(inks.value(2 * 120 + 2, 3) - 0.2) < 1e-3);
        // an opened picture's page (its grid "finest", 300 ppi): the paste on the picture's own pixels
        {
            const render::Image photo = eight_of(300, 200, grid_picture(300, 200)).convert("RGB");
            const std::string png = formats::png_bytes(photo, formats::PngSave{std::array<double, 2>{300, 300}, {}, false});
            const core::Document opened = formats::open_file(png, "photo.png").doc;
            const std::string id = opened.page(0).layers[0].id;
            const render::vanishing::Quad quad{XY{5, 3}, XY{20, 3}, XY{23, 15}, XY{2, 15}};
            Json corners = Json::array();
            for (const XY& q : quad) corners.push_back(Json::array({q[0], q[1]}));
            const auto done = apply_ops(opened, Json::array({Json{{"op", "vanishing_point"}, {"page", 1}, {"id", id},
                                                                  {"planes", Json::array({Json{{"corners_mm", corners}}})},
                                                                  {"paste", Json::array({Json{{"data", core::b64encode(render::write_png(picture))}, {"at", Json::array({0, 0})},
                                                                                              {"width", 1}}})}}}))
                                  .doc;
            const render::Image after = render::raster::layer_pixels(done.page(0), done.page(0).layers[0]).convert("RGBA");
            QCOMPARE(after.size(), (render::Size{300, 200}));
            const auto h = render::vanishing::homography(quad);
            const auto [mx, my] = h->apply(0.5, 0.2);
            const auto got = after.getpixel(static_cast<int>(mx * 300 / 25.4), static_cast<int>(my * 300 / 25.4));
            QVERIFY2(got[0] > 230 && got[1] < 30, "the paste on the picture's own grid");
        }
        // inside a selection: the left half only
        const auto doc = eight_page(120, 120, {{"floor", render::Image::create("RGBA", render::Size{120, 120}, render::Ink{64, 64, 64, 255})},
                                               {"sticker", render::Image::create("RGBA", render::Size{120, 120}, render::Ink{255, 0, 0, 255})}});
        const auto half = apply_ops(doc, Json::array({Json{{"op", "vanishing_point"}, {"page", 1}, {"id", "floor"}, {"planes", planes},
                                                           {"area", Json{{"rect", Json::array({0, 0, mm(60), mm(120)})}}},
                                                           {"paste", Json::array({Json{{"layer", "sticker"}, {"at", Json::array({0, 0})}, {"width", 1}}})}}}))
                              .doc;
        const render::Image after = picture_of(half, "floor");
        QVERIFY(after.getpixel(40, 45)[0] > 230);
        QCOMPARE(after.getpixel(80, 45)[0], 64.0);
    }

    // vanishing-point@1: the planes written with the page and the feature named, read back the same; a book whose planes are
    // broken read-only; one without them names nothing.
    void thePlanesAreKeptWithTheBook() {
        const auto doc = eight_page(400, 300, {{"floor", render::Image::create("RGBA", render::Size{400, 300}, render::Ink{64, 64, 64, 255})}});
        const Json planes = Json::array({Json{{"corners_mm", quad_json({{120, 120}, {280, 120}, {360, 280}, {40, 280}})}}, Json{{"from", 0}, {"edge", "left"}, {"angle", 80}}});
        const auto done = apply_ops(doc, Json::array({Json{{"op", "vanishing_point"}, {"page", 1}, {"id", "floor"}, {"planes", planes}, {"focal_mm", 60}}})).doc;
        QTemporaryDir tmp;
        const std::filesystem::path dir = std::filesystem::path(tmp.path().toStdString()) / "b";
        const auto save = [](const core::Document& book, const std::filesystem::path& at) {
            storage::ProjectLock lock(at, "human:test");
            lock.try_acquire();
            storage::SaveRequest request;
            request.actor = "human:test";
            request.ops = Json::array();
            storage::Saver(lock).save(book, request);
        };
        save(done, dir);
        const auto loaded = storage::load_document(dir);
        QVERIFY2(loaded.document.read_only_reason.empty(), loaded.document.read_only_reason.c_str());
        QVERIFY(std::find(loaded.document.features.begin(), loaded.document.features.end(), std::string(core::kVanishingFeature)) != loaded.document.features.end());
        QVERIFY(core::page_vanishing(loaded.document.page(0)) != nullptr);
        QCOMPARE(*core::page_vanishing(loaded.document.page(0)), *core::page_vanishing(done.page(0)));
        QCOMPARE((*core::page_vanishing(loaded.document.page(0)))["focal_mm"].get<double>(), 60.0);
        // a book without planes: no feature
        const std::filesystem::path plain = std::filesystem::path(tmp.path().toStdString()) / "plain";
        save(doc, plain);
        const auto read_plain = storage::load_document(plain);
        QVERIFY(std::find(read_plain.document.features.begin(), read_plain.document.features.end(), std::string(core::kVanishingFeature)) ==
                read_plain.document.features.end());
        // broken planes on disk (a bow-tie): the book opens read-only
        bool broke = false;
        for (const auto& entry : std::filesystem::recursive_directory_iterator(dir)) {
            if (!entry.is_regular_file() || entry.path().extension() != ".json") continue;
            std::ifstream in(entry.path(), std::ios::binary);
            std::stringstream text;
            text << in.rdbuf();
            const std::string body = text.str();
            if (body.find("\"vanishing\"") == std::string::npos) continue;
            Json json = Json::parse(body);
            std::function<bool(Json&)> twist = [&](Json& node) {
                if (node.is_object()) {
                    if (node.contains("vanishing") && node["vanishing"].is_object()) {
                        Json& corners = node["vanishing"]["planes"][0]["corners_mm"];
                        std::swap(corners[1], corners[2]);
                        return true;
                    }
                    for (auto& [key, value] : node.items())
                        if (twist(value)) return true;
                } else if (node.is_array()) {
                    for (Json& value : node)
                        if (twist(value)) return true;
                }
                return false;
            };
            if (!twist(json)) continue;
            std::ofstream(entry.path(), std::ios::binary | std::ios::trunc) << json.dump();
            broke = true;
        }
        QVERIFY(broke);
        const auto twisted = storage::load_document(dir);
        QVERIFY(!twisted.document.read_only_reason.empty());
        const bool said = std::any_of(twisted.report.issues.begin(), twisted.report.issues.end(),
                                      [](const storage::LoadIssue& issue) { return issue.message.find("vanishing point planes") != std::string::npos; });
        QVERIFY2(said, twisted.document.read_only_reason.c_str());
        QVERIFY(core::page_vanishing(twisted.document.page(0)) != nullptr);  // (kept as read: never written without them)
        QVERIFY(storage::is_known_feature(core::kVanishingFeature));
    }
};

QTEST_GUILESS_MAIN(TestFxLens)
#include "test_fx_lens.moc"
