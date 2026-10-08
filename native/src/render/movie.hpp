#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "render/image.hpp"

// Moving pictures (Python's genko/timelapse.write_movie, which the animation export writes through too): pictures of one
// size as an animated WebP, GIF or PNG written here (libwebp, Pillow's own quantizer, zlib), or an MP4 made by the ffmpeg
// on the computer (never shipped: docs/cpp-migration/THIRD_PARTY.md). The same picture twice in a row is one picture
// shown twice as long; `hold` keeps the last one on screen that many seconds more. As Python's files:
//   GIF:  each picture reduced to 128 colours (Image.quantize: median cut, no dither), durations in 1/100 s, looping
//         forever when `loop` (no loop block otherwise, nor for one picture); pictures the same after the reduction are
//         one frame (Pillow merges them).
//   PNG:  APNG (acTL plays forever, or once), every frame whole (Pillow crops a frame to what changed: the frames show
//         the same); one picture is a plain PNG.
//   WebP: lossy, quality 80: WebPAnimEncoder (kmin 3, kmax 5, method 0, loop 0 or 1); one picture WebPEncode (method 4).
//   MP4:  ffmpeg -framerate <fps> -i %06d.png -c:v libx264 -pix_fmt yuv420p, each picture cut to even sides and the
//         last repeated for `hold` (no pictures merged).
// The pictures go to the file as they come (only the last one and the encoder's own state are held). The file is
// written beside `dest` and moved into place when it is whole: a failed or abandoned writing leaves nothing behind.

namespace genko::render::movie {

inline constexpr std::array<std::string_view, 4> kFormats{"webp", "gif", "png", "mp4"};

class Writer {
public:
    // PyValueError "format must be one of webp, gif, png, mp4"; for mp4 without ffmpeg "MP4 needs ffmpeg on this
    // computer; WebP, GIF and PNG need nothing".
    Writer(std::filesystem::path dest, double fps, std::string_view fmt, double hold = 0.0, bool loop = true);
    ~Writer();
    Writer(const Writer&) = delete;
    Writer& operator=(const Writer&) = delete;

    // One more picture (as RGB). The first sets the size; core::Error("value") for one of another size.
    void add(const Image& picture);
    // The file, whole (PyValueError "ffmpeg failed: …"; core::Error("io") when it cannot be written). `frames`: the
    // pictures in it (Python's report["frames"]).
    std::filesystem::path finish(int* frames = nullptr);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// timelapse.write_movie(pictures, dest, fps, fmt, hold=hold, loop=loop): Writer with each picture added.
std::filesystem::path write_movie(const std::vector<Image>& pictures, const std::filesystem::path& dest, double fps,
                                  std::string_view fmt, double hold = 0.0, bool loop = true, int* frames = nullptr);

// GIF's LZW of a frame's colour indices (codes of min_bits + 1 bits and up, least significant bit first, in sub-blocks of
// up to 255 bytes, ended by the empty block): what a frame's image data holds (tests decode it strictly).
std::string gif_lzw(const std::vector<std::uint8_t>& indices, int min_bits);

// shutil.which("ffmpeg").
std::optional<std::filesystem::path> ffmpeg();

}  // namespace genko::render::movie
