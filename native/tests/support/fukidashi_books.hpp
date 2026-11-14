#pragma once

#include <algorithm>
#include <string>
#include <vector>

#include "core/base64.hpp"
#include "core/command_bus.hpp"
#include "core/json.hpp"
#include "core/model.hpp"
#include "formats/pillow_save.hpp"
#include "render/image.hpp"
#include "render/ops_registry.hpp"

// fukidashi@1 (SPEC BALLOON-01): the sample books of the balloons' tests.
//
// classic_book: every balloon kind, tail and outline option of before the clean renderer (the hand-drawn look), none of
// the new keys. tests/data/fukidashi/old_book/project.json is this book as main 5cf6551a (the last build of the
// hand-drawn look) wrote it, and tests/data/fukidashi/old_letters.json the size its letters had there: the book of
// before the tests read. (Built by this build it is a book of now: its ovals true ellipses in the boxes given.)
//
// clean_book: the kinds and tails of now — the sample sheets.

namespace genko::test {

inline const core::CommandBus& classic_bus() {
    static const core::CommandBus b(render::ops_registry());
    return b;
}

// A small RGBA picture (a 画像のフキダシ's): a grey ramp with a soft edge, as a base64 PNG.
inline std::string classic_picture() {
    std::string bytes;
    for (int y = 0; y < 16; ++y) {
        for (int x = 0; x < 24; ++x) {
            const int edge = std::min(std::min(x, 23 - x), std::min(y, 15 - y));
            bytes.push_back(static_cast<char>(40 + x * 8));
            bytes.push_back(static_cast<char>(200 - y * 6));
            bytes.push_back(static_cast<char>(120));
            bytes.push_back(static_cast<char>(std::min(255, 60 + edge * 60)));
        }
    }
    return core::b64encode(formats::png_bytes(render::Image::frombytes("RGBA", render::Size{24, 16}, bytes)));
}

// The sample book of before on `spec` (four pages): 1 the kinds and their outline options, 2 the tails, 3 the lines across
// (horizontal), bare (hand: false), tall, and with words left to break by themselves, 4 joined balloons, cuts and turns. Fixed ids throughout, so a save is the same
// bytes each time.
inline core::Document classic_book(const core::PageSpec& spec) {
    using core::Json;
    core::Document doc = core::new_episode("フキダシの見本", core::Num(1), 4, spec);
    doc.book_id = "0123456789abcdef0123456789abcdef";
    for (std::size_t i = 0; i < doc.pages.size(); ++i) {
        core::Page& page = doc.edit_page(i);
        page.id = "pg" + std::to_string(i + 1);
        for (std::size_t k = 0; k < page.layers.size(); ++k) page.layers[k].id = "ly" + std::to_string(i + 1) + "_" + std::to_string(k);
        for (std::size_t k = 0; k < page.frames.size(); ++k) page.frames[k].id = "fr" + std::to_string(i + 1) + "_" + std::to_string(k);
    }
    Json ops = Json::array();
    int n = 0;
    const auto add = [&](int page, const char* kind, double x, double y, double w, double h, const char* text, Json more = Json::object()) {
        Json op{{"op", "add_line"}, {"page", page}, {"id", "L" + std::to_string(++n)}, {"balloon", kind}, {"x_mm", x}, {"y_mm", y},
                {"w_mm", w}, {"h_mm", h}, {"text", text}};
        for (const auto& [key, value] : more.items()) op[key] = value;
        ops.push_back(std::move(op));
        return "L" + std::to_string(n);
    };
    // --- page 1: the kinds, and what their outline takes ---
    const char* kinds[] = {"speech", "rounded", "box", "cloud", "thought", "shout", "electric", "flash", "whisper", "narration",
                           "sfx", "none", "dotted_box", "tone_box", "fancy_box"};
    int at = 0;
    const auto cell = [&](int index) { return std::pair<double, double>{12.0 + (index % 4) * 48.0, 14.0 + (index / 4) * 35.0}; };
    for (const char* kind : kinds) {
        const auto [x, y] = cell(at++);
        add(1, kind, x, y, 40, 30, "今日は\nいい天気");
    }
    {
        const auto [x, y] = cell(at++);
        add(1, "picture", x, y, 40, 30, "絵の中", Json{{"style", Json{{"picture", classic_picture()}}}});
    }
    const std::pair<const char*, Json> styled[] = {
        {"speech", Json{{"hand", false}}},
        {"speech", Json{{"wobble", 0.5}}},
        {"rounded", Json{{"wobble", 0.8}}},
        {"speech", Json{{"double", true}}},
        {"cloud", Json{{"bumps", 12}}},
        {"shout", Json{{"spikes", 16}, {"spike_depth", 0.3}, {"spike_jitter", 0.5}}},
        {"electric", Json{{"spikes", 20}, {"spike_depth", 0.2}}},
        {"speech", Json{{"fill", "none"}}},
        {"speech", Json{{"fill_rgb", Json::array({255, 240, 200})}, {"line_rgb", Json::array({160, 30, 30})}, {"fill_opacity", 0.6}}},
        {"speech", Json{{"border_mm", 0.8}}},
        {"rounded", Json{{"border_mm", 0.2}, {"double", true}}},
        {"whisper", Json{{"hand", false}, {"border_mm", 0.5}}},
    };
    for (const auto& [kind, style] : styled) {
        const auto [x, y] = cell(at++);
        add(1, kind, x, y, 40, 30, "そうだね", Json{{"style", style}});
    }
    {  // (drawn by hand: its outline as given, and smoothed)
        const auto [x, y] = cell(at++);
        const Json path = Json::array({Json::array({x + 2, y + 4}), Json::array({x + 30, y}), Json::array({x + 40, y + 14}),
                                       Json::array({x + 34, y + 30}), Json::array({x + 8, y + 27}), Json::array({x, y + 15})});
        add(1, "speech", x, y, 40, 30, "手描き", Json{{"path", path}});
        const auto [x2, y2] = cell(at++);
        Json path2 = Json::array();
        for (const Json& p : path) path2.push_back(Json::array({p[0].get<double>() + (x2 - x), p[1].get<double>() + (y2 - y)}));
        add(1, "speech", x2, y2, 40, 30, "なめらか", Json{{"path", path2}, {"style", Json{{"path_curve", true}}}});
    }
    // --- page 2: the tails; page 4: joined balloons, cuts and turns ---
    at = 0;
    const auto tail_cell = [&](int index) { return std::pair<double, double>{16.0 + (index % 3) * 62.0, 16.0 + (index / 3) * 44.0}; };
    const auto tails = [&](const char* kind, Json list, const char* text = "ねえ\n聞いて") {
        const auto [x, y] = tail_cell(at++);
        Json placed = Json::array();
        for (Json t : list) {
            for (const char* key : {"to", "via"}) {
                if (t.contains(key)) t[key] = Json::array({x + t[key][0].get<double>(), y + t[key][1].get<double>()});
            }
            if (t.contains("vias")) {
                Json vias = Json::array();
                for (const Json& v : t["vias"]) vias.push_back(Json::array({x + v[0].get<double>(), y + v[1].get<double>()}));
                t["vias"] = std::move(vias);
            }
            placed.push_back(std::move(t));
        }
        return add(2, kind, x, y, 36, 28, text, Json{{"tails", placed}});
    };
    tails("speech", Json::array({Json{{"to", Json::array({46, 38})}}}));
    tails("speech", Json::array({Json{{"to", Json::array({-8, 36})}}}));
    tails("speech", Json::array({Json{{"to", Json::array({18, -10})}}}));
    tails("speech", Json::array({Json{{"to", Json::array({50, 14})}, {"via", Json::array({44, 4})}}}));
    tails("speech", Json::array({Json{{"to", Json::array({40, 40})}, {"width_mm", 7.0}}}));
    tails("speech", Json::array({Json{{"to", Json::array({44, 38})}, {"kind", "straight"}}}));
    tails("speech", Json::array({Json{{"to", Json::array({44, 38})}, {"kind", "zigzag"}}}));
    tails("speech", Json::array({Json{{"to", Json::array({46, 40})}, {"kind", "fade"}}}));
    tails("speech", Json::array({Json{{"to", Json::array({46, 40})}, {"kind", "bubbles"}}}));
    tails("speech", Json::array({Json{{"to", Json::array({52, 40})}, {"vias", Json::array({Json::array({44, 20}), Json::array({40, 34})})}}}));
    tails("speech", Json::array({Json{{"to", Json::array({-8, 38})}}, Json{{"to", Json::array({44, 38})}}}));
    tails("rounded", Json::array({Json{{"to", Json::array({30, 40})}}}));
    tails("box", Json::array({Json{{"to", Json::array({48, 20})}}}));
    tails("cloud", Json::array({Json{{"to", Json::array({44, 38})}}}));
    tails("thought", Json::array({Json{{"to", Json::array({46, 40})}}}));
    tails("shout", Json::array({Json{{"to", Json::array({46, 40})}}}));
    tails("electric", Json::array({Json{{"to", Json::array({46, 40})}}}));
    tails("whisper", Json::array({Json{{"to", Json::array({44, 38})}}}));
    {  // (joined: two said in one breath, and two rounded with a tail)
        const double y = 70.0;
        add(4, "speech", 16, y, 34, 26, "それで", Json{{"style", Json{{"group", "g1"}}}, {"tail", Json::array({20, y + 36})}});
        add(4, "speech", 42, y + 14, 30, 24, "どうした", Json{{"style", Json{{"group", "g1"}}}});
        add(4, "rounded", 86, y, 30, 22, "あのね", Json{{"style", Json{{"group", "g2"}, {"double", true}}}});
        add(4, "rounded", 106, y + 16, 30, 22, "じつは", Json{{"style", Json{{"group", "g2"}}}, {"tail", Json::array({140, y + 46})}});
        const std::string cut = add(4, "speech", 150, y, 40, 30, "消しゴム", Json{{"tail", Json::array({186, y + 40})}});
        ops.push_back(Json{{"op", "cut_balloon"}, {"id", cut}, {"points", Json::array({Json::array({150, y + 4}), Json::array({168, y - 2}), Json::array({184, y + 6})})},
                           {"width_mm", 3.0}});
        add(4, "speech", 150, y - 50, 36, 28, "傾けて", Json{{"style", Json{{"rotate_deg", 15.0}}}, {"tail", Json::array({192, y - 14})}});
        add(4, "shout", 104, y - 50, 36, 28, "まわる", Json{{"style", Json{{"rotate_deg", -20.0}}}});
    }
    // --- page 3: across, and without the hand ---
    at = 0;
    for (const char* kind : {"speech", "rounded", "box", "cloud", "thought", "shout", "electric", "flash", "whisper", "narration"}) {
        const auto [x, y] = cell(at++);
        add(3, kind, x, y, 44, 24, "よこがき\nです", Json{{"wrap", "horizontal"}, {"style", Json{{"hand", false}}}});
    }
    for (const char* kind : {"speech", "thought", "whisper", "shout"}) {
        const auto [x, y] = cell(at++);
        add(3, kind, x, y + 6, 26, 44, "たてに長いフキダシ", Json{{"tail", Json::array({x + 30, y + 56})}});
    }
    // (words left to break by themselves: the columns as long as the oval's room lets them be)
    const std::pair<const char*, Json> wrapped[] = {
        {"speech", Json::object()}, {"speech", Json{{"hand", false}}}, {"thought", Json::object()}, {"whisper", Json::object()},
        {"speech", Json{{"wobble", 0.4}}}, {"shout", Json::object()}, {"flash", Json::object()}, {"speech", Json{{"size_mm", 3.5}}},
    };
    int wi = 0;
    for (const auto& [kind, style] : wrapped) {
        const double x = 12.0 + (wi % 4) * 48.0;
        const double y = 196.0 + (wi / 4) * 50.0;
        const double w = wi % 2 == 0 ? 30.0 : 24.0;
        const double h = wi % 3 == 0 ? 44.0 : 36.0;
        ++wi;
        Json more = Json::object();
        if (!style.empty()) more["style"] = style;
        add(3, kind, x, y, w, h, "長い台詞をそのまま入れて折り返しにまかせるとどうなるか", more);
    }
    return classic_bus().apply(doc, ops, core::Actor("human:作者")).doc;
}

// The sample of now on A4: page 1 the kinds and what their outline takes, page 2 a tail eight ways — its tip near and
// far —, bent through a point, on the other smooth balloons; page 3 the other tails, the other balloons' tails, joined
// balloons, and at its foot the balloon eraser and turned balloons. Ids C1, C2, … in the order added.
inline core::Document clean_book() {
    using core::Json;
    core::Document doc = core::new_episode("今風のフキダシ", core::Num(1), 3, core::PageSpec::a4_mono());
    Json ops = Json::array();
    int n = 0;
    const auto add = [&](int page, const char* kind, double x, double y, double w, double h, const char* text, Json more = Json::object()) {
        Json op{{"op", "add_line"}, {"page", page}, {"id", "C" + std::to_string(++n)}, {"balloon", kind}, {"x_mm", x}, {"y_mm", y},
                {"w_mm", w}, {"h_mm", h}, {"text", text}};
        for (const auto& [key, value] : more.items()) op[key] = value;
        ops.push_back(std::move(op));
        return "C" + std::to_string(n);
    };
    const auto style = [](Json st) { return Json{{"style", std::move(st)}}; };
    const Json black{{"fill_rgb", Json::array({0, 0, 0})}, {"line_rgb", Json::array({0, 0, 0})}, {"rgb", Json::array({255, 255, 255})}};
    // --- page 1 ---
    struct Cell {
        const char* kind;
        double w, h;
        const char* text;
        Json more;
    };
    const Cell cells[] = {
        {"speech", 34, 28, "今日は\nいい天気", Json::object()},
        {"capsule", 14, 30, "そうなんだ", Json::object()},
        {"rounded", 34, 28, "今日は\nいい天気", Json::object()},
        {"rounded", 34, 28, "スクワ\nークル", style(Json{{"squircle", true}})},
        {"box", 34, 26, "四角い\nフキダシ", Json::object()},
        {"narration", 34, 26, "その日の\n午後", Json::object()},
        {"polygon", 34, 28, "キカイ\nノコエ", Json::object()},
        {"polygon", 34, 28, "ロッカク", style(Json{{"sides", 6}})},
        {"cloud", 36, 30, "もくもく\nしてる", Json::object()},
        {"thought", 34, 28, "どうしよう\nかな", Json::object()},
        {"shout", 38, 32, "なんだ\nって！", Json::object()},
        {"shout", 38, 32, "長短の\nトゲ！", style(Json{{"spike_alt", 0.5}})},
        {"electric", 36, 30, "ピピッ\n受信中", Json::object()},
        {"flash", 30, 26, "ハッ", Json::object()},
        {"whisper", 34, 28, "ここだけ\nの話", Json::object()},
        {"whisper", 34, 28, "長い\n破線", style(Json{{"dash_mm", 2.5}})},
        {"dotted_box", 34, 26, "点線の\n枠", Json::object()},
        {"tone_box", 34, 26, "トーンの\n枠", Json::object()},
        {"fancy_box", 34, 26, "飾りの\n枠", Json::object()},
        {"speech", 34, 28, "二重の\n線", style(Json{{"double", true}})},
        {"speech", 34, 28, "ふるえる\n声", style(Json{{"wave_mm", 0.4}})},
        {"rounded", 34, 28, "角を\n小さく", style(Json{{"corner_mm", 2.5}})},
        {"speech", 34, 28, "白抜きの\n文字", style(black)},
        {"shout", 38, 32, "ベタの\n叫び！", style(black)},
        {"flash", 30, 26, "ベタ\nフラ", style(black)},
        {"speech", 34, 28, "線なしの\n白", style(Json{{"border_mm", 0}})},
        {"capsule", 36, 13, "よこ長", Json{{"wrap", "horizontal"}}},
        {"speech", 34, 28, "太い線", style(Json{{"border_mm", 0.8}})},
    };
    int at = 0;
    for (const Cell& cell : cells) {
        const double cx = 36.0 + (at % 4) * 46.0;
        const double cy = 30.0 + (at / 4) * 37.5;
        ++at;
        add(1, cell.kind, cx - cell.w / 2, cy - cell.h / 2, cell.w, cell.h, cell.text, cell.more);
    }
    // --- pages 2 and 3: the tails ---
    int page = 2;
    at = 0;
    const auto tailed = [&](const char* kind, double w, double h, Json list, const char* text = "ねえ\n聞いて", Json more = Json::object()) {
        const double cx = 36.0 + (at % 4) * 46.0;
        const double cy = 30.0 + (at / 4) * 43.0;
        ++at;
        const double x = cx - w / 2, y = cy - h / 2;
        Json placed = Json::array();
        for (Json t : list) {
            for (const char* key : {"to", "via"}) {
                if (t.contains(key)) t[key] = Json::array({cx + t[key][0].get<double>(), cy + t[key][1].get<double>()});
            }
            if (t.contains("vias")) {
                Json vias = Json::array();
                for (const Json& v : t["vias"]) vias.push_back(Json::array({cx + v[0].get<double>(), cy + v[1].get<double>()}));
                t["vias"] = std::move(vias);
            }
            placed.push_back(std::move(t));
        }
        more["tails"] = placed;
        return add(page, kind, x, y, w, h, text, more);
    };
    const auto to = [](double x, double y) { return Json{{"to", Json::array({x, y})}}; };
    const auto with = [](Json tail, const char* key, Json value) {
        tail[key] = std::move(value);
        return tail;
    };
    // (page 2: eight ways, a near tip and a far one; then bent through a point)
    const double ways[8][2] = {{0, 1}, {0.7, 0.7}, {1, 0}, {0.7, -0.7}, {0, -1}, {-0.7, -0.7}, {-1, 0}, {-0.7, 0.7}};
    for (const auto& way : ways) tailed("speech", 28, 22, Json::array({to(way[0] * 20, way[1] * 17)}), "近い");
    for (const auto& way : ways) tailed("speech", 26, 20, Json::array({to(way[0] * 23, way[1] * 20)}), "遠い");
    tailed("speech", 28, 22, Json::array({with(to(20, 14), "via", Json::array({19, 2}))}), "まげる");
    tailed("speech", 28, 22, Json::array({with(to(-8, 21), "via", Json::array({6, 16}))}), "まげる");
    tailed("speech", 28, 22, Json::array({with(to(21, -6), "via", Json::array({10, -17}))}), "まげる");
    tailed("capsule", 13, 30, Json::array({to(15, 14)}), "そうなんだ");
    tailed("rounded", 28, 22, Json::array({to(4, 20)}));
    tailed("box", 28, 20, Json::array({to(22, 5)}));
    tailed("polygon", 28, 22, Json::array({to(17, 18)}));
    tailed("narration", 28, 20, Json::array(), "しっぽなし");
    // (page 3: the other tails, and the other balloons' tails; joined balloons)
    page = 3;
    at = 0;
    tailed("speech", 30, 24, Json::array({with(to(14, 21), "width_mm", 7.0)}), "太い\n付け根");
    tailed("speech", 30, 24, Json::array({with(to(16, 20), "kind", "straight")}));
    tailed("speech", 30, 24, Json::array({with(to(16, 20), "kind", "zigzag")}));
    tailed("speech", 30, 24, Json::array({with(to(18, 21), "kind", "fade")}));
    tailed("speech", 30, 24, Json::array({with(to(18, 21), "kind", "bubbles")}));
    tailed("speech", 30, 24, Json::array({with(to(24, 20), "vias", Json::array({Json::array({20, 4}), Json::array({15, 16})}))}));
    tailed("speech", 30, 24, Json::array({to(-16, 20), to(16, 20)}));
    tailed("speech", 30, 24, Json::array({with(to(8, 4), "kind", "inward")}), "内向き");
    tailed("speech", 30, 24, Json::array({with(to(16, 20), "fillet_mm", 0.0)}), "角の\n付け根");
    tailed("speech", 30, 24, Json::array({with(to(16, 20), "fillet_mm", 2.5)}), "大きな\n丸み");
    tailed("cloud", 32, 26, Json::array({to(20, 20)}));
    tailed("thought", 30, 24, Json::array({to(20, 21)}));
    tailed("shout", 34, 28, Json::array({to(22, 21)}));
    tailed("electric", 32, 26, Json::array({to(22, 21)}));
    tailed("whisper", 30, 24, Json::array({to(16, 20)}));
    tailed("speech", 30, 24, Json::array({to(16, 20)}), "ふるえる\n声", Json{{"style", Json{{"wave_mm", 0.35}}}});
    {
        const double y = 30.0 + 4 * 43.0 - 6;
        add(3, "speech", 20, y, 30, 24, "それで", Json{{"style", Json{{"group", "g1"}}}, {"tail", Json::array({22, y + 34})}});
        add(3, "speech", 42, y + 13, 28, 22, "どうした", style(Json{{"group", "g1"}}));
        add(3, "speech", 78, y, 24, 20, "あのね", style(Json{{"group", "g2"}}));
        add(3, "speech", 104, y + 14, 24, 20, "じつは", Json{{"style", Json{{"group", "g2"}}}, {"tail", Json::array({130, y + 40})}});
        add(3, "rounded", 136, y, 26, 20, "かどまる", style(Json{{"group", "g3"}}));
        add(3, "rounded", 152, y + 14, 26, 20, "つなぐ", style(Json{{"group", "g3"}}));
    }
    {  // (page 3's foot: the balloon eraser — a stroke across the balloon's upper left, from outside to outside — and
       // turned balloons)
        const double y = 238.0;
        const std::string cut = add(3, "speech", 22, y, 30, 24, "消しゴム", Json{{"tail", Json::array({56, y + 30})}});
        ops.push_back(Json{{"op", "cut_balloon"}, {"id", cut}, {"points", Json::array({Json::array({19, y + 12}), Json::array({27, y + 3}), Json::array({36, y - 3})})},
                           {"width_mm", 3.0}});
        add(3, "speech", 70, y, 28, 24, "傾けて", Json{{"style", Json{{"rotate_deg", 15.0}}}, {"tail", Json::array({102, y + 30})}});
        add(3, "shout", 116, y - 2, 34, 28, "まわる\n叫び", style(Json{{"rotate_deg", -12.0}}));
        add(3, "whisper", 162, y, 30, 24, "ねえ\n聞いて", Json{{"tail", Json::array({186, y + 32})}});
    }
    return classic_bus().apply(doc, ops, core::Actor("human:作者")).doc;
}

}  // namespace genko::test
