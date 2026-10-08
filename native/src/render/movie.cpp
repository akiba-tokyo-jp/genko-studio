#include "render/movie.hpp"

#include <QProcess>
#include <QStandardPaths>
#include <QString>
#include <QStringList>
#include <QTemporaryDir>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <random>
#include <string>
#include <utility>

#include <webp/encode.h>
#include <webp/mux.h>
#include <zlib.h>

#include "core/error.hpp"
#include "core/paths.hpp"
#include "core/pynum.hpp"
#include "render/png.hpp"

namespace genko::render::movie {

namespace {

[[noreturn]] void io_error(const std::filesystem::path& path, const std::string& what) {
    throw core::Error("io", what + ": " + core::path_to_utf8(path));
}

// A name beside `dest` that nothing else uses, ending as `dest` does (ffmpeg chooses its container by the suffix).
std::filesystem::path part_of(const std::filesystem::path& dest) {
    static thread_local std::mt19937_64 rng{std::random_device{}()};
    const std::string stem = core::path_to_utf8(dest.stem());
    for (;;) {
        std::filesystem::path part = dest.parent_path() / core::path_from_utf8("." + stem + ".part" + std::to_string(rng() % 1000000000ULL) +
                                                                                core::path_to_utf8(dest.extension()));
        std::error_code ignored;
        if (!std::filesystem::exists(part, ignored)) return part;
    }
}

void put16le(std::string& out, int v) {
    out.push_back(static_cast<char>(v & 0xff));
    out.push_back(static_cast<char>((v >> 8) & 0xff));
}

void put32be(std::string& out, std::uint32_t v) {
    for (int shift = 24; shift >= 0; shift -= 8) out.push_back(static_cast<char>((v >> shift) & 0xff));
}

// --- GIF ---------------------------------------------------------------------------------------------------------

// GIF's LZW (codes least significant bit first, in sub-blocks of 255 bytes; a clear code when the table is full).
class LzwEncoder {
public:
    LzwEncoder() : table_(4096 * 256, 0) {}

    std::string encode(const std::uint8_t* data, std::size_t count, int min_bits) {
        out_.clear();
        block_.clear();
        bits_ = 0;
        held_ = 0;
        const int clear = 1 << min_bits;
        int size = min_bits + 1;
        int max_code = clear + 1;
        std::fill(table_.begin(), table_.end(), std::uint16_t{0});
        put(clear, size);
        int current = -1;
        for (std::size_t i = 0; i < count; ++i) {
            const int value = data[i];
            if (current < 0) {
                current = value;
                continue;
            }
            const std::uint16_t next = table_[static_cast<std::size_t>(current) * 256 + static_cast<std::size_t>(value)];
            if (next != 0) {
                current = next;
                continue;
            }
            put(current, size);
            table_[static_cast<std::size_t>(current) * 256 + static_cast<std::size_t>(value)] = static_cast<std::uint16_t>(++max_code);
            if (max_code >= (1 << size)) ++size;
            if (max_code == 4095) {
                put(clear, size);
                std::fill(table_.begin(), table_.end(), std::uint16_t{0});
                size = min_bits + 1;
                max_code = clear + 1;
            }
            current = value;
        }
        if (current >= 0) put(current, size);
        put(clear, size);
        put(clear + 1, min_bits + 1);
        if (bits_ > 0) byte(static_cast<std::uint8_t>(held_ & 0xff));
        if (!block_.empty()) flush_block();
        out_.push_back('\0');
        return std::move(out_);
    }

private:
    void put(int code, int size) {
        held_ |= static_cast<std::uint32_t>(code) << bits_;
        bits_ += size;
        while (bits_ >= 8) {
            byte(static_cast<std::uint8_t>(held_ & 0xff));
            held_ >>= 8;
            bits_ -= 8;
        }
    }
    void byte(std::uint8_t b) {
        block_.push_back(static_cast<char>(b));
        if (block_.size() == 255) flush_block();
    }
    void flush_block() {
        out_.push_back(static_cast<char>(block_.size()));
        out_ += block_;
        block_.clear();
    }

    std::vector<std::uint16_t> table_;
    std::string out_;
    std::string block_;
    std::uint32_t held_ = 0;
    int bits_ = 0;
};

struct GifFrame {
    Image picture;      // "P"
    std::string shown;  // its RGB bytes (frames that show the same are merged)
    int duration = 0;   // ms
};

// --- PNG ---------------------------------------------------------------------------------------------------------

std::string chunk(std::string_view type, std::string_view data) {
    std::string out;
    put32be(out, static_cast<std::uint32_t>(data.size()));
    out.append(type);
    out.append(data);
    uLong crc = crc32(0L, Z_NULL, 0);
    crc = crc32(crc, reinterpret_cast<const Bytef*>(type.data()), static_cast<uInt>(type.size()));
    crc = crc32(crc, reinterpret_cast<const Bytef*>(data.data()), static_cast<uInt>(data.size()));
    put32be(out, static_cast<std::uint32_t>(crc));
    return out;
}

int paeth(int a, int b, int c) {
    const int p = a + b - c, pa = std::abs(p - a), pb = std::abs(p - b), pc = std::abs(p - c);
    if (pa <= pb && pa <= pc) return a;
    return pb <= pc ? b : c;
}

// The rows of an RGB picture, each with the filter that gives the smallest sum (libpng's heuristic), deflated.
std::string png_rows(const Image& rgb) {
    const std::string pixels = rgb.tobytes();
    const std::size_t stride = static_cast<std::size_t>(rgb.width()) * 3;
    std::string filtered;
    filtered.reserve((stride + 1) * static_cast<std::size_t>(rgb.height()));
    std::string zero(stride, '\0');
    std::array<std::string, 5> candidates;
    for (auto& c : candidates) c.resize(stride);
    for (int y = 0; y < rgb.height(); ++y) {
        const auto* row = reinterpret_cast<const std::uint8_t*>(pixels.data() + static_cast<std::size_t>(y) * stride);
        const auto* up = y > 0 ? reinterpret_cast<const std::uint8_t*>(pixels.data() + static_cast<std::size_t>(y - 1) * stride)
                               : reinterpret_cast<const std::uint8_t*>(zero.data());
        std::uint64_t best_sum = UINT64_MAX;
        int best = 0;
        for (int f = 0; f < 5; ++f) {
            std::uint64_t sum = 0;
            for (std::size_t i = 0; i < stride; ++i) {
                const int a = i >= 3 ? row[i - 3] : 0, b = up[i], c = i >= 3 ? up[i - 3] : 0;
                int v = row[i];
                if (f == 1) v -= a;
                else if (f == 2) v -= b;
                else if (f == 3) v -= (a + b) / 2;
                else if (f == 4) v -= paeth(a, b, c);
                const auto byte = static_cast<std::uint8_t>(v & 0xff);
                candidates[f][i] = static_cast<char>(byte);
                sum += byte < 128 ? byte : 256 - byte;
            }
            if (sum < best_sum) {
                best_sum = sum;
                best = f;
            }
        }
        filtered.push_back(static_cast<char>(best));
        filtered += candidates[best];
    }
    uLongf size = compressBound(static_cast<uLong>(filtered.size()));
    std::string out(size, '\0');
    if (compress2(reinterpret_cast<Bytef*>(out.data()), &size, reinterpret_cast<const Bytef*>(filtered.data()),
                  static_cast<uLong>(filtered.size()), 6) != Z_OK) {
        throw core::Error("memory", "the picture could not be compressed");
    }
    out.resize(size);
    return out;
}

// --- WebP --------------------------------------------------------------------------------------------------------

// _webp.c import_frame_libwebp: an RGB picture as ARGB (alpha 255).
void import_rgb(WebPPicture& picture, const Image& rgb) {
    picture.width = rgb.width();
    picture.height = rgb.height();
    picture.use_argb = 1;
    if (!WebPPictureAlloc(&picture)) throw core::Error("memory", "cannot allocate the WebP picture");
    const std::string pixels = rgb.tobytes();
    for (int y = 0; y < rgb.height(); ++y) {
        const auto* row = reinterpret_cast<const std::uint8_t*>(pixels.data() + static_cast<std::size_t>(y) * rgb.width() * 3);
        std::uint32_t* dst = picture.argb + static_cast<std::size_t>(y) * static_cast<std::size_t>(picture.argb_stride);
        for (int x = 0; x < rgb.width(); ++x) {
            dst[x] = 0xff000000U | (static_cast<std::uint32_t>(row[x * 3]) << 16) | (static_cast<std::uint32_t>(row[x * 3 + 1]) << 8) |
                     static_cast<std::uint32_t>(row[x * 3 + 2]);
        }
    }
}

WebPConfig webp_config(int method) {
    WebPConfig config;
    if (!WebPConfigInit(&config)) throw core::Error("value", "the WebP library does not match");
    config.lossless = 0;
    config.quality = 80.0F;
    config.alpha_quality = 100;
    config.method = method;
    config.exact = 0;
    if (!WebPValidateConfig(&config)) throw core::Error("value", "invalid WebP configuration");
    return config;
}

std::string webp_still(const Image& rgb) {
    WebPConfig config = webp_config(4);
    WebPPicture picture;
    if (!WebPPictureInit(&picture)) throw core::Error("value", "the WebP library does not match");
    WebPMemoryWriter writer;
    WebPMemoryWriterInit(&writer);
    std::string out;
    try {
        import_rgb(picture, rgb);
        picture.writer = WebPMemoryWrite;
        picture.custom_ptr = &writer;
        if (!WebPEncode(&config, &picture)) throw core::Error("value", "cannot write file as WebP (encoder returned None)");
        out.assign(reinterpret_cast<const char*>(writer.mem), writer.size);
    } catch (...) {
        WebPPictureFree(&picture);
        WebPMemoryWriterClear(&writer);
        throw;
    }
    WebPPictureFree(&picture);
    WebPMemoryWriterClear(&writer);
    return out;
}

}  // namespace

// --- the writer --------------------------------------------------------------------------------------------------------

struct Writer::Impl {
    std::filesystem::path dest;
    std::filesystem::path part;
    std::string fmt;
    double fps = 12;
    double hold = 0;
    bool loop = true;
    int duration = 0;  // ms, each picture
    Size size{0, 0};
    bool finished = false;

    // the pictures after merging (not MP4): the last one, not written yet, and how many there were
    std::optional<Image> last;
    int last_duration = 0;
    int kept = 0;

    std::ofstream file;
    // GIF
    std::optional<GifFrame> gif_pending;
    LzwEncoder lzw;
    bool gif_started = false;
    // PNG
    std::streamoff actl_at = 0;
    int png_frames = 0;
    std::uint32_t sequence = 0;
    // WebP
    WebPAnimEncoder* webp = nullptr;
    long long timestamp = 0;
    // MP4
    std::unique_ptr<QTemporaryDir> frames_dir;
    int mp4_count = 0;
    std::filesystem::path last_png;

    ~Impl() {
        if (webp != nullptr) WebPAnimEncoderDelete(webp);
        if (file.is_open()) file.close();
        if (!finished && !part.empty()) {
            std::error_code ignored;
            std::filesystem::remove(part, ignored);
        }
    }

    void open_part() {
        part = part_of(dest);
        file.open(part, std::ios::binary | std::ios::trunc);
        if (!file) io_error(part, "cannot write");
    }

    void write(std::string_view bytes) {
        file.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        if (!file) io_error(part, "cannot write");
    }

    // A picture after the merging, with its duration (the next one has come, or it is the last).
    void push_frame(const Image& rgb, int ms, bool final) {
        if (fmt == "gif") {
            GifFrame frame{rgb.quantize(128), {}, ms};
            frame.shown = frame.picture.convert("RGB").tobytes();
            if (gif_pending && gif_pending->shown == frame.shown) {
                gif_pending->duration += ms;
            } else {
                if (gif_pending) gif_frame(*gif_pending);
                gif_pending = std::move(frame);
            }
            if (final) gif_frame(*gif_pending);
        } else if (fmt == "png") {
            png_frame(rgb, ms);
        } else if (fmt == "webp") {
            webp_frame(rgb, ms);
        }
    }

    void gif_frame(const GifFrame& frame) {
        const bool many = kept >= 2;  // (one picture: Python's save() with a duration, no loop block)
        if (!gif_started) {
            open_part();
            std::string head = "GIF89a";
            put16le(head, size.width);
            put16le(head, size.height);
            head.push_back('\0');  // (no global colour table: each frame has its own)
            head.push_back('\0');
            head.push_back('\0');
            if (many && loop) {
                head += std::string("\x21\xff\x0bNETSCAPE2.0\x03\x01", 16);
                put16le(head, 0);
                head.push_back('\0');
            }
            write(head);
            gif_started = true;
        }
        const auto palette = frame.picture.palette();
        int bits = 1;
        while ((1 << bits) < static_cast<int>(std::max<std::size_t>(palette.size(), 2))) ++bits;
        std::string out;
        out += std::string("\x21\xf9\x04\x00", 4);  // graphic control: no disposal, no transparency
        put16le(out, frame.duration / 10);          // (Pillow: int(duration / 10))
        out.push_back('\0');
        out.push_back('\0');
        out.push_back('\x2c');
        put16le(out, 0);
        put16le(out, 0);
        put16le(out, size.width);
        put16le(out, size.height);
        out.push_back(static_cast<char>(0x80 | (bits - 1)));
        for (int i = 0; i < (1 << bits); ++i) {
            const std::array<std::uint8_t, 3> rgb = i < static_cast<int>(palette.size()) ? palette[static_cast<std::size_t>(i)]
                                                                                         : std::array<std::uint8_t, 3>{0, 0, 0};
            for (const std::uint8_t c : rgb) out.push_back(static_cast<char>(c));
        }
        const int min_bits = std::max(2, bits);
        out.push_back(static_cast<char>(min_bits));
        const std::string indices = frame.picture.tobytes();
        out += lzw.encode(reinterpret_cast<const std::uint8_t*>(indices.data()), indices.size(), min_bits);
        write(out);
    }

    void png_frame(const Image& rgb, int ms) {
        if (png_frames == 0) {
            open_part();
            std::string head("\x89PNG\r\n\x1a\n", 8);
            std::string ihdr;
            put32be(ihdr, static_cast<std::uint32_t>(size.width));
            put32be(ihdr, static_cast<std::uint32_t>(size.height));
            ihdr += std::string("\x08\x02\x00\x00\x00", 5);
            head += chunk("IHDR", ihdr);
            write(head);
            actl_at = file.tellp();
            std::string actl;
            put32be(actl, 0);  // (the number of frames, written at the end)
            put32be(actl, loop ? 0 : 1);
            write(chunk("acTL", actl));
        }
        std::string fctl;
        put32be(fctl, sequence++);
        put32be(fctl, static_cast<std::uint32_t>(size.width));
        put32be(fctl, static_cast<std::uint32_t>(size.height));
        put32be(fctl, 0);
        put32be(fctl, 0);
        const int delay = std::clamp(ms, 0, 65535);
        fctl.push_back(static_cast<char>((delay >> 8) & 0xff));
        fctl.push_back(static_cast<char>(delay & 0xff));
        fctl.push_back(static_cast<char>((1000 >> 8) & 0xff));
        fctl.push_back(static_cast<char>(1000 & 0xff));
        fctl.push_back('\0');  // dispose: none
        fctl.push_back('\0');  // blend: source
        std::string out = chunk("fcTL", fctl);
        const std::string data = png_rows(rgb);
        constexpr std::size_t kChunk = 1 << 20;
        for (std::size_t at = 0; at < data.size(); at += kChunk) {
            const std::string_view piece = std::string_view(data).substr(at, kChunk);
            if (png_frames == 0) {
                out += chunk("IDAT", piece);
            } else {
                std::string fdat;
                put32be(fdat, sequence++);
                fdat.append(piece);
                out += chunk("fdAT", fdat);
            }
        }
        write(out);
        ++png_frames;
    }

    void webp_frame(const Image& rgb, int ms) {
        if (webp == nullptr) {
            WebPAnimEncoderOptions options;
            if (!WebPAnimEncoderOptionsInit(&options)) throw core::Error("value", "the WebP library does not match");
            options.anim_params.bgcolor = 0;
            options.anim_params.loop_count = loop ? 0 : 1;
            options.minimize_size = 0;
            options.kmin = 3;
            options.kmax = 5;
            options.allow_mixed = 0;
            options.verbose = 0;
            webp = WebPAnimEncoderNew(size.width, size.height, &options);
            if (webp == nullptr) throw core::Error("memory", "cannot make the WebP encoder");
        }
        WebPConfig config = webp_config(0);
        WebPPicture picture;
        if (!WebPPictureInit(&picture)) throw core::Error("value", "the WebP library does not match");
        try {
            import_rgb(picture, rgb);
            if (!WebPAnimEncoderAdd(webp, &picture, static_cast<int>(timestamp), &config)) {
                throw core::Error("value", std::string("cannot write file as WebP: ") + WebPAnimEncoderGetError(webp));
            }
        } catch (...) {
            WebPPictureFree(&picture);
            throw;
        }
        WebPPictureFree(&picture);
        timestamp += ms;
    }

    std::filesystem::path close_and_place() {
        file.flush();
        if (!file) io_error(part, "cannot write");
        file.close();
        std::error_code error;
        std::filesystem::rename(part, dest, error);
        if (error) io_error(dest, "cannot write");
        finished = true;
        return dest;
    }
};

Writer::Writer(std::filesystem::path dest, double fps, std::string_view fmt, double hold, bool loop) : impl_(std::make_unique<Impl>()) {
    if (std::find(kFormats.begin(), kFormats.end(), fmt) == kFormats.end()) {
        throw core::PyValueError("format must be one of webp, gif, png, mp4");
    }
    impl_->dest = std::move(dest);
    impl_->fmt = std::string(fmt);
    impl_->fps = fps;
    impl_->hold = hold;
    impl_->loop = loop;
    impl_->duration = static_cast<int>(std::max<std::int64_t>(20, core::py_round_int(1000.0 / fps)));
    std::error_code ignored;
    if (!impl_->dest.parent_path().empty()) std::filesystem::create_directories(impl_->dest.parent_path(), ignored);
    if (fmt == "mp4") {
        if (!ffmpeg()) throw core::PyValueError("MP4 needs ffmpeg on this computer; WebP, GIF and PNG need nothing");
        impl_->frames_dir = std::make_unique<QTemporaryDir>();
        if (!impl_->frames_dir->isValid()) throw core::Error("io", "cannot make a folder for the frames");
    }
}

Writer::~Writer() = default;

void Writer::add(const Image& picture) {
    Impl& w = *impl_;
    const Image rgb = picture.mode() == "RGB" ? picture : picture.convert("RGB");
    if (w.size.width == 0 && w.size.height == 0) {
        w.size = rgb.size();
    } else if (rgb.size() != w.size) {
        throw core::Error("value", "the pictures of a moving picture are all of one size");
    }
    if (w.fmt == "mp4") {
        const Image even = rgb.crop(Box{0, 0, rgb.width() - rgb.width() % 2, rgb.height() - rgb.height() % 2});
        char name[16];
        std::snprintf(name, sizeof(name), "%06d.png", w.mp4_count++);
        w.last_png = core::path_from_utf8(w.frames_dir->path().toStdString()) / name;
        save_png(even, w.last_png);
        return;
    }
    if (w.last && w.last->tobytes() == rgb.tobytes()) {
        w.last_duration += w.duration;
        return;
    }
    if (w.last) w.push_frame(*w.last, w.last_duration, false);
    w.last = rgb;
    w.last_duration = w.duration;
    ++w.kept;
}

std::filesystem::path Writer::finish(int* frames) {
    Impl& w = *impl_;
    if (w.fmt == "mp4") {
        if (w.mp4_count == 0) throw core::PyUncaught("IndexError", "list index out of range");
        const auto extra = std::max<std::int64_t>(0, core::py_round_int(w.hold * w.fps));
        for (std::int64_t i = 0; i < extra; ++i) {
            char name[16];
            std::snprintf(name, sizeof(name), "%06d.png", w.mp4_count++);
            std::error_code error;
            std::filesystem::copy_file(w.last_png, w.last_png.parent_path() / name, error);
            if (error) io_error(w.last_png.parent_path() / name, "cannot write");
        }
        if (frames != nullptr) *frames = w.mp4_count;
        w.part = part_of(w.dest);
        QProcess run;
        run.setProgram(QString::fromStdString(core::path_to_utf8(*ffmpeg())));
        run.setArguments({QStringLiteral("-y"), QStringLiteral("-loglevel"), QStringLiteral("error"), QStringLiteral("-framerate"),
                          QString::fromStdString(core::py_float_repr(w.fps)), QStringLiteral("-i"), w.frames_dir->filePath(QStringLiteral("%06d.png")),
                          QStringLiteral("-c:v"), QStringLiteral("libx264"), QStringLiteral("-pix_fmt"), QStringLiteral("yuv420p"),
                          QString::fromStdString(core::path_to_utf8(w.part))});
        run.start();
        if (!run.waitForStarted(30000)) throw core::PyValueError("ffmpeg failed: it could not be started");
        run.waitForFinished(-1);
        if (run.exitStatus() != QProcess::NormalExit || run.exitCode() != 0) {
            const QString err = QString::fromUtf8(run.readAllStandardError()).trimmed();
            throw core::PyValueError("ffmpeg failed: " + err.left(300).toStdString());
        }
        std::error_code error;
        std::filesystem::rename(w.part, w.dest, error);
        if (error) io_error(w.dest, "cannot write");
        w.finished = true;
        return w.dest;
    }
    if (!w.last) throw core::PyUncaught("IndexError", "list index out of range");
    w.last_duration += static_cast<int>(core::py_round_int(w.hold * 1000));  // (the finished picture stays a little)
    if (frames != nullptr) *frames = w.kept;
    if (w.kept == 1) {  // one picture: a still file (Python's save(), not save_all())
        if (w.fmt == "png") {
            w.open_part();
            w.write(write_png(*w.last));
        } else if (w.fmt == "webp") {
            w.open_part();
            w.write(webp_still(*w.last));
        } else {
            w.push_frame(*w.last, w.last_duration, true);
            w.write(";");
        }
        return w.close_and_place();
    }
    w.push_frame(*w.last, w.last_duration, true);
    if (w.fmt == "gif") {
        w.write(";");
    } else if (w.fmt == "png") {
        w.write(chunk("IEND", {}));
        w.file.seekp(w.actl_at);
        std::string actl;
        put32be(actl, static_cast<std::uint32_t>(w.png_frames));
        put32be(actl, w.loop ? 0 : 1);
        w.write(chunk("acTL", actl));
        w.file.seekp(0, std::ios::end);
    } else if (w.fmt == "webp") {
        if (!WebPAnimEncoderAdd(w.webp, nullptr, static_cast<int>(w.timestamp), nullptr)) {
            throw core::Error("value", std::string("cannot write file as WebP: ") + WebPAnimEncoderGetError(w.webp));
        }
        WebPData data;
        WebPDataInit(&data);
        if (!WebPAnimEncoderAssemble(w.webp, &data)) {
            throw core::Error("value", std::string("cannot write file as WebP: ") + WebPAnimEncoderGetError(w.webp));
        }
        w.open_part();
        try {
            w.write(std::string_view(reinterpret_cast<const char*>(data.bytes), data.size));
        } catch (...) {
            WebPDataClear(&data);
            throw;
        }
        WebPDataClear(&data);
    }
    return w.close_and_place();
}

std::filesystem::path write_movie(const std::vector<Image>& pictures, const std::filesystem::path& dest, double fps,
                                  std::string_view fmt, double hold, bool loop, int* frames) {
    Writer writer(dest, fps, fmt, hold, loop);
    for (const Image& picture : pictures) writer.add(picture);
    return writer.finish(frames);
}

std::optional<std::filesystem::path> ffmpeg() {
    const QString found = QStandardPaths::findExecutable(QStringLiteral("ffmpeg"));
    if (found.isEmpty()) return std::nullopt;
    return core::path_from_utf8(found.toStdString());
}

}  // namespace genko::render::movie
