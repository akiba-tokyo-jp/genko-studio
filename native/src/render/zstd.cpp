// zstd (render/zstd.hpp).

#include "render/zstd.hpp"

#include <algorithm>
#include <functional>
#include <memory>
#include <optional>
#include <string>

#include <zstd.h>

#include "core/error.hpp"

namespace genko::render::zstd {

namespace {

struct DCtxFree {
    void operator()(ZSTD_DCtx* ctx) const { ZSTD_freeDCtx(ctx); }
};
using DCtx = std::unique_ptr<ZSTD_DCtx, DCtxFree>;

DCtx make_dctx() {
    DCtx ctx(ZSTD_createDCtx());
    if (!ctx) throw core::Error("format", "zstd: no memory to unpack");
    // (zstd's own default, said out loud: a frame asking for a window past 128 MiB is refused, not made)
    ZSTD_DCtx_setParameter(ctx.get(), ZSTD_d_windowLogMax, 27);
    return ctx;
}

[[noreturn]] void broken(const std::string& why) { throw core::Error("format", "zstd: " + why); }

// What the frame's header says it unpacks to: its size, none (unknown), or broken.
std::optional<unsigned long long> said_size(std::string_view frame) {
    const unsigned long long said = ZSTD_getFrameContentSize(frame.data(), frame.size());
    if (said == ZSTD_CONTENTSIZE_ERROR) broken("not a zstd frame");
    if (said == ZSTD_CONTENTSIZE_UNKNOWN) return std::nullopt;
    return said;
}

// Unpacks the one frame into `out` from `at`, growing it up to `cap` bytes (refused past it: `past`). The frame must end
// with the input, and the input with it.
void unpack(std::string_view frame, std::string& out, std::size_t cap, const std::function<void()>& past) {
    DCtx ctx = make_dctx();
    ZSTD_inBuffer in{frame.data(), frame.size(), 0};
    std::size_t made = 0;
    for (;;) {
        if (made == out.size()) {
            if (out.size() >= cap) {
                // (full: one byte more than allowed would mean it is larger — try for one, never keep it)
                char extra = 0;
                ZSTD_outBuffer probe{&extra, 1, 0};
                const std::size_t r = ZSTD_decompressStream(ctx.get(), &probe, &in);
                if (ZSTD_isError(r)) broken(ZSTD_getErrorName(r));
                if (probe.pos > 0) past();
                if (r == 0) break;
                if (in.pos == in.size) broken("the frame is cut short");
                continue;
            }
            const std::size_t grow = std::min<std::size_t>(cap - out.size(), std::max<std::size_t>(out.size(), 1 << 16));
            out.resize(out.size() + grow);
        }
        ZSTD_outBuffer o{out.data(), out.size(), made};
        const std::size_t r = ZSTD_decompressStream(ctx.get(), &o, &in);
        if (ZSTD_isError(r)) broken(ZSTD_getErrorName(r));
        made = o.pos;
        if (r == 0) break;  // (the frame is whole and flushed)
        if (in.pos == in.size && o.pos < o.size) broken("the frame is cut short");
    }
    if (in.pos != in.size) broken("bytes after the frame");
    out.resize(made);
}

}  // namespace

std::string compress(std::string_view bytes, int level) {
    std::string out(ZSTD_compressBound(bytes.size()), '\0');
    const std::size_t n = ZSTD_compress(out.data(), out.size(), bytes.data(), bytes.size(), level);
    if (ZSTD_isError(n)) throw core::Error("io", std::string("zstd: ") + ZSTD_getErrorName(n));
    out.resize(n);
    return out;
}

std::string decompress_exact(std::string_view frame, std::size_t expected) {
    if (const auto said = said_size(frame); said && *said != expected) broken("the frame says " + std::to_string(*said) + " bytes, not " + std::to_string(expected));
    std::string out;
    out.reserve(expected);
    unpack(frame, out, expected, [&] { broken("the frame holds more than " + std::to_string(expected) + " bytes"); });
    if (out.size() != expected) broken("the frame holds " + std::to_string(out.size()) + " bytes, not " + std::to_string(expected));
    return out;
}

std::string decompress_bounded(std::string_view frame, std::size_t max) {
    const auto too_large = [&] { throw core::Error("image_too_large", "the .pcraft holds a blob too large to read (at most " + std::to_string(max) + " bytes)"); };
    const auto said = said_size(frame);
    if (said && *said > max) too_large();
    std::string out;
    if (said) out.reserve(static_cast<std::size_t>(*said));
    unpack(frame, out, max, too_large);
    if (said && out.size() != *said) broken("the frame holds " + std::to_string(out.size()) + " bytes, not the " + std::to_string(*said) + " it says");
    return out;
}

std::string version() { return ZSTD_versionString(); }

}  // namespace genko::render::zstd
