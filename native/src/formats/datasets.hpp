#pragma once

#include <cstddef>
#include <filesystem>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

#include "core/actor.hpp"
#include "core/json.hpp"
#include "core/model.hpp"
#include "core/variables.hpp"
#include "formats/copy.hpp"

// PC5 I-d (variables@1, SPEC.md AUTO-01「変数とデータセット」): the files of the book's data sets — read and written by the window
// and the command line, never by an op (ops never read a path) — after PhotoCraft v0.2.0's engine/src/variables_cmds.rs
// (270-352: the data sets of a text file, 354-395: one file a data set), MIT (native/third_party/photocraft/NOTICE.md).

namespace genko::formats {

// ファイル → 読み込み → 変数データセット… (file.import.variableDataSets): a text file of data sets — UTF-8 (a BOM taken off),
// its fields split by a comma or a tab ("" quotes a field, in which "" is a quote and a line may break), its first line the
// variables' names (a first column headed by nothing or by DataSet / データセット names each data set), each line after it
// one data set (named データセット N otherwise): a visibility's true (true, 1, yes, on, visible, 表示) or false (anything
// else), a text's words as they are, a picture's file — a path, relative to the text file's folder — read here (at most
// 256 MiB) and given as its bytes — each file read once and each picture's bytes given once (the next naming it gives its asset
// ref), all of them together within the book's 4 GiB, refused before a file past it is read. An empty field gives the variable no value; a column of a name the book has no variable
// of is left out (said). The result: set_data_sets's data_sets, ready to be applied. core::Error("value") for a file that
// is not one (not UTF-8, larger than 64 MiB, more than kDataSetsMax lines, two data sets of one name, a picture that cannot
// be read).
struct CsvOptions {
    char delimiter = 0;              // ',' or '\t'; 0: a tab when the first line has tabs and no commas, else a comma
    std::filesystem::path folder;    // where a picture's relative path is read from (the text file's folder)
};
struct CsvRead {
    core::Json data_sets = core::Json::array();
    std::vector<std::string> notes;  // what was left out, in Japanese
    std::size_t files_read = 0;      // (the pictures' files read: each once)
};
CsvRead read_data_sets_csv(std::string_view text, const core::Variables& variables, const CsvOptions& options);
// The fields of a text file's lines (the parser read_data_sets_csv uses), each line a list of its fields.
std::vector<std::vector<std::string>> csv_rows(std::string_view text, char delimiter);

// ファイル → 書き出し → データセットからファイル… (file.export.dataSetsAsFiles): each data set — or those chosen — applied to a
// copy of the book (apply_data_set as `actor` on the copy, its page locks and strict_gates let go: the copy is never saved,
// the book itself unchanged), and the pages chosen written each as one file by
// write_copy: <naming>.<extension> in `folder` (naming's {name} the data set's name, {index} its place from 001, {document}
// the book's title; a path's and a file system's marks made "_"; a page's file stem — p001 — added after "_" when more than
// one page is written; a name met again given -2, -3 …). The files written before one that fails stay. core::Error("value")
// for a book without data sets, a name it has not, or none chosen; the op's refusal as core::Error("value").
struct DataSetFiles {
    std::string extension = "png";           // one of copy_extensions()
    std::string naming = "{name}";
    std::vector<std::string> data_sets;      // none: every one
    std::vector<std::size_t> pages;          // positions; none: the pages the variables are on (a book of one page: it)
    CopyOptions options;
    core::Actor actor;
    // (the coordinator's P3-8) each file's place before it is written: throws to write it and those after it not
    std::function<void(const std::filesystem::path&)> check;
};
std::vector<CopyResult> export_data_sets(const core::Document& doc, const std::filesystem::path& folder, const DataSetFiles& files);
// The pages export_data_sets writes when none are chosen (positions, in the book's order).
std::vector<std::size_t> data_set_pages(const core::Document& doc);

}  // namespace genko::formats
