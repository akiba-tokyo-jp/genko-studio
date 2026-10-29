// PC5 I-d: a text file of data sets and data sets as files (formats/datasets.hpp) — PhotoCraft v0.2.0's
// engine/src/variables_cmds.rs (270-352: import_data_sets and split_csv, 354-395: export_as_files), MIT
// (native/third_party/photocraft/NOTICE.md).

#include "formats/datasets.hpp"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <map>
#include <set>

#include "core/base64.hpp"
#include "core/command_bus.hpp"
#include "core/covers.hpp"
#include "core/error.hpp"
#include "core/paths.hpp"
#include "core/pyconv.hpp"
#include "render/ops_layer_comps.hpp"
#include "render/ops_registry.hpp"
#include "render/smart_decode.hpp"

namespace genko::formats {

namespace {

namespace fs = std::filesystem;
using core::Json;

constexpr std::size_t kCsvMaxBytes = 64ull << 20;

[[noreturn]] void bad(const std::string& message) { throw core::Error("value", message); }

std::string lower(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return text;
}

std::string trimmed(const std::string& text) {
    const auto first = text.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return std::string();
    return text.substr(first, text.find_last_not_of(" \t\r\n") - first + 1);
}

bool shown(const std::string& cell) {
    const std::string v = lower(trimmed(cell));
    return v == "true" || v == "1" || v == "yes" || v == "on" || v == "visible" || v == "表示";
}

// A picture's file: its bytes (at most 256 MiB), read from `folder` when its path is relative.
std::string picture_file(const std::string& path, const fs::path& folder, std::size_t row) {
    fs::path file = core::path_from_utf8(trimmed(path));
    if (file.is_relative()) file = folder / file;
    std::error_code ec;
    const auto size = fs::is_regular_file(file, ec) ? fs::file_size(file, ec) : 0;
    if (ec || size == 0) bad("row " + std::to_string(row) + ": the picture " + path + " cannot be read");
    if (size > render::smart::kFileMaxBytes) bad("row " + std::to_string(row) + ": the picture " + path + " is larger than 256 MiB");
    std::ifstream in(file, std::ios::binary);
    std::string bytes(static_cast<std::size_t>(size), '\0');
    if (!in.read(bytes.data(), static_cast<std::streamsize>(bytes.size()))) bad("row " + std::to_string(row) + ": the picture " + path + " cannot be read");
    return bytes;
}

}  // namespace

std::vector<std::vector<std::string>> csv_rows(std::string_view text, char delimiter) {
    std::vector<std::vector<std::string>> rows;
    std::vector<std::string> row;
    std::string field;
    bool quoted = false;
    bool any = false;  // (the line holds something: a field or a delimiter)
    for (std::size_t i = 0; i < text.size(); ++i) {
        const char c = text[i];
        if (quoted) {
            if (c == '"') {
                if (i + 1 < text.size() && text[i + 1] == '"') {
                    field += '"';
                    ++i;
                } else {
                    quoted = false;
                }
            } else {
                field += c;
            }
            continue;
        }
        if (c == '"') {
            quoted = true;
            any = true;
        } else if (c == delimiter) {
            row.push_back(std::move(field));
            field.clear();
            any = true;
        } else if (c == '\r' || c == '\n') {
            if (c == '\r' && i + 1 < text.size() && text[i + 1] == '\n') ++i;
            if (any || !field.empty()) {
                row.push_back(std::move(field));
                rows.push_back(std::move(row));
            }
            row.clear();
            field.clear();
            any = false;
        } else {
            field += c;
        }
    }
    if (quoted) bad("a quoted field is not closed");
    if (any || !field.empty()) {
        row.push_back(std::move(field));
        rows.push_back(std::move(row));
    }
    // (lines of nothing but blanks are no data sets)
    std::erase_if(rows, [](const std::vector<std::string>& r) {
        return std::all_of(r.begin(), r.end(), [](const std::string& f) { return trimmed(f).empty(); });
    });
    return rows;
}

CsvRead read_data_sets_csv(std::string_view text, const core::Variables& variables, const CsvOptions& options) {
    if (text.size() > kCsvMaxBytes) bad("the text file is larger than 64 MiB");
    if (text.starts_with("\xEF\xBB\xBF")) text.remove_prefix(3);
    if (core::utf8_error(text)) bad("the text file is not UTF-8 (save it as UTF-8 — CSV UTF-8)");
    char delimiter = options.delimiter;
    if (delimiter == 0) {
        const std::string_view first = text.substr(0, text.find_first_of("\r\n"));
        delimiter = first.find('\t') != std::string_view::npos && first.find(',') == std::string_view::npos ? '\t' : ',';
    }
    const auto rows = csv_rows(text, delimiter);
    if (rows.empty()) bad("the text file holds no line of names");
    if (rows.size() - 1 > core::kDataSetsMax) bad("the text file holds more than " + std::to_string(core::kDataSetsMax) + " data sets");
    const std::vector<std::string>& header = rows.front();
    const std::string head = lower(trimmed(header.empty() ? std::string() : header.front()));
    const bool named = head.empty() || head == "dataset" || head == "data set" || head == "データセット";
    CsvRead out;
    std::vector<const core::VariableDef*> columns(header.size(), nullptr);
    std::vector<std::string> unknown;
    for (std::size_t c = named ? 1 : 0; c < header.size(); ++c) {
        columns[c] = core::find_def(variables, trimmed(header[c]));
        if (columns[c] == nullptr && !trimmed(header[c]).empty()) unknown.push_back(trimmed(header[c]));
    }
    if (!unknown.empty()) {
        std::string listed;
        for (const std::string& name : unknown) listed += (listed.empty() ? "" : "、") + name;
        out.notes.push_back("変数の無い列を外しました: " + listed);
    }
    std::set<std::string> names;
    for (std::size_t r = 1; r < rows.size(); ++r) {
        const std::vector<std::string>& cells = rows[r];
        std::string name = named && !cells.empty() ? trimmed(cells.front()) : std::string();
        if (name.empty()) name = "データセット " + std::to_string(r);
        if (!names.insert(name).second) bad("two data sets are named " + name);
        Json values = Json::array();
        for (std::size_t c = 0; c < columns.size() && c < cells.size(); ++c) {
            const core::VariableDef* def = columns[c];
            if (def == nullptr) continue;
            const std::string& cell = cells[c];
            if (def->type == "visibility") {
                if (trimmed(cell).empty()) continue;
                values.push_back(Json{{"variable", def->name}, {"value", shown(cell)}});
            } else if (def->type == "text") {
                if (cell.empty()) continue;
                values.push_back(Json{{"variable", def->name}, {"value", cell}});
            } else {
                if (trimmed(cell).empty()) continue;
                const std::string bytes = picture_file(cell, options.folder, r + 1);
                const std::string file = core::path_to_utf8(core::path_from_utf8(trimmed(cell)).filename());
                values.push_back(Json{{"variable", def->name}, {"value", Json{{"data", core::b64encode(bytes)}, {"name", file}}}});
            }
        }
        out.data_sets.push_back(Json{{"name", name}, {"values", std::move(values)}});
    }
    return out;
}

std::vector<std::size_t> data_set_pages(const core::Document& doc) {
    if (doc.pages.size() == 1) return {0};
    std::set<std::size_t> at;
    for (const core::VariableDef& def : core::variables_of(doc).defs) {
        if (def.line) {
            for (const core::StoryLine& line : doc.story) {
                if (line.id != *def.line) continue;
                for (std::size_t i = 0; i < doc.pages.size(); ++i) {
                    if (doc.pages[i]->index == line.page_index) at.insert(i);
                }
            }
            continue;
        }
        for (std::size_t i = 0; i < doc.pages.size(); ++i) {
            if (def.page && doc.pages[i]->id == *def.page) at.insert(i);
        }
    }
    return {at.begin(), at.end()};
}

std::vector<CopyResult> export_data_sets(const core::Document& doc, const fs::path& folder, const DataSetFiles& files) {
    const core::Variables variables = core::variables_of(doc);
    if (variables.data_sets.empty()) bad("the book has no data sets");
    for (const std::string& name : files.data_sets) {
        if (!core::find_data_set(variables, name)) bad("no data set " + name);
    }
    const std::vector<std::size_t> pages = files.pages.empty() ? data_set_pages(doc) : files.pages;
    if (pages.empty()) bad("no page to write: the variables name no page of the book");
    for (const std::size_t at : pages) {
        if (at >= doc.pages.size()) bad("no such page");
        if (doc.is_deferred(at)) throw core::Error("page_not_loaded", "the page is not read yet");
    }
    const std::string title = render::layer_comps::file_part(doc.title.empty() ? std::string("無題") : doc.title);
    static const core::CommandBus bus(render::ops_registry());
    core::Document free = doc;  // (a copy never saved: what locks and gates keep from changing is the book, not its files)
    free.page_locks = Json::object();
    free.strict_gates = false;
    std::vector<CopyResult> out;
    std::map<std::string, int> seen;
    for (std::size_t i = 0; i < variables.data_sets.size(); ++i) {
        const core::DataSet& set = variables.data_sets[i];
        if (!files.data_sets.empty() && std::find(files.data_sets.begin(), files.data_sets.end(), set.name) == files.data_sets.end()) continue;
        char number[32];
        std::snprintf(number, sizeof number, "%03zu", i + 1);
        std::string stem = files.naming.empty() ? std::string("{name}") : files.naming;
        const auto put = [&stem](std::string_view key, const std::string& value) {
            for (std::size_t at = stem.find(key); at != std::string::npos; at = stem.find(key, at + value.size())) stem.replace(at, key.size(), value);
        };
        put("{name}", render::layer_comps::file_part(set.name));
        put("{index}", number);
        put("{document}", title);
        stem = render::layer_comps::file_part(stem);
        if (stem.size() > 200) {  // (a file system takes 255 bytes a name: cut by whole characters)
            stem.resize(200);
            while (!stem.empty() && (static_cast<unsigned char>(stem.back()) & 0xC0) == 0x80) stem.pop_back();
            if (!stem.empty() && (static_cast<unsigned char>(stem.back()) & 0x80) != 0) stem.pop_back();
        }
        if (const int n = ++seen[stem]; n > 1) stem += "-" + std::to_string(n);
        core::ApplyResult applied;
        try {
            applied = bus.apply(free, Json::array({Json{{"op", "apply_data_set"}, {"index", i}}}), files.actor);
        } catch (const core::ApplyError& error) {
            bad(error.what());
        }
        for (const std::size_t at : pages) {
            std::string name = stem;
            if (pages.size() > 1) name += "_" + core::file_stem(applied.doc.page(at));
            const fs::path dest = folder / core::path_from_utf8(name + "." + files.extension);
            if (files.check) files.check(dest);
            out.push_back(write_copy(applied.doc, at, dest, files.options));  // (one data set's book at a time)
        }
    }
    if (out.empty()) bad("no data sets chosen");
    return out;
}

}  // namespace genko::formats
