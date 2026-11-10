#include "render/asl.hpp"

#include <algorithm>
#include <cmath>
#include <set>

#include "core/error.hpp"
#include "core/layer_styles.hpp"
#include "core/patterns.hpp"
#include "core/pynum.hpp"
#include "core/pyops.hpp"
#include "render/psd_descriptor.hpp"
#include "render/psd_effects.hpp"
#include "render/psd_shapes.hpp"

namespace genko::render::asl {

using core::Json;
namespace d = psd::descriptor;

namespace {

const d::Object* object_at(const d::Object& o, std::string_view key) {
    const d::Value* v = o.get(key);
    return v != nullptr && v->object && (v->type == "Objc" || v->type == "GlbO") ? v->object.get() : nullptr;
}

// One style's data (its two descriptors) read.
Style style_of(std::string_view data, const psd_shapes::Placement& placement, std::vector<std::string>& notes, std::size_t index,
               std::size_t& budget) {
    Style out;
    d::Bytes b(data, budget);  // (the file's one budget: what each style takes spent from it)
    if (b.u32() != 16) throw d::Broken("its name's descriptor is not of version 16");
    const d::Object head = d::read_structure(b, 0);
    if (const d::Value* name = head.get("Nm  "); name != nullptr && name->type == "TEXT") out.name = psd::utf8_of(name->text);
    if (out.name.empty()) out.name = "スタイル " + std::to_string(index + 1);
    if (b.u32() != 16) throw d::Broken("its style's descriptor is not of version 16");
    const d::Object body = d::read_structure(b, 0);
    budget -= std::min(budget, b.spent());
    const d::Object* effects = object_at(body, "Lefx");
    std::vector<std::string> own;
    Json style = effects != nullptr ? psd_effects::effects_of(*effects, placement, own) : Json{{"effects", Json::array()}};
    if (const d::Object* blending = object_at(body, "blendOptions")) {
        if (const d::Value* mode = blending->get("Md  "); mode != nullptr && mode->type == "enum") {
            if (auto blend = psd_effects::blend_of_id(mode->enum_value)) {
                out.blend = *blend;
            } else {
                own.push_back("描画モード " + mode->enum_value + " は Genko にないので「通常」にしました");
                out.blend = "normal";
            }
        }
        for (const char* key : {"fillOpacity", "FlOp"}) {
            const d::Value* fill = blending->get(key);
            if (fill == nullptr) continue;
            if (const auto v = fill->numeric(); v && std::isfinite(*v)) {
                const double f = std::clamp(*v / 100.0, 0.0, 1.0);
                if (f < 1.0) style["fill_opacity"] = core::py_round(f, 4);
            }
            break;
        }
    }
    out.style = core::layer_style_spec(style);
    for (std::string& n : own) notes.push_back("「" + out.name + "」: " + n);
    return out;
}

}  // namespace

File read(std::string_view data, double dpi) {
    if (data.size() > kMaxFileBytes) {
        throw core::Error("too_large", "the .asl is larger than " + std::to_string(kMaxFileBytes >> 20) + " MB");
    }
    File out;
    d::Bytes b(data);
    std::size_t count = 0;
    try {
        const std::uint32_t version = b.u16();
        if (b.take(4) != "8BSL") throw core::Error("format", "not a Photoshop styles file (.asl): no 8BSL");
        if (version != 2) throw core::Error("format", "an .asl of version " + std::to_string(version) + " (2 is read)");
        b.u16();  // (the patterns' version: 3)
        const std::uint32_t patterns_length = b.u32();
        if (patterns_length > b.left()) throw d::Broken("its patterns end early");
        const std::string_view patterns = b.take(patterns_length);
        if (!patterns.empty()) out.patterns = pat::parse_block(patterns);
        count = b.u32();
        // (each style takes 4 bytes at least: a count past what is left is not believed, and nothing is made for it)
        if (count > kMaxStyles) throw core::Error("too_large", "the .asl holds " + std::to_string(count) + " styles (at most " + std::to_string(kMaxStyles) + ")");
        if (count * 4 > b.left()) throw d::Broken("its styles end early");
    } catch (const d::Broken& broken) {
        throw core::Error("format", std::string("the .asl cannot be read: ") + broken.what());
    }

    psd_shapes::Placement placement;
    placement.dpi = std::max(dpi, 1.0);
    placement.mm_x = placement.mm_y = 25.4 / placement.dpi;
    placement.width_px = placement.height_px = 1.0;
    placement.pattern = [&out, dpi](const std::string& id, const std::string& name) -> std::optional<std::pair<std::string, double>> {
        for (std::size_t i = 0; i < out.patterns.size(); ++i) {
            if (!id.empty() && out.patterns[i].id == id) return std::pair{std::string(kPatternPrefix) + std::to_string(i), dpi};
        }
        for (std::size_t i = 0; i < out.patterns.size(); ++i) {
            if (!name.empty() && out.patterns[i].name == name) return std::pair{std::string(kPatternPrefix) + std::to_string(i), dpi};
        }
        return std::nullopt;
    };

    out.styles.reserve(count);
    std::size_t budget = d::kUnpackedBudget;  // (one for the whole file: its descriptors past it refuse the file)
    for (std::size_t i = 0; i < count; ++i) {
        std::string_view chunk;
        try {
            const std::uint32_t length = b.u32();
            if (length > b.left()) throw d::Broken("a style ends early");
            chunk = b.take(length);
            // (its padding to 4 bytes, when its length does not count it)
            if (length % 4 != 0 && b.left() >= 4 - length % 4) b.take(4 - length % 4);
        } catch (const d::Broken& broken) {
            out.notes.push_back(std::string("スタイル ") + std::to_string(i + 1) + " から先を読めませんでした（" + broken.what() + "）");
            break;
        }
        try {
            out.styles.push_back(style_of(chunk, placement, out.notes, i, budget));
        } catch (const d::TooLarge& error) {
            throw core::Error("too_large", std::string("the .asl cannot be read: ") + error.what());
        } catch (const std::exception& error) {
            out.notes.push_back(std::string("スタイル ") + std::to_string(i + 1) + " を読めませんでした（" + error.what() + "）");
        }
    }
    return out;
}

std::string write(const std::vector<Style>& styles, const core::Document& book, double dpi) {
    // (the patterns the styles name, each once, in the order met)
    std::vector<pat::PsdPattern> patterns;
    std::set<std::string> seen;
    for (const Style& s : styles) {
        if (s.style.is_null()) continue;
        core::each_style_pattern(s.style, [&](const Json& fill, const std::string&) {
            const std::string id = fill.value("id", std::string());
            if (!seen.insert(id).second) return;
            const core::Pattern* p = core::find_pattern(book, id);
            if (p == nullptr) throw core::Error("value", "the book keeps no pattern " + id);
            const auto file = book.pattern_files.find(p->asset);
            if (file == book.pattern_files.end() || !file->second) throw core::Error("value", "the book holds no picture of the pattern " + id);
            patterns.push_back(pat::to_psd(*p, *file->second, pat::file_id(*p)));
        }, "");
    }
    psd_shapes::Canvas canvas;
    canvas.dpi = dpi;
    canvas.width_mm = canvas.height_mm = 25.4;
    canvas.width_px = canvas.height_px = std::max(1, static_cast<int>(dpi + 0.5));
    canvas.book = &book;

    std::string out;
    d::put_u16(out, 2);
    out += "8BSL";
    d::put_u16(out, 3);
    const std::string block = patterns.empty() ? std::string() : pat::write_block(patterns);
    d::put_u32(out, static_cast<std::uint32_t>(block.size()));
    out += block;
    d::put_u32(out, static_cast<std::uint32_t>(styles.size()));
    for (std::size_t i = 0; i < styles.size(); ++i) {
        const Style& s = styles[i];
        d::Writer head;
        head.text("Nm  ", psd::utf16(s.name));
        head.text("Idnt", psd::utf16("genko-style-" + std::to_string(i + 1)));
        d::Writer body;
        body.object("documentMode", d::Writer(), "documentMode");
        const Json style = s.style.is_null() ? Json{{"effects", Json::array()}} : s.style;
        body.object("Lefx", psd_effects::effects_descriptor(style, canvas), "Lefx");
        if (s.blend || core::style_fill_opacity(style) < 1.0) {
            d::Writer blending;
            if (s.blend) blending.enumerated("Md  ", "BlnM", psd_effects::blend_id(*s.blend));
            if (core::style_fill_opacity(style) < 1.0) blending.unit("fillOpacity", "#Prc", core::style_fill_opacity(style) * 100.0);
            body.object("blendOptions", blending, "blendOptions");
        }
        std::string data = head.versioned("null") + body.versioned("Styl");
        while (data.size() % 4 != 0) data.push_back('\0');
        d::put_u32(out, static_cast<std::uint32_t>(data.size()));
        out += data;
    }
    return out;
}

}  // namespace genko::render::asl
