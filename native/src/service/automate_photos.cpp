// HDR Pro に統合・写真の切り抜きと角度補正 (PC5 I-h): PhotoCraft v0.2.0's engine/src/photo_cmds.rs (559-755: merge_to_hdr,
// crop_and_straighten; MIT, native/third_party/photocraft/NOTICE.md) read as Genko's: the files opened as 開く… opens them,
// the results new books of one page as 開く… makes them (formats::picture_book). See service/automate_photos.hpp.

#include "service/automate_photos.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <numeric>
#include <string>
#include <utility>

#include "core/color_model.hpp"
#include "core/error.hpp"
#include "core/exif.hpp"
#include "core/paths.hpp"
#include "formats/page_picture.hpp"
#include "render/codecs/format.hpp"
#include "render/codecs/limits.hpp"
#include "render/codecs/codecs.hpp"
#include "render/image.hpp"
#include "render/image_ops.hpp"
#include "render/ops_filters.hpp"
#include "render/page.hpp"
#include "render/raw.hpp"
#include "render/scancrop.hpp"
#include "storage/fsutil.hpp"

namespace genko::service::automate {

namespace {

using core::Json;
using render::hdr::Shot;
namespace codecs = render::codecs;

constexpr std::uint64_t kMiB = 1024ull * 1024ull;

const std::string& linear_srgb() {
    static const std::string bytes = [] {
        const core::BuiltinProfile* p = core::builtin_profile("linear-srgb");
        return p != nullptr && p->bytes ? *p->bytes : std::string();
    }();
    return bytes;
}

// The page's merged picture as an exposure: RGB of its depth — floats only when they are linear light (the built-in
// linear-srgb: an OpenEXR's, a 32-bit PSD's of sRGB); a 32-bit page of another profile holds values of its curve, taken as
// 16-bit display values —, its camera's exposure.
Shot page_shot(const core::Document& doc, const core::Page& page, const std::string& name) {
    const int dpi = static_cast<int>(std::lround(page.spec.dpi.value()));
    codecs::CodecImage picture = render::hdr::rgb_of(formats::page_picture(page, doc, dpi).image);
    if (picture.sample() == codecs::Sample::F32 && (!picture.icc || *picture.icc != linear_srgb())) picture = picture.convert(codecs::Layout::Rgb, codecs::Sample::U16);
    std::optional<double> exposure;
    if (const auto camera = render::picture_camera(doc, page, nullptr)) exposure = render::hdr::exposure_factor(camera->exposure_time, camera->f_number, camera->iso);
    if (!exposure && page.native_source && page.native_source->camera) {
        const core::CameraInfo& c = *page.native_source->camera;
        exposure = render::hdr::exposure_factor(c.exposure, c.aperture, c.iso);
    }
    return Shot(name, std::move(picture), exposure);
}

std::uint64_t shot_bytes(const Shot& s) { return s.picture().data().size(); }

std::string mib(std::uint64_t bytes) { return std::to_string((bytes + kMiB - 1) / kMiB); }

[[noreturn]] void too_large(std::uint64_t need) {
    throw core::Error("image_too_large", "the exposures are too large to merge at once (about " + mib(need) + " MiB of work; at most " +
                                             mib(codecs::kAllocBudget) + " MiB)");
}

void count_check(std::size_t n) {
    if (n < kHdrMinImages || n > kHdrMaxImages) {
        throw core::Error("value", "Merge to HDR Pro takes " + std::to_string(kHdrMinImages) + " to " + std::to_string(kHdrMaxImages) + " exposures (" +
                                       std::to_string(n) + " given)");
    }
}

void check_settings(const std::vector<Shot>& shots, const HdrSettings& settings) {
    check_shots(shots);
    if (settings.depth != 32 && settings.depth != 16 && settings.depth != 8) throw core::Error("value", "the HDR mode is 32, 16 or 8 bits");
    if (settings.ev && settings.ev->size() != shots.size()) throw core::Error("value", "give one EV value an exposure");
    if (settings.ev) {
        for (const double v : *settings.ev)
            if (!std::isfinite(v) || std::abs(v) > 30) throw core::Error("value", "an EV value is a number within -30..30");
    }
    if (settings.ghost_base && *settings.ghost_base >= shots.size()) throw core::Error("value", "the ghosts' base is one of the exposures");
}

}  // namespace

// --- HDR Pro に統合 ---------------------------------------------------------------------------------------------------------

std::uint64_t hdr_work_bytes(std::uint32_t width, std::uint32_t height, std::uint64_t shots_bytes, const HdrSettings& settings) {
    // (the shots, the radiance (RGB floats) and the ghosts' mask with the copy its blur takes; at 16 and 8 bits the toning's RGBA floats and its two
    // planes, the shots given up by then; then the picture and the page's raster)
    const std::uint64_t px = std::uint64_t(width) * height;
    const std::uint64_t merging = shots_bytes + px * 12 + (settings.remove_ghosts ? px * 8 : 0);  // (the mask, and its blur's copy)
    const std::uint64_t toning = settings.depth == 32 ? 0 : px * 12 + px * (16 + 8);
    const std::uint64_t making = settings.depth == 32 ? px * (12 + 12 + 16) : px * (16 + 6 + 8);
    return std::max({merging, toning, making});
}

void check_work(std::uint32_t width, std::uint32_t height, std::uint64_t shots_bytes, const HdrSettings& settings) {
    if (const std::uint64_t need = hdr_work_bytes(width, height, shots_bytes, settings); need > codecs::kAllocBudget) too_large(need);
}

render::hdr::Tone hdr_tone(std::string_view method, const Json& settings) {
    const auto kind = render::hdr::method_from(method);
    if (!kind) throw core::Error("value", "the HDR toning method is local_adaptation, exposure_gamma, highlight_compression or equalize_histogram");
    if (!settings.is_object()) throw core::Error("value", "the toning's settings are a JSON object");
    render::hdr::Tone tone;
    tone.method = *kind;
    struct Key {
        const char* name;
        float* to;
        float lo, hi;
    };
    std::vector<Key> keys;
    if (*kind == render::hdr::Tone::Method::LocalAdaptation) {
        keys ={{"radius", &tone.local.radius, 1, 500},          {"strength", &tone.local.strength, 0.1f, 4},
                {"gamma", &tone.local.gamma, 0.1f, 2},           {"exposure", &tone.local.exposure, -5, 5},
                {"detail", &tone.local.detail, -100, 300},       {"shadow", &tone.local.shadow, -100, 100},
                {"highlight", &tone.local.highlight, -100, 100}, {"vibrance", &tone.local.vibrance, -100, 100},
                {"saturation", &tone.local.saturation, -100, 100}};
    } else if (*kind == render::hdr::Tone::Method::ExposureGamma) {
        keys = {{"exposure", &tone.exposure, -20, 20}, {"gamma", &tone.gamma, 0.1f, 10}};
    }
    for (const auto& [key, value] : settings.items()) {
        const auto found = std::find_if(keys.begin(), keys.end(), [&](const Key& k) { return key == k.name; });
        if (found == keys.end()) throw core::Error("value", "the toning " + std::string(method) + " has no setting " + key);
        if (!value.is_number() || value.is_boolean() || !std::isfinite(value.get<double>())) throw core::Error("value", "the toning's " + key + " is a number");
        *found->to = static_cast<float>(std::clamp(value.get<double>(), static_cast<double>(found->lo), static_cast<double>(found->hi)));
    }
    return tone;
}

render::hdr::Shot hdr_shot_file(const fs::path& file, const formats::OpenOptions& options, const std::function<bool()>& stopped) {
    const std::string name = core::path_to_utf8(file.filename());
    const std::string bytes = storage::read_file(file);
    std::string suffix = core::path_to_utf8(file.extension());
    std::transform(suffix.begin(), suffix.end(), suffix.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (render::raw::is_raw(bytes, suffix)) {
        // (a camera RAW file: linear light as LibRaw develops it — its values the radiance, no response to estimate)
        render::raw::Options raw;
        raw.limits = options.limits;
        raw.stopped = stopped;
        render::raw::Developed made = render::raw::develop_linear(bytes, raw);
        const auto& camera = made.info.camera;
        return Shot(name, std::move(made.picture), render::hdr::exposure_factor(camera.shutter, camera.aperture, camera.iso));
    }
    formats::OpenOptions open = options;
    open.folder = file.parent_path();
    const formats::Opened opened = formats::open_file(bytes, name, open);
    return page_shot(opened.doc, opened.doc.page(0), name);
}

void check_shots(const std::vector<Shot>& shots) {
    count_check(shots.size());
    const std::uint32_t w = shots[0].width(), h = shots[0].height();
    const bool linear = shots[0].linear();
    std::uint64_t held = 0;
    for (const Shot& s : shots) {
        if (s.width() != w || s.height() != h) {
            throw core::Error("value", "the exposures must have the same pixel dimensions (" + std::to_string(w) + "×" + std::to_string(h) + " and " +
                                           std::to_string(s.width()) + "×" + std::to_string(s.height()) + ")");
        }
        if (s.linear() != linear) throw core::Error("value", "the exposures must be all of linear light (camera RAW files, 32-bit pictures) or all not");
        held += shot_bytes(s);
    }
    if (held > codecs::kAllocBudget) too_large(held);
}

HdrReadError::HdrReadError(std::string source, std::string error_code, std::string why)
    : core::Error(error_code, source + ": " + why), name(std::move(source)), reason(std::move(why)) {}

namespace {

// A big-endian number of `n` bytes at `at` (none past the bytes).
std::optional<std::uint32_t> be(std::string_view b, std::size_t at, std::size_t n) {
    if (at + n > b.size()) return std::nullopt;
    std::uint32_t v = 0;
    for (std::size_t i = 0; i < n; ++i) v = (v << 8) | static_cast<unsigned char>(b[at + i]);
    return v;
}

// What one exposure will be, seen before it is decoded.
struct Seen {
    std::uint32_t width = 0, height = 0;
    std::uint64_t shot = 0;   // the shot's bytes as held
    std::uint64_t read = 0;   // what reading it holds at once besides the shots (its file, the decoded picture, the book's raster, the merged picture and its RGB)
    bool linear = false;
    bool known = false;       // its header said its size (a .pcraft, a picture of a header this build does not read: checked once read)
};

std::string lower_suffix(const fs::path& file) {
    std::string suffix = core::path_to_utf8(file.extension());
    std::transform(suffix.begin(), suffix.end(), suffix.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return suffix;
}

Seen seen_file(const fs::path& file, const formats::OpenOptions& options) {
    Seen out;
    const std::string bytes = storage::read_file(file);
    const std::uint64_t size = bytes.size();
    if (render::raw::is_raw(bytes, lower_suffix(file))) {
        const render::raw::Info info = render::raw::identify(bytes, options.limits);
        const auto [w, h] = render::raw::developed_size(info, core::raw::Develop{});
        out.width = static_cast<std::uint32_t>(w);
        out.height = static_cast<std::uint32_t>(h);
        const std::uint64_t px = std::uint64_t(out.width) * out.height;
        out.shot = px * 12;
        out.read = size + std::uint64_t(info.raw_width) * std::uint64_t(info.raw_height) * 2 + px * 18;  // (render/raw check_size's linear stage)
        out.linear = true;
        out.known = true;
        return out;
    }
    // (a PSD's or PSB's header: its size and depth; the codecs' formats: their size, the depth their format may hold)
    std::uint64_t per = 6;
    if (bytes.size() >= 26 && bytes.compare(0, 4, "8BPS") == 0) {
        const auto h = be(bytes, 14, 4), w = be(bytes, 18, 4), depth = be(bytes, 22, 2);
        if (!h || !w || !depth) return out;
        out.width = *w;
        out.height = *h;
        per = *depth == 32 ? 12 : *depth == 16 ? 6 : 3;
        out.linear = *depth == 32;
    } else {
        const auto format = codecs::detect(bytes);
        const auto size_said = codecs::header_size(bytes);
        if (!format || !size_said) return out;
        out.width = size_said->first;
        out.height = size_said->second;
        switch (*format) {
            case codecs::Format::OpenExr:
            case codecs::Format::Hdr: per = 12; out.linear = true; break;
            case codecs::Format::Jpeg: case codecs::Format::Gif: case codecs::Format::Bmp: case codecs::Format::WebP:
            case codecs::Format::Qoi: case codecs::Format::Ico: case codecs::Format::Tga: per = 3; break;
            default: per = 6; break;  // (PNG, TIFF, Netpbm, AVIF: 16 bits at most here — 32-bit TIFF is checked once read)
        }
    }
    options.limits.check(out.width, out.height, codecs::Layout::Rgb, codecs::Sample::U8);  // (the sides and pixels 開く… takes)
    const std::uint64_t px = std::uint64_t(out.width) * out.height;
    out.shot = px * per;
    out.read = size + px * (per * 3 + 16);
    out.known = true;
    return out;
}

Seen seen_page(const core::Document& doc, std::size_t at) {
    Seen out;
    if (at >= doc.pages.size()) throw core::Error("value", "no such page");
    if (doc.is_deferred(at)) throw core::Error("page_not_loaded", "the page is not read yet");
    const core::Page& page = doc.page(at);
    const auto [w, h] = formats::page_picture_size(page, static_cast<int>(std::lround(page.spec.dpi.value())));
    out.width = w;
    out.height = h;
    codecs::Limits{}.check(w, h, codecs::Layout::Rgb, codecs::Sample::U8);
    const int depth = page.color ? page.color->depth : page.rgb ? page.rgb->depth : 8;
    const std::uint64_t per = depth >= 32 ? 12 : depth >= 16 ? 6 : 3;
    const std::uint64_t px = std::uint64_t(w) * h;
    out.shot = px * per;
    out.read = px * (per * 2 + 16);  // (the page drawn at its depth, its RGB)
    out.linear = depth >= 32;
    out.known = true;
    return out;
}

void same_as_first(const Seen& first, const Seen& seen, const std::string& name) {
    if (seen.width != first.width || seen.height != first.height) {
        throw HdrReadError(name, "value",
                           "the exposures must have the same pixel dimensions (" + std::to_string(first.width) + "×" + std::to_string(first.height) + " and " +
                               std::to_string(seen.width) + "×" + std::to_string(seen.height) + ")");
    }
}

}  // namespace

HdrPlan hdr_plan(const std::vector<HdrSource>& sources, const formats::OpenOptions& options, const std::function<bool()>& stopped) {
    count_check(sources.size());
    HdrPlan plan;
    std::optional<Seen> first;
    std::uint64_t held = 0;
    for (const HdrSource& source : sources) {
        if (stopped && stopped()) throw render::Cancelled();
        Seen seen;
        try {
            seen = source.file ? seen_file(*source.file, options) : seen_page(*source.book, source.page);
        } catch (const HdrReadError&) {
            throw;
        } catch (const core::Error& error) {
            throw HdrReadError(source.name, error.code(), error.what());
        }
        if (!seen.known) {
            plan.unknown = true;
            continue;
        }
        if (first) {
            same_as_first(*first, seen, source.name);
        } else {
            first = seen;
            plan.width = seen.width;
            plan.height = seen.height;
        }
        plan.peak = std::max(plan.peak, held + seen.read);
        held += seen.shot;
        plan.held = held;
    }
    if (first) plan.peak = std::max(plan.peak, held + std::uint64_t(plan.width) * plan.height * 12);  // (and the merge's radiance)
    if (plan.peak > codecs::kAllocBudget) too_large(plan.peak);
    return plan;
}

std::vector<Shot> hdr_read(const std::vector<HdrSource>& sources, const formats::OpenOptions& options, const std::function<bool()>& stopped) {
    (void)hdr_plan(sources, options, stopped);  // (refused before anything is decoded when the headers say it cannot fit)
    std::vector<Shot> shots;
    std::uint64_t held = 0;
    for (const HdrSource& source : sources) {
        if (stopped && stopped()) throw render::Cancelled();
        // (each read within what the budget has left after the shots held: its decoding and a RAW's development refused
        // past it)
        formats::OpenOptions within = options;
        within.limits.max_alloc = std::min(options.limits.max_alloc, codecs::kAllocBudget - std::min(held, codecs::kAllocBudget));
        try {
            shots.push_back(source.file ? hdr_shot_file(*source.file, within, stopped) : hdr_shot_of(*source.book, source.page, source.name));
        } catch (const render::Cancelled&) {
            throw;
        } catch (const HdrReadError&) {
            throw;
        } catch (const core::Error& error) {
            throw HdrReadError(source.name, error.code(), error.what());
        }
        held += shot_bytes(shots.back());
        if (held > codecs::kAllocBudget) too_large(held);
        const Shot& one = shots.back();
        if (one.width() != shots.front().width() || one.height() != shots.front().height()) {
            throw HdrReadError(source.name, "value",
                               "the exposures must have the same pixel dimensions (" + std::to_string(shots.front().width()) + "×" +
                                   std::to_string(shots.front().height()) + " and " + std::to_string(one.width()) + "×" + std::to_string(one.height()) + ")");
        }
        if (one.linear() != shots.front().linear())
            throw HdrReadError(source.name, "value", "the exposures must be all of linear light (camera RAW files, 32-bit pictures) or all not");
    }
    check_shots(shots);
    return shots;
}

std::vector<Shot> hdr_shots(const std::vector<fs::path>& files, const formats::OpenOptions& options, const std::function<bool()>& stopped) {
    std::vector<HdrSource> sources;
    for (const fs::path& file : files) sources.push_back(HdrSource{file, nullptr, 0, core::path_to_utf8(file.filename())});
    return hdr_read(sources, options, stopped);
}

render::hdr::Shot hdr_shot_of(const core::Document& doc, std::size_t page, const std::string& name) {
    if (page >= doc.pages.size()) throw core::Error("value", "no such page");
    if (doc.is_deferred(page)) throw core::Error("page_not_loaded", "the page is not read yet");
    return page_shot(doc, doc.page(page), name);
}

HdrRadiance hdr_radiance(std::vector<Shot> shots, const HdrSettings& settings, const std::function<bool()>& stopped) {
    check_settings(shots, settings);
    const std::size_t n = shots.size();
    HdrRadiance out;
    out.dpi = shots[0].picture().meta.dpi;
    // (the exposures: given, the cameras' when every one says its own, else estimated from the pictures)
    std::vector<double> exposures(n, 1.0);
    std::string source;
    if (settings.ev) {
        for (std::size_t j = 0; j < n; ++j) exposures[j] = std::exp2((*settings.ev)[j]);
        source = "given";
    } else if (std::all_of(shots.begin(), shots.end(), [](const Shot& s) { return s.exposure.has_value(); })) {
        for (std::size_t j = 0; j < n; ++j) exposures[j] = *shots[j].exposure;
        source = "exif";
    } else {
        std::vector<const Shot*> refs;
        for (const Shot& s : shots) refs.push_back(&s);
        exposures = render::hdr::estimate_exposures(refs, stopped);
        source = "estimated";
    }
    for (const double e : exposures)
        if (!(std::isfinite(e) && e > 0)) throw core::Error("value", "the exposures must be positive");
    // (pictures of display values whose exposures hardly differ — the same EXIF, the same EV, a file twice —: no camera
    // response can be recovered from them; refused, saying why — the review's P2-3. Linear light is only averaged)
    if (!shots[0].linear()) {
        const auto [lo, hi] = std::minmax_element(exposures.begin(), exposures.end());
        if (std::log2(*hi / *lo) < 0.5) {
            throw core::Error("value", "the exposures hardly differ (" + std::to_string(std::log2(*hi / *lo)).substr(0, 4) +
                                           " EV from the darkest to the brightest): choose pictures of different exposures");
        }
    }
    if (settings.align) render::hdr::align(shots, exposures, stopped);
    render::hdr::MergeOptions merge;
    merge.exposures = exposures;
    merge.remove_ghosts = settings.remove_ghosts;
    merge.ghost_base = settings.ghost_base;
    merge.stopped = stopped;
    out.merged = render::hdr::merge(shots, merge);
    const double darkest = *std::min_element(exposures.begin(), exposures.end());
    Json ev = Json::array(), offsets = Json::array();
    for (std::size_t j = 0; j < n; ++j) {
        ev.push_back(std::round(std::log2(exposures[j] / darkest) * 1e6) / 1e6);
        offsets.push_back(Json::array({shots[j].dx, shots[j].dy}));
    }
    out.report = Json{{"exposure_source", source},
                      {"ev", std::move(ev)},
                      {"offsets", std::move(offsets)},
                      {"ghost_base", static_cast<std::int64_t>(out.merged.ghost_base)},
                      {"ghost_fraction", std::round(out.merged.ghost_fraction * 1e6) / 1e6},
                      {"stops", std::round(out.merged.stops * 1e3) / 1e3},
                      {"width", static_cast<std::int64_t>(out.merged.width)},
                      {"height", static_cast<std::int64_t>(out.merged.height)}};
    return out;
}

namespace {

// The radiance tone mapped (16, 8 bits) as display values, RGBA floats; the radiance given up as it is read.
std::vector<render::hdr::Rgba> toned(render::hdr::Merged& merged, const HdrSettings& settings) {
    const std::size_t count = std::size_t(merged.width) * merged.height;
    std::vector<render::hdr::Rgba> px(count);
    for (std::size_t i = 0; i < count; ++i) px[i] = {merged.rgb[i * 3], merged.rgb[i * 3 + 1], merged.rgb[i * 3 + 2], 1.0f};
    std::vector<float>().swap(merged.rgb);
    render::hdr::tone_map(px, merged.width, merged.height, settings.tone);
    return px;
}

}  // namespace

render::codecs::CodecImage hdr_display(const HdrRadiance& radiance, const HdrSettings& settings, double view_exposure) {
    const render::hdr::Merged& m = radiance.merged;
    const std::size_t count = std::size_t(m.width) * m.height;
    std::string rgb(count * 3, '\0');
    if (settings.depth == 32) {
        const auto k = static_cast<float>(std::exp2(view_exposure));
        for (std::size_t i = 0; i < count * 3; ++i)
            rgb[i] = static_cast<char>(std::lround(render::hdr::linear_to_srgb(std::clamp(m.rgb[i] * k, 0.0f, 1.0f)) * 255.0f));
    } else {
        render::hdr::Merged copy = m;
        const auto px = toned(copy, settings);
        for (std::size_t i = 0; i < count; ++i)
            for (std::size_t c = 0; c < 3; ++c) rgb[i * 3 + c] = static_cast<char>(std::lround(std::clamp(px[i][c], 0.0f, 1.0f) * 255.0f));
    }
    return codecs::CodecImage::from_bytes(m.width, m.height, codecs::Layout::Rgb, codecs::Sample::U8, std::move(rgb));
}

HdrMade merge_to_hdr(std::vector<Shot> shots, const HdrSettings& settings, const std::function<bool()>& stopped) {
    check_settings(shots, settings);
    check_work(shots[0].width(), shots[0].height(),
               std::accumulate(shots.begin(), shots.end(), std::uint64_t{0}, [](std::uint64_t a, const Shot& s) { return a + shot_bytes(s); }), settings);
    const render::ImageAllocationBudget budget{codecs::kAllocBudget};
    HdrRadiance radiance;
    {
        const render::image_ops::Held held(std::accumulate(shots.begin(), shots.end(), std::uint64_t{0}, [](std::uint64_t a, const Shot& s) { return a + shot_bytes(s); }));
        radiance = hdr_radiance(std::move(shots), settings, stopped);
    }
    render::hdr::Merged& m = radiance.merged;
    const std::uint32_t w = m.width, h = m.height;
    const std::size_t count = std::size_t(w) * h;
    codecs::CodecImage picture;
    if (settings.depth == 32) {
        std::string bytes(count * 3 * sizeof(float), '\0');
        std::memcpy(bytes.data(), m.rgb.data(), bytes.size());
        std::vector<float>().swap(m.rgb);
        picture = codecs::CodecImage::from_bytes(w, h, codecs::Layout::Rgb, codecs::Sample::F32, std::move(bytes));
        picture.icc = linear_srgb();
    } else {
        if (stopped && stopped()) throw render::Cancelled();
        const auto px = toned(m, settings);
        const bool sixteen = settings.depth == 16;
        std::string bytes(count * 3 * (sixteen ? 2 : 1), '\0');
        for (std::size_t i = 0; i < count; ++i) {
            for (std::size_t c = 0; c < 3; ++c) {
                const float v = std::clamp(px[i][c], 0.0f, 1.0f);
                if (sixteen) {
                    const auto s = static_cast<std::uint16_t>(std::lround(v * 65535.0f));
                    std::memcpy(bytes.data() + (i * 3 + c) * 2, &s, 2);
                } else {
                    bytes[i * 3 + c] = static_cast<char>(std::lround(v * 255.0f));
                }
            }
        }
        picture = codecs::CodecImage::from_bytes(w, h, codecs::Layout::Rgb, sixteen ? codecs::Sample::U16 : codecs::Sample::U8, std::move(bytes));
    }
    picture.meta.dpi = radiance.dpi;
    if (stopped && stopped()) throw render::Cancelled();
    formats::OpenOptions as_is;
    as_is.working_rgb.clear();  // (its own profile — linear-srgb at 32 bits —, or sRGB: never the working one)
    HdrMade out;
    out.book = formats::picture_book(std::move(picture), settings.title.empty() ? std::string("HDR") : settings.title, as_is);
    out.report = std::move(radiance.report);
    out.report["mode"] = settings.depth;
    if (settings.depth != 32) out.report["method"] = render::hdr::method_name(settings.tone.method);
    return out;
}

// --- 写真の切り抜きと角度補正 --------------------------------------------------------------------------------------------------

std::uint64_t crop_work_bytes(const render::codecs::CodecImage& scan, const std::vector<render::scancrop::FoundPhoto>& photos) {
    // (the scan held; then each photo in turn: the part of the scan it lies on and its floats, the photo's floats, its
    // picture and its book's raster — the books made before it kept)
    const std::uint64_t n = codecs::channels(scan.layout()), bytes = codecs::sample_bytes(scan.sample());
    std::uint64_t peak = scan.data().size(), kept = 0;
    for (const render::scancrop::FoundPhoto& f : photos) {
        double x0 = 1e300, y0 = 1e300, x1 = -1e300, y1 = -1e300;
        for (const auto& c : f.corners()) {
            x0 = std::min(x0, c[0]);
            y0 = std::min(y0, c[1]);
            x1 = std::max(x1, c[0]);
            y1 = std::max(y1, c[1]);
        }
        const auto part = static_cast<std::uint64_t>(std::max(1.0, std::ceil(x1 - x0) + 7) * std::max(1.0, std::ceil(y1 - y0) + 7));
        const auto px = static_cast<std::uint64_t>(std::max(1.0, std::round(f.width)) * std::max(1.0, std::round(f.height)));
        const std::uint64_t book = px * 4 * std::max<std::uint64_t>(bytes, 2);  // (its page's raster: RGBA at its depth)
        const std::uint64_t now = part * n * (bytes + 4) + px * n * (4 + bytes) + book;
        peak = std::max(peak, scan.data().size() + kept + now);
        kept += book;
    }
    return peak;
}

std::vector<formats::Opened> crop_and_straighten(const core::Document& doc, std::size_t at, const formats::OpenOptions& options, Json* report,
                                                 const std::function<bool()>& stopped) {
    if (at >= doc.pages.size()) throw core::Error("value", "no such page");
    if (doc.is_deferred(at)) throw core::Error("page_not_loaded", "the page is not read yet");
    const auto stop_here = [&] {
        if (stopped && stopped()) throw render::Cancelled();
    };
    const core::Page& page = doc.page(at);
    const int dpi = static_cast<int>(std::lround(page.spec.dpi.value()));
    const formats::PagePicture scan = formats::page_picture(page, doc, dpi);
    stop_here();
    const std::vector<render::scancrop::FoundPhoto> found = render::scancrop::find_photos(scan.image, stopped);
    if (found.size() > kMaxPhotos) {
        throw core::Error("value", "too many photos on the scan (" + std::to_string(found.size()) + "; at most " + std::to_string(kMaxPhotos) + ")");
    }
    // (what cutting them out holds at most, before any is cut — the review's P2-2 —, and their floats in the budget)
    if (const std::uint64_t need = crop_work_bytes(scan.image, found); need > codecs::kAllocBudget) {
        throw core::Error("image_too_large", "the photos are too large to cut out at once (about " + mib(need) + " MiB of work; at most " +
                                                 mib(codecs::kAllocBudget) + " MiB)");
    }
    const render::ImageAllocationBudget budget{codecs::kAllocBudget};
    formats::OpenOptions as_is = options;
    as_is.working_rgb.clear();  // (the scan's own profile, or sRGB: never the working one assigned to it)
    std::vector<formats::Opened> out;
    Json photos = Json::array(), skipped = Json::array();
    const std::string title = doc.title.empty() ? std::string("スキャン") : doc.title;
    for (std::size_t k = 0; k < found.size(); ++k) {
        stop_here();
        const render::scancrop::FoundPhoto& f = found[k];
        std::optional<codecs::CodecImage> cut;
        try {
            cut = render::scancrop::cut_photo(scan.image, f);
        } catch (const render::scancrop::Unmappable&) {
            // (a photo whose corners cannot be mapped: that one left out and said, the others made — the review's P3-5)
            skipped.push_back(static_cast<std::int64_t>(k + 1));
            continue;
        }
        const std::uint32_t pw = cut->width(), ph = cut->height();
        formats::Opened made = formats::picture_book(std::move(*cut), title + " のコピー " + std::to_string(out.size() + 1), as_is);
        // (the grid of an opened picture's pixels only for an 8-bit RGB page, as 複製… makes one)
        const core::Page& p = made.doc.page(0);
        if (p.color || (p.rgb && p.rgb->depth != 8)) {
            made.doc.spec.grid.reset();
            made.doc.edit_page(0).spec.grid.reset();
        }
        out.push_back(std::move(made));
        photos.push_back(Json{{"width", pw},
                              {"height", ph},
                              {"angle", std::round(f.angle * 1e3) / 1e3},
                              {"center", Json::array({std::round(f.center[0] * 1e2) / 1e2, std::round(f.center[1] * 1e2) / 1e2})}});
    }
    if (!skipped.empty() && !out.empty()) {
        std::string said = "写真";
        for (std::size_t i = 0; i < skipped.size(); ++i) said += (i == 0 ? " " : "・") + std::to_string(skipped[i].get<std::int64_t>());
        out.front().notes.push_back(said + " は角を求められなかったので、切り抜きませんでした");
    }
    if (report != nullptr) {
        *report = Json{{"photos", std::move(photos)}};
        if (!skipped.empty()) (*report)["skipped"] = std::move(skipped);
    }
    return out;
}

}  // namespace genko::service::automate
