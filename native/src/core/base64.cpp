#include "core/base64.hpp"

#include <array>
#include <string>

#include "core/error.hpp"

namespace genko::core {

namespace {

constexpr char kAlphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

constexpr std::array<unsigned char, 256> make_table() {
    std::array<unsigned char, 256> table{};
    for (auto& v : table) v = 0xFF;
    for (unsigned char i = 0; i < 64; ++i) table[static_cast<unsigned char>(kAlphabet[i])] = i;
    return table;
}

constexpr std::array<unsigned char, 256> kTable = make_table();

}  // namespace

std::string b64encode(std::string_view bytes) {
    std::string out;
    out.reserve((bytes.size() + 2) / 3 * 4);
    std::size_t i = 0;
    for (; i + 3 <= bytes.size(); i += 3) {
        const unsigned v = (static_cast<unsigned char>(bytes[i]) << 16) | (static_cast<unsigned char>(bytes[i + 1]) << 8) |
                           static_cast<unsigned char>(bytes[i + 2]);
        out += kAlphabet[(v >> 18) & 63];
        out += kAlphabet[(v >> 12) & 63];
        out += kAlphabet[(v >> 6) & 63];
        out += kAlphabet[v & 63];
    }
    const std::size_t rest = bytes.size() - i;
    if (rest == 1) {
        const unsigned v = static_cast<unsigned char>(bytes[i]) << 16;
        out += kAlphabet[(v >> 18) & 63];
        out += kAlphabet[(v >> 12) & 63];
        out += "==";
    } else if (rest == 2) {
        const unsigned v = (static_cast<unsigned char>(bytes[i]) << 16) | (static_cast<unsigned char>(bytes[i + 1]) << 8);
        out += kAlphabet[(v >> 18) & 63];
        out += kAlphabet[(v >> 12) & 63];
        out += kAlphabet[(v >> 6) & 63];
        out += '=';
    }
    return out;
}

std::string a2b_base64(std::string_view text) {
    // CPython 3.12 binascii_a2b_base64_impl with strict_mode=False.
    for (const char c : text) {
        if (static_cast<unsigned char>(c) >= 0x80) throw Error("format", "string argument should contain only ASCII characters");
    }
    std::string out;
    out.reserve(text.size() / 4 * 3 + 3);
    int quad_pos = 0;
    unsigned char left = 0;
    int pads = 0;
    for (const char ch : text) {
        if (ch == '=') {
            if (quad_pos >= 2 && quad_pos + ++pads >= 4) return out;  // a complete padding ends the data
            continue;
        }
        const unsigned char v = kTable[static_cast<unsigned char>(ch)];
        if (v >= 64) continue;
        pads = 0;
        switch (quad_pos) {
            case 0:
                quad_pos = 1;
                left = v;
                break;
            case 1:
                quad_pos = 2;
                out += static_cast<char>((left << 2) | (v >> 4));
                left = v & 0x0F;
                break;
            case 2:
                quad_pos = 3;
                out += static_cast<char>((left << 4) | (v >> 2));
                left = v & 0x03;
                break;
            default:
                quad_pos = 0;
                out += static_cast<char>((left << 6) | v);
                left = 0;
                break;
        }
    }
    if (quad_pos == 1) {
        throw Error("format", "Invalid base64-encoded string: number of data characters (" +
                                  std::to_string(out.size() / 3 * 4 + 1) + ") cannot be 1 more than a multiple of 4");
    }
    if (quad_pos != 0) throw Error("format", "Incorrect padding");
    return out;
}

}  // namespace genko::core
