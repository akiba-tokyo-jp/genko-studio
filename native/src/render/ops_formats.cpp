// PC4 節 F (SPEC.md FILE-01): the ops of the files' group — registered here; each group writes its op's work in this file
// (F-b File Info, F-e a RAW smart object's development).

#include "render/ops_formats.hpp"

#include <algorithm>
#include <string>

#include "core/file_info.hpp"
#include "core/pyconv.hpp"
#include "core/pyops.hpp"

namespace genko::render {

namespace {

using core::Json;

constexpr std::string_view kFileInfoUsage =
    "set_file_info takes {title?: str, author?: str, author_title?: str, description?: str, keywords?: [str] | \"a; b\", copyright?: str, "
    "copyright_status?: \"unknown\" | \"copyrighted\" | \"public_domain\", copyright_url?: str}";

[[noreturn]] void refuse(const std::string& message) { throw core::OpError(message + " (" + std::string(kFileInfoUsage) + ")"); }

std::string trimmed(const std::string& text) {
    const auto space = [](unsigned char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; };
    std::size_t a = 0, b = text.size();
    while (a < b && space(static_cast<unsigned char>(text[a]))) ++a;
    while (b > a && space(static_cast<unsigned char>(text[b - 1]))) --b;
    return text.substr(a, b - a);
}

std::string text_at(const Json& value, const char* key) {
    if (value.is_null()) return {};
    if (!value.is_string()) refuse(std::string(key) + " is text");
    return trimmed(value.get<std::string>());
}

// "a; b, c" or ["a", "b"] as the keywords (trimmed, the empty ones left out, each once in its first place).
std::vector<std::string> keywords_at(const Json& value) {
    std::vector<std::string> out;
    const auto add = [&out](const std::string& text) {
        std::size_t from = 0;
        for (std::size_t i = 0; i <= text.size(); ++i) {
            if (i == text.size() || text[i] == ';' || text[i] == ',') {
                const std::string item = trimmed(text.substr(from, i - from));
                if (!item.empty() && std::find(out.begin(), out.end(), item) == out.end()) out.push_back(item);
                from = i + 1;
            }
        }
    };
    if (value.is_null()) return out;
    if (value.is_string()) {
        add(value.get<std::string>());
    } else if (value.is_array()) {
        for (const Json& item : value) {
            if (!item.is_string()) refuse("keywords are text or a list of text");
            const std::string one = trimmed(item.get<std::string>());
            if (!one.empty() && std::find(out.begin(), out.end(), one) == out.end()) out.push_back(one);
        }
    } else {
        refuse("keywords are text or a list of text");
    }
    return out;
}

// ファイル情報… (file.fileInfo; PhotoCraft's file_cmds.rs file_info): the items given replace the book's (an empty one
// removes it), the others stay; the title is the book's (set_meta's). The XMP packet the book was opened with stays: its
// eight items are written from these when the book's files carry its XMP.
void set_file_info(core::OpContext& c) {
    static constexpr std::string_view kKeys[] = {"op", "title", "author", "author_title", "description", "keywords", "copyright",
                                                 "copyright_status", "copyright_url"};
    for (const auto& [key, value] : c.op.items()) {
        if (std::find(std::begin(kKeys), std::end(kKeys), key) == std::end(kKeys) && !key.starts_with("_")) refuse("unknown key " + core::py_repr_str(key));
    }
    core::FileInfo info = c.doc.file_info.value_or(core::FileInfo{});
    const auto item = [&](const char* key, std::string& into) {
        if (const Json* v = core::get(c.op, key)) into = text_at(*v, key);
    };
    std::optional<std::string> title;
    if (const Json* v = core::get(c.op, "title")) title = text_at(*v, "title");
    item("author", info.author);
    item("author_title", info.author_title);
    item("description", info.description);
    item("copyright", info.copyright);
    item("copyright_url", info.copyright_url);
    if (const Json* v = core::get(c.op, "keywords")) info.keywords = keywords_at(*v);
    if (const Json* v = core::get(c.op, "copyright_status")) {
        const std::string status = text_at(*v, "copyright_status");
        if (!status.empty() && status != "unknown" && status != "copyrighted" && status != "public_domain")
            refuse("copyright_status is unknown, copyrighted or public_domain");
        info.copyright_status = status == "unknown" ? std::string() : status;  // (unknown: not said)
    }
    const auto long_text = [](const std::string& text) {
        std::size_t n = 0;
        for (const char ch : text) n += (static_cast<unsigned char>(ch) & 0xC0) != 0x80 ? 1 : 0;
        return n > core::kFileInfoTextMax;
    };
    for (const std::string* text : {&info.author, &info.author_title, &info.description, &info.copyright, &info.copyright_url}) {
        if (long_text(*text)) refuse("an item is longer than 2000 characters");
    }
    if (title && long_text(*title)) refuse("the title is longer than 2000 characters");
    if (info.keywords.size() > core::kKeywordsMax) refuse("at most 200 keywords");
    for (const std::string& keyword : info.keywords) {
        if (long_text(keyword)) refuse("a keyword is longer than 2000 characters");
    }
    core::check_file_info(info);
    if (title) c.doc.title = *title;
    if (info.empty()) {
        c.doc.file_info.reset();
    } else {
        c.doc.file_info = info;
    }
    c.report = Json{{"file_info", core::to_json(info)}, {"title", c.doc.title}};
}

}  // namespace

void register_format_ops(core::OpRegistry& registry) {
    // F-b: ファイル情報… ({"title"?, "author"?, "author_title"?, "description"?, "keywords"?: [str] | "a; b", "copyright"?,
    // "copyright_status"?: "unknown" | "copyrighted" | "public_domain", "copyright_url"?} → {"file_info", "title"}: the book's
    // file_info, file-info@1; the title is set_meta's)
    registry.add("set_file_info", set_file_info);
    // F-e: a RAW smart object's development ({"contents", "develop"?, "adjust"?} → {"contents", "raw", "size_px", "depth"}:
    // camera-raw@1; render/ops_camera_raw.cpp)
    registry.add("set_camera_raw", set_camera_raw);
}

}  // namespace genko::render
