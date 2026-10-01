#include "core/ids.hpp"

#include <QRandomGenerator>

#include <array>
#include <memory>
#include <utility>

namespace genko::core {

namespace {

constexpr char kHex[] = "0123456789abcdef";

ScopedIdSource::Source& test_source() {
    static ScopedIdSource::Source source;
    return source;
}

std::string hex_bits(std::uint64_t bits, int digits) {
    std::string out(static_cast<std::size_t>(digits), '0');
    for (int i = digits - 1; i >= 0; --i) {
        out[static_cast<std::size_t>(i)] = kHex[bits & 0xF];
        bits >>= 4;
    }
    return out;
}

}  // namespace

std::string new_id() {
    if (const auto& source = test_source()) return source();
    // The first 48 bits of a version 4 UUID are all random (its version digit comes after them).
    return hex_bits(QRandomGenerator::system()->generate64() >> 16, 12);
}

std::string new_page_id() { return "pg_" + new_id(); }

std::string new_book_id() {
    std::array<quint32, 4> words{};
    QRandomGenerator::system()->fillRange(words.data(), static_cast<qsizetype>(words.size()));
    std::array<unsigned char, 16> bytes{};
    for (std::size_t i = 0; i < 16; ++i) bytes[i] = static_cast<unsigned char>(words[i / 4] >> (8 * (i % 4)));
    bytes[6] = static_cast<unsigned char>((bytes[6] & 0x0F) | 0x40);  // version 4
    bytes[8] = static_cast<unsigned char>((bytes[8] & 0x3F) | 0x80);  // RFC 4122 variant
    std::string out;
    out.reserve(32);
    for (const unsigned char b : bytes) {
        out += kHex[b >> 4];
        out += kHex[b & 0xF];
    }
    return out;
}

std::string new_txn_id() { return new_book_id(); }

bool is_book_id(std::string_view text) {
    if (text.size() != 32) return false;
    for (const char c : text) {
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
    }
    return true;
}

ScopedIdSource::ScopedIdSource(Source source) : previous_(std::exchange(test_source(), std::move(source))) {}

ScopedIdSource::~ScopedIdSource() { test_source() = std::move(previous_); }

ScopedIdSource::Source counting_ids(std::uint64_t first) {
    auto next = std::make_shared<std::uint64_t>(first);
    return [next]() { return hex_bits((*next)++, 12); };
}

}  // namespace genko::core
