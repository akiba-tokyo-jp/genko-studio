// fonts@1 (core/font_catalog.hpp): the catalogue read from the embedded FONTS.json, and the feature's walk of a book.

#include "core/font_catalog.hpp"

#include <algorithm>
#include <cstddef>
#include <map>
#include <set>

#include "core/error.hpp"
#include "core/smart.hpp"

namespace genko::core::detail {
extern const unsigned char kFontCatalogJson[];
extern const std::size_t kFontCatalogJsonSize;
}  // namespace genko::core::detail

namespace genko::core::fonts {

namespace {

// The first code point of a UTF-8 string (0: none, or not UTF-8).
char32_t first_code_point(const std::string& s) {
    if (s.empty()) return 0;
    const auto c = static_cast<unsigned char>(s[0]);
    std::size_t n = 0;
    char32_t cp = 0;
    if (c < 0x80) {
        return c;
    } else if ((c >> 5) == 6) {
        n = 2;
        cp = c & 0x1f;
    } else if ((c >> 4) == 14) {
        n = 3;
        cp = c & 0x0f;
    } else if ((c >> 3) == 30) {
        n = 4;
        cp = c & 0x07;
    } else {
        return 0;
    }
    if (s.size() < n) return 0;
    for (std::size_t k = 1; k < n; ++k) {
        const auto d = static_cast<unsigned char>(s[k]);
        if ((d >> 6) != 2) return 0;
        cp = (cp << 6) | (d & 0x3f);
    }
    return cp;
}

std::string text_at(const Json& object, const char* key) {
    const auto it = object.find(key);
    return it != object.end() && it->is_string() ? it->get<std::string>() : std::string();
}

std::int64_t int_at(const Json& object, const char* key) {
    const auto it = object.find(key);
    return it != object.end() && it->is_number_integer() ? it->get<std::int64_t>() : 0;
}

bool flag_at(const Json& object, const char* key, bool otherwise) {
    const auto it = object.find(key);
    return it != object.end() && it->is_boolean() ? it->get<bool>() : otherwise;
}

struct Catalog {
    std::vector<Entry> entries;
    std::vector<Category> categories;
    std::map<std::string, std::size_t, std::less<>> by_key;
    std::map<std::string, std::size_t, std::less<>> by_postscript;  // (the added faces only)
};

const Catalog& table() {
    static const Catalog made = [] {
        Catalog out;
        const Json& all = catalog();
        for (const Json& c : all.at("categories")) out.categories.push_back(Category{text_at(c, "id"), text_at(c, "label_ja")});
        for (const Json& f : all.at("fonts")) {
            Entry e;
            e.key = text_at(f, "key");
            e.file = text_at(f, "file");
            e.added = text_at(f, "storage") == "file";
            e.label = text_at(f, "name_ja");
            e.category = text_at(f, "category");
            e.use = text_at(f, "use_ja");
            e.family = text_at(f, "family");
            e.style = text_at(f, "style");
            e.postscript = text_at(f, "postscript_name");
            e.weight = int_at(f, "weight");
            e.bytes = int_at(f, "bytes");
            e.sha256 = text_at(f, "sha256");
            e.kanji = text_at(f, "kanji");
            e.kana_complete = flag_at(f, "kana_complete", true);
            e.fallback = text_at(f, "fallback");
            if (const auto look = f.find("lookalike"); look != f.end() && look->is_object()) {
                for (const auto& [from, to] : look->items()) {
                    const char32_t a = first_code_point(from);
                    const char32_t b = to.is_string() ? first_code_point(to.get<std::string>()) : 0;
                    if (a != 0 && b != 0) e.lookalike.emplace_back(a, b);
                }
            }
            if (const auto vertical = f.find("vertical"); vertical != f.end() && vertical->is_object()) {
                e.vertical = flag_at(*vertical, "supported", true);
                e.presentation_forms = flag_at(*vertical, "presentation_forms", true);
            }
            if (e.key.empty() || e.file.empty()) throw Error("value", "the font catalogue has a font without a key or a file");
            if (!out.by_key.emplace(e.key, out.entries.size()).second) throw Error("value", "the font catalogue names " + e.key + " twice");
            if (e.added && !e.postscript.empty()) out.by_postscript.emplace(e.postscript, out.entries.size());
            out.entries.push_back(std::move(e));
        }
        return out;
    }();
    return made;
}

// Every string under a key "font" anywhere in a value (a record's character attributes, its ranges', a style's).
template <typename Fn>
void fonts_in(const Json& value, Fn& note) {
    if (value.is_object()) {
        for (const auto& [key, item] : value.items()) {
            if (key == "font" && item.is_string()) note(item.get_ref<const std::string&>());
            else fonts_in(item, note);
        }
    } else if (value.is_array()) {
        for (const Json& item : value) fonts_in(item, note);
    }
}

template <typename Fn>
void fonts_of(const StoryLine& line, Fn& note) {
    if (line.style.is_object()) {
        if (const auto font = line.style.find("font"); font != line.style.end() && font->is_string()) note(font->get_ref<const std::string&>());
    }
    if (line.ruby_spans) {
        for (const RubySpan& span : *line.ruby_spans) {
            if (span.font) note(*span.font);
        }
    }
    if (line.native_type) fonts_in(*line.native_type, note);
}

}  // namespace

std::string_view catalog_text() {
    return {reinterpret_cast<const char*>(core::detail::kFontCatalogJson), core::detail::kFontCatalogJsonSize};
}

const Json& catalog() {
    static const Json parsed = parse_python_json(catalog_text());
    return parsed;
}

const std::vector<Entry>& entries() { return table().entries; }

const std::vector<Category>& categories() { return table().categories; }

const Entry* find(std::string_view key) {
    const Catalog& t = table();
    const auto it = t.by_key.find(key);
    return it == t.by_key.end() ? nullptr : &t.entries[it->second];
}

bool added(std::string_view key) {
    const Entry* e = find(key);
    return e != nullptr && e->added;
}

const Entry* by_postscript(std::string_view name) {
    const Catalog& t = table();
    const auto it = t.by_postscript.find(name);
    return it == t.by_postscript.end() ? nullptr : &t.entries[it->second];
}

std::vector<std::string> named(const Document& doc) {
    std::set<std::string> seen;
    auto note = [&seen](const std::string& font) {
        if (added(font)) seen.insert(font);
    };
    for (const StoryLine& line : doc.story) fonts_of(line, note);
    for (const smart::WrittenPage& written : smart::written_pages(doc)) {
        for (const Layer& layer : written.page->layers) {
            if (layer.type) fonts_of(*layer.type, note);
        }
    }
    if (doc.type_styles) fonts_in(*doc.type_styles, note);
    note(doc.font_path);
    return {seen.begin(), seen.end()};
}

bool uses_feature(const Document& doc) { return !named(doc).empty(); }

std::vector<std::string> named_unannounced(const Document& doc) {
    if (std::find(doc.features.begin(), doc.features.end(), std::string(kFontsFeature)) != doc.features.end()) return {};
    return named(doc);
}

}  // namespace genko::core::fonts
