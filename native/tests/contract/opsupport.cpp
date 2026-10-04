#include "opsupport.hpp"

#include <QByteArray>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>

#include <algorithm>
#include <bit>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <vector>

#include "core/actor.hpp"
#include "core/base64.hpp"
#include "core/command_bus.hpp"
#include "core/error.hpp"
#include "core/ids.hpp"
#include "render/ops_registry.hpp"
#include "storage/lock.hpp"
#include "storage/reader.hpp"
#include "core/paths.hpp"
#include "render/png.hpp"
#include "storage/snapshot.hpp"
#include "storage/transaction.hpp"
#include "storage/writer.hpp"
#include "testsupport.hpp"

namespace genko::test {

using core::Json;

Json as_v3_payload(Json payload) {
    for (const char* key : {"min_reader", "writer", "book_id", "features", "revision"}) payload.erase(key);
    payload["version"] = 3;
    return payload;
}

std::string json_digest(const Json& value) {
    const std::string text = core::dump_python(value);
    return QCryptographicHash::hash(QByteArray::fromStdString(text), QCryptographicHash::Sha256).toHex().toStdString();
}

std::string pixels_digest(const std::optional<std::string>& bytes) {
    if (!bytes) return "missing";
    QCryptographicHash hash(QCryptographicHash::Sha256);
    try {
        const render::Image image = render::open_image(*bytes, render::kPillowOpenLimits);
        const std::string rgba = image.convert("RGBA").tobytes();
        const std::string head = std::string(image.mode()) + "|" + std::to_string(image.width()) + "x" + std::to_string(image.height()) + "|";
        hash.addData(QByteArrayView(head.data(), static_cast<qsizetype>(head.size())));
        hash.addData(QByteArrayView(rgba.data(), static_cast<qsizetype>(rgba.size())));
        return "png:" + hash.result().toHex().toStdString();
    } catch (const core::Error&) {
        return "unreadable:" + QCryptographicHash::hash(QByteArray::fromStdString(*bytes), QCryptographicHash::Sha256).toHex().toStdString();
    }
}

Json with_png_pixels(Json payload, const storage::AssetStore& store) {
    if (!payload.is_object() || !payload.contains("pages") || !payload["pages"].is_array()) return payload;
    for (Json& page : payload["pages"]) {
        if (!page.is_object()) continue;
        if (page.contains("layers") && page["layers"].is_array()) {
            for (Json& layer : page["layers"]) {
                if (!layer.is_object() || layer.value("kind", Json()) == Json("placed")) continue;
                if (layer.contains("asset") && layer["asset"].is_string()) {
                    layer["asset"] = pixels_digest(store.get_bytes(layer["asset"].get<std::string>(), ".png"));
                }
                if (layer.contains("mask") && layer["mask"].is_object() && layer["mask"].contains("asset") &&
                    layer["mask"]["asset"].is_string()) {
                    layer["mask"]["asset"] = pixels_digest(store.get_bytes(layer["mask"]["asset"].get<std::string>(), ".png"));
                }
                if (layer.contains("patches") && layer["patches"].is_array()) {
                    for (Json& patch : layer["patches"]) {
                        if (patch.is_object() && patch.contains("asset") && patch["asset"].is_string()) {
                            patch["asset"] = pixels_digest(store.get_bytes(patch["asset"].get<std::string>(), ".png"));
                        }
                    }
                }
            }
        }
        if (page.contains("saved_areas") && page["saved_areas"].is_object()) {
            for (auto& [name, area] : page["saved_areas"].items()) {
                if (!area.is_object() || !area.contains("mask") || !area["mask"].is_object()) continue;
                Json& mask = area["mask"];
                if (!mask.contains("png") || !mask["png"].is_string()) continue;
                try {
                    mask["png"] = pixels_digest(core::a2b_base64(mask["png"].get<std::string>()));
                } catch (const core::Error&) {
                }
            }
        }
    }
    return payload;
}

StepOutcome state_of(const core::Document& doc, storage::AssetStore& store) {
    StepOutcome out;
    out.full = storage::snapshot(doc, true);
    out.payload = with_png_pixels(as_v3_payload(storage::project_payload_v4(doc, store)), store);
    return out;
}

std::vector<StepOutcome> run_steps(core::Document doc, const Json& steps, std::uint64_t first_id, storage::AssetStore& store,
                                   bool digest, core::Document* last,
                                   const std::function<void(core::Document&, std::size_t, const StepOutcome&)>& after_step) {
    const core::ScopedIdSource ids(core::counting_ids(first_id));
    std::vector<StepOutcome> out;
    for (const Json& step : steps) {
        StepOutcome outcome;
        const std::string agent = step.contains("agent") ? step["agent"].get<std::string>() : std::string("genko");
        const bool dry_run = step.contains("dry_run") && step["dry_run"].get<bool>();
        try {
            core::ApplyResult result = core::CommandBus(render::ops_registry()).apply(doc, step["ops"], core::Actor(agent), dry_run);
            Json reply = Json::object();
            reply["ok"] = true;
            reply["applied"] = result.applied;
            reply["snapshot"] = storage::snapshot(result.doc);
            reply["job_id"] = core::new_id();
            if (result.has_warnings) reply["warnings"] = result.warnings;
            if (!result.results.empty()) reply["results"] = result.results;
            if (!dry_run) doc = std::move(result.doc);
            outcome.reply = std::move(reply);
        } catch (const core::ApplyError& error) {
            outcome.reply = Json::object({{"ok", false}, {"error", error.what()}});
            outcome.code = error.code();
        }
        StepOutcome state = state_of(doc, store);
        outcome.full = std::move(state.full);
        outcome.payload = std::move(state.payload);
        if (digest) {
            if (outcome.reply.contains("snapshot")) outcome.reply["snapshot"] = json_digest(outcome.reply["snapshot"]);
            outcome.full = json_digest(outcome.full);
            outcome.payload = json_digest(outcome.payload);
        }
        if (after_step) after_step(doc, out.size(), outcome);
        out.push_back(std::move(outcome));
    }
    if (last != nullptr) *last = std::move(doc);
    return out;
}

std::string without_addresses(std::string text) {
    // "<_io.BytesIO object at 0x7f3a…>" → "<_io.BytesIO object>": the address of an object in Python's process
    for (std::size_t at = text.find(" at 0x"); at != std::string::npos; at = text.find(" at 0x", at)) {
        std::size_t end = at + 6;
        while (end < text.size() && std::isxdigit(static_cast<unsigned char>(text[end])) != 0) ++end;
        if (end < text.size() && text[end] == '>' && end > at + 6) {
            text.erase(at, end - at);
        } else {
            at = end;
        }
    }
    return text;
}

void dump_pictures(const core::Document& doc, storage::AssetStore& store, const QString& dir, const std::string& prefix) {
    const Json payload = as_v3_payload(storage::project_payload_v4(doc, store));
    QDir().mkpath(dir);
    const auto write = [&](const Json& asset, const std::string& name) {
        if (!asset.is_string()) return;
        const auto bytes = store.get_bytes(asset.get<std::string>(), ".png");
        if (bytes) write_bytes(dir + QLatin1Char('/') + QString::fromStdString(prefix + name + ".png"), *bytes);
    };
    if (!payload.contains("pages") || !payload["pages"].is_array()) return;
    for (std::size_t p = 0; p < payload["pages"].size(); ++p) {
        const Json& page = payload["pages"][p];
        if (!page.is_object() || !page.contains("layers") || !page["layers"].is_array()) continue;
        for (std::size_t k = 0; k < page["layers"].size(); ++k) {
            const Json& layer = page["layers"][k];
            if (!layer.is_object() || layer.value("kind", Json()) == Json("placed")) continue;
            const std::string at = "p" + std::to_string(p) + "-l" + std::to_string(k) + "-";
            if (layer.contains("asset")) write(layer["asset"], at + "asset");
            if (layer.contains("mask") && layer["mask"].is_object() && layer["mask"].contains("asset")) {
                write(layer["mask"]["asset"], at + "mask");
            }
            if (layer.contains("patches") && layer["patches"].is_array()) {
                for (std::size_t n = 0; n < layer["patches"].size(); ++n) {
                    const Json& patch = layer["patches"][n];
                    if (patch.is_object() && patch.contains("asset")) write(patch["asset"], at + "patch" + std::to_string(n));
                }
            }
        }
    }
}

std::string picture_differences(const QString& cpp_dir, const QString& py_dir, const std::string& prefix) {
    const QStringList filter{QString::fromStdString(prefix) + QStringLiteral("*.png")};
    QStringList names = QDir(py_dir).entryList(filter, QDir::Files, QDir::Name);
    for (const QString& name : QDir(cpp_dir).entryList(filter, QDir::Files, QDir::Name)) {
        if (!names.contains(name)) names.append(name);
    }
    std::string out;
    for (const QString& name : names) {
        const QString a = cpp_dir + QLatin1Char('/') + name;
        const QString b = py_dir + QLatin1Char('/') + name;
        const std::string what = name.toStdString();
        if (!QFile::exists(a) || !QFile::exists(b)) {
            out += what + (QFile::exists(a) ? ": only C++ has it; " : ": only Python has it; ");
            continue;
        }
        try {
            const render::Image x = render::read_png(read_bytes(a), render::kPillowOpenLimits);
            const render::Image y = render::read_png(read_bytes(b), render::kPillowOpenLimits);
            const std::string mode_x(x.mode());
            const std::string mode_y(y.mode());
            if (mode_x != mode_y || x.width() != y.width() || x.height() != y.height()) {
                out += what + ": C++ " + mode_x + " " + std::to_string(x.width()) + "x" + std::to_string(x.height()) + ", Python " +
                       mode_y + " " + std::to_string(y.width()) + "x" + std::to_string(y.height()) + "; ";
                continue;
            }
            const std::string p = x.convert("RGBA").tobytes();
            const std::string q = y.convert("RGBA").tobytes();
            long differing = 0;
            int most = 0;
            int x0 = x.width(), y0 = x.height(), x1 = -1, y1 = -1;
            for (std::size_t i = 0; i < p.size(); i += 4) {
                int worst = 0;
                for (std::size_t c = 0; c < 4; ++c) {
                    worst = std::max(worst, std::abs(static_cast<int>(static_cast<unsigned char>(p[i + c])) -
                                                     static_cast<int>(static_cast<unsigned char>(q[i + c]))));
                }
                if (worst == 0) continue;
                ++differing;
                most = std::max(most, worst);
                const int px = static_cast<int>((i / 4) % static_cast<std::size_t>(x.width()));
                const int py = static_cast<int>((i / 4) / static_cast<std::size_t>(x.width()));
                x0 = std::min(x0, px);
                y0 = std::min(y0, py);
                x1 = std::max(x1, px);
                y1 = std::max(y1, py);
            }
            if (differing > 0) {
                out += what + " (" + mode_x + " " + std::to_string(x.width()) + "x" + std::to_string(x.height()) + "): " +
                       std::to_string(differing) + " pixels differ, by up to " + std::to_string(most) + ", within (" +
                       std::to_string(x0) + ", " + std::to_string(y0) + ")–(" + std::to_string(x1) + ", " + std::to_string(y1) + "); ";
            }
        } catch (const core::Error& error) {
            out += what + ": " + error.what() + "; ";
        }
    }
    return out;
}

namespace {

// ARCHITECTURE.md §9 for two pictures the dumps hold: "" when within it
std::string pictures_near(const QString& cpp_file, const QString& py_file) {
    if (!QFile::exists(cpp_file) || !QFile::exists(py_file)) return "a picture is missing";
    try {
        const render::Image x = render::read_png(read_bytes(cpp_file), render::kPillowOpenLimits);
        const render::Image y = render::read_png(read_bytes(py_file), render::kPillowOpenLimits);
        if (std::string(x.mode()) != std::string(y.mode()) || x.width() != y.width() || x.height() != y.height()) {
            return "the pictures differ in mode or size";
        }
        const std::string p = x.convert("RGBA").tobytes();
        const std::string q = y.convert("RGBA").tobytes();
        long long total = 0, far = 0;
        for (std::size_t i = 0; i < p.size(); i += 4) {
            int worst = 0;
            for (std::size_t c = 0; c < 4; ++c) {
                const int d = std::abs(static_cast<int>(static_cast<unsigned char>(p[i + c])) - static_cast<int>(static_cast<unsigned char>(q[i + c])));
                total += d;
                worst = std::max(worst, d);
            }
            if (worst > 32) ++far;
        }
        const double values = static_cast<double>(p.size());
        const double pixels = values / 4;
        if (values > 0 && static_cast<double>(total) / values > 2.0) return "the pictures differ by more than 2/255 on average";
        if (pixels > 0 && static_cast<double>(far) > 0.01 * pixels) return "more than 1% of the pixels differ by more than 32/255";
        if (p == q) return {};
        // Capacity-one local correspondence: a surviving sample cannot pay for
        // several lost samples, and remote growth cannot compensate local loss.
        // Seed same-position matches, then use iterative augmenting paths for
        // the remaining samples. Retain all existing error and mass guards.
        const std::size_t count = p.size()/4;
        if (count > static_cast<std::size_t>(std::numeric_limits<int>::max())) return "too many pixels for local correspondence";
        // The one-to-one guard additionally bounds matched alpha and premultiplied
        // colour differences to 2/255. It must not map faint ink to the background.
        const auto compatible = [&](int left, int right) {
            const auto i = static_cast<std::size_t>(left)*4, j = static_cast<std::size_t>(right)*4;
            const auto alpha_a = static_cast<unsigned char>(p[i+3]), alpha_b = static_cast<unsigned char>(q[j+3]);
            if (std::abs(static_cast<int>(alpha_a)-static_cast<int>(alpha_b)) > 2) return false;
            for (std::size_t c = 0; c < 3; ++c) {
                const int a = static_cast<unsigned char>(p[i+c])*alpha_a;
                const int b = static_cast<unsigned char>(q[j+c])*alpha_b;
                if (std::abs(a-b) > 2*255) return false;
            }
            return true;
        };
        std::vector<int> left_match(count, -1), right_match(count, -1), parent(count, -1);
        std::vector<std::uint32_t> correspondence_seen(count, 0);
        std::vector<int> pending;
        for (std::size_t i = 0; i < count; ++i) {
            const int pixel = static_cast<int>(i);
            if (compatible(pixel, pixel)) left_match[i] = right_match[i] = pixel;
        }
        std::uint32_t epoch = 0;
        for (std::size_t root = 0; root < count; ++root) {
            if (left_match[root] >= 0) continue;
            ++epoch;
            pending.clear();
            pending.push_back(static_cast<int>(root));
            correspondence_seen[root] = epoch;
            parent[root] = -1;
            bool found = false;
            for (std::size_t k = 0; k < pending.size() && !found; ++k) {
                const int left = pending[k], row = left/x.width(), col = left%x.width();
                for (int dy = -1; dy <= 1 && !found; ++dy) {
                    const int yy = row+dy;
                    if (yy < 0 || yy >= x.height()) continue;
                    for (int dx = -1; dx <= 1 && !found; ++dx) {
                        const int xx = col+dx;
                        if (xx < 0 || xx >= x.width()) continue;
                        const int right = yy*x.width()+xx;
                        if (!compatible(left, right)) continue;
                        const int owner = right_match[static_cast<std::size_t>(right)];
                        if (owner < 0) {
                            int l = left, r = right;
                            while (l >= 0) {
                                const auto index = static_cast<std::size_t>(l);
                                const int old = left_match[index];
                                left_match[index] = r;
                                right_match[static_cast<std::size_t>(r)] = l;
                                r = old;
                                l = parent[index];
                            }
                            found = true;
                        } else if (correspondence_seen[static_cast<std::size_t>(owner)] != epoch) {
                            const auto index = static_cast<std::size_t>(owner);
                            correspondence_seen[index] = epoch;
                            parent[index] = left;
                            pending.push_back(owner);
                        }
                    }
                }
            }
            if (!found) return "local pixel correspondence is not one-to-one";
        }
        // A global mean alone can accept a vanished one-pixel mark. Every visible
        // sample must have a comparable sample within the documented one-pixel
        // displacement, in both directions. Hidden RGB under zero alpha is ignored.
        const auto retained = [&](const std::string& a, const std::string& b) {
            for (int row = 0; row < x.height(); ++row) {
                for (int col = 0; col < x.width(); ++col) {
                    const std::size_t i = (static_cast<std::size_t>(row) * x.width() + col) * 4;
                    const auto av = [&](std::size_t c) { return static_cast<unsigned char>(a[i + c]); };
                    if (av(3) <= 32) continue;
                    bool match = false;
                    for (int dy = -1; dy <= 1 && !match; ++dy) {
                        const int yy = row + dy;
                        if (yy < 0 || yy >= x.height()) continue;
                        for (int dx = -1; dx <= 1 && !match; ++dx) {
                            const int xx = col + dx;
                            if (xx < 0 || xx >= x.width()) continue;
                            const std::size_t j = (static_cast<std::size_t>(yy) * x.width() + xx) * 4;
                            match = true;
                            for (std::size_t c = 0; c < 4; ++c) {
                                if (std::abs(static_cast<int>(av(c)) - static_cast<int>(static_cast<unsigned char>(b[j + c]))) > 32) {
                                    match = false;
                                    break;
                                }
                            }
                        }
                    }
                    if (!match) return false;
                }
            }
            return true;
        };
        if (!retained(p, q) || !retained(q, p)) return "a local mark, line or mask is missing or displaced";
        // Neighbour matches are not one-to-one. Preserve coverage/ink mass around
        // each connected component as well, so adjacent samples cannot both use
        // one surviving pixel. Compare the same one-pixel-expanded window on both
        // sides (including neighbours), keeping a legitimate one-pixel shift.
        const bool mask = x.mode() == "L" || x.mode() == "1";
        const auto mass = [&](const std::string& data, std::size_t pixel, int metric) -> std::uint64_t {
            const std::size_t i = pixel*4;
            const unsigned r = static_cast<unsigned char>(data[i]);
            const unsigned g = static_cast<unsigned char>(data[i+1]);
            const unsigned b = static_cast<unsigned char>(data[i+2]);
            const unsigned a = static_cast<unsigned char>(data[i+3]);
            if (mask) return metric == 0 ? r : 255-r;
            return metric == 0 ? a : static_cast<std::uint64_t>(765-r-g-b)*a;
        };
        const auto components_retained = [&](const std::string& a, const std::string& b, int metric) {
            const std::size_t n = a.size()/4;
            std::vector<unsigned char> seen(n, 0);
            std::vector<std::size_t> queue;
            for (std::size_t seed = 0; seed < n; ++seed) {
                if (seen[seed] || mass(a, seed, metric) == 0) continue;
                queue.clear();
                queue.push_back(seed);
                seen[seed] = 1;
                int left = x.width(), right = -1, top = x.height(), bottom = -1;
                for (std::size_t k = 0; k < queue.size(); ++k) {
                    const auto pixel = queue[k];
                    const int col = static_cast<int>(pixel % x.width());
                    const int row = static_cast<int>(pixel / x.width());
                    left = std::min(left, col); right = std::max(right, col);
                    top = std::min(top, row); bottom = std::max(bottom, row);
                    for (const auto& [dx, dy] : {std::pair{-1,0}, {1,0}, {0,-1}, {0,1}}) {
                        const int xx = col+dx, yy = row+dy;
                        if (xx < 0 || xx >= x.width() || yy < 0 || yy >= x.height()) continue;
                        const auto next = static_cast<std::size_t>(yy)*x.width()+xx;
                        if (!seen[next] && mass(a, next, metric) != 0) {
                            seen[next] = 1;
                            queue.push_back(next);
                        }
                    }
                }
                std::uint64_t am = 0, bm = 0;
                for (int row = std::max(0, top-1); row <= std::min(x.height()-1, bottom+1); ++row) {
                    for (int col = std::max(0, left-1); col <= std::min(x.width()-1, right+1); ++col) {
                        const auto pixel = static_cast<std::size_t>(row)*x.width()+col;
                        am += mass(a, pixel, metric);
                        bm += mass(b, pixel, metric);
                    }
                }
                const auto difference = am > bm ? am-bm : bm-am;
                // Whole-image means can hide loss in a small component.
                // Check local alpha/ink mass without relaxing the pixel guards.
                if (static_cast<long double>(difference) > 0.01L*std::max(am, bm)) return false;
            }
            return true;
        };
        for (const int metric : {0, 1}) {
            if (!components_retained(p, q, metric) || !components_retained(q, p, metric)) {
                return "a local mark's alpha or ink mass was lost";
            }
        }
    } catch (const core::Error& error) {
        return std::string("a picture cannot be read: ") + error.what();
    }
    return {};
}

class Near {
public:
    Near(const NearSides& sides, int* tolerated) : sides_(sides), tolerated_(tolerated) {}

    std::string walk(const Json& a, const Json& b, const std::string& at) {
        if (a.is_number() && b.is_number() && !a.is_boolean() && !b.is_boolean()) {
            if (a.is_number_float() != b.is_number_float()) return at + ": an int and a float";
            if (!a.is_number_float()) return a == b ? std::string() : at + ": ints differ";
            const double x = a.get<double>();
            const double y = b.get<double>();
            if (!allowed(sides_.numeric_paths, at)) {
                return strict_equal(a, b) ? std::string() : at + ": floats differ outside the perspective target";
            }
            if (x == y || (std::isnan(x) && std::isnan(y))) return {};
            if (!std::isfinite(x) || !std::isfinite(y) || std::fabs(x - y) > 1e-9 * std::max({1.0, std::fabs(x), std::fabs(y)})) {
                return at + ": floats differ: " + core::dump_python(a) + " != " + core::dump_python(b);
            }
            count();
            return {};
        }
        if (a.type() != b.type()) return at + ": " + core::dump_python(a).substr(0, 200) + " != " + core::dump_python(b).substr(0, 200);
        if (a.is_object()) {
            if (a.size() != b.size()) return at + ": the keys differ";
            auto i = a.begin();
            auto j = b.begin();
            for (; i != a.end(); ++i, ++j) {
                if (i.key() != j.key()) return at + ": the keys differ (" + i.key() + ", " + j.key() + ")";
                const std::string here = at + "/" + i.key();
                if (in_lines_ && (i.key() == "xy" || i.key() == "p" || i.key() == "r") && i.value().is_string() &&
                    j.value().is_string()) {
                    if (std::string d = packed(i.value().get<std::string>(), j.value().get<std::string>(), here); !d.empty()) return d;
                    continue;
                }
                if (i.key() == "strokes_blob" && i.value().is_array() && j.value().is_array()) {
                    const bool previous = in_lines_;
                    in_lines_ = true;
                    std::string d = walk(i.value(), j.value(), here);
                    in_lines_ = previous;
                    if (!d.empty()) return d;
                    continue;
                }
                if (std::string d = walk(i.value(), j.value(), here); !d.empty()) return d;
            }
            return {};
        }
        if (a.is_array()) {
            if (a.size() != b.size()) return at + ": lengths differ";
            for (std::size_t k = 0; k < a.size(); ++k) {
                if (std::string d = walk(a[k], b[k], at + "/" + std::to_string(k)); !d.empty()) return d;
            }
            return {};
        }
        if (a == b) return {};
        if (!a.is_string()) return at + ": " + core::dump_python(a) + " != " + core::dump_python(b);
        const std::string x = a.get<std::string>();
        const std::string y = b.get<std::string>();
        if (x.starts_with("png:") && y.starts_with("png:")) {
            if (!allowed(sides_.picture_paths, at)) return at + ": pixels differ outside the perspective target";
            return picture(at);
        }
        if (storage::AssetStore::is_ref(x) && storage::AssetStore::is_ref(y) && sides_.cpp_store != nullptr && at.ends_with("/strokes_blob")) {
            // a layer's lines (models.stroke_to_packed): what each side wrote, compared the same way
            const auto mine = sides_.cpp_store->get_bytes(x, ".strokes.json");
            const auto theirs = storage::AssetStore(core::path_from_utf8(sides_.py_store.toStdString())).get_bytes(y, ".strokes.json");
            if (!mine || !theirs) return at + ": an asset is missing";
            in_lines_ = true;
            std::string d = walk(Json::parse(*mine), Json::parse(*theirs), at + " (its lines)");
            in_lines_ = false;
            if (d.empty()) count();
            return d;
        }
        return at + ": strings differ: " + core::dump_python(a).substr(0, 200) + " != " + core::dump_python(b).substr(0, 200);
    }

private:
    static bool allowed(const std::vector<std::string>& paths, std::string at) {
        if (at.starts_with("/snapshot/")) at.erase(0, 9);
        for (const std::string& path : paths) {
            if (at == path || at.starts_with(path + "/") || at.starts_with(path + " (its lines)")) return true;
        }
        return false;
    }

    void count() {
        if (tolerated_ != nullptr) ++*tolerated_;
    }

    // numbers packed as little-endian float64, base64 (a line's points, pressures, rotations)
    std::string packed(const std::string& a, const std::string& b, const std::string& at) {
        if (a == b) return {};
        if (!at.ends_with("/xy")) return at + ": pressure or rotation differs";
        std::string x;
        std::string y;
        try {
            x = core::a2b_base64(a);
            y = core::a2b_base64(b);
        } catch (const core::Error&) {
            return at + ": not base64";
        }
        if (x.size() != y.size() || x.size() % 8 != 0) return at + ": the lengths differ";
        for (std::size_t i = 0; i < x.size(); i += 8) {
            std::uint64_t u = 0;
            std::uint64_t v = 0;
            for (int k = 7; k >= 0; --k) {
                u = (u << 8) | static_cast<unsigned char>(x[i + static_cast<std::size_t>(k)]);
                v = (v << 8) | static_cast<unsigned char>(y[i + static_cast<std::size_t>(k)]);
            }
            if (std::string d = walk(Json(std::bit_cast<double>(u)), Json(std::bit_cast<double>(v)), at + "/" + std::to_string(i / 8));
                !d.empty()) {
                return d;
            }
        }
        return {};
    }

    bool in_lines_ = false;

    // a picture the payload refers to: its place ("/pages/P/layers/K/asset", ".../mask/asset", ".../patches/N/asset")
    // names the dumped files
    std::string picture(const std::string& at) {
        std::vector<std::string> parts;
        for (std::size_t from = 1; from <= at.size();) {
            const std::size_t to = at.find('/', from);
            parts.push_back(at.substr(from, to == std::string::npos ? std::string::npos : to - from));
            if (to == std::string::npos) break;
            from = to + 1;
        }
        std::string name;
        if (parts.size() >= 5 && parts[0] == "pages" && parts[2] == "layers") {
            const std::string base = "p" + parts[1] + "-l" + parts[3] + "-";
            if (parts.size() == 5 && parts[4] == "asset") name = base + "asset";
            if (parts.size() == 6 && parts[4] == "mask" && parts[5] == "asset") name = base + "mask";
            if (parts.size() == 7 && parts[4] == "patches" && parts[6] == "asset") name = base + "patch" + parts[5];
        }
        if (name.empty()) return at + ": the pictures differ";
        const QString file = QString::fromStdString(sides_.prefix + name + ".png");
        const std::string d = pictures_near(sides_.cpp_dump + QLatin1Char('/') + file, sides_.py_dump + QLatin1Char('/') + file);
        if (!d.empty()) return at + ": " + d;
        count();
        return {};
    }

    const NearSides& sides_;
    int* tolerated_;
};

}  // namespace

std::string step_coverage_error(const Json& sequences, const std::vector<std::size_t>& taken,
    const std::set<std::pair<std::size_t, std::size_t>>& compared, std::size_t steps, std::size_t ops) {
    if (!sequences.is_array() || taken.empty()) return "no generated sequences to compare";
    const std::set<std::size_t> chosen(taken.begin(), taken.end());
    if (chosen.size() != taken.size()) return "a sequence was selected twice";
    std::set<std::pair<std::size_t, std::size_t>> expected;
    std::size_t expected_ops = 0;
    for (const std::size_t n : taken) {
        if (n >= sequences.size()) return "selected sequence is out of range";
        const auto& sequence = sequences[n];
        if (!sequence.is_object() || !sequence.contains("steps") || !sequence["steps"].is_array()) return "generated steps must be an array";
        for (std::size_t s = 0; s < sequence["steps"].size(); ++s) {
            const auto& step = sequence["steps"][s];
            if (!step.is_object() || !step.contains("ops") || !step["ops"].is_array()) return "generated batch operations must be an array";
            expected.emplace(n, s);
            expected_ops += step["ops"].size();
        }
    }
    if (expected != compared) return "generated steps were not all compared exactly once";
    if (steps != expected.size()) return "logical comparison count differs from generated steps";
    if (ops != expected_ops) return "compared batch operation count differs from generated input";
    return {};
}

std::string compare_picture_near(const QString& cpp_file, const QString& py_file) {
    return pictures_near(cpp_file, py_file);
}

void affect_perspective(NearSides& sides, const Json& ops, const Json& payload) {
    if (!ops.is_array() || !payload.contains("pages")) return;
    for (const Json& op : ops) {
        if (!warps_in_perspective(Json::array({op}))) continue;
        for (std::size_t p = 0; p < payload["pages"].size(); ++p) {
            const Json& page = payload["pages"][p];
            if (op.value("page", Json()) != page.value("index", Json())) continue;
            const std::string wanted = op.value("layer_id", std::string());
            const std::string role = op.value("layer", std::string("ink"));
            bool found = false;
            for (std::size_t k = 0; k < page["layers"].size(); ++k) {
                const Json& layer = page["layers"][k];
                const bool match = wanted.empty() ? !found && layer.value("role", std::string()) == role
                                                  : layer.value("id", std::string()) == wanted;
                if (!match) continue;
                found = true;
                const std::string base = "/pages/" + std::to_string(p) + "/layers/" + std::to_string(k);
                sides.picture_paths.push_back(base + "/asset");
                sides.picture_paths.push_back(base + "/patches");
                sides.numeric_paths.push_back(base + "/patches");
                sides.numeric_paths.push_back(base + "/strokes_blob");
                const std::string r = layer.value("role", std::string());
                if (r == "ink" || r == "name") {
                    sides.numeric_paths.push_back("/pages/" + std::to_string(p) + "/" + r + "_strokes");
                }
            }
        }
    }
}

bool warps_in_perspective(const Json& ops) {
    if (!ops.is_array()) return false;
    for (const Json& op : ops) {
        if (!op.is_object() || op.value("op", Json()) != Json("transform_area") || !op.contains("warp")) continue;
        const Json& warp = op["warp"];
        if (warp.is_object() && warp.contains("perspective") && !warp["perspective"].is_null()) return true;
    }
    return false;
}

std::string compare_step_near(const StepOutcome& cpp, const Json& python, const NearSides& sides, int* tolerated) {
    Json py_reply = python["reply"];
    if (py_reply.contains("uncaught") || cpp.reply.value("ok", false) != py_reply.value("ok", false)) {
        return compare_step(cpp, python);  // (the words of a failure are compared as they are)
    }
    if (py_reply.contains("error") && py_reply["error"].is_string()) py_reply["error"] = without_addresses(py_reply["error"].get<std::string>());
    Near near(sides, tolerated);
    if (std::string d = near.walk(cpp.reply, py_reply, ""); !d.empty()) return "reply: " + d;
    if (std::string d = near.walk(cpp.full, python["full"], ""); !d.empty()) return "full snapshot: " + d;
    if (std::string d = near.walk(cpp.payload, python["payload"], ""); !d.empty()) return "payload: " + d;
    return {};
}

std::string compare_step(const StepOutcome& cpp, const Json& python) {
    std::string where;
    Json py_reply = python["reply"];
    if (py_reply.contains("error") && py_reply["error"].is_string()) py_reply["error"] = without_addresses(py_reply["error"].get<std::string>());
    if (py_reply.contains("uncaught")) {
        // (Python's command line prints a ValueError's message as its error, and stops with a traceback on the others:
        // its last line is the type alone when the exception has no message)
        const std::string type = py_reply["uncaught"].get<std::string>();
        const std::string message = py_reply["error"].get<std::string>();
        const std::string want = message.empty() ? type : type + ": " + message;
        const std::string got = cpp.reply.value("error", std::string());
        if (cpp.code != "python_error" || !(type == "ValueError" ? got == message : got.ends_with(want))) {
            return "Python stopped with " + want + "; C++ gave " + core::dump_python(cpp.reply) + " (" + cpp.code + ")";
        }
    } else if (cpp.reply.value("ok", false) != py_reply.value("ok", false)) {
        return "C++ gave " + core::dump_python(cpp.reply).substr(0, 600) + " (" + cpp.code + "), Python " +
               core::dump_python(py_reply).substr(0, 600);
    } else if (!strict_equal(cpp.reply, py_reply, &where)) {
        return "reply: " + where;
    }
    if (!strict_equal(cpp.full, python["full"], &where)) return "full snapshot: " + where;
    if (!strict_equal(cpp.payload, python["payload"], &where)) return "payload: " + where;
    return {};
}

std::string read_back_difference(const core::Document& doc, const std::filesystem::path& dir, storage::AssetStore& store,
                                 const Json& reread, ReadBackNotes& notes) {
    StepOutcome before = state_of(doc, store);
    std::filesystem::create_directories(dir);
    {
        storage::ProjectLock lock(dir, "genko");
        lock.try_acquire();
        storage::SaveRequest request;
        request.ops = Json::array();
        storage::Saver(lock).save(doc, request);
    }
    const auto loaded = [&] {
        const core::ScopedIdSource ids(core::counting_ids());
        return storage::load_document(dir);
    }();
    if (!loaded.report.clean()) return "read back with " + core::dump_python(loaded.report.to_json()).substr(0, 600);
    const StepOutcome after = state_of(loaded.document, store);

    // the same as Python's, saved and read back
    std::string where;
    if (!reread.is_object() || !reread.contains("reread")) return "Python did not read it back";
    if (!strict_equal(after.full, reread["full"], &where)) return "read back, full snapshot (Python's read back): " + where.substr(0, 1200);
    if (!strict_equal(after.payload, reread["payload"], &where)) return "read back, payload (Python's read back): " + where.substr(0, 1200);
    // the same as before the save, but for the selection, the default layers of a page that had none and the margins
    // of a paper preset (ints there; Python's reader makes them floats)
    for (Json* spec : {&before.full["spec"], &before.payload["spec"]}) {
        if (!spec->contains("margins_mm")) continue;
        for (Json& margin : (*spec)["margins_mm"]) {
            if (margin.is_number_integer()) {
                margin = static_cast<double>(margin.get<std::int64_t>());
                ++notes.floated;
            }
        }
    }
    for (std::size_t p = 0; p < before.full["pages"].size(); ++p) {
        Json& page = before.full["pages"][p];
        if (!page["selected_frame_id"].is_null()) ++notes.unselected;
        page["selected_frame_id"] = nullptr;
        if (page["layers"].empty() && p < after.full["pages"].size()) {
            std::string roles;
            for (const Json& layer : after.full["pages"][p]["layers"]) roles += layer["role"].get<std::string>() + " ";
            if (roles != "bg name ink finish ") return "page " + std::to_string(p + 1) + " had no layers, read back with " + roles;
            page["layers"] = after.full["pages"][p]["layers"];
            before.payload["pages"][p]["layers"] = after.payload["pages"][p]["layers"];
            ++notes.refilled;
        }
    }
    if (!strict_equal(after.full, before.full, &where)) return "read back, full snapshot: " + where.substr(0, 1200);
    if (!strict_equal(after.payload, before.payload, &where)) return "read back, payload: " + where.substr(0, 1200);
    return {};
}

}  // namespace genko::test
