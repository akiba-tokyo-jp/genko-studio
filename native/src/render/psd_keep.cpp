// psd-keep@1's bytes (render/psd_keep.hpp).

#include "render/psd_keep.hpp"

#include "core/psd_keep.hpp"

namespace genko::render::psd_keep {

namespace {

void put_u32(std::string& out, std::uint32_t v) {
    for (int shift = 24; shift >= 0; shift -= 8) out += static_cast<char>((v >> shift) & 0xff);
}

std::uint32_t be32(std::string_view d, std::size_t at) {
    std::uint32_t v = 0;
    for (std::size_t k = 0; k < 4; ++k) v = (v << 8) | static_cast<unsigned char>(d[at + k]);
    return v;
}

}  // namespace

std::string tagged_block(std::string_view signature, std::string_view key, std::string_view payload, bool psb, bool global) {
    std::string out(signature);
    out += key;
    if (psb && core::psb_long_key(key)) put_u32(out, static_cast<std::uint32_t>(static_cast<std::uint64_t>(payload.size()) >> 32));
    put_u32(out, static_cast<std::uint32_t>(payload.size() & 0xffffffffu));
    out += payload;
    if (global) out.append((4 - payload.size() % 4) % 4, '\0');
    return out;
}

std::vector<PatternEntry> pattern_entries(std::string_view payload) {
    std::vector<PatternEntry> out;
    std::size_t at = 0;
    while (payload.size() - at >= 4) {
        const std::uint32_t length = be32(payload, at);
        if (length > payload.size() - at - 4) break;
        PatternEntry entry;
        entry.bytes = payload.substr(at + 4, length);
        // version u32, image mode u32, height and width i16, its name (u32 count and UTF-16 units), its unique id (a Pascal
        // string, unpadded)
        const std::string_view p = entry.bytes;
        if (p.size() >= 16) {
            const std::uint64_t units = be32(p, 12);
            const std::uint64_t id_at = 16 + 2 * units;
            if (id_at < p.size()) {
                const std::size_t n = static_cast<unsigned char>(p[static_cast<std::size_t>(id_at)]);
                if (id_at + 1 + n <= p.size()) entry.id = std::string(p.substr(static_cast<std::size_t>(id_at) + 1, n));
            }
        }
        out.push_back(std::move(entry));
        at += 4 + length;
        const std::size_t pad = (4 - length % 4) % 4;
        if (pad > payload.size() - at) break;
        at += pad;
    }
    return out;
}

std::string pattern_payload(const std::vector<PatternEntry>& entries) {
    std::string out;
    for (const PatternEntry& e : entries) {
        put_u32(out, static_cast<std::uint32_t>(e.bytes.size()));
        out += e.bytes;
        out.append((4 - e.bytes.size() % 4) % 4, '\0');
    }
    return out;
}

}  // namespace genko::render::psd_keep
