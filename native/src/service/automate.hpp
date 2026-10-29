#pragma once

#include <cstddef>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "core/actor.hpp"
#include "core/command_bus.hpp"
#include "core/error.hpp"
#include "core/json.hpp"
#include "core/model.hpp"
#include "formats/open_file.hpp"

// 自動処理の土台 (PC5 I-0, SPEC.md AUTO-01; PhotoCraft v0.2.0 engine/src/automate_cmds.rs and file_cmds.rs, MIT, read as
// Genko's ops): the steps a script, a batch, a droplet or a script event runs, and the runner that takes files one by one
// through them. Not an op (an op reads no path): the app's ファイル → 自動処理 ▸ / スクリプト ▸ and the command line's
// `genko script` and `genko batch` call it. The user's decision 5 (2026-10-28): the steps are ops — no language, no
// condition, no loop (a program outside calls the command line or MCP for those).
//
// The rules (SPEC.md AUTO-01「承認の決まり」, design 節 I 3.5): every change goes through core::CommandBus (the op checks,
// the page locks, strict_gates, the journal), as the person or agent that runs it — never round the approvals:
//   - the ops that approve or lock (service::actions::gate_op) and undo are never a step, wherever the steps come from;
//   - an actor that cannot approve (an agent, ai:…) runs only the ops MCP's apply_ops takes (service::agent_op), inside
//     for_pages too, and what a pseudo op stands for is checked the same;
//   - the files: read and written as the person that runs it (the app, the command line); MCP and HTTP have no way in.

namespace genko::service::automate {

namespace fs = std::filesystem;

// Steps that cannot be read or run as they are: core::Error with code "steps" and where ("line 3: …", "step 2: …").
class StepsError : public core::Error {
public:
    explicit StepsError(const std::string& message) : core::Error("steps", message) {}
};

// --- the steps --------------------------------------------------------------------------------------------------------

// What steps may be: the file read whole at most this big, this many steps (counted after for_pages is not expanded),
// JSON nested at most this deep.
inline constexpr std::size_t kMaxStepsBytes = 1u << 20;
inline constexpr std::size_t kMaxSteps = 10000;
inline constexpr int kMaxStepsDepth = 64;

// The steps of a script as op objects, from any of its three forms (a UTF-8 BOM skipped):
//   - a JSON array of ops, as `genko apply` takes them: [{"op": "add_layer", "page": 1, …}, …];
//   - a JSON object {"steps": [ …ops… ]} (a droplet's and a saved script's own);
//   - one op a line: `add_layer {"page": 1, "kind": "paint"}` — the op's name, then its arguments as one JSON object
//     (none: no arguments); blank lines and lines beginning with # or // are notes.
// StepsError ("line N: …" for the third form; "step N: …" for an item that is not an op object, an "op" that is not a
// name); NaN and Infinity refused as `genko apply` refuses them.
core::Json read_steps(std::string_view text);
// read_steps of a file, refused unread when it is bigger than kMaxStepsBytes (core::Error "not_found" when it is not there).
core::Json read_steps_file(const fs::path& file);

// Pseudo ops: steps that are not ops (never in `genko apply` nor the ops' list), turned into ops by the runner on the book
// as it is when their turn comes — fit_image {"page", "width_px", "height_px", "dont_enlarge"?, "resample"?} (画像を合わせる:
// resize_pages to fit the box, its shape kept), conditional_mode_change {"page", "from": [modes], "to"}
// (条件付きモード変更: set_color_mode when the page's mode is one of them; refused with not_yet_ported until PC5 I-e).
bool pseudo_op(std::string_view name);
std::vector<std::string> pseudo_op_names();
// The ops one pseudo op stands for on `doc` now (maybe none: the page fits already). core::OpError with the op's words.
core::Json expand_pseudo(const core::Document& doc, const core::Json& op);

// Why these steps cannot be run by `actor` (StepsError "step N: …"), checked before anything is run: an item that is not an
// op, a gate op or undo (anywhere, for_pages' ops too), more than kMaxSteps, and — for an actor that cannot approve —
// an op MCP's apply_ops does not take.
void check_steps(const core::Json& steps, const core::Actor& actor);

// The steps run on the book in memory as one change: the ops up to each pseudo op through the bus, the pseudo op turned
// into ops on the book as it is then (and checked as the steps are), and so on. The result as CommandBus::apply's (the
// ops applied; their warnings; their results, "index" counted over the ops as run; the journal's record). core::ApplyError
// as the bus throws it (nothing changed), StepsError from check_steps.
core::ApplyResult run_steps(const core::Document& doc, const core::Json& steps, const core::Actor& actor, bool dry_run = false);

// The steps aimed at the pages of `doc` the runner takes them to: each step once per page (for_pages' rule: "body" — the
// pages but the covers — for a book, every page for a picture opened as a book), its "page" that page. `recorded`: the
// steps are a recorded action (オートアクション), played back on each page as the app plays one (service::actions::replay:
// the page bound steps left out, new ids for what it makes, `layer_id` the layer drawn on — for a picture its top layer).
core::Json aim_steps(const core::Document& doc, const core::Json& steps, bool picture, bool recorded);

// The journal's name of a script, a batch, a script event: at most this many characters (core::Error("value") past it).
inline constexpr std::size_t kMaxScriptName = 200;
void check_script_name(const std::string& name);

// --- the books --------------------------------------------------------------------------------------------------------

// An agent's book (Python's _is_studio_project: strict gates, the studio's state, or its drafts folder): an unnamed caller
// of the command line is not trusted there as a person (`genko apply`, `genko script`, `genko batch` alike: legacy:unknown).
bool studio_book(const fs::path& dir);
// A v1–v3 book is never written: storage::NeedsMigration before anything in it is touched (no project.lock made).
void refuse_legacy(const fs::path& dir);

// --- the inputs -------------------------------------------------------------------------------------------------------

struct Input {
    fs::path path;
    bool book = false;  // a .genko book's folder (it holds project.json); else a file opened as a book of one page
};

struct InputOptions {
    std::vector<std::string> extensions;  // lower case, no dot; empty: every file this build opens (input_extensions())
    bool recurse = false;                 // the folders inside a folder given too (a book's folder is a book, never entered)
    std::size_t limit = 10000;            // more inputs than this: core::Error("value") before anything is run
    std::size_t scan_limit = 100000;      // more files and folders looked at than this: core::Error("value") (a folder of folders)
};

struct Inputs {
    std::vector<Input> items;
    core::Json skipped = core::Json::array();  // {"path", "reason"}: a path given that is not there, a hidden file …
};

// The files this build opens as a book of one page (formats::open_file): the codecs' formats, PSD and PSB, .pcraft and
// the camera RAW files.
const std::vector<std::string>& input_extensions();
// The inputs of these paths in order: a file as it is given (any extension), a book's folder, a folder's books and files
// whose extension is listed (sorted by name; hidden ones left out; the folders inside it with `recurse` — never a link to
// a folder, so no folder is entered twice and a loop of links ends).
Inputs list_inputs(const std::vector<fs::path>& given, const InputOptions& options = {});

// --- the outputs ------------------------------------------------------------------------------------------------------

struct Output {
    // None: nothing written (the steps tried); Book: a book written in place (a picture never: its file is not written);
    // Folder: each page a file in `folder` (コピーを保存, formats::write_copy).
    enum class To { None, Book, Folder };
    To to = To::Folder;
    fs::path folder;
    std::string format = "same";  // same (the input's own when write_copy writes it, else png) | an extension write_copy writes
    std::optional<int> quality;   // JPEG's 0..12
    std::string prefix;
    std::string suffix;
};

// The file a page of an input is written as: <folder>/<prefix><stem>[_<page's file stem> for a book's page]<suffix>.<ext>,
// a name every OS takes (formats::safe_name); a file there, or one in `taken`, gets the next free -2, -3 … (core::Error
// "refused_target" past 9999), as クイック書き出し names its files.
fs::path output_path(const Output& output, const Input& input, std::string_view page_stem, std::string_view extension,
                     const std::vector<fs::path>& taken = {});
// The extension a page of this input is written as (Output::format; "same": the input's own when write_copy writes it).
std::string output_extension(const Output& output, const Input& input);

// --- the runner -------------------------------------------------------------------------------------------------------

struct Options {
    core::Actor actor;
    // The command line's caller gave no --agent: in an agent's book (studio_book) the steps run as legacy:unknown — the
    // gates of strict_gates and the agent's ops only —, as `genko apply` runs them there.
    bool unnamed = false;
    bool recorded = false;       // the steps are a recorded action (aim_steps)
    bool stop_on_error = false;  // else a file that fails is said in `errors` and the next one taken (Photoshop's バッチ)
    std::function<bool()> cancelled;  // asked before each file: true stops (the files done stay)
    std::function<void(std::size_t done, std::size_t total, const fs::path& now)> progress;
    formats::OpenOptions open;   // how a picture is opened (the window's colour settings)
    std::string script;          // the journal's name for a change written to a book (SaveRequest::script)
    bool book_brushes = true;    // a book's own brushes made known before it is drawn (as `genko copy`; the app follows its own)
};

struct Result {
    core::Json files = core::Json::array();    // {"input", "output"?, "notes"?}: done
    core::Json errors = core::Json::array();   // {"input", "error", "code"}
    core::Json skipped = core::Json::array();  // {"path", "reason"}
    bool cancelled = false;
    // {"ok": no errors, "files", "errors", "skipped", "cancelled"?}
    core::Json json() const;
};

// Each input in turn (one book held at a time): opened (a picture as a book of one page — formats::open_file —, a book
// read; with Output::To::Book a v1–v3 book refused first, then under its lock, its journal settled), the steps aimed at it
// and run (run_steps), then written (Output). The steps are checked first (check_steps: nothing is opened when they
// cannot run). Never written: a picture's own file, anything inside a book's folder — the output folder in a book, or a
// book itself, is refused before it is made (core::Error "refused_target").
Result run(const std::vector<Input>& inputs, const core::Json& steps, const Output& output, const Options& options);

}  // namespace genko::service::automate
