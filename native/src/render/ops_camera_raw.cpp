// PC4 F-e (SPEC.md FILE-01「カメラ RAW」): set_camera_raw — a RAW smart object's development (camera-raw@1), its RAW
// file's bytes never changed: the contents' picture is made again from them (render/smart.cpp keeps the developed picture
// by the file and its development).

#include <algorithm>
#include <string>

#include "core/pyconv.hpp"
#include "core/pyops.hpp"
#include "core/raw_develop.hpp"
#include "core/smart.hpp"
#include "render/ops_formats.hpp"
#include "render/raw.hpp"
#include "render/smart.hpp"

namespace genko::render {

namespace {

using core::Json;
using core::OpError;

constexpr std::string_view kUsage =
    "set_camera_raw takes {contents: \"sc_…\" (a RAW smart object's contents), develop?: {white_balance?: \"as_shot\" | \"auto\" | \"custom\", "
    "temperature_k?: 2000..50000, tint?: -150..150, exposure_ev?: -5..5, demosaic?: \"ahd\" | \"aahd\" | \"dht\" | \"dcb\" | \"ppg\" | \"vng\" | "
    "\"linear\", highlights?: \"clip\" | \"unclip\" | \"blend\" | \"rebuild\", space?: \"prophoto-compat\" | \"adobe-rgb-compat\" | \"srgb\" | "
    "\"display-p3\", depth?: 8 | 16, orientation?: \"file\" | \"none\", sharpen?: \"none\" | \"screen\" | \"glossy\" | \"matte\"}, adjust?: "
    "camera_raw's settings | null}";

[[noreturn]] void refuse(const std::string& message) { throw OpError(message + " (" + std::string(kUsage) + ")"); }

}  // namespace

// The development given over the contents' own: develop's keys given replace theirs (the others kept), adjust's keys
// given replace theirs (null: none), the camera as it was. The picture's size follows its orientation, its depth the
// development's; its colours are the output space's (the developed picture carries its profile: the colour use
// "embedded"). {"contents", "raw": its normal form, "size_px", "depth"}.
void set_camera_raw(core::OpContext& c) {
    for (const auto& [key, value] : c.op.items()) {
        if (key != "op" && key != "contents" && key != "develop" && key != "adjust" && !key.starts_with("_")) refuse("unknown key " + core::py_repr_str(key));
    }
    const Json* id_value = core::get(c.op, "contents");
    if (id_value == nullptr || !id_value->is_string()) refuse("contents is a smart object's contents id (sc_…)");
    const std::string id = id_value->get<std::string>();
    core::Document& doc = c.doc;
    const auto found = doc.smart_contents.find(id);
    if (found == doc.smart_contents.end() || !found->second) throw core::OpKeyError(core::py_repr_str(id));
    const core::SmartContents& was = *found->second;
    if (was.kind != "file" || was.format != "raw") refuse("the smart object contents " + id + " are not a camera RAW file");
    // (what it has, as a book written by this build holds it; a book's own that this build cannot read was refused when read)
    Json now = was.raw ? *was.raw : Json::object();
    Json develop = core::raw::to_json(core::raw::develop_of(was.raw));
    if (const Json* given = core::get(c.op, "develop"); given != nullptr && !given->is_null()) {
        if (!given->is_object()) refuse("develop is an object");
        for (const auto& [key, value] : given->items()) develop[key] = value;
        if (given->contains("white_balance") && (*given)["white_balance"] != Json("custom")) {
            develop.erase("temperature_k");
            develop.erase("tint");
        }
    }
    Json next = Json::object();
    next["develop"] = develop;
    if (const Json* given = core::get(c.op, "adjust"); given != nullptr) {
        if (!given->is_null()) {
            if (!given->is_object()) refuse("adjust is camera_raw's settings (an object) or null");
            Json adjust = core::raw::adjust_of(was.raw).value_or(Json::object());
            for (const auto& [key, value] : given->items()) adjust[key] = value;
            next["adjust"] = adjust;
        }
    } else if (const auto kept = core::raw::adjust_of(was.raw)) {
        next["adjust"] = *kept;
    }
    if (const Json* camera = core::get(now, "camera"); camera != nullptr) next["camera"] = *camera;
    try {
        next = core::raw::normal_raw(next);
    } catch (const OpError& error) {
        refuse(error.what());
    }
    const core::raw::Develop d = core::raw::develop_from(core::get(next, "develop"));
    // (its size as the development turns it: the RAW's header read, nothing decoded)
    const core::Bytes bytes = smart::file_bytes(doc, was);
    if (!bytes) throw OpError("the smart object's file is not in the book");
    raw::Info info;
    try {
        info = raw::identify(*bytes);
    } catch (const core::Error& error) {
        throw OpError(std::string("the camera RAW file cannot be read (") + error.what() + ")");
    }
    const auto [w, h] = raw::developed_size(info, d);
    core::SmartContents& contents = doc.edit_contents(id);
    contents.raw = next;
    contents.width_px = w;
    contents.height_px = h;
    contents.depth = d.depth;
    contents.mode = "rgb";
    contents.profile = Json::object({{"use", "embedded"}, {"intent", "relative"}, {"bpc", true}});
    // (a PSD's picture of the smart objects showing it is no longer theirs: drawn from the file again)
    const auto drop_shown = [&](core::Page& on) {
        for (core::Layer& l : on.layers) {
            if (core::smart::contents_id(l) != id || !l.smart || !l.smart->contains("psd") || !(*l.smart)["psd"].is_object()) continue;
            (*l.smart)["psd"].erase("shown");
        }
    };
    for (const std::size_t i : core::smart::pages_using(doc, id)) drop_shown(doc.edit_page(i));
    c.report = Json{{"contents", id}, {"raw", next}, {"size_px", Json::array({w, h})}, {"depth", d.depth}};
}

}  // namespace genko::render
