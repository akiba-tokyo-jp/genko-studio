// 生成 → 画像アセット (PC5 I-f): the files the layers' names ask for (PhotoCraft v0.2.0's web_cmds.rs generate_assets, MIT).

#include "formats/image_assets.hpp"

#include <algorithm>
#include <cmath>
#include <set>
#include <system_error>

#include "core/error.hpp"
#include "core/image_assets.hpp"
#include "core/paths.hpp"
#include "render/codecs/format.hpp"
#include "render/codecs/image.hpp"
#include "render/codecs/limits.hpp"
#include "render/codecs/resize.hpp"
#include "render/page.hpp"
#include "storage/fsutil.hpp"

namespace genko::formats::image_assets {

namespace fs = std::filesystem;
using core::Json;
using render::codecs::CodecImage;

namespace {

int page_dpi(const core::Page& page) { return std::max(1, static_cast<int>(std::lround(page.spec.dpi.value()))); }

std::string parent_of(const core::Layer& layer) { return layer.parent_id.is_string() ? layer.parent_id.get<std::string>() : std::string(); }

// The layers inside a folder (at any depth), in the page's order.
std::vector<core::Layer> inside(const core::Page& page, const std::string& folder) {
    std::set<std::string> within{folder};
    std::vector<core::Layer> out;
    // (a child may come before or after its folder in the list: looked at until nothing more is taken in)
    for (bool grew = true; grew;) {
        grew = false;
        for (const core::Layer& layer : page.layers) {
            if (!within.contains(layer.id) && within.contains(parent_of(layer))) {
                within.insert(layer.id);
                grew = true;
            }
        }
    }
    for (const core::Layer& layer : page.layers) {
        if (within.contains(layer.id)) out.push_back(layer);
    }
    return out;
}

// The part of an RGBA 8-bit picture that shows.
std::optional<std::array<int, 4>> shown(const render::Image& rgba) {
    const std::string bytes = rgba.tobytes();
    int x0 = rgba.width(), y0 = rgba.height(), x1 = 0, y1 = 0;
    for (int y = 0; y < rgba.height(); ++y) {
        for (int x = 0; x < rgba.width(); ++x) {
            if (bytes[(std::size_t(y) * rgba.width() + x) * 4 + 3] == 0) continue;
            x0 = std::min(x0, x);
            y0 = std::min(y0, y);
            x1 = std::max(x1, x + 1);
            y1 = std::max(y1, y + 1);
        }
    }
    if (x1 <= x0 || y1 <= y0) return std::nullopt;
    return std::array<int, 4>{x0, y0, x1, y1};
}

struct Job {
    std::size_t page = 0;
    const core::Layer* layer = nullptr;
    core::image_assets::Spec spec;
};

// The size the spec and the variant ask for, from the layer's own (w0 × h0 at the page's resolution): PhotoCraft's rule —
// the spec's scale or size first, then the variant's on top of it.
std::pair<double, double> wanted(double w0, double h0, const core::image_assets::Spec& spec, const core::image_assets::Default& v) {
    double k = spec.scale.value_or(1.0);
    double tw = w0 * k, th = h0 * k;
    if (spec.width && spec.height) {
        tw = *spec.width;
        th = *spec.height;
    } else if (spec.width) {
        tw = *spec.width;
        th = h0 * *spec.width / w0;
    } else if (spec.height) {
        tw = w0 * *spec.height / h0;
        th = *spec.height;
    }
    k = v.scale.value_or(1.0);
    tw *= k;
    th *= k;
    if (v.width && v.height) {
        tw = *v.width;
        th = *v.height;
    } else if (v.width) {
        th = th * *v.width / tw;
        tw = *v.width;
    } else if (v.height) {
        tw = tw * *v.height / th;
        th = *v.height;
    }
    return {std::max(1.0, std::round(tw)), std::max(1.0, std::round(th))};
}

// A size other than the layer's own at the page's resolution: drawn at that, then made the size asked exactly (PhotoCraft's:
// the layer's pixels resized — a 50 % of 64 × 40 is 32 × 20).
bool sized(const core::image_assets::Spec& spec, const core::image_assets::Default& v) {
    return spec.width || spec.height || v.width || v.height || spec.scale.value_or(1.0) != 1.0 || v.scale.value_or(1.0) != 1.0;
}

bool transparent(const std::string& format) { return format != "jpg" && format != "png24"; }

// A group's layers drawn alone over nothing at `dpi`, cut to what shows ("RGBA", sRGB 8 bits).
CodecImage group_picture(const core::Document& doc, const core::Page& page, const core::Layer& group, int dpi) {
    core::Page alone = page;
    alone.layers = inside(page, group.id);
    if (alone.layers.empty()) throw core::Error("value", "the group has nothing to write");
    render::RenderOptions options;
    options.contents = true;
    options.mode = "print";
    const render::Image drawn = render::render_contents(alone, dpi, options, &doc).image.convert("RGBA");
    const auto box = shown(drawn);
    if (!box) throw core::Error("value", "the group has nothing to write");
    const render::Image cut = drawn.crop(render::Box{(*box)[0], (*box)[1], (*box)[2], (*box)[3]});
    CodecImage out = CodecImage::from_bytes(static_cast<std::uint32_t>(cut.width()), static_cast<std::uint32_t>(cut.height()),
                                            render::codecs::Layout::Rgba, render::codecs::Sample::U8, cut.tobytes());
    out.meta.dpi = std::make_pair(double(dpi), double(dpi));
    return out;
}

// A picture with an alpha over white.
CodecImage over_white(const CodecImage& rgba) {
    CodecImage out = CodecImage::blank(rgba.width(), rgba.height(), render::codecs::Layout::Rgb, render::codecs::Sample::U8);
    out.meta = rgba.meta;
    std::string& data = out.data();
    for (std::size_t i = 0; i < std::size_t(rgba.width()) * rgba.height(); ++i) {
        const float a = std::clamp(rgba.value(i * 4 + 3), 0.0f, 1.0f);
        for (unsigned c = 0; c < 3; ++c) {
            const float v = rgba.value(i * 4 + c) * a + (1.0f - a);
            data[i * 3 + c] = static_cast<char>(static_cast<unsigned char>(std::lround(std::clamp(v, 0.0f, 1.0f) * 255.0f)));
        }
    }
    return out;
}

// One group's file: drawn at the resolution its scale asks (a size asked exactly: at the page's, then made that size).
std::vector<std::string> write_group(const core::Document& doc, const Job& job, const core::image_assets::Default& v, const fs::path& out,
                                     const CopyOptions& options) {
    const core::Page& page = doc.page(job.page);
    const int base = page_dpi(page);
    CodecImage picture;
    if (sized(job.spec, v)) {
        picture = group_picture(doc, page, *job.layer, base);
        const auto [tw, th] = wanted(picture.width(), picture.height(), job.spec, v);
        render::codecs::Limits{}.check(static_cast<std::int64_t>(std::min(tw, 1.0e12)), static_cast<std::int64_t>(std::min(th, 1.0e12)),
                                       render::codecs::Layout::Rgba, render::codecs::Sample::U8);
        picture = render::codecs::resized(picture, static_cast<std::uint32_t>(tw), static_cast<std::uint32_t>(th), options.resample);
    } else {
        picture = group_picture(doc, page, *job.layer, base);
    }
    if (!options.transparency) picture = over_white(picture);
    const std::string ext = core::path_to_utf8(out.extension()).substr(1);
    const auto format = render::codecs::from_extension(ext);
    if (!format) throw core::Error("value", "no format for ." + ext);
    std::vector<std::string> notes;
    const std::string bytes = encode_for(std::move(picture), *format, encode_options(options, ext), true, notes);
    storage::write_atomic(out, bytes);
    return notes;
}

}  // namespace

fs::path assets_dir(const fs::path& book) {
    fs::path folder = book;
    if (!folder.has_filename()) folder = folder.parent_path();  // ("x.genko/")
    return folder.parent_path() / core::path_from_utf8(core::path_to_utf8(folder.stem()) + "-assets");
}

Json Result::json() const {
    Json written = Json::array();
    for (const fs::path& file : files) written.push_back(core::path_to_utf8(file));
    Json said = Json::array();
    for (const std::string& note : notes) said.push_back(note);
    return Json{{"dir", core::path_to_utf8(dir)}, {"files", std::move(written)}, {"errors", errors}, {"notes", std::move(said)}};
}

std::size_t named_layers(const core::Document& doc) {
    std::size_t count = 0;
    for (const auto& page : doc.pages) {
        const double dpi = page_dpi(*page);
        for (const core::Layer& layer : page->layers) {
            if (layer.kind == core::LayerKind::Adjust || core::image_assets::parse_defaults(layer.title, dpi)) continue;
            if (!core::image_assets::parse_name(layer.title, dpi).empty()) ++count;
        }
    }
    return count;
}

Result generate(const core::Document& doc, const fs::path& dir, const WriteGuard& guard) {
    Result result;
    result.dir = dir;
    if (!doc.deferred.empty()) throw core::Error("page_not_loaded", "the book's pages are still being read: try again in a moment");
    // (PhotoCraft's walk: the "default …" layer anywhere in the book — the last one met — gives the variants)
    std::optional<std::vector<core::image_assets::Default>> defaults;
    std::vector<Job> jobs;
    for (std::size_t at = 0; at < doc.pages.size(); ++at) {
        const core::Page& page = doc.page(at);
        const double dpi = page_dpi(page);
        for (const core::Layer& layer : page.layers) {
            if (auto found = core::image_assets::parse_defaults(layer.title, dpi)) {
                defaults = std::move(found);
                continue;
            }
            if (layer.kind == core::LayerKind::Adjust) continue;  // (a correction has no pixels of its own)
            for (core::image_assets::Spec& spec : core::image_assets::parse_name(layer.title, dpi)) jobs.push_back(Job{at, &layer, std::move(spec)});
        }
    }
    const std::vector<core::image_assets::Default> variants =
        defaults && !defaults->empty() ? *defaults : std::vector<core::image_assets::Default>{core::image_assets::Default{}};
    std::set<std::string> taken;
    const auto said = [&](const std::vector<std::string>& notes) {
        for (const std::string& note : notes) {
            if (std::find(result.notes.begin(), result.notes.end(), note) == result.notes.end()) result.notes.push_back(note);
        }
    };
    for (const Job& job : jobs) {
        const core::Page& page = doc.page(job.page);
        for (const core::image_assets::Default& v : variants) {
            const std::size_t dot = job.spec.file.rfind('.');
            const std::string rel = v.folder + job.spec.file.substr(0, dot) + v.suffix + job.spec.file.substr(dot);
            const auto fail = [&](const std::string& why) {
                result.errors.push_back(Json{{"page", page.index.json()}, {"layer", job.layer->title}, {"asset", rel}, {"error", why}});
            };
            if (!core::image_assets::safe_relative(rel)) {
                fail("the asset's name is not a path inside the assets' folder");
                continue;
            }
            if (!taken.insert(rel).second) {
                fail("another layer writes an asset of this name");
                continue;
            }
            try {
                const fs::path out = dir / core::path_from_utf8(rel);
                check_target(guard, out);
                std::error_code ec;
                fs::create_directories(out.parent_path(), ec);
                check_target(guard, out.parent_path());
                CopyOptions options;
                options.layer = job.layer->id;
                options.transparency = transparent(job.spec.format);
                options.metadata = Metadata::None;
                options.srgb = true;
                if (job.spec.format == "jpg") options.quality_percent = job.spec.quality.value_or(90);
                std::vector<std::string> notes;
                if (job.layer->kind == core::LayerKind::Folder) {
                    notes = write_group(doc, job, v, out, options);
                } else {
                    if (sized(job.spec, v)) {
                        // (the layer's own size first — what it holds is known once it is drawn —, then the size asked)
                        CopyOptions plain = options;
                        plain.transparency = true;
                        const PagePicture own = copy_picture(doc, job.page, plain);
                        const auto [tw, th] = wanted(own.image.width(), own.image.height(), job.spec, v);
                        options.width = static_cast<std::uint32_t>(std::min(tw, 4.0e9));
                        options.height = static_cast<std::uint32_t>(std::min(th, 4.0e9));
                    }
                    notes = write_copy(doc, job.page, out, options).notes;
                }
                if (job.spec.format == "png8") notes.push_back("png8 は 256 色にせず、フルカラーの PNG で書きました");
                said(notes);
                result.files.push_back(out);
            } catch (const std::exception& error) {
                fail(error.what());
            }
        }
    }
    return result;
}

}  // namespace genko::formats::image_assets
