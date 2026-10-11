// The blind character test (D5) and the panels recovered from scanned names (D8) (Python's genko/studio/evaluate.py:
// eval_sample, eval_score, eval_atari): tests a person runs on a finished book, from the command line.

#include <QString>

#include <algorithm>
#include <map>
#include <tuple>
#include <vector>

#include "core/error.hpp"
#include "core/model.hpp"
#include "core/paths.hpp"
#include "core/pyconv.hpp"
#include "core/pynum.hpp"
#include "core/pyops.hpp"
#include "core/pyrandom.hpp"
#include "render/text/fonts.hpp"
#include "formats/pillow_save.hpp"
#include "service/atari.hpp"
#include "service/book.hpp"
#include "service/briefs.hpp"
#include "service/evaluate.hpp"
#include "service/guide.hpp"
#include "service/service_util.hpp"
#include "service/sheets.hpp"
#include "storage/asset_store.hpp"
#include "storage/fsutil.hpp"
#include "studio/files.hpp"

namespace genko::service::evaluate {

namespace fs = std::filesystem;
using core::py_get;
using core::py_str;
using core::py_truthy;

namespace {

constexpr double kTargetAccuracy = 0.9;

// _crop_art(store, cand, frame, long_side): the panel's adopted picture as printed (fit_cover), without balloons.
std::optional<render::Image> crop_art(const storage::AssetStore& store, const Json& cand, const core::Frame& frame, int long_side = 512) {
    const auto data = store.get_bytes(py_str(util::item(cand, "asset")), ".png");
    if (!data) return std::nullopt;
    const render::Image image = open_picture(*data).convert("RGB");
    const double aspect = (frame.rect.width / frame.rect.height).value();
    const std::array<int, 2> size =
        aspect >= 1 ? std::array<int, 2>{long_side, std::max(1, static_cast<int>(core::py_round_int(long_side / aspect)))}
                    : std::array<int, 2>{std::max(1, static_cast<int>(core::py_round_int(long_side * aspect))), long_side};
    return guide::fit_cover(image, size);
}

std::string html_escape(std::string_view text) {
    std::string out;
    for (const char c : text) {
        switch (c) {
            case '&': out += "&amp;"; break;
            case '<': out += "&lt;"; break;
            case '>': out += "&gt;"; break;
            case '"': out += "&quot;"; break;
            case '\'': out += "&#x27;"; break;
            default: out += c;
        }
    }
    return out;
}

// _form(letters, samples): the evaluators' page.
std::string form(const std::vector<std::string>& letters, const std::vector<std::string>& samples) {
    std::string parts = "<!doctype html><html lang='ja'><meta charset='utf-8'><title>キャラクター同定</title>";
    parts += "<style>body{font-family:sans-serif;margin:16px}.row{display:flex;gap:12px;flex-wrap:wrap}"
             "img{max-width:240px;border:1px solid #999}.s{margin:12px 0;border-top:1px solid #ccc;padding-top:8px}</style>";
    parts += "<h1>キャラクター同定（盲検）</h1><p>上の設定画だけを見て、各コマの人物がどれかを選び、answers.csv に書く"
             "（evaluator に自分の名前、choice に文字）。</p><div class='row'>";
    for (const std::string& letter : letters) parts += "<div><h2>" + letter + "</h2><img src='sheets/" + letter + ".png' alt='" + letter + "'></div>";
    parts += "</div>";
    for (const std::string& name : samples) {
        parts += "<div class='s'><h3>" + html_escape(name) + "</h3><img src='samples/" + name + ".png' alt='" + name + "'></div>";
    }
    parts += "</html>";
    return parts;
}

// csv.reader over a text file opened with universal newlines (the excel dialect, not strict): the rows.
std::vector<std::vector<std::string>> read_csv(const std::string& text) {
    std::vector<std::vector<std::string>> rows;
    std::vector<std::string> row;
    std::string field;
    bool has_field = false;  // (a field started: saved at the end of the data)
    enum class State { StartRecord, StartField, InField, InQuoted, QuoteInQuoted, EatNewline } state = State::StartRecord;
    const auto save = [&] {
        row.push_back(field);
        field.clear();
        has_field = false;
    };
    // lines as the file gives them, each with its "\n"
    std::size_t start = 0;
    while (start < text.size()) {
        std::size_t end = text.find('\n', start);
        end = end == std::string::npos ? text.size() : end + 1;
        const std::string_view line(text.data() + start, end - start);
        start = end;
        for (const char c : line) {
            switch (state) {
                case State::StartRecord:
                    if (c == '\n' || c == '\r') {
                        state = State::EatNewline;
                        break;
                    }
                    state = State::StartField;
                    [[fallthrough]];
                case State::StartField:
                    if (c == '\n' || c == '\r') {
                        save();
                        state = State::EatNewline;
                    } else if (c == '"') {
                        has_field = true;
                        state = State::InQuoted;
                    } else if (c == ',') {
                        save();
                    } else {
                        field += c;
                        has_field = true;
                        state = State::InField;
                    }
                    break;
                case State::InField:
                    if (c == '\n' || c == '\r') {
                        save();
                        state = State::EatNewline;
                    } else if (c == ',') {
                        save();
                        state = State::StartField;
                    } else {
                        field += c;
                    }
                    break;
                case State::InQuoted:
                    if (c == '"') {
                        state = State::QuoteInQuoted;
                    } else {
                        field += c;
                    }
                    break;
                case State::QuoteInQuoted:
                    if (c == '"') {
                        field += c;
                        state = State::InQuoted;
                    } else if (c == ',') {
                        save();
                        state = State::StartField;
                    } else if (c == '\n' || c == '\r') {
                        save();
                        state = State::EatNewline;
                    } else {
                        field += c;
                        state = State::InField;
                    }
                    break;
                case State::EatNewline:
                    if (c != '\n' && c != '\r') {
                        throw core::PyUncaught("_csv.Error", "new-line character seen in unquoted field - do you need to open the file with newline=''?");
                    }
                    break;
            }
        }
        // the end of the line
        switch (state) {
            case State::StartRecord:
                break;
            case State::StartField:
            case State::InField:
            case State::QuoteInQuoted:
                save();
                state = State::StartRecord;
                break;
            case State::EatNewline:
                state = State::StartRecord;
                break;
            case State::InQuoted:
                break;  // (the field goes on in the next line)
        }
        if (state == State::StartRecord) {
            rows.push_back(std::move(row));
            row.clear();
        }
    }
    if (has_field || !field.empty() || state == State::InQuoted) {  // (the data ended inside a field: kept, as the reader does when not strict)
        save();
        rows.push_back(std::move(row));
    }
    return rows;
}

// (row.get(key) or "").strip()
std::string cell(const std::map<std::string, std::string>& row, const std::string& key) {
    const auto found = row.find(key);
    return found == row.end() ? std::string() : core::py_strip(found->second);
}

double ratio(const std::vector<int>& hits) {
    double sum = 0;
    for (const int h : hits) sum += h;
    return sum / static_cast<double>(hits.size());
}

// _edge_error(a, b): the largest difference of the two boxes' edges.
double edge_error(const std::vector<double>& a, const std::vector<double>& b) {
    return std::max({std::abs(a[0] - b[0]), std::abs(a[1] - b[1]), std::abs(a[0] + a[2] - b[0] - b[2]), std::abs(a[1] + a[3] - b[1] - b[3])});
}

}  // namespace

Json eval_sample(const fs::path& project, const fs::path& out, std::int64_t per_character, std::int64_t seed) {
    const core::Document episode = load_episode(project);
    const storage::AssetStore store(project);
    std::vector<Json> chars;
    for (const Json& c : core::iterate(episode.bible.characters)) {
        if (!core::truthy_at(c, "locked")) continue;
        bool sheet = false;
        for (const Json& r : core::iterate(py_get(c, "refs", Json::array()))) sheet = sheet || core::py_equals(py_get(r, "kind"), Json("sheet"));
        if (sheet) chars.push_back(c);
    }
    if (chars.size() < 2) return Json::object({{"ok", false}, {"error", "設定画が承認された登場人物が 2 人以上要る"}});
    core::PyRandom rng(seed);
    std::vector<std::string> letters;
    for (std::size_t i = 0; i < chars.size(); ++i) letters.push_back(render::text::utf8(std::u32string(1, static_cast<char32_t>(U'A' + i))));
    std::vector<Json> shuffled = chars;
    rng.shuffle(shuffled);
    PyDict letter_of;
    for (std::size_t i = 0; i < shuffled.size(); ++i) letter_of.set(util::item(shuffled[i], "id"), Json(letters[i]));
    const auto letter = [&letter_of](const Json& id) -> std::string {
        const Json* found = letter_of.find(id);
        if (found == nullptr) throw core::PyUncaught("KeyError", core::py_repr(id));
        return found->get<std::string>();
    };
    struct Item {
        const core::Page* page;
        const core::Frame* frame;
        Json cand;
    };
    std::vector<std::pair<Json, std::vector<Item>>> pools;  // (in the characters' order, each id once)
    const auto pool_of = [&pools](const Json& id) -> std::vector<Item>* {
        for (auto& [key, items] : pools) {
            if (core::py_equals(key, id)) return &items;
        }
        return nullptr;
    };
    for (const Json& c : chars) {
        const Json id = util::item(c, "id");
        core::require_hashable(id);
        if (pool_of(id) == nullptr) pools.emplace_back(id, std::vector<Item>{});
    }
    for (const auto& page : episode.pages) {
        for (const core::Frame* frame : page->leaf_frames()) {
            const Json panel = frame->panel && py_truthy(*frame->panel) ? *frame->panel : Json::object();
            std::vector<Json> cast;
            for (const Json& c : core::iterate(py_get(panel, "characters", Json::array()))) {
                if (c.is_object()) cast.push_back(py_get(c, "id"));
            }
            const Json art = py_get(core::py_or(py_get(panel, "adopted"), Json::object()), "art");
            if (cast.size() != 1) continue;
            core::require_hashable(cast[0]);
            std::vector<Item>* pool = pool_of(cast[0]);
            if (pool == nullptr || !py_truthy(art)) continue;
            for (const Json& cand : core::iterate(py_get(panel, "candidates", Json::array()))) {
                if (core::py_equals(py_get(cand, "id"), art)) {
                    pool->push_back(Item{page.get(), frame, cand});
                    break;
                }
            }
        }
    }
    for (auto& [id, pool] : pools) {
        (void)id;
        rng.shuffle(pool);
    }
    std::vector<std::size_t> order(pools.size());
    for (std::size_t i = 0; i < order.size(); ++i) order[i] = i;
    std::stable_sort(order.begin(), order.end(), [&](std::size_t x, std::size_t y) { return letter(pools[x].first) < letter(pools[y].first); });
    std::vector<std::pair<Json, Item>> chosen;
    for (const std::size_t i : order) {
        const auto& [id, pool] = pools[i];
        // (pool[:per_character], a negative count from the end as Python slices)
        const auto size = static_cast<std::int64_t>(pool.size());
        const std::int64_t stop = per_character < 0 ? std::max<std::int64_t>(0, size + per_character) : std::min(size, per_character);
        for (std::int64_t k = 0; k < stop; ++k) chosen.emplace_back(id, pool[static_cast<std::size_t>(k)]);
    }
    rng.shuffle(chosen);
    fs::create_directories(out / "sheets");
    fs::create_directories(out / "samples");
    for (const Json& character : chars) {
        Json ref;
        for (const Json& r : core::iterate(util::item(character, "refs"))) {
            if (core::py_equals(py_get(r, "kind"), Json("sheet"))) {
                ref = r;
                break;
            }
        }
        const auto data = store.get_bytes(py_str(util::item(ref, "asset")), ".png");
        if (!data) continue;
        render::Image image = open_picture(*data).convert("RGB");
        image.thumbnail(render::Size{640, 640});
        util::write_file(out / "sheets" / core::path_from_utf8(letter(character["id"]) + ".png"), formats::png_bytes(image));
    }
    Json letters_key = Json::object();
    for (const Json& c : chars) letters_key[letter(c["id"])] = c["id"];
    Json samples = Json::object();
    for (std::size_t i = 0; i < chosen.size(); ++i) {
        const auto& [cid, item] = chosen[i];
        std::string name = std::to_string(i + 1);
        if (name.size() < 2) name.insert(0, 1, '0');
        name = "s" + name;
        const auto image = crop_art(store, item.cand, *item.frame);
        if (!image) continue;
        util::write_file(out / "samples" / core::path_from_utf8(name + ".png"), formats::png_bytes(*image));
        samples[name] = Json::object({{"answer", letter(cid)}, {"character", cid}, {"page", item.page->index.json()}, {"frame_id", item.frame->id}});
    }
    const Json key = Json::object({{"seed", seed}, {"letters", letters_key}, {"samples", samples}});
    util::write_file(out / "key.json", studio::text_mode(core::dump_python_indent2(key) + "\n"));
    std::string csv = "evaluator,sample,choice\r\n";
    std::vector<std::string> names;
    for (const auto& [name, sample] : samples.items()) {
        (void)sample;
        csv += "," + name + ",\r\n";
        names.push_back(name);
    }
    util::write_file(out / "answers.csv", csv);
    std::vector<std::string> sorted_letters;
    for (const auto& [l, id] : letters_key.items()) {
        (void)id;
        sorted_letters.push_back(l);
    }
    std::sort(sorted_letters.begin(), sorted_letters.end());
    util::write_file(out / "form.html", studio::text_mode(form(sorted_letters, names)));
    Json per = Json::object();
    bool short_of = false;
    for (const Json& c : chars) {
        std::int64_t count = 0;
        for (const auto& [name, sample] : samples.items()) {
            (void)name;
            count += core::py_equals(sample["character"], c["id"]) ? 1 : 0;
        }
        per[letter(c["id"])] = count;
        short_of = short_of || static_cast<std::int64_t>(pool_of(c["id"])->size()) < per_character;
    }
    return Json::object({{"ok", true},
                         {"out", util::path_text(out)},
                         {"samples", static_cast<std::int64_t>(samples.size())},
                         {"characters", static_cast<std::int64_t>(chars.size())},
                         {"per_character", per},
                         {"short", short_of}});
}

Json eval_score(const fs::path& folder, const fs::path& answers) {
    const Json key = core::parse_python_json(storage::read_file(folder / "key.json"), nullptr, core::ParseOptions{false, true});
    const Json samples = util::item(key, "samples");
    if (!samples.is_object()) core::raise_attribute_error(samples, "items");
    std::string text = storage::read_file(answers);
    if (const auto bad = core::utf8_error(text)) throw core::PyUncaught("UnicodeDecodeError", *bad);
    // (universal newlines: "\r\n" and "\r" read as "\n")
    std::string lines;
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (text[i] == '\r') {
            lines += '\n';
            if (i + 1 < text.size() && text[i + 1] == '\n') ++i;
        } else {
            lines += text[i];
        }
    }
    const std::vector<std::vector<std::string>> rows = read_csv(lines);
    std::vector<std::pair<std::string, std::vector<int>>> per_evaluator;
    std::vector<std::pair<Json, std::vector<int>>> per_character;
    std::vector<int> hits;
    if (!rows.empty()) {
        const std::vector<std::string>& fields = rows.front();
        for (std::size_t r = 1; r < rows.size(); ++r) {
            const std::vector<std::string>& values = rows[r];
            if (values.empty()) continue;  // (DictReader skips empty rows)
            std::map<std::string, std::string> row;
            for (std::size_t i = 0; i < fields.size() && i < values.size(); ++i) row[fields[i]] = values[i];
            const std::string who = cell(row, "evaluator");
            const std::string name = cell(row, "sample");
            const std::string choice = QString::fromStdString(cell(row, "choice")).toUpper().toStdString();
            if (who.empty() || !samples.contains(name) || choice.empty()) continue;
            const Json& sample = samples[name];
            const int hit = core::py_equals(Json(choice), util::item(sample, "answer")) ? 1 : 0;
            hits.push_back(hit);
            const auto e = std::find_if(per_evaluator.begin(), per_evaluator.end(), [&who](const auto& p) { return p.first == who; });
            (e == per_evaluator.end() ? per_evaluator.emplace_back(who, std::vector<int>{}).second : e->second).push_back(hit);
            const Json character = util::item(sample, "character");
            core::require_hashable(character);
            const auto c = std::find_if(per_character.begin(), per_character.end(), [&character](const auto& p) { return core::py_equals(p.first, character); });
            (c == per_character.end() ? per_character.emplace_back(character, std::vector<int>{}).second : c->second).push_back(hit);
        }
    }
    // (the answers in the evaluators' order, as Python sums them)
    std::vector<int> ordered;
    for (const auto& [who, list] : per_evaluator) {
        (void)who;
        ordered.insert(ordered.end(), list.begin(), list.end());
    }
    const std::optional<double> accuracy = ordered.empty() ? std::nullopt : std::optional<double>(ratio(ordered));
    Json by_evaluator = Json::object();
    for (const auto& [who, list] : per_evaluator) by_evaluator[who] = core::py_round(ratio(list), 3);
    Json by_character = Json::object();
    for (const auto& [character, list] : per_character) by_character[py_str(character)] = core::py_round(ratio(list), 3);
    return Json::object({{"ok", true},
                         {"answers", static_cast<std::int64_t>(ordered.size())},
                         {"accuracy", accuracy ? Json(core::py_round(*accuracy, 3)) : Json()},
                         {"passed", accuracy.has_value() && *accuracy >= kTargetAccuracy},
                         {"target", kTargetAccuracy},
                         {"by_evaluator", by_evaluator},
                         {"by_character", by_character}});
}

Json eval_atari(const fs::path& truth_path, double tolerance_mm) {
    const Json truth = core::parse_python_json(storage::read_file(truth_path), nullptr, core::ParseOptions{false, true});
    core::PageSpec spec = core::PageSpec::b4_comic();
    if (core::truthy_at(truth, "page_mm")) {
        spec.width_mm = core::Num(core::to_float(core::subscript(truth["page_mm"], 0)));
        spec.height_mm = core::Num(core::to_float(core::subscript(truth["page_mm"], 1)));
    }
    const core::Document episode = core::new_episode("eval", core::Num(1), 1, spec);
    const core::Page& page = episode.page(0);
    const std::string align = py_str(py_get(truth, "align", Json("auto")));
    Json rows = Json::array();
    std::int64_t hits = 0;
    std::int64_t total = 0;
    for (const Json& item : core::iterate(py_get(truth, "pages", Json::array()))) {
        const Json scan_text = util::item(item, "scan");
        fs::path scan = core::path_from_utf8(py_str(scan_text));
        scan = scan.is_absolute() ? scan : truth_path.parent_path() / scan;
        const render::Image image = open_picture(storage::read_file(scan));
        const auto [placement, used] = atari::placement_for(image.convert("L"), page, align);
        const std::vector<double> box = core::unpack_floats(placement, 4);
        const xycut::Detection found = xycut::detect(image, {box[0], box[1], box[2], box[3]}, xycut::Params{}, true);
        const std::vector<Json> panels = core::iterate(util::item(item, "panels"));
        std::int64_t matched = 0;
        for (const Json& t : panels) {
            const std::vector<double> want = core::unpack_floats(t, 4);
            for (const Json& d : found.leaves_mm) {
                if (edge_error(core::unpack_floats(d["rect_mm"], 4), want) <= tolerance_mm) {
                    ++matched;
                    break;
                }
            }
        }
        hits += matched;
        total += static_cast<std::int64_t>(panels.size());
        rows.push_back(Json::object({{"scan", py_str(scan_text)}, {"panels", static_cast<std::int64_t>(panels.size())},
                                     {"detected", static_cast<std::int64_t>(found.leaves_mm.size())}, {"matched", matched}, {"align", used}}));
    }
    const std::optional<double> rate = total != 0 ? std::optional<double>(static_cast<double>(hits) / static_cast<double>(total)) : std::nullopt;
    return Json::object({{"ok", true},
                         {"pages", rows},
                         {"matched", hits},
                         {"panels", total},
                         {"rate", rate ? Json(core::py_round(*rate, 3)) : Json()},
                         {"passed", rate.has_value() && *rate >= 0.9},
                         {"tolerance_mm", tolerance_mm}});
}

}  // namespace genko::service::evaluate
