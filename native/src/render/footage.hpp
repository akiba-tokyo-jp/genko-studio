#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "core/json.hpp"
#include "core/model.hpp"
#include "render/codecs/limits.hpp"

// PC5 J-e2 (video@1, SPEC.md PROD-01「ビデオレイヤー」): a video layer's footage — read from a file, kept as the book's
// pictures, and one of its frames laid on its page.
//
// Footage is read (read_footage) from one picture ("image"), a numbered sequence of pictures — a folder of them, or a file
// whose name ends in digits and the files beside it of the same name and other digits ("sequence") — or every frame of a
// moving picture: GIF, APNG and animated WebP ("animation"). The user's decision 4 (PC5): a video file (MP4・MOV・WebM) is
// never read. Each frame becomes a PNG of RGBA (8 bits, or 16 where a sequence's pictures hold more: their samples kept to
// 16 bits; colours as their numbers — sRGB —, a profile left aside and said, a picture of linear light given sRGB's curve)
// handed to the caller's `Store` (the book's asset store, or the book in memory), the same picture held once. What the
// footage would take decoded (each frame w × h × 4 samples) is checked before any frame is decoded, against what the
// book's footage may take more (core::kFootageMaxBytes, 4 GiB, less what its video rows hold): past it, refused
// ("footage_too_large"). A footage of more than 3000 frames (core::anim::kMaxFrames) is refused too. A moving picture's
// frames are laid at the speed of its shortest frame (1/1000 s to 120 frames a second), a longer one shown as many times
// as it lasts. A broken or huge moving picture is refused (core::Error "format", "image_too_large"), never read past its
// bounds: the codecs' limits (100000 pixels a side, 120 million pixels, 2 GiB) on its canvas before its frames.
//
// A footage frame is laid on its page (footage_layer) as rasterize_layer makes a smart object of its picture a paint
// layer: through its placement (core::footage_placement), at the paint layers' 200 dpi, an 8-bit page's PNG, a page in
// precise colour's raster (a 16-bit footage keeping its bits), a page of inks' inks — a placed picture's way, the
// monochrome finish on a monochrome page. What it makes is remembered (the latest, up to 256 MiB of pixels' bytes),
// forgotten with the drawing's caches (clear_render_caches). frame_page is the page at a frame of its animation with each
// video row's footage frame laid in its folder (core::anim::at_frame's copy, the altered cels shown there).

namespace genko::render::footage {

// Keeps a frame's PNG (its bytes), giving the asset ref it is held under ("sha256:…").
using Store = std::function<std::string(const std::string& png)>;

struct ReadOptions {
    bool sequence = true;                                 // a file whose name ends in digits: the files beside it too
    std::optional<double> fps;                            // the footage's speed (none: a moving picture's own, else 30)
    std::int64_t every = 1;                               // every n-th frame of the file (1: all)
    std::optional<std::array<std::int64_t, 2>> range;     // the file's frames read, [first, last] from 1 (none: all)
    std::uint64_t most_bytes = 4ull << 30;                // what the footage may take decoded (the book's room for it)
    codecs::Limits limits{};                              // each picture's (a test's smaller ones)
    // (the review's P2-1, P3-3) at most this many frames, a moving picture's long frames shown as many times as they last
    // counted (ビデオフレームからレイヤー…: 500 pictures, `repeat` false: each picture once) — refused before any is read
    std::int64_t most_frames = 3000;
    bool repeat = true;
    // (the review's P2-6, P3-4) asked between frames: true stops the reading (render::Cancelled); told how far it is
    // (0..1); each picture the store kept handed back when the reading fails or is stopped (the store's to take away)
    std::function<bool()> stopped;
    std::function<void(double)> progress;
    std::function<void(const std::string& ref)> drop;
};

struct Footage {
    core::Json json;                 // add_video_layer's "footage": {kind, name, frames, fps, size_px, depth, alpha, par, source}
    std::int64_t read = 0;           // the file's frames read (before a long one was shown more than once)
    std::vector<std::string> notes;  // what was said of the reading (Japanese)
};

// The footage of `path` (above). core::Error: "io" for a file that cannot be read, "unidentified_image" for no picture this
// build reads (a video file: "video_file", decision 4), "format" for one broken, "image_too_large" past a picture's limits,
// "footage_too_large" past `most_bytes`, "value" for options or a sequence that do not fit (pictures of another size,
// more than 3000 frames, a range outside the file's).
Footage read_footage(const std::filesystem::path& path, const ReadOptions& options, const Store& store);

// The files a numbered sequence is read from: the pictures of a folder (in their names' order, numbers as numbers), or
// the file and those beside it of its name with other digits (`sequence`; otherwise the file alone).
std::vector<std::filesystem::path> sequence_files(const std::filesystem::path& path, bool sequence);

// A Store of the book's asset store (<book>/assets/…, PNG), and of the book in memory (Document::smart_files).
Store book_store(const std::filesystem::path& book);
Store memory_store(core::Document& doc);

// The bytes of a footage frame the book holds (in memory, or in its folder); none: it does not.
std::optional<std::string> frame_bytes(const core::Document& doc, const std::string& ref);

// The footage frame `index` of the video row as a paint layer of the page (its id, title, parent and visibility the
// caller's): raster_png, color_raster or ink_raster and the finish's screen, as rasterize_layer gives a smart object of
// its picture. core::Error("missing_asset") when the book does not hold the frame; the drawing's errors.
core::Layer footage_layer(const core::Document& doc, const core::Page& page, const core::Json& video, std::int64_t index);

// The page as it is at this frame of its animation: core::anim::at_frame's copy, with each video row's footage frame laid
// as a paint layer in its folder (under the folder, as a cel; shown as the folder is), id "<folder>~footage". A frame the
// book does not hold, or cannot draw, is left out. The page as at_frame makes it when it has no video row or there is no
// book to read the footage from.
core::Page frame_page(const core::Page& page, std::int64_t frame, const core::Document* episode);
// The page as it is (every layer as it shows, the animation's cels too), with each video row's footage frame at this frame
// laid as frame_page lays it: what a file that holds no video rows is written from (a .pcraft: formats/pcraft_write).
// `laid`: the video layers whose frame was laid (their folders' titles).
core::Page with_footage(const core::Page& page, std::int64_t frame, const core::Document* episode, std::vector<std::string>* laid = nullptr);

// The footage frames remembered (their paint layers), forgotten; the bytes they hold; how many were made (tests).
void clear_cache();
std::int64_t cache_bytes();
std::uint64_t layers_made();

}  // namespace genko::render::footage
