// PC4 節 D6: the camera's EXIF (core/exif.hpp) — PhotoCraft v0.2.0's algo/src/exif.rs (read, build), MIT
// (native/third_party/photocraft/NOTICE.md).

#include "core/exif.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <tuple>
#include <vector>

namespace genko::core::exif {

namespace {

class Tiff {
public:
    Tiff(std::string_view bytes, bool little) : b_(bytes), le_(little) {}

    std::optional<std::uint16_t> u16(std::size_t o) const {
        if (o > b_.size() || b_.size() - o < 2) return std::nullopt;
        const auto a = static_cast<unsigned char>(b_[o]), c = static_cast<unsigned char>(b_[o + 1]);
        return static_cast<std::uint16_t>(le_ ? (a | (c << 8)) : ((a << 8) | c));
    }
    std::optional<std::uint32_t> u32(std::size_t o) const {
        if (o > b_.size() || b_.size() - o < 4) return std::nullopt;
        std::uint32_t v = 0;
        for (std::size_t k = 0; k < 4; ++k) {
            const std::uint32_t byte = static_cast<unsigned char>(b_[o + (le_ ? 3 - k : k)]);
            v = (v << 8) | byte;
        }
        return v;
    }
    // The entries of the IFD at `off`: (tag, type, count, where its value field is).
    std::vector<std::tuple<std::uint16_t, std::uint16_t, std::uint32_t, std::size_t>> ifd(std::size_t off) const {
        std::vector<std::tuple<std::uint16_t, std::uint16_t, std::uint32_t, std::size_t>> out;
        const auto n = u16(off);
        if (!n) return out;
        for (std::size_t k = 0; k < *n; ++k) {
            const std::size_t e = off + 2 + k * 12;
            const auto tag = u16(e), type = u16(e + 2);
            const auto count = u32(e + 4);
            if (!tag || !type || !count) break;
            out.emplace_back(*tag, *type, *count, e + 8);
        }
        return out;
    }
    std::optional<std::size_t> data_at(std::uint16_t type, std::uint32_t count, std::size_t field) const {
        std::size_t unit = 0;
        switch (type) {
            case 1: case 2: case 6: case 7: unit = 1; break;
            case 3: case 8: unit = 2; break;
            case 4: case 9: case 11: unit = 4; break;
            case 5: case 10: case 12: unit = 8; break;
            default: return std::nullopt;
        }
        const std::uint64_t size = std::uint64_t(unit) * count;
        if (size <= 4) return field;
        const auto at = u32(field);
        if (!at) return std::nullopt;
        return static_cast<std::size_t>(*at);
    }
    std::optional<double> number(std::uint16_t type, std::uint32_t count, std::size_t field) const {
        const auto at = data_at(type, count, field);
        if (!at) return std::nullopt;
        switch (type) {
            case 3:
                if (const auto v = u16(*at)) return static_cast<double>(*v);
                return std::nullopt;
            case 4:
                if (const auto v = u32(*at)) return static_cast<double>(*v);
                return std::nullopt;
            case 5: {
                const auto n = u32(*at), d = u32(*at + 4);
                if (!n || !d || *d == 0) return std::nullopt;
                return static_cast<double>(*n) / static_cast<double>(*d);
            }
            case 10: {
                const auto n = u32(*at), d = u32(*at + 4);
                if (!n || !d || static_cast<std::int32_t>(*d) == 0) return std::nullopt;
                return static_cast<double>(static_cast<std::int32_t>(*n)) / static_cast<double>(static_cast<std::int32_t>(*d));
            }
            default: return std::nullopt;
        }
    }
    std::optional<std::string> text(std::uint16_t type, std::uint32_t count, std::size_t field) const {
        if (type != 2) return std::nullopt;
        const auto at = data_at(type, count, field);
        if (!at || *at > b_.size() || b_.size() - *at < count) return std::nullopt;
        std::string s(b_.substr(*at, count));
        // (as String::from_utf8_lossy … trim_end_matches('\0').trim(): the zeros at its end, then the blanks)
        while (!s.empty() && s.back() == '\0') s.pop_back();
        const auto blank = [](char ch) { return ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r' || ch == '\f' || ch == '\v'; };
        while (!s.empty() && blank(s.back())) s.pop_back();
        std::size_t start = 0;
        while (start < s.size() && blank(s[start])) ++start;
        s.erase(0, start);
        if (s.empty()) return std::nullopt;
        return s;
    }

private:
    std::string_view b_;
    bool le_;
};

}  // namespace

Camera read(std::string_view exif) {
    std::string_view b = exif;
    if (b.starts_with(std::string_view("Exif\0\0", 6))) b.remove_prefix(6);
    Camera info;
    if (b.size() < 4) return info;
    bool le = false;
    if (b.substr(0, 4) == std::string_view("II*\0", 4)) le = true;
    else if (b.substr(0, 4) == std::string_view("MM\0*", 4)) le = false;
    else return info;
    const Tiff t(b, le);
    const auto ifd0 = t.u32(4);
    if (!ifd0) return info;
    std::vector<std::size_t> queue{*ifd0}, seen;
    while (!queue.empty()) {
        const std::size_t off = queue.back();
        queue.pop_back();
        if (std::find(seen.begin(), seen.end(), off) != seen.end() || seen.size() > 8) continue;
        seen.push_back(off);
        for (const auto& [tag, type, count, field] : t.ifd(off)) {
            switch (tag) {
                case 0x010F: info.make = t.text(type, count, field); break;
                case 0x0110: info.model = t.text(type, count, field); break;
                case 0x829A: info.exposure_time = t.number(type, count, field); break;
                case 0x829D: info.f_number = t.number(type, count, field); break;
                case 0x8827: info.iso = t.number(type, count, field); break;
                case 0x920A: info.focal_length = t.number(type, count, field); break;
                case 0xA405: {
                    const auto v = t.number(type, count, field);
                    info.focal_length_35mm = v && *v > 0.0 ? v : std::nullopt;
                    break;
                }
                case 0xA434: info.lens = t.text(type, count, field); break;
                case 0x8769:
                    if (const auto p = t.u32(field)) queue.push_back(*p);
                    break;
                default: break;
            }
        }
    }
    return info;
}

std::string build(const Camera& info) {
    struct Entry {
        std::uint16_t tag, type;
        std::uint32_t count;
        std::string bytes;
    };
    const auto le16 = [](std::uint32_t v) { return std::string{static_cast<char>(v & 255), static_cast<char>((v >> 8) & 255)}; };
    const auto le32 = [](std::uint32_t v) {
        return std::string{static_cast<char>(v & 255), static_cast<char>((v >> 8) & 255), static_cast<char>((v >> 16) & 255), static_cast<char>((v >> 24) & 255)};
    };
    const auto text = [](const std::string& s) { return s + std::string(1, '\0'); };
    const auto rational = [&](double v) { return le32(static_cast<std::uint32_t>(std::llround(v * 10000.0))) + le32(10000); };
    std::vector<Entry> ifd0, sub;
    if (info.make) ifd0.push_back({0x010F, 2, static_cast<std::uint32_t>(info.make->size() + 1), text(*info.make)});
    if (info.model) ifd0.push_back({0x0110, 2, static_cast<std::uint32_t>(info.model->size() + 1), text(*info.model)});
    if (info.exposure_time) sub.push_back({0x829A, 5, 1, rational(*info.exposure_time)});
    if (info.f_number) sub.push_back({0x829D, 5, 1, rational(*info.f_number)});
    if (info.iso) sub.push_back({0x8827, 3, 1, le16(static_cast<std::uint32_t>(*info.iso))});
    if (info.focal_length) sub.push_back({0x920A, 5, 1, rational(*info.focal_length)});
    if (info.focal_length_35mm) sub.push_back({0xA405, 3, 1, le16(static_cast<std::uint32_t>(*info.focal_length_35mm))});
    if (info.lens) sub.push_back({0xA434, 2, static_cast<std::uint32_t>(info.lens->size() + 1), text(*info.lens)});
    const auto ifd_len = [](std::size_t n) { return 2 + 12 * n + 4; };
    const std::size_t ifd0_off = 8, sub_off = ifd0_off + ifd_len(ifd0.size() + 1);
    std::size_t data_off = sub_off + ifd_len(sub.size());
    std::string out("II*\0", 4);
    out += le32(static_cast<std::uint32_t>(ifd0_off));
    std::string data;
    const auto write_ifd = [&](const std::vector<Entry>& entries, std::optional<std::uint32_t> pointer) {
        out += le16(static_cast<std::uint32_t>(entries.size() + (pointer ? 1 : 0)));
        for (const Entry& e : entries) {
            out += le16(e.tag) + le16(e.type) + le32(e.count);
            if (e.bytes.size() <= 4) {
                std::string field = e.bytes;
                field.resize(4, '\0');
                out += field;
            } else {
                out += le32(static_cast<std::uint32_t>(data_off));
                data += e.bytes;
                data_off += e.bytes.size();
            }
        }
        if (pointer) out += le16(0x8769) + le16(4) + le32(1) + le32(*pointer);
        out += le32(0);
    };
    write_ifd(ifd0, static_cast<std::uint32_t>(sub_off));
    write_ifd(sub, std::nullopt);
    return out + data;
}

}  // namespace genko::core::exif
