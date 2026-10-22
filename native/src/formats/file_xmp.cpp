// The book's File Info in the files it is written to (formats/file_xmp.hpp).

#include "formats/file_xmp.hpp"

#include <zlib.h>

#include "core/file_info.hpp"
#include "storage/asset_store.hpp"

namespace genko::formats {

namespace {

void put32(std::string& out, std::uint32_t v) {
    for (int shift = 24; shift >= 0; shift -= 8) out += static_cast<char>((v >> shift) & 0xFF);
}

std::uint32_t be32(std::string_view b, std::size_t at) {
    return (std::uint32_t(static_cast<unsigned char>(b[at])) << 24) | (std::uint32_t(static_cast<unsigned char>(b[at + 1])) << 16) |
           (std::uint32_t(static_cast<unsigned char>(b[at + 2])) << 8) | std::uint32_t(static_cast<unsigned char>(b[at + 3]));
}

}  // namespace

std::optional<std::string> book_xmp(const core::Document& doc) {
    if (!doc.file_info || doc.file_info->empty()) return std::nullopt;
    const core::FileInfo& info = *doc.file_info;
    std::optional<std::string> packet;
    if (!info.xmp.empty()) {
        if (const auto it = doc.native_files.find(info.xmp); it != doc.native_files.end() && it->second) {
            packet = *it->second;
        } else if (doc.asset_dir) {
            try {
                packet = storage::AssetStore(*doc.asset_dir).get_bytes(info.xmp, core::kXmpSuffix);
            } catch (const std::exception&) {
                packet.reset();  // (the packet cannot be read: the items made anew)
            }
        }
    }
    return core::write_xmp(packet ? std::optional<std::string_view>(*packet) : std::nullopt, info, doc.title);
}

std::string png_with_xmp(std::string png, std::string_view xmp) {
    if (png.size() < 8) return png;
    std::size_t at = 8;
    while (at + 12 <= png.size()) {
        const std::uint32_t length = be32(png, at);
        if (png.compare(at + 4, 4, "IDAT") == 0) {
            std::string data = std::string("XML:com.adobe.xmp", 17) + std::string(5, '\0');  // (keyword, no compression, no language, no translation)
            data += xmp;
            std::string chunk;
            put32(chunk, static_cast<std::uint32_t>(data.size()));
            chunk += "iTXt";
            chunk += data;
            uLong crc = crc32(0L, reinterpret_cast<const Bytef*>("iTXt"), 4);
            crc = crc32(crc, reinterpret_cast<const Bytef*>(data.data()), static_cast<uInt>(data.size()));
            put32(chunk, static_cast<std::uint32_t>(crc));
            png.insert(at, chunk);
            return png;
        }
        if (length > png.size() - at - 12) break;
        at += 12 + std::size_t(length);
    }
    return png;
}

std::string jpeg_with_xmp(std::string jpeg, std::string_view xmp) {
    static constexpr std::string_view kXmp("http://ns.adobe.com/xap/1.0/\0", 29);
    if (jpeg.size() < 4 || static_cast<unsigned char>(jpeg[0]) != 0xFF || static_cast<unsigned char>(jpeg[1]) != 0xD8) return jpeg;
    if (xmp.size() + kXmp.size() + 2 > 65535) return jpeg;
    std::size_t at = 2;
    // (after the JFIF header, as libjpeg writes it first)
    if (jpeg.size() >= at + 4 && static_cast<unsigned char>(jpeg[at]) == 0xFF && static_cast<unsigned char>(jpeg[at + 1]) == 0xE0) {
        const std::size_t length = (std::size_t(static_cast<unsigned char>(jpeg[at + 2])) << 8) | static_cast<unsigned char>(jpeg[at + 3]);
        if (at + 2 + length <= jpeg.size()) at += 2 + length;
    }
    std::string segment = "\xFF\xE1";
    const std::size_t length = xmp.size() + kXmp.size() + 2;
    segment += static_cast<char>((length >> 8) & 0xFF);
    segment += static_cast<char>(length & 0xFF);
    segment += kXmp;
    segment += xmp;
    jpeg.insert(at, segment);
    return jpeg;
}

std::string psd_with_xmp(std::string psd, std::string_view xmp) {
    if (psd.size() < 34 || psd.compare(0, 4, "8BPS") != 0) return psd;
    const std::size_t mode_data = 26;
    const std::uint32_t mode_length = be32(psd, mode_data);
    const std::size_t resources = mode_data + 4 + std::size_t(mode_length);
    if (resources + 4 > psd.size()) return psd;
    const std::uint32_t length = be32(psd, resources);
    const std::size_t end = resources + 4 + std::size_t(length);
    if (end > psd.size()) return psd;
    std::string block = "8BIM";
    block += '\x04';
    block += '\x24';  // (1060)
    block += std::string(2, '\0');  // (no name: an empty Pascal string, padded)
    put32(block, static_cast<std::uint32_t>(xmp.size()));
    block += xmp;
    if (xmp.size() % 2) block += '\0';
    psd.insert(end, block);
    std::string sized;
    put32(sized, static_cast<std::uint32_t>(length + block.size()));
    psd.replace(resources, 4, sized);
    return psd;
}

}  // namespace genko::formats
