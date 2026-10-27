#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// Photoshop's ActionDescriptor (the Adobe spec's "Descriptor structure": a class — an empty name, its class id — and its
// items, each a key, a type and a value), written and read for every PSD block made of one: a text layer's TySh and the
// layer effects' lfx2 (render/psd_text, PSD-01), the adjustment layers' CgEd, clrL, vibA and blwh (render/psd_adjust,
// PC2). One writer and one reader, as Photoshop writes them and psd-tools reads them.

namespace genko::render::psd::descriptor {

// Big-endian numbers, as a PSD holds them.
void put_u8(std::string& out, unsigned v);
void put_u16(std::string& out, std::uint32_t v);
void put_u32(std::string& out, std::uint32_t v);
void put_i32(std::string& out, std::int32_t v);
void put_f64(std::string& out, double v);
// A key or a class or enum id: 4 characters as an id (length 0), longer ones as a length and the characters — and
// "warp", which Photoshop writes as a string.
void put_id(std::string& out, std::string_view id);
// A unicode string: its length (with the closing 0) and its UTF-16 characters.
void put_unicode(std::string& out, std::u16string_view text);

// A descriptor written: its items in the order given.
class Writer {
public:
    void text(std::string_view key, std::u16string_view value);
    void enumerated(std::string_view key, std::string_view type, std::string_view value);
    void integer(std::string_view key, std::int32_t value);
    void number(std::string_view key, double value);
    void unit(std::string_view key, std::string_view unit, double value);
    void boolean(std::string_view key, bool value);
    void object(std::string_view key, const Writer& value, std::string_view class_id);
    void raw(std::string_view key, std::string_view bytes);
    // (PC3 A3) a list of doubles (VlLs of doub: a placed layer's Trnf), and an object array whose items are lists of
    // unit floats (ObAr: a custom warp's meshPoints) — `count` the items of each list.
    void numbers(std::string_view key, const std::vector<double>& values);
    void object_array(std::string_view key, std::uint32_t count, std::string_view class_id,
                      const std::vector<std::pair<std::string, std::vector<double>>>& lists, std::string_view unit);
    // (PC3 C-b) a list of objects (VlLs of Objc: a gradient's stops, a shape's originations), each its class id and its
    // items; and a list of unit floats (VlLs of UntF: a shape stroke's dashes).
    void objects(std::string_view key, const std::vector<std::pair<std::string, Writer>>& items);
    void units(std::string_view key, std::string_view unit, const std::vector<double>& values);
    // The class structure (an empty name, the class id), its count and items.
    std::string structure(std::string_view class_id) const;
    // A versioned descriptor: version 16, then the structure.
    std::string versioned(std::string_view class_id) const;

private:
    void head(std::string_view key, std::string_view type);
    std::string body_;
    std::uint32_t count_ = 0;
};

// What a reader could not read (the words say what: "it ends early", "descriptors nested too deep", …).
struct Broken : std::runtime_error {
    using std::runtime_error::runtime_error;
};
// (PC4 E2, review P1-1) The budget below spent: a Broken, told apart by a reader that refuses the whole file then.
struct TooLarge : Broken {
    using Broken::Broken;
};

// (PC4 E4, review P1-1: design 4.3's "ほどいた標本は合わせて 512 MiB まで") What a descriptor read may take in memory: its
// values, items, texts and lists counted as they are made (a list's slots twice, as a vector grows) — a file of a few
// bytes a value cannot make some fifty times more of them (a .grd, a PSD block, an ABR or PAT descriptor).
inline constexpr std::size_t kUnpackedBudget = std::size_t(512) << 20;

// What is read: bytes with a place, every read checked against their end (Broken), and what the values read take in
// memory against `budget` (Broken "too large" past it).
class Bytes {
public:
    explicit Bytes(std::string_view data, std::size_t budget = kUnpackedBudget) : data_(data), budget_(budget) {}
    // `n` bytes more kept of what is read: Broken when past the budget.
    void spend(std::size_t n) {
        if (n > budget_ - spent_) throw TooLarge("its descriptors are too large to read (more than " + std::to_string(budget_ >> 20) + " MB unpacked)");
        spent_ += n;
    }
    // (PC4 E2) What the values read have taken so far.
    std::size_t spent() const { return spent_; }
    std::size_t left() const { return data_.size() - at_; }
    std::size_t at() const { return at_; }
    std::string_view take(std::size_t n);
    std::uint32_t u8() { return static_cast<unsigned char>(take(1)[0]); }
    std::uint32_t u16();
    std::uint32_t u32();
    std::int32_t i32() { return static_cast<std::int32_t>(u32()); }
    double f64();

private:
    std::string_view data_;
    std::size_t at_ = 0;
    std::size_t budget_ = kUnpackedBudget;
    std::size_t spent_ = 0;
};

// A descriptor read back: each item's value by its type.
struct Object;
struct Value {
    std::string type;
    std::u16string text;              // TEXT
    std::int64_t integer = 0;         // long, comp
    double number = 0.0;              // doub, UntF; UnFl: its first value
    std::string unit;                 // UntF, UnFl
    std::vector<double> numbers;      // UnFl (a unit and a list of doubles)
    bool boolean = false;             // bool
    std::string enum_type;            // enum
    std::string enum_value;           // enum; type and GlbC: the class id
    std::shared_ptr<Object> object;   // Objc, GlbO, ObAr (its lists of unit floats)
    std::string raw;                  // tdta, alis, Pth
    std::vector<Value> list;          // VlLs
    // A number of any of the number types (long, comp, doub, UntF, UnFl); none for another type.
    std::optional<double> numeric() const;
};
struct Object {
    std::string class_id;
    std::vector<std::pair<std::string, Value>> items;
    // The first item of the key, or none.
    const Value* get(std::string_view key) const;
};

std::string read_id(Bytes& b);
// (the closing 0s taken off)
std::u16string read_unicode(Bytes& b);
// A class structure at the reader's place (nested ones read too, to a depth of 32).
Object read_structure(Bytes& b, int depth = 0);
// A versioned descriptor (version 16) at `at` of `payload`.
Object read_versioned(std::string_view payload, std::size_t at = 0);
// (PC4 E2, review P1-1) The same within `budget` (shared with the rest of a file), what it took spent from it; TooLarge
// past it.
Object read_versioned(std::string_view payload, std::size_t at, std::size_t& budget);

}  // namespace genko::render::psd::descriptor

namespace genko::render::psd {

// UTF-8 ↔ UTF-16 (the PSD's text: a descriptor's, the text engine's).
std::u16string utf16(std::string_view utf8);
std::string utf8_of(std::u16string_view text);

}  // namespace genko::render::psd
