// PC4 E-0 (SPEC.md COLOR-09, ARCHITECTURE.md「描く道具の土台」): a layer's sparse tiles (render/tiled_pixels) and the
// sequential-dab stroke on them (retouch::apply_dab_stroke and Smudge: upstream's paint/src/retouch.rs tests), on every
// kind of layer (8 bits; 16 and 32 bits; the inks of CMYK, Lab and grey pages): only the tiles reached are read (each
// once, all in one read) and only the pixels edited written back — every other pixel of the layer keeps its value —; the
// tiles read are the layer's values as the windowed read gives them; the dabs run in order on a working copy each sees,
// and are mixed back by the opacity and the selection (a protected alpha kept); the Smudge drags a colour along; the
// tiles are held in the imaging budget (past it: too_large, nothing changed); and the drawing tools' and patterns' ops
// are frames until their groups (not_yet_ported naming the group).

#include <QtTest>

#include <algorithm>
#include <array>
#include <cmath>
#include <functional>
#include <string>
#include <vector>

#include "core/command_bus.hpp"
#include "core/error.hpp"
#include "render/image.hpp"
#include "render/image_ops.hpp"
#include "render/ops_registry.hpp"
#include "render/raster.hpp"
#include "render/retouch_dabs.hpp"
#include "render/tiled_pixels.hpp"
#include "testsupport.hpp"

using namespace genko;
using core::Json;
using render::Box;
using render::Size;
using render::image_ops::Pixels;
using render::tiled::TiledPixels;
namespace retouch = render::retouch;

namespace {

constexpr double kInch = 25.4;
constexpr int kSide = 600;  // (3 inches at the working 200 dpi: 3 × 3 tiles on an 8-bit layer)

core::ApplyResult run_ops(const core::Document& doc, const Json& ops) {
    return core::CommandBus(render::ops_registry()).apply(doc, ops, core::Actor("human:test"));
}
core::Document run(const core::Document& doc, const Json& op) { return run_ops(doc, Json::array({op})).doc; }

// A page of one 8-bit paint layer "paint": a colour that changes across it, opaque but a clear corner.
core::Document book() {
    auto doc = core::new_episode("タイル", core::Num(1), 1,
                                 core::PageSpec::custom(3 * kInch, 3 * kInch, 60, 60, 1, 2, 2, 2, 2, 200, "color"));
    auto& p = doc.edit_page(0);
    p.numero = false;
    p.frames.clear();
    p.layers.clear();
    core::Layer layer;
    layer.id = "paint";
    layer.role = core::LayerRole::User;
    layer.kind = core::LayerKind::Raster;
    layer.panel_clip = false;
    std::string bytes;
    bytes.reserve(std::size_t(kSide) * kSide * 4);
    for (int y = 0; y < kSide; ++y) {
        for (int x = 0; x < kSide; ++x) {
            bytes.push_back(static_cast<char>(x * 255 / kSide));
            bytes.push_back(static_cast<char>(y * 255 / kSide));
            bytes.push_back(static_cast<char>((x + y) % 256));
            bytes.push_back(static_cast<char>(x < 40 && y < 40 ? 0 : 255));
        }
    }
    render::raster::save_raster(p, layer, render::Image::frombytes("RGBA", Size{kSide, kSide}, bytes));
    p.layers.push_back(layer);
    return doc;
}

// The book's page as a kind: "8", "16" and "32" (precise RGB), "cmyk" and "lab" (16 bits), "gray".
core::Document as_kind(const core::Document& doc, const std::string& kind) {
    if (kind == "8") return doc;
    if (kind == "16" || kind == "32") return run(doc, Json{{"op", "set_color_mode"}, {"page", 1}, {"mode", "rgb"}, {"depth", kind == "16" ? 16 : 32}});
    if (kind == "cmyk") return run(doc, Json{{"op", "set_color_mode"}, {"page", 1}, {"mode", "cmyk"}, {"profile", "coated-cmyk"}, {"depth", 16}});
    if (kind == "lab") return run(doc, Json{{"op", "set_color_mode"}, {"page", 1}, {"mode", "lab"}, {"depth", 16}});
    return run(doc, Json{{"op", "set_color_mode"}, {"page", 1}, {"mode", "gray"}});
}

const std::vector<std::string>& kinds() {
    static const std::vector<std::string> all{"8", "16", "32", "cmyk", "lab", "gray"};
    return all;
}

// The layer's whole values (as the windowed read gives them).
Pixels whole(core::Document& doc) {
    const Json op = Json::object();
    const core::Actor actor("human:test");
    core::OpContext c{doc, op, actor};
    render::image_ops::Window window;
    (void)render::image_ops::layer_pixels(c, 0, "paint", Box{0, 0, 1, 1}, &window);
    return render::image_ops::layer_pixels(c, 0, "paint", Box{0, 0, window.grid.width, window.grid.height});
}

// The pixels of `a` and `b` (the same grid) that differ.
std::vector<std::pair<int, int>> differing(const Pixels& a, const Pixels& b) {
    std::vector<std::pair<int, int>> out;
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (!std::equal(a.at(i), a.at(i) + a.all(), b.at(i))) out.emplace_back(static_cast<int>(i % a.width()), static_cast<int>(i / a.width()));
    }
    return out;
}

std::string refusal(const core::Document& doc, const Json& op) {
    try {
        run_ops(doc, Json::array({op}));
    } catch (const core::ApplyError& error) {
        return error.code() + ": " + error.what();
    }
    return {};
}

retouch::Brush round(float size) {
    retouch::Brush b;
    b.size = size;
    b.hardness = 1.0f;
    b.spacing = 0.25f;
    b.pressure_size = false;
    return b;
}

}  // namespace

class TestTiledPixels : public QObject {
    Q_OBJECT

private slots:
    // The tiles boxes reach: each once, row by row, clamped to the grid (smaller at its edges), grown by a margin.
    void theTilesReached() {
        const Size grid{600, 520};
        QCOMPARE(render::tiled::tiles_reached(grid, {Box{10, 10, 20, 20}}), (std::vector<Box>{Box{0, 0, 256, 256}}));
        QCOMPARE(render::tiled::tiles_reached(grid, {Box{250, 250, 260, 260}, Box{10, 10, 20, 20}}),
                 (std::vector<Box>{Box{0, 0, 256, 256}, Box{256, 0, 512, 256}, Box{0, 256, 256, 512}, Box{256, 256, 512, 512}}));
        QCOMPARE(render::tiled::tiles_reached(grid, {Box{590, 515, 700, 900}}), (std::vector<Box>{Box{512, 512, 600, 520}}));
        QCOMPARE(render::tiled::tiles_reached(grid, {Box{240, 10, 250, 20}}, 10), (std::vector<Box>{Box{0, 0, 256, 256}, Box{256, 0, 512, 256}}));
        QVERIFY(render::tiled::tiles_reached(grid, {Box{-50, -50, -10, -10}, Box{5, 5, 5, 9}}).empty());
    }

    // On every kind of layer: a tile read once, its values as the windowed read gives them; a pixel changed written back
    // and every other pixel of the layer as it was (a clear one's hidden colours too); only the pixels edited written; the multi-window read of several
    // boxes the same as each read alone.
    void onlyTheTilesReachedAreReadAndWritten() {
        for (const std::string& kind : kinds()) {
            core::Document doc = as_kind(book(), kind);
            const Pixels before = whole(doc);
            const Json op = Json::object();
            const core::Actor actor("human:test");
            core::OpContext c{doc, op, actor};
            TiledPixels tiles(c, 0, "paint");
            const Size grid = tiles.grid();
            QCOMPARE(grid, (Size{static_cast<int>(before.width()), static_cast<int>(before.height())}));
            QVERIFY2(grid.width > 2 * render::tiled::kTile, kind.c_str());
            QCOMPARE(tiles.reach({Box{300, 300, 310, 310}}), std::size_t(1));
            QCOMPARE(tiles.reach({Box{300, 300, 310, 310}}), std::size_t(0));
            QCOMPARE(tiles.held(), (std::vector<Box>{Box{256, 256, 512, 512}}));
            QVERIFY(tiles.holds(256, 256) && !tiles.holds(255, 256) && !tiles.holds(-1, 0) && !tiles.holds(0, grid.height));
            bool refused = false;
            try {
                (void)tiles.at(0, 0);
            } catch (const core::Error&) {
                refused = true;
            }
            QVERIFY(refused);
            for (const auto& [x, y] : {std::pair{256, 256}, std::pair{300, 311}, std::pair{511, 511}}) {
                const float* got = tiles.at(x, y);
                const float* want = before.at(std::size_t(y) * before.width() + std::size_t(x));
                QVERIFY2(std::equal(got, got + tiles.all(), want), kind.c_str());
            }
            // (the multi-window read: each box as read alone)
            std::vector<Box> boxes;
            const std::vector<Pixels> parts =
                render::image_ops::layer_pixels(c, 0, "paint", std::vector<Box>{Box{0, 0, 3, 2}, Box{grid.width - 2, 5, grid.width + 9, 7}}, &boxes);
            QCOMPARE(boxes, (std::vector<Box>{Box{0, 0, 3, 2}, Box{grid.width - 2, 5, grid.width, 7}}));
            for (std::size_t k = 0; k < parts.size(); ++k) {
                const Pixels alone = render::image_ops::layer_pixels(c, 0, "paint", boxes[k]);
                QCOMPARE(parts[k].size(), alone.size());
                for (std::size_t i = 0; i < alone.size(); ++i) QVERIFY(std::equal(alone.at(i), alone.at(i) + alone.all(), parts[k].at(i)));
            }
            // a pixel changed: only it, and only its tile written
            QVERIFY(tiles.changed().empty());
            tiles.reach({Box{0, 300, 4, 304}});
            float* px = tiles.edit(300, 301);
            px[0] = 0.5f;
            px[tiles.colours()] = 0.75f;
            QCOMPARE(tiles.changed(), (std::vector<Box>{Box{256, 256, 512, 512}}));
            QCOMPARE(tiles.put(doc), (Box{300, 301, 301, 302}));  // (only the pixels edited are written)
            const Pixels after = whole(doc);
            QCOMPARE(differing(before, after), (std::vector<std::pair<int, int>>{{300, 301}}));
            QVERIFY2(std::fabs(after.at(std::size_t(301) * after.width() + 300)[after.colours()] - 0.75f) < 1e-3f, kind.c_str());
            // pixels far apart in one tile (opaque ones, and in the clear corner): only they written, every pixel between
            // them (a clear one's hidden colours or inks too) as it was
            TiledPixels again(c, 0, "paint");
            again.reach({Box{0, 0, 256, 512}});
            for (const auto& [x, y] : {std::pair{100, 300}, std::pair{200, 400}, std::pair{5, 5}, std::pair{30, 30}}) {
                float* v = again.edit(x, y);
                v[0] = 0.5f;
                v[again.colours()] = 0.75f;
            }
            QCOMPARE(again.put(doc), (Box{5, 5, 201, 401}));
            QCOMPARE(differing(after, whole(doc)), (std::vector<std::pair<int, int>>{{5, 5}, {30, 30}, {100, 300}, {200, 400}}));
        }
    }

    // A copy's tiles start clear (a working copy over the same grid); edit() marks a tile changed; past the imaging
    // budget the tiles are refused (too_large) and nothing is held.
    void aCopyAndTheBudget() {
        Pixels like(Pixels::Kind::Precise, 1, 1, 3);
        like.floating = true;
        TiledPixels copy(Size{1000, 300}, like);
        QCOMPARE(copy.reach({Box{0, 0, 1000, 300}}), std::size_t(8));
        QCOMPARE(copy.at(999, 299)[3], 0.0f);
        QCOMPARE(copy.kind(), Pixels::Kind::Precise);
        bool refused = false;
        core::Document doc = book();
        try {
            copy.put(doc);
        } catch (const core::Error&) {
            refused = true;
        }
        QVERIFY(refused);
        TiledPixels big(Size{4096, 4096}, like);
        {
            render::ImageAllocationBudget budget(16u << 20);  // (16 MB: some 16 tiles of floats)
            bool too_large = false;
            try {
                big.reach({Box{0, 0, 4096, 4096}});
            } catch (const core::OpError& error) {
                too_large = std::string(error.what()).find("too large") != std::string::npos;
            }
            QVERIFY(too_large);
        }
        QVERIFY(big.held().empty());
        // a layer's tiles read past the budget: refused, none held, the layer as it was
        // (each a budget the layer's own read fits in and its tiles in floats — 600² × 16 bytes, beside what the read
        // holds — do not: an 8-bit layer's picture is decoded in the budget; a precise layer's samples and an ink layer's
        // inks are read outside it)
        for (const auto& [kind, budget_bytes] : {std::pair{std::string("8"), 6u << 20}, std::pair{std::string("32"), 4u << 20},
                                                 std::pair{std::string("lab"), 4u << 20}}) {
            core::Document layered = as_kind(book(), kind);
            const Pixels before = whole(layered);
            const Json op = Json::object();
            const core::Actor actor("human:test");
            core::OpContext c{layered, op, actor};
            TiledPixels tiles(c, 0, "paint");
            const Size grid = tiles.grid();
            {
                render::ImageAllocationBudget budget(budget_bytes);
                bool too_large = false;
                std::string why = "nothing refused";
                try {
                    tiles.reach({Box{0, 0, grid.width, grid.height}});
                } catch (const core::OpError& error) {
                    why = error.what();
                    too_large = why.find("too large") != std::string::npos;
                } catch (const std::exception& error) {
                    why = error.what();
                }
                QVERIFY2(too_large, (kind + ": " + why).c_str());
            }
            QVERIFY2(tiles.held().empty() && tiles.changed().empty(), kind.c_str());
            QVERIFY2(differing(before, whole(layered)).empty(), kind.c_str());
        }
    }

    // upstream's dab_stroke_runs_sequentially_and_respects_selection: the dabs in order on a working copy each sees,
    // mixed back by the selection; and by the opacity; a protected alpha kept; a pixel the effect left keeps its value.
    void dabsRunInOrderWithinTheSelection() {
        for (const std::string& kind : kinds()) {
            core::Document doc = as_kind(book(), kind);
            const Pixels before = whole(doc);
            const Json op = Json::object();
            const core::Actor actor("human:test");
            core::OpContext c{doc, op, actor};
            TiledPixels tiles(c, 0, "paint");
            std::vector<std::size_t> seen;
            const std::vector<retouch::StrokePoint> points{{240.0, 250.0, 1.0f}, {280.0, 250.0, 1.0f}};
            const unsigned n = tiles.all();
            const Box changed = retouch::apply_dab_stroke(
                tiles, round(6.0f), points, [](int x, int) { return x < 260 ? 1.0f : 0.0f; }, false, 2,
                [&](TiledPixels& work, const retouch::Footprint& fp) {
                    seen.push_back(fp.dab.index);
                    for (int y = fp.rect.y0; y < fp.rect.y1; ++y) {
                        for (int x = fp.rect.x0; x < fp.rect.x1; ++x) {
                            if (fp.at(x, y) > 0.5f) work.edit(x, y)[0] = std::min(work.at(x, y)[0] + 0.1f, 1.0f);
                        }
                    }
                });
            QVERIFY(seen.size() > 2);
            for (std::size_t i = 0; i < seen.size(); ++i) QCOMPARE(seen[i], i);
            QVERIFY2(changed.x0 >= 236 && changed.x1 <= 260 && changed.width() > 0, kind.c_str());
            QCOMPARE(tiles.changed(), (std::vector<Box>{Box{0, 0, 256, 256}, Box{256, 0, 512, 256}}));
            const float* at = before.at(std::size_t(250) * before.width() + 250);
            QVERIFY2(tiles.at(250, 250)[0] > at[0] + 0.05f || at[0] > 0.95f, kind.c_str());  // (the dabs build up)
            const float* out = before.at(std::size_t(250) * before.width() + 270);
            QVERIFY(std::equal(out, out + n, tiles.at(270, 250)));  // (outside the selection: untouched)
            QCOMPARE(tiles.put(doc), changed);
            for (const auto& [x, y] : differing(before, whole(doc))) QVERIFY2(x >= changed.x0 && x < changed.x1 && y >= changed.y0 && y < changed.y1, kind.c_str());
        }
        // the opacity and a protected alpha
        core::Document doc = book();
        const Pixels before = whole(doc);
        const Json op = Json::object();
        const core::Actor actor("human:test");
        core::OpContext c{doc, op, actor};
        TiledPixels tiles(c, 0, "paint");
        retouch::Brush half = round(8.0f);
        half.opacity = 0.5f;
        retouch::apply_dab_stroke(tiles, half, {{100.0, 100.0, 1.0f}}, {}, true, 0, [](TiledPixels& work, const retouch::Footprint& fp) {
            for (int y = fp.rect.y0; y < fp.rect.y1; ++y) {
                for (int x = fp.rect.x0; x < fp.rect.x1; ++x) {
                    float* v = work.edit(x, y);
                    v[0] = 1.0f;
                    v[3] = 0.0f;
                }
            }
        });
        const float* was = before.at(std::size_t(100) * before.width() + 100);
        QVERIFY(std::fabs(tiles.at(100, 100)[0] - (was[0] + (1.0f - was[0]) * 0.5f)) < 1e-6f);
        QCOMPARE(tiles.at(100, 100)[3], was[3]);
    }

    // upstream's smudge_drags_colour_along_the_stroke (8, 16 and 32 bits): red dragged right of its edge, fading; off the
    // stroke untouched; finger painting starts with its colour.
    void smudgeDragsAColour() {
        for (const std::string& kind : {std::string("8"), std::string("16"), std::string("32")}) {
            Pixels like(kind == "8" ? Pixels::Kind::Eight : Pixels::Kind::Precise, 1, 1, 3);
            like.floating = kind == "32";
            TiledPixels surface(Size{100, 40}, like);
            surface.reach({Box{0, 0, 100, 40}});
            for (int y = 0; y < 40; ++y) {
                for (int x = 0; x < 100; ++x) {
                    float* v = surface.edit(x, y);
                    v[0] = 1.0f;
                    v[1] = v[2] = x < 30 ? 0.0f : 1.0f;
                    v[3] = 1.0f;
                }
            }
            const std::vector<retouch::StrokePoint> stroke{{20.0, 20.0, 1.0f}, {70.0, 20.0, 1.0f}};
            retouch::Smudge smudge(0.8f, std::nullopt, 12.0f);
            retouch::apply_dab_stroke(surface, round(12.0f), stroke, {}, false, smudge.halo(),
                                      [&](TiledPixels& w, const retouch::Footprint& fp) { smudge.dab(w, fp); });
            const auto g = [&](int x) { return surface.at(x, 20)[1]; };
            QVERIFY2(g(35) < 0.5f, kind.c_str());
            QVERIFY2(g(35) < g(60), kind.c_str());
            QCOMPARE(surface.at(35, 35)[1], 1.0f);
            TiledPixels white(Size{100, 40}, like);
            white.reach({Box{0, 0, 100, 40}});
            for (int y = 0; y < 40; ++y) {
                for (int x = 0; x < 100; ++x) std::fill_n(white.edit(x, y), 4, 1.0f);
            }
            retouch::Smudge finger(0.8f, std::vector<float>{0.0f, 0.0f, 1.0f, 1.0f}, 12.0f);
            retouch::apply_dab_stroke(white, round(12.0f), stroke, {}, false, finger.halo(),
                                      [&](TiledPixels& w, const retouch::Footprint& fp) { finger.dab(w, fp); });
            QVERIFY2(white.at(22, 20)[0] < 0.5f, kind.c_str());
        }
    }

    // The drawing tools' ops are frames until their group writes them. (The patterns' three ops are PC4 E3's now — what
    // they do is test_contract_patterns' —: an op of them without its keys is refused as theirs, not as a frame.)
    void theNewOpsAreFrames() {
        const core::Document doc = book();
        for (const char* op : {"dodge", "burn", "sponge", "blur_brush", "sharpen_brush", "smudge_brush", "mixer_brush", "background_eraser",
                               "color_replacement"}) {
            QCOMPARE(refusal(doc, Json{{"op", op}, {"page", 1}}), "not_yet_ported: ops[0] " + std::string(op) + ": " + op + " is not in the C++ build yet (PC4 E5)");
        }
        for (const char* op : {"add_pattern", "rename_pattern", "delete_pattern"}) {
            const std::string why = refusal(doc, Json{{"op", op}});
            QVERIFY2(!why.empty() && why.find("not_yet_ported") == std::string::npos && why.find("not in the C++ build") == std::string::npos, why.c_str());
        }
    }
};

QTEST_GUILESS_MAIN(TestTiledPixels)
#include "test_tiled_pixels.moc"
