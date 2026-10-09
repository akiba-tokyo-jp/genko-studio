// Pages carrying what the ops of M3-A1 make — pixels put, filtered, filled and erased; patches filled, transformed and
// pasted; masks set and painted; layers merged and converted; correction layers with each filter of filters.py; paper
// colours — and the covers add_cover makes (a jacket, a band, a back cover), the whole book then put on other paper
// (set_page_spec: the pixels moved onto the new basic frame), drawn by render::render_page against Python's
// genko.render.render_page, every pixel (RGB):
//   1. the book `pyref_harness.py make-rasterbook` makes, after the ops below (kDressing) applied by Python's apply_ops
//      and saved by save_episode: its pages 1 to 4 and its covers (5 to 7) drawn on both sides in print, proof and name
//      at 72, 150 and 350 dpi (page 3 with a nombre, a tone and a line of dialogue in its balloon; the jacket and the
//      band with their folds);
//   2. the same ops applied by this build (render::ops_registry) to the same book, drawn by this build: the same pixels
//      as Python's drawing of its own book;
//   3. five random parts of each page (proof at 150 dpi, print at 350), drawn alone: the same pixels as Python's whole
//      page cut there — of the book before the correction layers put on by hand (each part drawn by itself) and with
//      them (their filters look beyond each pixel: such a page's part is cut from the whole page drawn);
//   4. the same book put on another paper (78 × 104 mm, trimmed to 68 × 94), its pages with waves and a twirl (1, 2
//      and 4) drawn as in 1 and 2: sizes where numpy's float32 sine and cosine and another way of computing them part
//      (see dressing()).
// The user's decision D1 (SPEC §2): set_page_spec moves a layer's mask with what it masks, where Python leaves it
// stretched over the new paper. This build's book differs from Python's there alone — the masks page 1 has before the
// move, each Python's mask moved from the old basic frame onto the new one (render::relayout_mask) — and is drawn for 2
// and 4 with those masks as Python keeps them, every other pixel compared.
// Skipped without the Python reference.

#include <QtTest>

#include <QDir>
#include <QTemporaryDir>

#include <string>
#include <tuple>
#include <vector>

#include "core/command_bus.hpp"
#include "core/ids.hpp"
#include "render/bookpages.hpp"
#include "render/brushes.hpp"
#include "render/ops_registry.hpp"
#include "render/page.hpp"
#include "rendertest.hpp"
#include "storage/fsutil.hpp"
#include "storage/reader.hpp"

using genko::core::Json;
namespace render = genko::render;

namespace {

const std::vector<std::string> kModes{"print", "proof", "name"};
const std::vector<int> kDpis{72, 150, 350};
const std::vector<int> kPages{1, 2, 3, 4, 5, 6, 7};
const std::vector<int> kWavedPages{1, 2, 4};  // (a wave filtered into page 1, a twirl correction on 2, a wave on 4)
const Json kOtherPaper = Json::array({78, 104});
const Json kOtherTrim = Json::array({68, 94});

// A small RGBA picture (Pillow: rectangles and an ellipse on 24 × 18) and a grey one (30 × 20).
constexpr const char* kPicture =
    "iVBORw0KGgoAAAANSUhEUgAAABgAAAASCAYAAABB7B6eAAAAL0lEQVR4nGNgGAUEACMyR26BzQlqGPoo4YgFjM1EDQPxgVELRi0YtWA4WDAKCAIAOWEEGEUyqpIAAAAASUVORK5CYII=";
constexpr const char* kGrey =
    "iVBORw0KGgoAAAANSUhEUgAAAB4AAAAUCAAAAAC/wNIYAAAAXklEQVR4nH2SQRLAIAgDN07//2V6qa3QABeUJBgYCaYQ8eQRxpMEtb1m+CSJDgdg9RAg0cs1qrWb26EFcA3S15pbB636Y69ayBfjXO4cBvyri8mV63WE5Lz5E/n1M24N1gkwJ2mpfAAAAABJRU5ErkJggg==";
constexpr const char* kColours =
    "iVBORw0KGgoAAAANSUhEUgAAABQAAAAQCAYAAAAWGF8bAAAAXklEQVR4nGNgGOyAEZ/kCTm5/9jELR49wqkPqwQug4gxmIlcw3CpZcSmwOfbqkZ0hVu4wuqJcSnchaS4DJ9LMbxMKRj8BmKNFFIB1khBlyDHMAwDSTUUm1qqZ73BDwCofSgUOvhv4gAAAABJRU5ErkJggg==";

// The ops applied to the book before it is drawn (each succeeds in Python), the book then put on this paper and trim.
Json dressing(const Json& paper = Json::array({76, 100}), const Json& trim = Json::array({66, 90})) {
    Json ops = Json::array();
    const auto add = [&](const char* text) { ops.push_back(Json::parse(text)); };
    // page 1: pixels waved and erased roughly; a fill closing a gap; a radial gradient in a box; masks set and painted;
    // patches turned; a picture pasted larger; a mosaic correction in a box; its own paper
    add(R"({"op": "filter_raster", "page": 1, "id": "paint-1", "kind": "wave", "amplitude": 4, "wavelength": 30})");
    add(R"({"op": "fill", "page": 1, "x_mm": 25, "y_mm": 55, "gap_mm": 1.5, "rgb": [250, 210, 120]})");
    add(R"({"op": "gradient_fill", "page": 1, "layer_id": "pen-1", "from": [40, 50], "to": [56, 66], "shape": "radial",
            "rgb_from": [255, 0, 0], "rgb_to": [0, 0, 255], "area": {"rect": [40, 50, 16, 16]}})");
    add(R"({"op": "set_layer_mask", "page": 1, "id": "in-a", "area": {"ellipse": [15, 55, 15, 15]}})");
    add(R"({"op": "paint_mask", "page": 1, "id": "mask-1", "points": [[15, 15], [35, 40]], "width_mm": 5, "show": false})");
    add(R"({"op": "transform_area", "page": 1, "layer_id": "in-b", "area": {"rect": [40, 44, 16, 14]},
            "matrix": [0.866, 0.5, -0.5, 0.866, 20, -15]})");
    ops.push_back(Json::object({{"op", "paste"}, {"page", 1}, {"layer_id", "pen-1"},
                                {"items", Json::object({{"patches", Json::array({Json::object({{"box", Json::array({20, 20, 10, 6.667})},
                                                                                               {"mode", "mask"},
                                                                                               {"png", kGrey},
                                                                                               {"rgb", Json::array({200, 10, 10})}})})}})},
                                {"matrix", Json::array({1.5, 0, 0, 1.5, -5, 0})}}));
    add(R"({"op": "add_layer", "page": 1, "kind": "adjust", "id": "adj-levels", "adjust": {"kind": "posterize", "levels": 4}})");
    add(R"({"op": "set_layer_mask", "page": 1, "id": "adj-levels", "area": {"rect": [10, 60, 25, 20]}})");
    add(R"({"op": "erase_raster", "page": 1, "layer_id": "paint-1", "points": [[10, 80], [60, 85]], "texture": "rough",
            "width_mm": 4})");
    add(R"({"op": "set_paper", "page": 1, "rgb": [250, 245, 230]})");
    // page 2: an enclosed fill, a feathered area filled, the background flood-filled, the ink merged into the name, the
    // gradient made pixels, a twirl
    add(R"({"op": "fill_enclosed", "page": 2, "poly": [[15, 42], [52, 42], [52, 78], [15, 78]], "ignore": ["draft"],
            "rgb": [120, 200, 255]})");
    add(R"({"op": "fill_area", "page": 2, "layer_id": "paint-2", "area": {"union": [{"rect": [20, 20, 15, 10]},
            {"ellipse": [30, 25, 12, 12]}], "feather_mm": 1}, "rgb": [200, 60, 160], "opacity": 0.7})");
    add(R"({"op": "flood_fill", "page": 2, "layer": "bg", "x_mm": 3, "y_mm": 3, "rgb": [230, 240, 255]})");
    add(R"({"op": "merge_down", "page": 2, "id": "00000000000a"})");
    add(R"({"op": "convert_layer", "page": 2, "id": "grad-2", "to": "paint"})");
    add(R"({"op": "add_layer", "page": 2, "kind": "adjust", "id": "adj-hue", "adjust": {"kind": "hue", "shift": 40}})");
    // page 4: a picture over the page; the page merged into a copy and glowing; a part deleted; grey paper
    ops.push_back(Json::object({{"op", "put_raster"}, {"page", 4}, {"layer", "finish"}, {"png_base64", kPicture}}));
    add(R"({"op": "merge_visible", "page": 4, "id": "vis-4"})");
    add(R"({"op": "filter_raster", "page": 4, "id": "vis-4", "kind": "glow", "radius": 5, "threshold": 120})");
    add(R"({"op": "set_layer", "page": 4, "id": "vis-4", "opacity": 0.5})");
    add(R"({"op": "delete_area", "page": 4, "layer_id": "multi-4", "area": {"rect": [30, 40, 10, 10]}})");
    ops.push_back(Json::object({{"op", "put_raster"}, {"page", 4}, {"id", "blank-4"}, {"png_base64", kColours}}));
    add(R"({"op": "set_paper", "page": 4, "rgb": [235, 235, 235]})");
    // covers (pages 5 to 7), then the book on a larger paper: everything moved onto the new basic frame. (The waves and
    // the twirl take numpy's float32 np.sin and np.cos. The reference runs with NPY_DISABLE_CPU_FEATURES, where numpy
    // has no FMA3 and computes each with the C library's sinf and cosf, as render/filters.cpp does; numpy's vectorised
    // way, with fused multiply-adds, gives other last bits for some angles. On most papers no pixel shows it; on 78 ×
    // 104 mm trimmed to 68 × 94 page 2's twirl correction did, at a pixel or two by 1: anotherPaper() draws that too.)
    add(R"({"op": "add_cover", "kind": "jacket", "spine_mm": 6, "flap_mm": 15})");
    add(R"({"op": "add_cover", "kind": "obi", "spine_mm": 6, "flap_mm": 9, "height_mm": 22, "bleed": false})");
    add(R"({"op": "add_cover", "kind": "back"})");
    Json spec = Json::parse(R"({"op": "set_page_spec", "bleed_mm": 3, "margins": [9, 10, 7, 6]})");
    spec["paper"] = paper;
    spec["trim"] = trim;
    ops.push_back(std::move(spec));
    return ops;
}

// Correction layers of the filters add_layer does not make (filters.py draws them when a book has them: render/
// filters.cpp here), put on top of the pages by hand: {page, id, adjust}.
Json adjust_layers() {
    Json out = Json::array();
    const auto add = [&](int page, const std::string& id, const char* adjust) {
        out.push_back(Json::object({{"page", page}, {"id", id}, {"adjust", Json::parse(adjust)}}));
    };
    add(1, "adj-mosaic", R"({"kind": "mosaic", "block": 6})");
    add(2, "adj-twirl", R"({"kind": "twirl", "angle": 60, "radius": 0.4})");
    add(4, "adj-blur", R"({"kind": "blur", "radius": 3})");
    add(4, "adj-sharpen", R"({"kind": "sharpen", "amount": 2})");
    add(4, "adj-motion", R"({"kind": "motion_blur", "distance": 8, "angle": 30})");
    add(4, "adj-radial", R"({"kind": "radial_blur", "amount": 0.1})");
    add(4, "adj-zoom", R"({"kind": "zoom_blur", "amount": 0.15})");
    add(4, "adj-lineart", R"({"kind": "lineart"})");
    add(4, "adj-despeckle", R"({"kind": "despeckle", "size_px": 10, "what": "both"})");
    add(4, "adj-glow", R"({"kind": "glow", "radius": 4})");
    add(4, "adj-wave", R"({"kind": "wave", "amplitude": 3})");
    add(4, "adj-noise", R"({"kind": "noise", "amount": 0.2, "seed": "n"})");
    add(4, "adj-rain", R"({"kind": "rain", "count": 30, "seed": 4})");
    return out;
}

// Then each of them on page 4 (but noise and rain) and the mosaic kept to its box by a mask.
Json masks() {
    Json ops = Json::array();
    ops.push_back(Json::parse(R"({"op": "set_layer_mask", "page": 1, "id": "adj-mosaic", "area": {"rect": [36, 14, 22, 30]}})"));
    int i = 0;
    for (const char* id : {"adj-blur", "adj-sharpen", "adj-motion", "adj-radial", "adj-zoom", "adj-lineart", "adj-despeckle", "adj-glow",
                           "adj-wave"}) {
        const double x = 12 + static_cast<double>(i % 2) * 24;
        const double y = 14 + static_cast<double>(i / 2) * 15;
        ops.push_back(Json::object({{"op", "set_layer_mask"}, {"page", 4}, {"id", id},
                                    {"area", Json::object({{"rect", Json::array({x, y, 22, 14})}})}}));
        ++i;
    }
    return ops;
}

// adjust_layers() as add_adjust_layers_job puts them (role user, kind adjust, the rest as a new layer has it)
void add_adjust_layers(genko::core::Document& doc) {
    for (const Json& spec : adjust_layers()) {
        genko::core::Layer layer;
        layer.id = spec["id"].get<std::string>();
        layer.role = genko::core::LayerRole::User;
        layer.kind = genko::core::LayerKind::Adjust;
        layer.adjust = spec["adjust"];
        for (std::size_t i = 0; i < doc.pages.size(); ++i) {
            if (doc.page(i).index == genko::core::Num(spec["page"].get<std::int64_t>())) doc.edit_page(i).layers.push_back(layer);
        }
    }
}

const genko::core::Page* page_of(const genko::core::Document& doc, int index) {
    for (const auto& p : doc.pages) {
        if (p->index == genko::core::Num(static_cast<std::int64_t>(index))) return p.get();
    }
    return nullptr;
}

}  // namespace

class TestContractRasterRender : public QObject {
    Q_OBJECT

    QTemporaryDir scratch_;
    QString py_book_;                // the book after the ops, as Python saved it
    genko::core::Document py_doc_;   // … read by this build
    genko::core::Document cpp_doc_;  // the ops applied by this build
    genko::core::Document plain_doc_;  // Python's book before the correction layers put on by hand
    genko::core::Document py_other_;   // the book on the other paper, as Python saved it, read by this build
    genko::core::Document cpp_other_;  // … the ops applied by this build
    // (this build's books with the masks D1 moved put back as Python keeps them, for comparing every other pixel)
    genko::core::Document cpp_doc_kept_;
    genko::core::Document cpp_other_kept_;

    // The user's decision D1: `cpp` (this build's book after the ops) differs from `python` (Python's) in the masks the
    // pages had before set_page_spec (`before`: this build's book just before it) alone, each Python's moved with what it
    // masks; `cpp` with Python's masks put back in their place. How many there were in `moved`.
    static genko::core::Document masks_as_python(const genko::core::Document& cpp, const genko::core::Document& python,
                                                 const genko::core::Document& before, int& moved) {
        const auto pixels = [](const genko::core::Bytes& png) {
            const render::Image image = render::read_png(*png, render::kPillowOpenLimits).convert("L");
            return std::to_string(image.width()) + "x" + std::to_string(image.height()) + ":" + image.tobytes();
        };
        genko::core::Document out = cpp;
        moved = 0;
        for (std::size_t i = 0; i < cpp.pages.size(); ++i) {
            const genko::core::Page& page = *cpp.pages[i];
            const genko::core::Page* theirs = page_of(python, static_cast<int>(page.index.int_value()));
            const genko::core::Page* was = page_of(before, static_cast<int>(page.index.int_value()));
            for (std::size_t k = 0; k < page.layers.size(); ++k) {
                const genko::core::Layer& layer = page.layers[k];
                if (!layer.mask || !layer.mask->png) continue;
                const auto find = [&](const genko::core::Page* on) -> const genko::core::Layer* {
                    for (const auto& l : on->layers) {
                        if (l.id == layer.id) return &l;
                    }
                    return nullptr;
                };
                const genko::core::Layer* py_layer = find(theirs);
                if (py_layer == nullptr || !py_layer->mask || !py_layer->mask->png) {
                    qWarning("page %d layer %s: a mask Python's book does not have", static_cast<int>(page.index.int_value()), layer.id.c_str());
                    ++moved;  // (counted, so the count says so)
                    continue;
                }
                if (pixels(layer.mask->png) == pixels(py_layer->mask->png)) continue;
                const genko::core::Layer* old_layer = find(was);
                const std::string start_side = cpp.start_side.value_or("");
                const auto expected = render::relayout_mask(*old_layer->mask->png, was->spec, page.spec, was->inner_rect_mm(start_side),
                                                            page.inner_rect_mm(start_side));
                if (!expected || pixels(std::make_shared<const std::string>(*expected)) != pixels(layer.mask->png) ||
                    pixels(old_layer->mask->png) != pixels(py_layer->mask->png)) {
                    qWarning("page %d layer %s: the mask is not Python's moved with what it masks", static_cast<int>(page.index.int_value()),
                             layer.id.c_str());
                    moved += 100;
                    continue;
                }
                out.edit_page(i).layers[k].mask = py_layer->mask;
                ++moved;
            }
        }
        return out;
    }
    std::map<std::string, QString> drawn_;  // "<book>-<page>-<mode>-<dpi>" → Python's drawing

    QString path(const QString& relative) const { return scratch_.path() + QLatin1Char('/') + relative; }

    static std::string key(const std::string& book, int page, const std::string& mode, int dpi) {
        return book + "-" + std::to_string(page) + "-" + mode + "-" + std::to_string(dpi);
    }

    render::Image python_picture(const std::string& book, int page, const std::string& mode, int dpi) {
        return render::read_png(genko::test::read_bytes(drawn_.at(key(book, page, mode, dpi))), render::kPillowOpenLimits).convert("RGB");
    }

    static render::Image draw(const genko::core::Document& doc, int index, const std::string& mode, int dpi,
                              const std::optional<render::RenderRegion>& region = std::nullopt) {
        render::brushes::clear_custom();
        render::brushes::register_brushes(doc.brush_custom);
        render::RenderOptions options;
        options.mode = mode;
        options.region = region;
        return render::render_page(*page_of(doc, index), dpi, options, &doc).image;
    }

private slots:
    void initTestCase() {
        if (genko::test::python_ref().isEmpty()) QSKIP("no reference Python: set GENKO_PYREF or install /opt/pyref/bin/python");
        QVERIFY(scratch_.isValid());
        QDir().mkpath(path("pyenv/config"));
        qputenv("GENKO_CONFIG_DIR", path("pyenv/config").toUtf8());
        const QString original = path("rasterbook.genko");
        const auto made = genko::test::harness({"make-rasterbook", original}, path("pyenv"));
        QVERIFY2(made.finished && made.exit_code == 0, made.err.right(4000).constData());

        // Python applies the ops, puts the correction layers on, masks them, and saves the book
        py_book_ = path("dressed.genko");
        const auto apply_job = [&](const QString& from, const QString& to, const Json& ops, const QString& out) {
            return Json::object({{"op", "apply"}, {"book", from.toStdString()}, {"ops", ops}, {"agent", "human:作者"}, {"ids", true},
                                 {"out", out.toStdString()}, {"dest", to.toStdString()}});
        };
        const Json jobs_made = Json::array(
            {apply_job(original, path("dressed-1.genko"), dressing(), path("dressed-1.json")),
             Json::object({{"op", "add_adjust_layers"}, {"book", path("dressed-1.genko").toStdString()},
                           {"dest", path("dressed-2.genko").toStdString()}, {"layers", adjust_layers()}}),
             apply_job(path("dressed-2.genko"), py_book_, masks(), path("dressed-3.json")),
             apply_job(original, path("other-1.genko"), dressing(kOtherPaper, kOtherTrim), path("other-1.json")),
             Json::object({{"op", "add_adjust_layers"}, {"book", path("other-1.genko").toStdString()},
                           {"dest", path("other-2.genko").toStdString()}, {"layers", adjust_layers()}}),
             apply_job(path("other-2.genko"), path("other.genko"), masks(), path("other-3.json"))});
        genko::test::write_bytes(path("dress-jobs.json"), genko::core::dump_python(jobs_made));
        const auto dressed = genko::test::harness({"batch", path("dress-jobs.json")}, path("pyenv"));
        QVERIFY2(dressed.finished && dressed.exit_code == 0, dressed.err.right(4000).constData());
        for (const char* out : {"dressed-1.json", "dressed-3.json", "other-1.json", "other-3.json"}) {
            const Json reply = genko::test::read_json(path(out));
            QVERIFY2(reply.value("ok", false), genko::core::dump_python(reply).substr(0, 2000).c_str());
        }
        for (const auto& [book, doc] : {std::pair<QString, genko::core::Document*>{py_book_, &py_doc_}, {path("dressed-1.genko"), &plain_doc_},
                                        {path("other.genko"), &py_other_}}) {
            const genko::core::ScopedIdSource ids(genko::core::counting_ids());
            const auto loaded = genko::storage::load_document(genko::storage::path_from_utf8(book.toStdString()));
            QVERIFY2(loaded.report.clean(), genko::core::dump_python(loaded.report.to_json()).c_str());
            *doc = loaded.document;
        }
        // this build does the same to the same book
        {
            const genko::core::ScopedIdSource ids(genko::core::counting_ids());
            const genko::core::Document doc = genko::storage::load_document(genko::storage::path_from_utf8(original.toStdString())).document;
            const genko::core::CommandBus bus(render::ops_registry());
            genko::core::Document dressed_doc = bus.apply(doc, dressing(), genko::core::Actor("human:作者"), false).doc;
            add_adjust_layers(dressed_doc);
            cpp_doc_ = bus.apply(dressed_doc, masks(), genko::core::Actor("human:作者"), false).doc;
        }
        {
            const genko::core::ScopedIdSource ids(genko::core::counting_ids());
            const genko::core::Document doc = genko::storage::load_document(genko::storage::path_from_utf8(original.toStdString())).document;
            const genko::core::CommandBus bus(render::ops_registry());
            genko::core::Document dressed_doc = bus.apply(doc, dressing(kOtherPaper, kOtherTrim), genko::core::Actor("human:作者"), false).doc;
            add_adjust_layers(dressed_doc);
            cpp_other_ = bus.apply(dressed_doc, masks(), genko::core::Actor("human:作者"), false).doc;
        }
        // (D1: this build's books but for the masks set_page_spec moved, which are Python's moved with what they mask)
        for (const auto& [paper, trim, cpp, python, kept] :
             {std::tuple<Json, Json, genko::core::Document*, genko::core::Document*, genko::core::Document*>{
                  Json::array({76, 100}), Json::array({66, 90}), &cpp_doc_, &py_doc_, &cpp_doc_kept_},
              {kOtherPaper, kOtherTrim, &cpp_other_, &py_other_, &cpp_other_kept_}}) {
            const genko::core::ScopedIdSource ids(genko::core::counting_ids());
            const genko::core::Document doc = genko::storage::load_document(genko::storage::path_from_utf8(original.toStdString())).document;
            Json unmoved = dressing(paper, trim);
            QCOMPARE(unmoved.back()["op"], Json("set_page_spec"));
            unmoved.erase(unmoved.size() - 1);
            const genko::core::Document before =
                genko::core::CommandBus(render::ops_registry()).apply(doc, unmoved, genko::core::Actor("human:作者"), false).doc;
            int moved = 0;
            *kept = masks_as_python(*cpp, *python, before, moved);
            QCOMPARE(moved, 3);  // (page 1's: in-a, mask-1, adj-levels)
        }

        // Python draws the pages (of the book before the correction layers too, in the modes the regions are cut in)
        Json jobs = Json::array();
        QDir().mkpath(path("py"));
        const auto add_job = [&](const std::string& book, const QString& folder, int page, const std::string& mode, int dpi) {
            const QString out = path(QStringLiteral("py/%1.png").arg(QString::fromStdString(key(book, page, mode, dpi))));
            jobs.push_back(Json::object({{"book", folder.toStdString()}, {"page", page}, {"dpi", dpi}, {"mode", mode}, {"out", out.toStdString()}}));
            drawn_[key(book, page, mode, dpi)] = out;
        };
        for (const int page : kPages) {
            for (const std::string& mode : kModes) {
                for (const int dpi : kDpis) add_job("dressed", py_book_, page, mode, dpi);
            }
            add_job("plain", path("dressed-1.genko"), page, "proof", 150);
            add_job("plain", path("dressed-1.genko"), page, "print", 350);
        }
        for (const int page : kWavedPages) {
            for (const std::string& mode : kModes) {
                for (const int dpi : kDpis) add_job("other", path("other.genko"), page, mode, dpi);
            }
        }
        genko::test::write_bytes(path("render-jobs.json"), genko::core::dump_python(jobs));
        const auto drawn = genko::test::render_harness({"render", path("render-jobs.json")}, path("pyenv"));
        QVERIFY2(drawn.finished && drawn.exit_code == 0, drawn.err.right(4000).constData());
    }

    // 1 and 2: whole pages
    void pages() {
        int compared = 0, failures = 0;
        for (const int page : kPages) {
            for (const std::string& mode : kModes) {
                for (const int dpi : kDpis) {
                    const render::Image want = python_picture("dressed", page, mode, dpi);
                    for (const auto& [side, doc] : {std::pair<const char*, const genko::core::Document*>{"Python's book", &py_doc_},
                                                   {"this build's book (D1's masks as Python's)", &cpp_doc_kept_}}) {
                        render::clear_render_caches();
                        const render::Image got = draw(*doc, page, mode, dpi);
                        ++compared;
                        if (got.size() != want.size() || got.tobytes() != want.tobytes()) {
                            const auto diff = got.size() == want.size() ? genko::test::pixel_diff(got, want) : genko::test::PixelDiff{};
                            genko::test::keep_pictures(QStringLiteral("raster-p%1-%2-%3-%4").arg(page).arg(QString::fromStdString(mode))
                                                           .arg(dpi).arg(compared),
                                                       got, want);
                            qWarning("%s, page %d, %s at %d dpi: %lld pixels differ (by up to %d)", side, page, mode.c_str(), dpi,
                                     diff.pixels, diff.largest);
                            ++failures;
                        }
                    }
                }
            }
        }
        qInfo("%d drawings the same as Python's, every pixel", compared - failures);
        QCOMPARE(compared, 126);
        QCOMPARE(failures, 0);
    }

    // 3: parts of pages: of the book before the correction layers put on by hand (each part drawn alone), and of the
    // book with them (whose filters look beyond each pixel: the part is cut from the whole page drawn)
    void regions() {
        unsigned seed = 20261003;
        const auto next = [&](int n) {
            seed = seed * 1103515245U + 12345U;
            return n <= 1 ? 0 : static_cast<int>((seed >> 8) % static_cast<unsigned>(n));
        };
        int checked = 0, failures = 0;
        for (const auto& [book, doc] : {std::pair<std::string, const genko::core::Document*>{"plain", &plain_doc_}, {"dressed", &py_doc_}}) {
            for (const int page : kPages) {
                for (const auto& [mode, dpi] : {std::pair<std::string, int>{"proof", 150}, {"print", 350}}) {
                    const render::Image whole = python_picture(book, page, mode, dpi);
                    for (int k = 0; k < 5; ++k) {
                        const int w = 1 + next(whole.width());
                        const int h = 1 + next(whole.height());
                        const render::RenderRegion r{next(whole.width() - w + 1), next(whole.height() - h + 1), w, h};
                        if (k % 2 == 0) render::clear_render_caches();  // (drawn from nothing, or from remembered lines)
                        const render::Image got = draw(*doc, page, mode, dpi, r);
                        const render::Image want = whole.crop(render::Box{r.x, r.y, r.x + r.w, r.y + r.h});
                        ++checked;
                        if (got.size() != want.size() || got.tobytes() != want.tobytes()) {
                            genko::test::keep_pictures(QStringLiteral("raster-region-%1").arg(checked), got, want);
                            qWarning("%s page %d %s @%d region (%d, %d, %d, %d) differs from Python's page cut there", book.c_str(), page,
                                     mode.c_str(), dpi, r.x, r.y, r.w, r.h);
                            ++failures;
                        }
                    }
                }
            }
        }
        qInfo("regions: %d the same as Python's pages cut", checked - failures);
        QCOMPARE(checked, 140);
        QCOMPARE(failures, 0);
    }

    // 4: the waved and twirled pages on the other paper
    void anotherPaper() {
        int compared = 0, failures = 0;
        for (const int page : kWavedPages) {
            for (const std::string& mode : kModes) {
                for (const int dpi : kDpis) {
                    const render::Image want = python_picture("other", page, mode, dpi);
                    for (const auto& [side, doc] : {std::pair<const char*, const genko::core::Document*>{"Python's book", &py_other_},
                                                   {"this build's book (D1's masks as Python's)", &cpp_other_kept_}}) {
                        render::clear_render_caches();
                        const render::Image got = draw(*doc, page, mode, dpi);
                        ++compared;
                        if (got.size() != want.size() || got.tobytes() != want.tobytes()) {
                            const auto diff = got.size() == want.size() ? genko::test::pixel_diff(got, want) : genko::test::PixelDiff{};
                            genko::test::keep_pictures(QStringLiteral("raster-other-p%1-%2-%3").arg(page).arg(QString::fromStdString(mode)).arg(dpi),
                                                       got, want);
                            qWarning("78 x 104 mm (68 x 94), %s, page %d, %s at %d dpi: %lld pixels differ (by up to %d)", side, page, mode.c_str(), dpi,
                                     diff.pixels, diff.largest);
                            ++failures;
                        }
                    }
                }
            }
        }
        qInfo("78 x 104 mm: %d drawings the same as Python's, every pixel", compared - failures);
        QCOMPARE(compared, 54);
        QCOMPARE(failures, 0);
    }

    void cleanupTestCase() {
        render::brushes::clear_custom();
        render::clear_render_caches();
    }
};

QTEST_GUILESS_MAIN(TestContractRasterRender)
#include "test_contract_raster_render.moc"
