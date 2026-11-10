#include "render/pattern_library.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <functional>
#include <map>
#include <mutex>
#include <set>

#include "core/base64.hpp"
#include "core/color_raster.hpp"
#include "core/error.hpp"
#include "core/ink.hpp"
#include "core/patterns.hpp"
#include "render/patterns.hpp"
#include "storage/fsutil.hpp"

// PhotoCraft v0.2.0 crates/engine/src/pattern_cmds.rs (hash01, procedural, builtin) and crates/engine/src/presets/
// patterns.rs (builtin_groups), MIT: native/third_party/photocraft/NOTICE.md.

namespace genko::render::pattern_library {

using core::Json;
namespace fs = std::filesystem;

namespace {

float hash01(std::uint32_t x, std::uint32_t y, std::uint32_t seed) {
    std::uint32_t h = (x * 0x8da6b343u) ^ (y * 0xd8163841u) ^ (seed * 0xcb1ab31fu);
    h ^= h >> 13;
    h *= 0x5bd1e995u;
    h ^= h >> 15;
    return static_cast<float>(h & 0xffffu) / 65535.0f;
}

Builtin procedural(const char* name, const char* group, std::uint32_t w, std::uint32_t h,
                   const std::function<std::array<float, 3>(std::uint32_t, std::uint32_t)>& f) {
    patterns::Tile t;
    t.mode = "rgb";
    t.depth = 8;
    t.width = w;
    t.height = h;
    t.colours = 3;
    t.values.reserve(std::size_t(w) * h * 4);
    for (std::uint32_t y = 0; y < h; ++y) {
        for (std::uint32_t x = 0; x < w; ++x) {
            const auto c = f(x, y);
            for (float v : c) t.values.push_back(std::round(std::clamp(v, 0.0f, 1.0f) * 255.0f) / 255.0f);  // (through 8 bits)
            t.values.push_back(1.0f);
        }
    }
    Builtin b;
    std::string bytes = patterns::encode(t);
    b.pattern.name = name;
    b.pattern.asset = core::bytes_ref(bytes);
    b.pattern.mode = "rgb";
    b.pattern.depth = 8;
    b.pattern.width = w;
    b.pattern.height = h;
    b.pattern.dpi = 300.0;
    b.bytes = std::make_shared<const std::string>(std::move(bytes));
    b.group = group;
    return b;
}

std::vector<Builtin> make_builtins() {
    const auto g = [](float v) { return std::array<float, 3>{v, v, v}; };
    std::vector<Builtin> out;
    const char* geometric = "幾何学";
    const char* textures = "テクスチャ";
    out.push_back(procedural("チェッカーボード", geometric, 16, 16, [&](std::uint32_t x, std::uint32_t y) { return g((x / 8 + y / 8) % 2 == 0 ? 0.8f : 1.0f); }));
    out.push_back(procedural("斜線", geometric, 8, 8, [&](std::uint32_t x, std::uint32_t y) { return g((x + y) % 8 < 2 ? 0.2f : 1.0f); }));
    out.push_back(procedural("水玉", geometric, 12, 12, [&](std::uint32_t x, std::uint32_t y) {
        const float dx = static_cast<float>(x) + 0.5f - 6.0f, dy = static_cast<float>(y) + 0.5f - 6.0f;
        return g(std::clamp(std::sqrt(dx * dx + dy * dy) - 3.0f, 0.0f, 1.0f));
    }));
    out.push_back(procedural("グリッド", geometric, 16, 16, [&](std::uint32_t x, std::uint32_t y) { return g(x == 0 || y == 0 ? 0.55f : 1.0f); }));
    out.push_back(procedural("レンガ", textures, 32, 16, [](std::uint32_t x, std::uint32_t y) {
        const std::uint32_t row = y / 8;
        const std::uint32_t xo = (x + (row % 2 == 1 ? 8 : 0)) % 16;
        const bool mortar = y % 8 == 7 || xo == 15;
        const float n = hash01(x, y, 7) * 0.08f;
        return mortar ? std::array<float, 3>{0.78f + n, 0.76f + n, 0.72f + n} : std::array<float, 3>{0.62f + n, 0.27f + n * 0.5f, 0.2f};
    }));
    out.push_back(procedural("織り目", textures, 16, 16, [](std::uint32_t x, std::uint32_t y) {
        const bool over = ((x / 4) + (y / 4)) % 2 == 0;
        const float t = over ? static_cast<float>(y % 4) / 3.0f : static_cast<float>(x % 4) / 3.0f;
        const float v = 0.55f + 0.35f * (1.0f - std::fabs(2.0f * t - 1.0f));
        return std::array<float, 3>{v, v * 0.92f, v * 0.8f};
    }));
    out.push_back(procedural("紙のノイズ", textures, 64, 64, [&](std::uint32_t x, std::uint32_t y) {
        // (two octaves of smoothed value noise, wrapping at 64 pixels)
        const auto oct = [&](std::uint32_t cell, std::uint32_t seed) {
            const std::uint32_t cx = x / cell, cy = y / cell;
            const float fx = static_cast<float>(x % cell) / static_cast<float>(cell), fy = static_cast<float>(y % cell) / static_cast<float>(cell);
            const std::uint32_t n = 64 / cell;
            const auto v = [&](std::uint32_t i, std::uint32_t j) { return hash01(i % n, j % n, seed); };
            const auto s = [](float t) { return t * t * (3.0f - 2.0f * t); };
            const float sx = s(fx), sy = s(fy);
            const float a = v(cx, cy) + (v(cx + 1, cy) - v(cx, cy)) * sx;
            const float b = v(cx, cy + 1) + (v(cx + 1, cy + 1) - v(cx, cy + 1)) * sx;
            return a + (b - a) * sy;
        };
        return g(0.78f + 0.14f * oct(8, 1) + 0.08f * oct(2, 2));
    }));
    return out;
}

std::string hex_of(const std::string& ref) { return ref.size() > 7 ? ref.substr(7) : ref; }

fs::path file_of(const fs::path& dir, const core::Pattern& p) { return dir / "patterns" / (hex_of(p.asset) + std::string(core::pattern_suffix(p))); }

preset_store::Kind make_kind() {
    preset_store::Kind k;
    k.file = "patterns.json";
    k.default_group = "パターン";
    k.builtin = [] {
        std::vector<preset_store::Group> groups;
        for (const Builtin& b : builtins()) {
            auto g = std::find_if(groups.begin(), groups.end(), [&](const preset_store::Group& x) { return x.name == b.group; });
            if (g == groups.end()) {
                groups.push_back(preset_store::Group{b.group, {}});
                g = groups.end() - 1;
            }
            g->items.push_back(item_of(b.pattern, b.pattern.name));
        }
        return groups;
    };
    k.check = [](const Json& item) { (void)pattern_of(item); };
    k.max_items = 4096;
    return k;
}

}  // namespace

const std::vector<Builtin>& builtins() {
    static const std::vector<Builtin> made = make_builtins();
    return made;
}

const preset_store::Kind& kind() {
    static const preset_store::Kind k = make_kind();
    return k;
}

core::Pattern pattern_of(const Json& item) {
    if (!item.is_object()) throw core::Error("value", "a pattern preset must be an object");
    Json as_book = Json::object();
    as_book["id"] = "library";
    for (const auto& [key, value] : item.items()) {
        if (key != "name" && key != "group") as_book[key] = value;
    }
    as_book["name"] = item.contains("name") ? item["name"] : Json("");
    core::Pattern p = core::pattern_from_json(as_book);
    p.id.clear();
    return p;
}

Json item_of(const core::Pattern& p, const std::string& name) {
    Json out = Json::object();
    out["name"] = name;
    out["asset"] = p.asset;
    out["mode"] = p.mode;
    out["depth"] = p.depth;
    out["width"] = p.width;
    out["height"] = p.height;
    out["dpi"] = p.dpi;
    return out;
}

namespace {

// The library's pictures read (by the folder and their ref: each file read and hashed once, then kept), 256 MB at most.
struct Read {
    std::mutex mutex;
    std::map<std::string, core::Bytes> items;
    std::size_t bytes = 0;
};

Read& read_kept() {
    static Read r;
    return r;
}

void forget_read() {
    Read& r = read_kept();
    const std::lock_guard lock(r.mutex);
    r.items.clear();
    r.bytes = 0;
}

}  // namespace

core::Bytes picture(const fs::path& dir, const core::Pattern& p) {
    for (const Builtin& b : builtins()) {
        if (b.pattern.asset == p.asset) return b.bytes;
    }
    const std::string key = dir.string() + "|" + p.asset;
    Read& r = read_kept();
    {
        const std::lock_guard lock(r.mutex);
        if (const auto it = r.items.find(key); it != r.items.end()) return it->second;
    }
    try {
        const fs::path file = file_of(dir, p);
        if (!fs::exists(file)) return nullptr;
        std::string bytes = storage::read_file(file);
        if (core::bytes_ref(bytes) != p.asset) return nullptr;
        auto held = std::make_shared<const std::string>(std::move(bytes));
        const std::lock_guard lock(r.mutex);
        if (r.bytes + held->size() > (256u << 20)) {
            r.items.clear();
            r.bytes = 0;
        }
        r.items[key] = held;
        r.bytes += held->size();
        return held;
    } catch (const std::exception&) {
        return nullptr;
    }
}

preset_store::Library load(const fs::path& dir) { return preset_store::load(dir, kind()); }

void save(const fs::path& dir, const std::vector<preset_store::Group>& groups) { preset_store::save(dir, kind(), groups); }

Json add(const fs::path& dir, std::vector<preset_store::Group>& groups, core::Pattern p, const core::Bytes& bytes,
         std::optional<std::string_view> group) {
    if (!bytes) throw core::Error("value", "the pattern has no picture");
    if (const auto why = core::check_pattern_file(p, *bytes)) throw core::Error("value", "the pattern's picture: " + *why);
    const bool builtin = std::any_of(builtins().begin(), builtins().end(), [&](const Builtin& b) { return b.pattern.asset == p.asset; });
    if (!builtin) {
        const fs::path file = file_of(dir, p);
        // (written when it is not there — or when what is there is not its picture: a broken file of its name)
        bool there = fs::exists(file);
        if (there) {
            try {
                there = core::bytes_ref(storage::read_file(file)) == p.asset;
            } catch (const std::exception&) {
                there = false;
            }
        }
        if (!there) {
            fs::create_directories(file.parent_path());
            storage::write_atomic(file, *bytes);
            forget_read();
        }
    }
    const std::string name = p.name.empty() ? std::string("パターン") : p.name;
    return preset_store::add(groups, kind(), item_of(p, name), group, "パターン");
}

void tidy(const fs::path& dir, const std::vector<preset_store::Group>& groups) {
    std::set<std::string> named;
    for (const preset_store::Group& g : groups) {
        for (const Json& item : g.items) {
            try {
                const core::Pattern p = pattern_of(item);
                named.insert(hex_of(p.asset) + std::string(core::pattern_suffix(p)));
            } catch (const core::Error&) {
            }
        }
    }
    std::error_code ec;
    const fs::path folder = dir / "patterns";
    forget_read();
    if (!fs::is_directory(folder, ec)) return;
    for (const fs::directory_entry& e : fs::directory_iterator(folder, ec)) {
        const std::string leaf = e.path().filename().string();
        const bool ours = leaf.size() > 64 && (leaf.ends_with(".png") || leaf.ends_with(core::kColorRasterSuffix) || leaf.ends_with(core::kInkRasterSuffix));
        if (ours && !named.contains(leaf)) fs::remove(e.path(), ec);
    }
}

Json list(const std::vector<preset_store::Group>& groups) {
    Json out = Json::array();
    for (const preset_store::Group& g : groups) {
        Json items = Json::array();
        for (const Json& item : g.items) {
            Json one = Json::object();
            one["name"] = item.value("name", Json(""));
            for (const char* key : {"mode", "depth", "width", "height", "dpi"}) one[key] = item.value(key, Json());
            items.push_back(std::move(one));
        }
        out.push_back(Json{{"name", g.name}, {"patterns", std::move(items)}});
    }
    return Json{{"groups", std::move(out)}};
}

Json add_op(const core::Pattern& p, const core::Bytes& bytes) {
    Json op = Json::object();
    op["op"] = "add_pattern";
    op["name"] = p.name;
    const std::string_view suffix = core::pattern_suffix(p);
    op[suffix == ".png" ? "png" : suffix == core::kColorRasterSuffix ? "gkcr" : "gkir"] = core::b64encode(bytes ? *bytes : std::string());
    op["mode"] = p.mode;
    op["depth"] = p.depth;
    op["dpi"] = p.dpi;
    return op;
}

}  // namespace genko::render::pattern_library
