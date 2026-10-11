#include "service/fxwords.hpp"

#include <QString>

#include <algorithm>
#include <map>
#include <set>
#include <tuple>

#include "core/error.hpp"
#include "core/pyconv.hpp"
#include "core/pyops.hpp"

namespace genko::service::fxwords {

namespace {

struct Entry {
    std::string kind;
    std::string key;
    std::string en;
};

// word (as written, lower-cased) → (what it becomes, its key, English for the art request), in Python's order
const std::vector<std::pair<std::string, Entry>>& table() {
    static const std::vector<std::pair<std::string, Entry>> rows = [] {
        std::vector<std::pair<std::string, Entry>> out;
        const auto add = [&out](const char* kind, const char* key, const char* en, std::initializer_list<const char*> words) {
            for (const char* word : words) {
                const std::string lower = QString::fromUtf8(word).toLower().toStdString();
                const auto at = std::find_if(out.begin(), out.end(), [&lower](const auto& row) { return row.first == lower; });
                if (at != out.end()) {
                    at->second = Entry{kind, key, en};  // (a later _add of the same word wins, in its first place)
                } else {
                    out.emplace_back(lower, Entry{kind, key, en});
                }
            }
        };
        add("effect", "focus", "focus lines closing in (drawn later by Genko: leave them out)", {"集中線", "focus", "focus_lines", "集中"});
        add("effect", "speed", "motion (speed lines drawn later by Genko: leave them out)",
            {"流線", "speed", "speed_lines", "スピード線", "効果線"});
        add("effect", "white", "a white-out (the panel left plain white)", {"white", "ホワイトアウト", "白で塗る", "白塗り"});
        add("effect", "uni_flash", "a sudden shock or a bright flash (a spiky flash drawn later by Genko)",
            {"ウニフラッシュ", "uni_flash", "ウニフラ", "フラッシュ", "flash", "白フラッシュ", "閃光"});
        add("effect", "beta_flash", "a dark shock (a black flash drawn later by Genko)", {"ベタフラッシュ", "beta_flash", "ベタフラ"});
        add("rain", "rain", "rainy weather, wet surfaces", {"雨", "rain", "rainy", "雨粒", "大雨", "小雨"});
        add("mark", "汗", "a nervous sweat drop", {"汗", "sweat", "冷や汗", "焦り"});
        add("mark", "怒りマーク", "an angry face", {"怒り", "anger", "angry", "怒りマーク", "青筋"});
        add("mark", "驚き線", "surprise", {"驚き", "surprise", "shock", "びっくり"});
        add("mark", "ハート", "affection", {"ハート", "heart", "love", "恋"});
        add("mark", "キラキラ", "sparkles", {"キラキラ", "sparkle", "sparkles", "shine", "glitter", "輝き"});
        add("mark", "音符", "humming happily", {"音符", "music", "humming", "鼻歌"});
        add("mark", "ぐるぐる", "dizzy confusion", {"ぐるぐる", "dizzy", "confused", "混乱", "目が回る"});
        add("mark", "はてなマーク", "puzzlement", {"はてな", "question", "疑問", "？", "?"});
        add("mark", "ひらめき", "a sudden idea", {"ひらめき", "idea", "閃き"});
        add("mark", "湯気", "fuming", {"湯気", "steam", "fuming", "ぷんぷん"});
        add("mark", "動きの線", "trembling", {"震え", "trembling", "shake", "shaking", "揺れ"});
        add("art", "splash", "water splashing up, flying droplets", {"水しぶき", "splash", "しぶき", "水はね"});
        add("art", "snow", "falling snow", {"雪", "snow", "snowfall"});
        add("art", "wind", "strong wind, hair and clothes blown", {"風", "wind", "突風"});
        add("art", "petals", "cherry blossom petals drifting", {"桜", "桜吹雪", "petals", "花びら"});
        add("art", "leaves", "falling leaves", {"落ち葉", "leaves", "木の葉"});
        add("art", "smoke", "smoke", {"煙", "smoke"});
        add("art", "fire", "fire and flames", {"炎", "火", "fire", "flames"});
        add("art", "explosion", "an explosion", {"爆発", "explosion"});
        add("art", "lightning", "lightning", {"稲妻", "雷", "lightning"});
        add("art", "fog", "fog, mist", {"霧", "もや", "fog", "mist"});
        add("art", "tears", "tears", {"涙", "tears", "泣き"});
        add("art", "light", "light rays, backlight", {"光", "逆光", "光線", "light rays", "backlight"});
        add("art", "stars", "a starry sky", {"星空", "starry sky"});
        add("art", "dust", "a cloud of dust", {"土煙", "砂煙", "dust"});
        add("art", "impact", "an impact", {"衝撃", "impact"});
        return out;
    }();
    return rows;
}

bool is_ascii(const std::string& text) {
    return std::all_of(text.begin(), text.end(), [](char c) { return static_cast<unsigned char>(c) < 0x80; });
}

// Python's sort of str: by code point (UTF-8 bytes sort the same way)
std::string lowered(const Json& word) {
    return QString::fromStdString(core::py_strip(core::py_str(word))).toLower().toStdString();
}

}  // namespace

const std::vector<std::string>& known() {
    static const std::vector<std::string> words = [] {
        std::set<std::string> found;
        for (const auto& [word, entry] : table()) {
            if (!is_ascii(word)) found.insert(word);
        }
        return std::vector<std::string>(found.begin(), found.end());
    }();
    return words;
}

std::optional<Json> resolve(const Json& word) {
    const std::string key = lowered(word);
    for (const auto& [text, entry] : table()) {
        if (text == key) return Json::object({{"kind", entry.kind}, {"key", entry.key}, {"en", entry.en}});
    }
    return std::nullopt;
}

std::pair<std::vector<std::string>, std::vector<std::string>> art_words(const Json& words) {
    std::vector<std::string> ja;
    std::vector<std::string> en;
    for (const Json& word : core::iterate(core::py_or(words, Json::array()))) {
        const std::string text = core::py_strip(core::py_str(word));
        if (text.empty()) continue;
        const auto found = resolve(Json(text));
        ja.push_back(text);
        en.push_back(found ? (*found)["en"].get<std::string>() : text + " (in Japanese)");
    }
    return {std::move(ja), std::move(en)};
}

std::optional<std::pair<std::string, std::string>> emphasis_words(const Json& emphasis) {
    double value = 0.0;
    try {
        value = core::to_float(emphasis);
    } catch (const core::PyValueError&) {
        return std::nullopt;
    } catch (const core::PyTypeError&) {
        return std::nullopt;
    }
    if (value >= 0.8) return std::pair<std::string, std::string>{"この話の山場。迫力のある大胆な構図にする。", "A climax of the story: a bold, dramatic, striking composition."};
    if (value >= 0.6) return std::pair<std::string, std::string>{"強調するコマ。印象に残る構図にする。", "An emphasised panel: a memorable composition."};
    if (value <= 0.2) return std::pair<std::string, std::string>{"静かな間のコマ。控えめな構図にする。", "A quiet pause: an understated composition."};
    return std::nullopt;
}

}  // namespace genko::service::fxwords
