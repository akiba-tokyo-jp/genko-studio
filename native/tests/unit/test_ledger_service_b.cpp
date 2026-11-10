// The checks of tests/test_m4_units.py, tests/test_m3_art.py, tests/test_m3_report.py, tests/test_m4_pipeline.py,
// tests/test_m5.py, tests/test_m6.py, tests/test_m8.py and tests/test_m9.py that no other C++ test made (the M7 ledger),
// each slot named after the Python test it ports, with Python's inputs and assertions: generation requests shaped by
// tools.json and a person's prompt, fix requests with their source and mask, candidates gone stale, a location's
// reference carried into panel requests, one person at a time in a panel of two; a panel's picture over the HTTP API;
// gc on a studio book; art on a bleed panel; focus lines without faces, a shout's room, a gate refusal in Japanese; the
// audit's violation for an approval saved around the ops; the pack's default resolution; hand-drawn names (Python's
// synthetic scans of tests/agents/atari.py drawn here the same way) detected, evaluated, accepted, rejected and
// analysed again, and their MCP tools; a mannequin as the pose guide; the colour vocabulary; the shipped skills. The
// studio service, the HTTP API and the MCP tools are called in process; the command line as Python's tests call it.
// The runs of Python's FakeAgent (tests/agents/fake_agent.py) and of integrations/generic/mcp_client_example.py drive
// this build's `genko mcp` through the MCP SDK's own client, as test_contract_pipeline does: they are skipped without a
// Python that has the mcp extra ($GENKO_MCP_PYTHON).
//
// DEPS genko_test_support genko_service genko_api
// (the FakeAgent runs make a 4-page book each: give the test the time test_contract_pipeline has — TIMEOUT 2400 and
// QTEST_FUNCTION_TIMEOUT in its ENVIRONMENT)

#include <QtTest>

#include <QByteArray>
#include <QDir>
#include <QFile>
#include <QProcessEnvironment>
#include <QTemporaryDir>

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <regex>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "api/headless_server.hpp"
#include "api/http_server.hpp"
#include "api/mcp_tools.hpp"
#include "core/base64.hpp"
#include "core/config_dir.hpp"
#include "core/error.hpp"
#include "core/json.hpp"
#include "core/mannequin.hpp"
#include "core/model.hpp"
#include "core/paths.hpp"
#include "core/pyconv.hpp"
#include "core/pynum.hpp"
#include "core/pyops.hpp"
#include "core/pyrandom.hpp"
#include "formats/export.hpp"
#include "formats/pillow_save.hpp"
#include "render/draw.hpp"
#include "render/image.hpp"
#include "render/page.hpp"
#include "render/png.hpp"
#include "render/text/lettering.hpp"
#include "service/atari.hpp"
#include "service/book.hpp"
#include "service/data.hpp"
#include "service/evaluate.hpp"
#include "service/finish.hpp"
#include "service/genreq.hpp"
#include "service/human_service.hpp"
#include "service/letter.hpp"
#include "service/studio_service.hpp"
#include "storage/asset_store.hpp"
#include "storage/fsutil.hpp"
#include "storage/gc.hpp"
#include "storage/lock.hpp"
#include "storage/transaction.hpp"
#include "storage/undo.hpp"
#include "studio/files.hpp"
#include "testsupport.hpp"

using genko::core::Json;
using genko::service::HumanService;
using genko::service::StudioService;
using genko::service::ToolResult;
namespace fs = std::filesystem;
namespace render = genko::render;

namespace {

constexpr const char* kBook = "demo.genko";
constexpr const char* kTool = "openai:gpt-image-1";  // (test_m5.py's TOOL)

QString q(const fs::path& path) { return QString::fromStdString(genko::core::path_to_utf8(path)); }
fs::path utf8_path(const std::string& text) { return genko::core::path_from_utf8(text); }
fs::path utf8_path(const QString& text) { return genko::core::path_from_utf8(text.toStdString()); }
fs::path utf8_path(const Json& text) { return genko::core::path_from_utf8(text.get<std::string>()); }

bool has(const std::string& text, const std::string& part) { return text.find(part) != std::string::npos; }
bool has(const Json& text, const std::string& part) { return text.is_string() && has(text.get<std::string>(), part); }

void write_file(const fs::path& path, const std::string& bytes) {
    fs::create_directories(path.parent_path());
    genko::storage::write_atomic(path, bytes);
}

// tests/fixtures/studio/demo4/<name>
Json fixture(const std::string& name) {
    return genko::core::parse_python_json(
        genko::storage::read_file(utf8_path(genko::test::repo_root()) / "tests" / "fixtures" / "studio" / "demo4" / name));
}

// A picture of one colour as a PNG (test_m3_art.py's _png).
std::string flat_png(int width, int height, const render::Ink& colour) {
    return genko::formats::png_bytes(render::Image::create("RGB", render::Size{width, height}, colour));
}

int px_of(const Json& px, int i) { return static_cast<int>(genko::core::to_float(px.at(i))); }

// A panel's picture of the size a request suggests (tests/agents/images.py's panel(): a grey ground and a darker body;
// what it shows does not matter here).
std::string panel_png(const Json& px) {
    const int w = px_of(px, 0);
    const int h = px_of(px, 1);
    render::Image image = render::Image::create("RGB", render::Size{w, h}, render::Ink{200, 200, 200});
    image.paste(render::Ink{90, 90, 90}, render::Box{w / 3, h / 3, 2 * w / 3, h});
    return genko::formats::png_bytes(image);
}

// images.decoy(px): a magenta picture of half the size (at least 64 px a side).
std::string decoy_png(int w, int h) { return flat_png(std::max(64, w / 2), std::max(64, h / 2), render::Ink{255, 0, 255}); }

// images.sheet(px): a light sheet of the size asked, a figure on it.
std::string sheet_png(const Json& px) {
    const int w = px_of(px, 0);
    const int h = px_of(px, 1);
    render::Image image = render::Image::create("RGB", render::Size{w, h}, render::Ink{245, 245, 245});
    image.paste(render::Ink{120, 120, 120}, render::Box{w * 35 / 100, h / 2, w * 65 / 100, h * 95 / 100});
    return genko::formats::png_bytes(image);
}

// Ops applied and saved as `actor` under the book's lock (Python's apply_ops + save_episode, HumanService._apply).
void apply_as(const fs::path& book, const std::string& actor, const Json& ops) {
    genko::service::Session session(book, actor);
    session.apply(ops, actor);
    session.save(actor);
}
void as_person(const fs::path& book, const Json& ops) { apply_as(book, "human:leaf", ops); }

std::string failure_of(const char* what, const ToolResult& r) { return std::string(what) + ": " + r.to_dict().dump(); }

// Python's _approved / _named / m5._project: the agent makes the demo book (4 pages) of tests/fixtures/studio/demo4 —
// the bible, the script and the names of pages 1..named — and a person approves those names when `approve`. "" when
// every step went through.
std::string make_demo(StudioService& agent, int named = 4, bool approve = true) {
    if (const ToolResult r = agent.call("create_project", Json::array({kBook, "demo", 4}), Json::object()); !r.ok) return failure_of("create_project", r);
    const std::vector<std::pair<const char*, std::string>> steps = {{"set_bible", "bible.json"}, {"set_script", "script.json"}};
    for (const auto& [tool, file] : steps) {
        if (const ToolResult r = agent.call(tool, Json::array({kBook, fixture(file)}), Json{{"commit", true}}); !r.ok) return failure_of(tool, r);
    }
    Json pages = Json::array();
    for (int n = 1; n <= named; ++n) {
        const ToolResult r = agent.call("submit_name", Json::array({kBook, fixture("p00" + std::to_string(n) + ".json")}), Json{{"commit", true}});
        if (!r.ok) return failure_of("submit_name", r);
        pages.push_back(n);
    }
    if (approve) {
        const Json approved = HumanService(agent.root() / kBook, "human:leaf").approve_name(pages);
        if (!approved.value("ok", false)) return "approve_name: " + approved.dump();
    }
    return {};
}

// Python's _slot: the panel of a page whose brief has this slot (a copy; no id when there is none).
genko::core::Frame slot(const fs::path& book, int page, const std::string& name) {
    const genko::core::Document doc = genko::service::load_episode(book);
    for (const genko::core::Frame* f : doc.page(static_cast<std::size_t>(page - 1)).leaf_frames()) {
        if (f->panel && genko::core::py_get(*f->panel, "slot") == Json(name)) return *f;
    }
    return {};
}

genko::core::Frame first_leaf(const fs::path& book, int page) {
    const genko::core::Document doc = genko::service::load_episode(book);
    return *doc.page(static_cast<std::size_t>(page - 1)).leaf_frames().at(0);
}

bool any_note(const Json& request, const std::string& words) {
    for (const Json& n : request.value("notes_for_agent", Json::array())) {
        if (has(n, words)) return true;
    }
    return false;
}

bool has_reference(const Json& request, const std::string& name) {
    for (const Json& r : request["files"]["references"]) {
        if (r == Json(name)) return true;
    }
    return false;
}

bool any_reference_starting(const Json& request, const std::string& start) {
    for (const Json& r : request["files"]["references"]) {
        if (r.is_string() && r.get<std::string>().starts_with(start)) return true;
    }
    return false;
}

bool contains_py(const Json& list, const Json& value) {
    for (const Json& item : list) {
        if (genko::core::py_equals(item, value)) return true;
    }
    return false;
}

std::set<std::string> kinds_of(const Json& items, std::optional<int> page = std::nullopt) {
    std::set<std::string> out;
    for (const Json& i : items) {
        if (page && !genko::core::py_equals(genko::core::py_get(i["target"], "page"), Json(*page))) continue;
        out.insert(i["kind"].get<std::string>());
    }
    return out;
}

// The picture written into the request's inbox and imported as its candidate (import_images); its id ("" with
// `problem` set when it was not taken).
std::string import_into(StudioService& agent, const Json& request_reply, const std::string& file, const std::string& bytes, const Json& origin,
                        std::string& problem) {
    const std::string id = request_reply["request"]["id"].get<std::string>();
    write_file(utf8_path(request_reply["inbox"]) / file, bytes);
    const ToolResult imported = agent.call(
        "import_images", Json::array({kBook, id, Json::array({Json{{"file", "studio/inbox/" + id + "/" + file}, {"origin", origin}}})}), Json::object());
    if (!imported.ok) {
        problem = failure_of("import_images", imported);
        return {};
    }
    return imported.data["candidates"].at(0).get<std::string>();
}

// test_m3_art.py's _import: the picture imported (import_image) and made a candidate of the panel (import_candidates,
// committed); the candidate's id.
std::string import_candidate(StudioService& agent, int page, const std::string& frame_id, const std::string& png, std::string& problem) {
    const ToolResult imported = agent.call("import_image", Json::array({kBook}), Json{{"png_base64", genko::core::b64encode(png)}});
    if (!imported.ok) {
        problem = failure_of("import_image", imported);
        return {};
    }
    const std::string asset = imported.data["asset"].get<std::string>();
    const std::string id = "c_" + frame_id + "_" + asset.substr(7, 8);
    const Json ops = Json::array({Json{{"op", "import_candidates"}, {"page", page}, {"frame_id", frame_id},
                                       {"candidates", Json::array({Json{{"id", id}, {"asset", asset}, {"px", imported.data["px"]},
                                                                         {"origin", Json{{"kind", "agent"}}}}})}}});
    if (const ToolResult r = agent.call("apply_ops", Json::array({kBook, ops}), Json{{"commit", true}}); !r.ok) problem = failure_of("import_candidates", r);
    return id;
}

std::string commit_ops(StudioService& agent, const Json& ops) {
    const ToolResult r = agent.call("apply_ops", Json::array({kBook, ops}), Json{{"commit", true}});
    return r.ok ? std::string() : failure_of("apply_ops", r);
}

// test_m5.py's _sheets: a sheet asked for, drawn and imported for hina and sora, each approved by the person.
std::string approve_sheets(StudioService& agent, HumanService& human) {
    for (const char* cid : {"hina", "sora"}) {
        const ToolResult req = agent.call("generation_request", Json::array({kBook}), Json{{"character_id", cid}});
        if (!req.ok) return failure_of("generation_request", req);
        std::string problem;
        const std::string cand = import_into(agent, req.data, "s.png", sheet_png(req.data["request"]["size"]["suggested_px"]), Json{{"tool_id", kTool}}, problem);
        if (!problem.empty()) return problem;
        const Json approved = human.approve_sheet(Json(cid), Json(cand));
        if (!approved.value("ok", false)) return "approve_sheet: " + approved.dump();
    }
    return {};
}

// test_m5.py's _art (report=False): a panel's picture asked for, drawn, imported and adopted (not enlarged).
std::string make_art(StudioService& agent, int page, const std::string& frame_id, std::string& problem) {
    const ToolResult req = agent.call("generation_request", Json::array({kBook}), Json{{"page", page}, {"frame_id", frame_id}});
    if (!req.ok) {
        problem = failure_of("generation_request", req);
        return {};
    }
    const std::string cand = import_into(agent, req.data, "a.png", panel_png(req.data["request"]["size"]["suggested_px"]),
                                         Json{{"tool_id", kTool}, {"model", "gpt-image-1"}, {"prompt", "x"}}, problem);
    if (!problem.empty()) return {};
    const ToolResult adopted = agent.call("adopt", Json::array({kBook, cand}), Json{{"page", page}, {"frame_id", frame_id}, {"upscale", false}});
    if (!adopted.ok) problem = failure_of("adopt", adopted);
    return cand;
}

// The command line's reply: its last line of output, as Python's tests read it.
Json last_line(const QByteArray& out) {
    const Json unread = Json{{"ok", false}, {"unread", out.toStdString()}};
    const QList<QByteArray> lines = out.trimmed().split('\n');
    if (lines.isEmpty()) return unread;
    try {
        const Json reply = genko::core::parse_python_json(lines.last().trimmed().toStdString());
        return reply.is_object() ? reply : unread;
    } catch (const std::exception&) {
        return unread;
    }
}

// --- tests/agents/atari.py ------------------------------------------------------------------------------------------
// Synthetic hand-drawn names with their true panel rectangles, drawn as Python draws them: random.Random through
// core::PyRandom (number for number) and ImageDraw through render::Draw (the same pixels), so these are Python's scans.
// Pen lines wobble, break and overshoot at the corners; panels hold stick figures and vertical scribbles of dialogue;
// the paper has specks. The truth is the panel rectangle the artist meant, in page mm.
namespace synth {

constexpr double kPageW = 257.0;  // B4 comic
constexpr double kPageH = 364.0;
constexpr std::array<double, 4> kLive{13.0, 13.0, 231.0, 338.0};  // its live area (bleed 3 + margin 10)

using Rect = std::array<double, 4>;
struct Tier {
    double share;
    std::vector<double> cols;  // column width shares, right to left
};
using Layout = std::vector<Tier>;

const std::vector<Layout>& layouts() {
    static const std::vector<Layout> all = {
        {{0.3, {1.0}}, {0.4, {0.55, 0.45}}, {0.3, {1.0}}},
        {{0.25, {0.5, 0.5}}, {0.5, {1.0}}, {0.25, {0.35, 0.3, 0.35}}},
        {{0.35, {1.0}}, {0.3, {0.4, 0.6}}, {0.35, {0.6, 0.4}}},
        {{1.0, {1.0}}},
        {{0.2, {1.0}}, {0.4, {0.5, 0.5}}, {0.4, {0.3, 0.7}}},
        {{0.5, {0.6, 0.4}}, {0.5, {0.4, 0.6}}},
    };
    return all;
}

std::vector<Rect> true_panels(const Layout& layout, double h_gutter, double v_gutter) {
    const double x = kLive[0];
    const double y = kLive[1];
    const double w = kLive[2];
    const double h = kLive[3];
    std::vector<Rect> rects;
    const double usable_h = h - h_gutter * static_cast<double>(layout.size() - 1);
    double ty = y;
    for (const Tier& tier : layout) {
        const double th = usable_h * tier.share;
        const double usable_w = w - v_gutter * static_cast<double>(tier.cols.size() - 1);
        double cx = x + w;  // (columns are listed right to left)
        for (const double cw : tier.cols) {
            const double width = usable_w * cw;
            cx -= width;
            rects.push_back({genko::core::py_round(cx, 2), genko::core::py_round(ty, 2), genko::core::py_round(width, 2), genko::core::py_round(th, 2)});
            cx -= v_gutter;
        }
        ty += th + h_gutter;
    }
    return rects;
}

void wobbly(render::Draw& draw, render::PointD a, render::PointD b, genko::core::PyRandom& rng, double px_per_mm, int width, bool breaks = true) {
    const double length = std::hypot(b.x - a.x, b.y - a.y);
    const int steps = std::max(4, static_cast<int>(length / (3 * px_per_mm)));
    const double jitter = 0.35 * px_per_mm;
    std::vector<render::PointD> points;
    for (int i = 0; i <= steps; ++i) {
        const double t = static_cast<double>(i) / steps;
        const double x = a.x + (b.x - a.x) * t + rng.uniform(-jitter, jitter);
        const double y = a.y + (b.y - a.y) * t + rng.uniform(-jitter, jitter);
        points.push_back(render::PointD{x, y});
    }
    std::vector<render::PointD> segment;
    for (const render::PointD& p : points) {
        if (breaks && rng.random() < 0.03) {  // (the pen skips)
            if (segment.size() > 1) draw.line(segment, render::Ink(30), width);
            segment.clear();
            continue;
        }
        segment.push_back(p);
    }
    if (segment.size() > 1) draw.line(segment, render::Ink(30), width);
}

struct Scan {
    render::Image image;  // RGB, as Image.open reads its PNG (formats::png_bytes of it)
    std::vector<Rect> truth;
};

// draw_page(layout, dpi, seed, margin_mm): the scan and the true panel rects in mm.
Scan draw_page(const Layout& layout, int dpi, std::int64_t seed, double margin_mm = 0.0) {
    genko::core::PyRandom rng(seed);
    const double px_per_mm = dpi / 25.4;
    const double width_mm = kPageW + 2 * margin_mm;
    const double height_mm = kPageH + 2 * margin_mm;
    render::Image image = render::Image::create(
        "L", render::Size{static_cast<int>(genko::core::py_round_int(width_mm * px_per_mm)), static_cast<int>(genko::core::py_round_int(height_mm * px_per_mm))},
        render::Ink(238));
    const auto px = [&](double x_mm, double y_mm) { return render::PointD{(x_mm + margin_mm) * px_per_mm, (y_mm + margin_mm) * px_per_mm}; };
    const double h_gutter = rng.uniform(5, 8);
    const double v_gutter = rng.uniform(2.5, 4);
    const std::vector<Rect> rects = true_panels(layout, h_gutter, v_gutter);
    const int pen = std::max(2, static_cast<int>(genko::core::py_round_int(0.6 * px_per_mm)));
    {
        render::Draw draw(image);
        for (const Rect& rect : rects) {
            const double x = rect[0];
            const double y = rect[1];
            const double w = rect[2];
            const double h = rect[3];
            const auto over = [&rng] { return rng.uniform(-1.0, 1.2); };  // (corners overshoot or fall short)
            const std::array<std::array<double, 2>, 4> corners{{{x, y}, {x + w, y}, {x + w, y + h}, {x, y + h}}};
            for (std::size_t i = 0; i < corners.size(); ++i) {
                const double ax = corners[i][0];
                const double ay = corners[i][1];
                const double bx = corners[(i + 1) % corners.size()][0];
                const double by = corners[(i + 1) % corners.size()][1];
                render::PointD a;
                render::PointD b;
                if (ax == bx) {
                    const double sign = by > ay ? 1 : -1;
                    a = px(ax, ay - over() * sign);
                    b = px(bx, by + over() * sign);
                } else {
                    const double sign = bx > ax ? 1 : -1;
                    a = px(ax - over() * sign, ay);
                    b = px(bx + over() * sign, by);
                }
                wobbly(draw, a, b, rng, px_per_mm, pen);
            }
            // a stick figure
            if (w > 30 && h > 40) {
                const double cx = x + w * rng.uniform(0.3, 0.7);
                const double cy = y + h * rng.uniform(0.35, 0.55);
                const double r = std::min(w, h) * 0.08;
                const render::PointD top_left = px(cx - r, cy - r);
                const render::PointD bottom_right = px(cx + r, cy + r);
                draw.ellipse(render::BoxF{top_left.x, top_left.y, bottom_right.x, bottom_right.y}, std::nullopt, render::Ink(60), std::max(1, pen - 1));
                wobbly(draw, px(cx, cy + r), px(cx, cy + h * 0.3), rng, px_per_mm, std::max(1, pen - 1), false);
            }
            // vertical scribbles of dialogue near the top right
            const std::int64_t columns = rng.randint(1, 3);
            for (std::int64_t k = 0; k < columns; ++k) {
                const double sx = x + w - 6 - static_cast<double>(k * 5);
                if (sx < x + 4) break;
                const double top = y + 5;
                const std::int64_t marks = rng.randint(3, 7);
                for (std::int64_t c = 0; c < marks; ++c) {
                    const double cy0 = top + static_cast<double>(c) * 4.5;
                    if (cy0 + 3 > y + h - 4) break;
                    wobbly(draw, px(sx - 1.5, cy0), px(sx + 1.5, cy0 + 3), rng, px_per_mm, std::max(1, pen - 1), false);
                }
            }
        }
        for (int i = 0; i < 80; ++i) {  // specks and eraser crumbs
            const double sx = rng.uniform(0, width_mm);
            const double sy = rng.uniform(0, height_mm);
            const double r = rng.uniform(0.1, 0.35) * px_per_mm;
            const double cx = sx * px_per_mm;
            const double cy = sy * px_per_mm;
            const int grey = static_cast<int>(rng.randint(60, 160));
            draw.ellipse(render::BoxF{cx - r, cy - r, cx + r, cy + r}, render::Ink(grey));
        }
    }
    Scan scan;
    scan.image = image.convert("RGB");
    scan.truth = rects;
    return scan;
}

}  // namespace synth

// evaluate._edge_error: how far apart two rects are, at their worst edge.
double edge_error(const Json& a, const synth::Rect& b) {
    const double a0 = genko::core::to_float(a.at(0));
    const double a1 = genko::core::to_float(a.at(1));
    const double a2 = genko::core::to_float(a.at(2));
    const double a3 = genko::core::to_float(a.at(3));
    return std::max({std::abs(a0 - b[0]), std::abs(a1 - b[1]), std::abs(a0 + a2 - b[0] - b[2]), std::abs(a1 + a3 - b[1] - b[3])});
}

// test_m8.py's _matched: how many of the true panels a found panel matches within `tol` mm on every edge.
int matched(const Json& found, const std::vector<synth::Rect>& truth, double tol = 5.0) {
    int hits = 0;
    for (const synth::Rect& t : truth) {
        bool any = false;
        for (const Json& d : found) any = any || edge_error(d["rect_mm"], t) <= tol;
        hits += any ? 1 : 0;
    }
    return hits;
}

// test_m8.py's _project: the agent's book of `pages` pages with the demo bible, and a scan of each page
// (synth.LAYOUTS[n] at 150 dpi, seed 40 + n) under <root>/scans/page<n>.png.
struct Scans {
    std::vector<fs::path> files;
    std::vector<std::vector<synth::Rect>> truths;
};
std::string atari_project(StudioService& agent, const fs::path& root, int pages, Scans& scans) {
    if (const ToolResult r = agent.call("create_project", Json::array({kBook, "demo", pages}), Json::object()); !r.ok) return failure_of("create_project", r);
    if (const ToolResult r = agent.call("set_bible", Json::array({kBook, fixture("bible.json")}), Json{{"commit", true}}); !r.ok) return failure_of("set_bible", r);
    for (int n = 0; n < pages; ++n) {
        const synth::Scan scan = synth::draw_page(synth::layouts().at(static_cast<std::size_t>(n)), 150, 40 + n);
        const fs::path path = root / "scans" / ("page" + std::to_string(n + 1) + ".png");
        write_file(path, genko::formats::png_bytes(scan.image));
        scans.files.push_back(path);
        scans.truths.push_back(scan.truth);
    }
    return {};
}

// --- the FakeAgent and the generic client, over this build's `genko mcp` ----------------------------------------------

// tests/test_m4_pipeline.py's scenarios with Python's FakeAgent on this build's `genko mcp` (stdio, the MCP SDK's own
// client) and the person of its ScriptedHuman answering with this build's `genko studio` — pipeline_harness.py's Person
// and Recorder (which waits for a background export_proof to end, as the harness does). Run as
// `<python> fake_agent_run.py <repo> <job.json> resume|broken <out.json>`; writes what the agents did.
constexpr const char* kFakeAgentRun = R"PY(import asyncio
import json
import sys
from pathlib import Path

sys.dont_write_bytecode = True
REPO = Path(sys.argv[1])
sys.path.insert(0, str(REPO / "tools" / "migration"))
sys.path.insert(0, str(REPO / "tests"))

import mcp_harness as mh  # noqa: E402
import pipeline_harness as ph  # noqa: E402


async def scenario(job: dict, which: str) -> dict:
    from mcp import Client
    from mcp.client.stdio import StdioServerParameters

    from agents.fake_agent import FakeAgent

    cmd = mh._mcp_command(job, "stdio")
    calls: list = []
    person = ph.Person(job, calls)
    root = Path(job["root"])
    out: dict = {}
    async with Client(StdioServerParameters(command=cmd[0], args=cmd[1:], env=mh._env(job))) as client:
        recorder = ph.Recorder(client, calls, root)
        if which == "resume":
            first = FakeAgent(recorder, root, ph.PROJECT, person, pages=4)
            created = await first.tool("create_project", name=ph.PROJECT, title="t", pages=4)
            assert created["ok"], created
            await first.run(stop_after=25)
            book = person.book()
            out["art_ok_after_first"] = [bool(p.get("art_ok")) for p in book["pages"]]
            out["requests_before"] = sorted((book.get("studio") or {}).get("requests") or {})
            second = FakeAgent(recorder, root, ph.PROJECT, person, pages=4)
            await second.run()
            out["first_seen"] = first.seen_ids
            out["second_seen"] = second.seen_ids
        else:
            agent = FakeAgent(recorder, root, ph.PROJECT, None, pages=4, broken_page=3)
            created = await agent.tool("create_project", name=ph.PROJECT, title=ph.TITLE, pages=4)
            assert created["ok"], created
            await agent.run()
            out["log"] = agent.log
    return out


def main() -> int:
    job = json.loads(Path(sys.argv[2]).read_text(encoding="utf-8"))
    for key in ("root", "config", "out"):
        Path(job[key]).mkdir(parents=True, exist_ok=True)
    out = asyncio.run(scenario(job, sys.argv[3]))
    Path(sys.argv[4]).write_text(json.dumps(out, ensure_ascii=False), encoding="utf-8")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
)PY";

// integrations/generic/mcp_client_example.py run as it is, its `python -m genko mcp …` replaced by the same command of
// this build's genko (the example starts Python's genko; its calls are what is checked). Run as
// `<python> example_run.py <example> <genko> <root>`; prints the example's report.
constexpr const char* kExampleRun = R"PY(import importlib.util
import json
import os
import sys
from pathlib import Path

sys.dont_write_bytecode = True

import anyio  # noqa: E402

example_file, genko, root = sys.argv[1], sys.argv[2], Path(sys.argv[3])
spec = importlib.util.spec_from_file_location("mcp_client_example", example_file)
example = importlib.util.module_from_spec(spec)
spec.loader.exec_module(example)
made = example.StdioServerParameters


def this_build(command, args, **rest):
    if list(args[:2]) != ["-m", "genko"]:
        raise SystemExit(f"the example starts something else: {command} {args}")
    rest.setdefault("env", dict(os.environ))
    return made(command=genko, args=list(args[2:]), **rest)


example.StdioServerParameters = this_build
root.mkdir(parents=True, exist_ok=True)
print(json.dumps(anyio.run(example.run, root, "ai:example"), ensure_ascii=False))
)PY";

QProcessEnvironment mcp_env() {
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    env.remove(QStringLiteral("PYTHONPATH"));
    env.insert(QStringLiteral("PYTHONDONTWRITEBYTECODE"), QStringLiteral("1"));
    env.insert(QStringLiteral("QT_QPA_PLATFORM"), QStringLiteral("offscreen"));
    return env;
}

// What went wrong with a Python run ("" when it ended well, with no sanitizer report from this build's processes).
QString run_problem(const genko::test::Run& run) {
    if (!run.finished || run.exit_code != 0) return run.described().right(8000);
    for (const char* report : {"ERROR: AddressSanitizer", "ERROR: LeakSanitizer", "runtime error:"}) {
        if (run.err.contains(report)) return QString::fromUtf8(run.err.right(8000));
    }
    return {};
}

// A FakeAgent scenario in `dir` (root, config, out under it); what the driver wrote, `problem` set when it failed.
Json fake_agent_run(const QString& dir, const char* scenario, QString& problem) {
    const QString driver = dir + QStringLiteral("/fake_agent_run.py");
    genko::test::write_bytes(driver, kFakeAgentRun);
    const Json job = Json{{"server", Json{{"kind", "cpp"}, {"genko", genko::test::genko_cli().toStdString()}}},
                          {"root", (dir + QStringLiteral("/root")).toStdString()},
                          {"config", (dir + QStringLiteral("/config")).toStdString()},
                          {"out", (dir + QStringLiteral("/out")).toStdString()},
                          {"agent", "ai:test"}};
    genko::test::write_bytes(dir + QStringLiteral("/job.json"), genko::core::dump_python(job));
    const genko::test::Run run =
        genko::test::run(genko::test::mcp_python(),
                         {driver, genko::test::repo_root(), dir + QStringLiteral("/job.json"), QString::fromLatin1(scenario), dir + QStringLiteral("/run.json")},
                         mcp_env(), 2000 * 1000);
    problem = run_problem(run);
    if (!problem.isEmpty()) return Json();
    return genko::test::read_json(dir + QStringLiteral("/run.json"));
}

}  // namespace

class TestLedgerServiceB : public QObject {
    Q_OBJECT

    QTemporaryDir dir_;

    fs::path folder(const QString& name) const {
        const fs::path out = utf8_path(dir_.path() + QLatin1Char('/') + name);
        fs::create_directories(out);
        return out;
    }

private slots:
    void initTestCase() {
        QVERIFY(dir_.isValid());
        // (as the Python tests' fixtures: the person's config folder of the test's own, GENKO_USER=leaf)
        qputenv("GENKO_CONFIG_DIR", QFile::encodeName(dir_.path() + QStringLiteral("/config")));
        qputenv("GENKO_USER", "leaf");
    }

    // --- tests/test_m4_units.py ----------------------------------------------------------------------------------

    // test_request_follows_tools_json_and_human_overrides: a tool described in tools.json shapes the request (its only
    // size, square, cut top and bottom for a wide panel; a note that it takes no reference pictures); an unknown tool
    // gets the generic sizes; a prompt a person wrote into the panel is used as it is, said so, and makes another request.
    void requestFollowsToolsJsonAndHumanOverrides() {
        const fs::path root = folder(QStringLiteral("tools"));
        StudioService agent(root, "ai:test");
        const std::string made = make_demo(agent);
        QVERIFY2(made.empty(), made.c_str());
        const fs::path book = root / kBook;
        const genko::core::Frame frame = slot(book, 1, "p1");  // (a wide panel)
        QVERIFY(!frame.id.empty());
        genko::studio::set_tool(genko::core::config_dir(), "square:only",
                                Json{{"sizes_px", Json::array({Json::array({1024, 1024})})}, {"supports", Json{{"references", false}, {"mask", false}}}});
        const ToolResult asked = agent.call("generation_request", Json::array({kBook}), Json{{"page", 1}, {"frame_id", frame.id}, {"tool", "square:only"}});
        QVERIFY2(asked.ok, asked.to_dict().dump().c_str());
        const Json request = asked.data["request"];
        QCOMPARE(request["tool"], Json("square:only"));
        QCOMPARE(request["size"]["tool_sizes"], Json::array({Json{{"px", Json::array({1024, 1024})}, {"crop", "上下を中央で切る"}}}));
        QVERIFY(any_note(request, "参照画像を渡せない"));
        const Json unknown =
            agent.call("generation_request", Json::array({kBook}), Json{{"page", 1}, {"frame_id", frame.id}, {"tool", "nobody:knows"}}).data["request"];
        QVERIFY(unknown["tool"].is_null());
        QCOMPARE(unknown["size"]["tool_sizes"].size(), std::size_t{3});
        as_person(book, Json::array({Json{{"op", "set_panel"}, {"page", 1}, {"frame_id", frame.id},
                                          {"set", Json{{"gen", Json{{"prompt_override", "人間が書いたプロンプト"}}}}}}}));
        const Json overridden = agent.call("generation_request", Json::array({kBook}), Json{{"page", 1}, {"frame_id", frame.id}}).data["request"];
        QCOMPARE(overridden["prompt"]["ja"], Json("人間が書いたプロンプト"));
        QVERIFY(any_note(overridden, "人間の指定"));
        QVERIFY(overridden["id"] != request["id"]);
    }

    // test_fix_requests_carry_the_source_and_a_mask: a fix (mode edit) needs the candidate it starts from (/parent);
    // with it the request carries source.png and this round's instruction; an inpaint carries a mask of the region
    // given (some of the picture, not most of it); each fix counts as a round.
    void fixRequestsCarryTheSourceAndAMask() {
        const fs::path root = folder(QStringLiteral("fixes"));
        StudioService agent(root, "ai:test");
        const std::string made = make_demo(agent);
        QVERIFY2(made.empty(), made.c_str());
        const fs::path book = root / kBook;
        const genko::core::Frame frame = slot(book, 1, "p2");
        const ToolResult asked = agent.call("generation_request", Json::array({kBook}), Json{{"page", 1}, {"frame_id", frame.id}});
        QVERIFY2(asked.ok, asked.to_dict().dump().c_str());
        std::string problem;
        const std::string cand = import_into(agent, asked.data, "a.png", panel_png(asked.data["request"]["size"]["suggested_px"]),
                                             Json{{"tool_id", "t"}, {"model", "m"}}, problem);
        QVERIFY2(problem.empty(), problem.c_str());
        const ToolResult no_parent = agent.call("generation_request", Json::array({kBook}), Json{{"page", 1}, {"frame_id", frame.id}, {"mode", "edit"}});
        QVERIFY(!no_parent.ok);
        QCOMPARE(no_parent.issues.at(0).path, std::string("/parent"));
        const ToolResult edit = agent.call("generation_request", Json::array({kBook}),
                                           Json{{"page", 1}, {"frame_id", frame.id}, {"mode", "edit"}, {"parent", cand}, {"instruction", "顔を大きく"}});
        QVERIFY2(edit.ok, edit.to_dict().dump().c_str());
        QVERIFY(edit.data["files"].contains("source.png"));
        QVERIFY(has(edit.data["request"]["prompt"]["ja"], "今回の指示: 顔を大きく"));
        const Json region = Json::array({Json::array({frame.rect.x.value() + 5, frame.rect.y.value() + 5, 20, 20})});
        const ToolResult inpaint = agent.call("generation_request", Json::array({kBook}),
                                              Json{{"page", 1}, {"frame_id", frame.id}, {"mode", "inpaint"}, {"parent", cand}, {"regions", region}});
        QVERIFY2(inpaint.ok, inpaint.to_dict().dump().c_str());
        const render::Image mask = render::read_png(genko::storage::read_file(utf8_path(inpaint.data["files"]["mask.png"]))).convert("L");
        const std::vector<long> hist = mask.histogram();
        long white = 0;
        for (std::size_t v = 128; v < hist.size(); ++v) white += hist[v];
        const double ratio = static_cast<double>(white) / (static_cast<double>(mask.width()) * mask.height());
        QVERIFY2(0 < ratio && ratio < 0.5, std::to_string(ratio).c_str());
        const genko::core::Document doc = genko::service::load_episode(book);
        const genko::core::Frame* fixed = doc.page(0).find_frame(frame.id);
        QVERIFY(fixed != nullptr && fixed->panel);
        QVERIFY(genko::core::py_equals(genko::core::py_get(genko::core::py_get(*fixed->panel, "attempts", Json::object()), "fix_rounds"), Json(2)));
    }

    // test_brief_change_marks_candidates_stale: a candidate made for a panel's brief turns stale once the brief changes.
    void briefChangeMarksCandidatesStale() {
        const fs::path root = folder(QStringLiteral("stale"));
        StudioService agent(root, "ai:test");
        const std::string made = make_demo(agent);
        QVERIFY2(made.empty(), made.c_str());
        const genko::core::Frame frame = slot(root / kBook, 1, "p1");
        const ToolResult asked = agent.call("generation_request", Json::array({kBook}), Json{{"page", 1}, {"frame_id", frame.id}});
        QVERIFY2(asked.ok, asked.to_dict().dump().c_str());
        std::string problem;
        import_into(agent, asked.data, "a.png", decoy_png(256, 128), Json{{"tool_id", "t"}}, problem);
        QVERIFY2(problem.empty(), problem.c_str());
        const auto first_stale = [&] {
            return agent.call("candidates", Json::array({kBook}), Json{{"page", 1}, {"frame_id", frame.id}}).data["candidates"].at(0)["stale"];
        };
        QCOMPARE(first_stale(), Json(false));
        const std::string changed = commit_ops(agent, Json::array({Json{{"op", "set_panel"}, {"page", 1}, {"frame_id", frame.id}, {"set", Json{{"action", "走り出す"}}}}}));
        QVERIFY2(changed.empty(), changed.c_str());
        QCOMPARE(first_stale(), Json(true));
    }

    // test_sheet_request_face_crop_and_location_reference: a character sheet is asked for upright; once its candidate is
    // in, the work waits for the person, whose approval cuts the face (half the sheet wide); the face and the sheet go
    // with every panel request for that character; a location's background, asked for, imported and adopted as its
    // reference, goes with the panel requests set there (refs/loc_…).
    void sheetRequestFaceCropAndLocationReference() {
        const fs::path root = folder(QStringLiteral("sheet"));
        StudioService agent(root, "ai:test");
        const std::string made = make_demo(agent, 1);
        QVERIFY2(made.empty(), made.c_str());
        const fs::path book = root / kBook;
        const ToolResult sheet = agent.call("generation_request", Json::array({kBook}), Json{{"character_id", "hina"}});
        QVERIFY2(sheet.ok, sheet.to_dict().dump().c_str());
        const Json request = sheet.data["request"];
        QCOMPARE(request["purpose"], Json("character_sheet"));
        QVERIFY(genko::core::to_float(request["size"]["suggested_px"][1]) > genko::core::to_float(request["size"]["suggested_px"][0]));
        std::string problem;
        const std::string cand =
            import_into(agent, sheet.data, "s.png", sheet_png(request["size"]["suggested_px"]), Json{{"kind", "agent"}, {"tool_id", "t"}}, problem);
        QVERIFY2(problem.empty(), problem.c_str());
        const std::set<std::string> kinds = kinds_of(agent.call("next", Json::array({kBook}), Json{{"limit", 20}}).data["items"]);
        QVERIFY(!kinds.count("make_sheet"));  // (candidates exist: now it waits for the person)
        QVERIFY(contains_py(agent.call("next", Json::array({kBook}), Json::object()).data["waiting_for"],
                            Json{{"gate", "sheet"}, {"characters", Json::array({"hina"})}}));
        const Json done = HumanService(book, "human:leaf").approve_sheet(Json("hina"), Json(cand), Json::array({0.25, 0.0, 0.5, 0.3}));
        QVERIFY2(done.value("ok", false), done.dump().c_str());
        const auto face = genko::storage::AssetStore(book).get_bytes(done["face_asset"].get<std::string>(), ".png");
        QVERIFY(face.has_value());
        const double width = genko::core::to_float(request["size"]["suggested_px"][0]);
        QVERIFY(std::abs(render::picture_size(*face).width - width * 0.5) <= 1);
        // the face now travels with every panel request for hina
        const genko::core::Frame frame = slot(book, 1, "p2");
        const Json panel_request = agent.call("generation_request", Json::array({kBook}), Json{{"page", 1}, {"frame_id", frame.id}}).data["request"];
        QVERIFY(has_reference(panel_request, "refs/hina_face.png") && has_reference(panel_request, "refs/hina_sheet.png"));
        // locations: a background reference request, adopted as the location's reference
        apply_as(book, "ai:test", Json::array({Json{{"op", "upsert_location"}, {"location", Json{{"id", "loc_rooftop"}}}}}));
        const ToolResult loc = agent.call("generation_request", Json::array({kBook}), Json{{"location_id", "loc_rooftop"}});
        QVERIFY2(loc.ok, loc.to_dict().dump().c_str());
        const std::string lcand = import_into(agent, loc.data, "l.png", decoy_png(512, 340), Json{{"tool_id", "t"}}, problem);
        QVERIFY2(problem.empty(), problem.c_str());
        const ToolResult adopted = agent.call("adopt", Json::array({kBook, lcand}), Json{{"location_id", "loc_rooftop"}});
        QVERIFY2(adopted.ok, adopted.to_dict().dump().c_str());
        const Json again = agent.call("generation_request", Json::array({kBook}), Json{{"page", 1}, {"frame_id", frame.id}}).data["request"];
        QVERIFY2(any_reference_starting(again, "refs/loc_rooftop_"), again["files"].dump().c_str());
    }

    // --- tests/test_m3_art.py -------------------------------------------------------------------------------------------

    // test_panel_crop_render_over_service_and_http: render with a frame_id shows that panel alone (its shape, within the
    // size asked) and writes it; the HTTP API serves the panel's picture (/v1/pages/1/frames/<id>.png, a PNG) and a 404
    // for a panel that is not there.
    void panelCropRenderOverServiceAndHttp() {
        const fs::path root = folder(QStringLiteral("crop"));
        StudioService agent(root, "ai:test");
        const std::string made = make_demo(agent);
        QVERIFY2(made.empty(), made.c_str());
        const genko::core::Frame frame = first_leaf(root / kBook, 1);
        const ToolResult result = agent.call("render", Json::array({kBook, 1, "proof", 400}), Json{{"frame_id", frame.id}});
        QVERIFY2(result.ok, result.to_dict().dump().c_str());
        QVERIFY(!result.files.empty() && fs::is_regular_file(utf8_path(result.files.at(0))));
        QVERIFY(!result.images.empty());
        const render::Size crop = render::picture_size(result.images.at(0));
        const double aspect = static_cast<double>(crop.width) / crop.height;
        QVERIFY(std::abs(aspect - frame.rect.width.value() / frame.rect.height.value()) < 0.05);
        QVERIFY(std::max(crop.width, crop.height) <= 420);
        // over HTTP (handle_request as the person, as Python's test calls it)
        genko::api::headless::Options options;
        options.root = root;
        genko::api::headless::Api api(options);
        const auto get = [&api](const std::string& target) {
            genko::api::http::Request request;
            request.method = "GET";
            request.target = target;
            request.version = "HTTP/1.1";
            request.principal = "human:leaf";
            return api.serve(request);
        };
        const genko::api::headless::Reply shown = get("/v1/pages/1/frames/" + frame.id + ".png?path=demo.genko&dpi=100");
        QCOMPARE(shown.status, 200);
        QVERIFY(shown.body.starts_with("\x89PNG"));
        QCOMPARE(get("/v1/pages/1/frames/nope.png?path=demo.genko").status, 404);
    }

    // test_gc_keeps_candidates_and_orphans: gc (a dry run, every asset three days old) would remove a picture nothing
    // refers to and keeps a panel's candidate.
    void gcKeepsCandidatesAndOrphans() {
        const fs::path root = folder(QStringLiteral("gc"));
        StudioService agent(root, "ai:test");
        const std::string made = make_demo(agent);
        QVERIFY2(made.empty(), made.c_str());
        const fs::path book = root / kBook;
        const genko::core::Frame frame = first_leaf(book, 1);
        std::string problem;
        import_candidate(agent, 1, frame.id, flat_png(100, 100, render::Ink{220, 30, 30}), problem);
        QVERIFY2(problem.empty(), problem.c_str());
        const ToolResult stray = agent.call("import_image", Json::array({kBook}), Json{{"png_base64", genko::core::b64encode(flat_png(50, 50, render::Ink{30, 30, 220}))}});
        QVERIFY(stray.ok);
        const auto old = fs::file_time_type::clock::now() - std::chrono::hours(72);
        for (fs::recursive_directory_iterator it(book / "assets"), end; it != end; ++it) {
            if (it->is_regular_file()) fs::last_write_time(it->path(), old);
        }
        const Json result = genko::storage::gc(book, true);
        std::string removed;
        for (const Json& r : result["removed"]) removed += r.get<std::string>() + " ";
        QVERIFY2(has(removed, stray.data["asset"].get<std::string>().substr(7)), removed.c_str());
        const genko::core::Document doc = genko::service::load_episode(book);
        const genko::core::Frame* panel = doc.page(0).find_frame(frame.id);
        QVERIFY(panel != nullptr && panel->panel);
        const std::string candidate = (*panel->panel)["candidates"].at(0)["asset"].get<std::string>();
        QVERIFY(!has(removed, candidate.substr(7)));
    }

    // test_bleed_panel_art_runs_out_to_the_bleed: art adopted on a bleed panel is clipped to the bleed by default, so it
    // prints out to the bleed's corner; the paper beyond the bleed and below the panel stays white. (Python's test turns
    // the monochrome finish off to see the art's colour: the page drawn here without the finish, as render_page's
    // finish=False draws it.)
    void bleedPanelArtRunsOutToTheBleed() {
        const fs::path root = folder(QStringLiteral("bleed"));
        StudioService agent(root, "ai:test");
        const std::string made = make_demo(agent);
        QVERIFY2(made.empty(), made.c_str());
        const fs::path book = root / kBook;
        genko::core::Frame top;
        {
            const genko::core::Document doc = genko::service::load_episode(book);
            const auto leaves = doc.page(0).leaf_frames();
            const genko::core::Frame* highest = leaves.at(0);
            for (const genko::core::Frame* f : leaves) {
                if (f->rect.y.value() < highest->rect.y.value()) highest = f;
            }
            top = *highest;
        }
        std::string step = commit_ops(agent, Json::array({Json{{"op", "set_frame"}, {"page", 1}, {"frame_id", top.id}, {"bleed", true}}}));
        QVERIFY2(step.empty(), step.c_str());
        std::string problem;
        const std::string cand = import_candidate(agent, 1, top.id, flat_png(800, 400, render::Ink{220, 30, 30}), problem);
        QVERIFY2(problem.empty(), problem.c_str());
        step = commit_ops(agent, Json::array({Json{{"op", "adopt_candidate"}, {"page", 1}, {"frame_id", top.id}, {"candidate_id", cand}}}));
        QVERIFY2(step.empty(), step.c_str());
        const genko::core::Document doc = genko::service::load_episode(book);
        const genko::core::Page& page = doc.page(0);
        const genko::core::Layer* placed = nullptr;
        for (const genko::core::Layer& layer : page.layers) {
            if (layer.kind == genko::core::LayerKind::Placed) {
                placed = &layer;
                break;
            }
        }
        QVERIFY(placed != nullptr);
        QCOMPARE(placed->clip_to, std::string("bleed"));
        render::RenderOptions options;
        options.mode = "print";
        options.finish = false;
        const render::Image image = render::render_page(page, 60, options, &doc).image;
        const genko::core::Rect bleed = page.bleed_rect_mm();
        const int x0 = render::mm_to_px(bleed.x.value(), 60);
        const int y0 = render::mm_to_px(bleed.y.value(), 60);
        const auto rgb = [&image](int x, int y) {
            const std::vector<double> p = image.getpixel(x, y);
            return std::array<int, 3>{static_cast<int>(p.at(0)), static_cast<int>(p.at(1)), static_cast<int>(p.at(2))};
        };
        QVERIFY((rgb(x0 + 1, y0 + 1) == std::array<int, 3>{220, 30, 30}));  // (the bleed's top-left corner, cut off in print)
        if (x0 > 2) QVERIFY((rgb(x0 - 2, y0 + 1) == std::array<int, 3>{255, 255, 255}));  // (the paper beyond the bleed)
        QVERIFY((rgb(x0 + 1, image.height() - 2) == std::array<int, 3>{255, 255, 255}));   // (below the panel: paper)
    }

    // --- tests/test_m3_report.py ------------------------------------------------------------------------------------------

    // test_focus_lines_without_faces_say_why: the finishing plan for a panel with 集中線 and no faces reported adds the
    // effect without aim (empty params) and says report_regions would aim it.
    void focusLinesWithoutFacesSayWhy() {
        genko::core::Document ep = genko::core::new_episode("t", genko::core::Num(1), 1, genko::core::PageSpec::b5_doujin());
        genko::core::Page& page = ep.edit_page(0);
        const std::string leaf = page.leaf_frames().at(0)->id;
        genko::core::Frame* frame = page.find_frame(leaf);
        QVERIFY(frame != nullptr);
        frame->panel = Json{{"status", "adopted"}, {"fx", Json::array({"集中線"})}, {"regions", Json::array()}};
        const auto [ops, notes] = genko::service::finish::plan(ep, ep.page(0));
        const Json* effect = nullptr;
        for (const Json& op : ops) {
            if (op.value("op", std::string()) == "add_effect") {
                effect = &op;
                break;
            }
        }
        QVERIFY(effect != nullptr);
        QVERIFY2(genko::core::py_get(*effect, "params") == Json::object(), effect->dump().c_str());
        bool said = false;
        for (const Json& n : notes) said = said || has(genko::core::py_get(n, "why", Json("")), "report_regions");
        QVERIFY2(said, notes.dump().c_str());
    }

    // test_a_shout_is_measured_for_its_valleys: a shout's balloon is measured bigger than a speech balloon (past 1.3
    // times both ways), so the room inside its spikes' valleys (balloons._inner) holds its two columns of six letters.
    void aShoutIsMeasuredForItsValleys() {
        namespace letter = genko::service::letter;
        const std::vector<std::string> breaks = {"オー・イエス", "お詫びに"};
        const auto speech = letter::measure(breaks, "speech");
        const auto shout = letter::measure(breaks, "shout");
        QVERIFY(shout[0] > speech[0] * 1.3 && shout[1] > speech[1] * 1.3);
        const double text_w = letter::kEmMm * 2 + letter::kEmMm * letter::kLeading;
        const double text_h = letter::kEmMm * 6;
        const auto room = render::text::inner("shout", shout[0], shout[1], letter::kPadMm, Json(0.2));
        QVERIFY2(room.first >= text_w - 0.01 && room.second >= text_h - 0.01, (std::to_string(room.first) + " × " + std::to_string(room.second)).c_str());
    }

    // test_person_only_and_gate_refusals_come_back_in_japanese: through the service, the person-only refusal (an
    // agent's set_studio) says in Japanese that only a person may, with the English detail; a gate refusal (a stroke
    // on the ink before the name is approved) says ネームの承認, after the op's place.
    void personOnlyAndGateRefusalsComeBackInJapanese() {
        const fs::path root = folder(QStringLiteral("refusals"));
        StudioService agent(root, "ai:hermes");
        QVERIFY(agent.call("create_project", Json::array({kBook, "demo", 2}), Json::object()).ok);
        const Json policy = agent.call("apply_ops", Json::array({kBook, Json::array({Json{{"op", "set_studio"}, {"policy", Json{{"pilot", false}}}}})}),
                                       Json{{"commit", true}})
                                .to_dict();
        QVERIFY2(!policy.value("ok", true) && has(genko::core::py_get(policy, "error"), "人だけ") && has(genko::core::py_get(policy, "detail"), "only a person"),
                 policy.dump().c_str());
        const Json stroke =
            agent.call("apply_ops",
                       Json::array({kBook, Json::array({Json{{"op", "add_stroke"}, {"page", 1}, {"layer", "ink"},
                                                             {"points", Json::array({Json::array({50, 50}), Json::array({60, 60})})}}})}),
                       Json{{"commit", true}})
                .to_dict();
        const Json said = genko::core::py_get(stroke, "error");
        QVERIFY2(!stroke.value("ok", true) && has(said, "ネームの承認") && said.get<std::string>().starts_with("ops[0] add_stroke: "), stroke.dump().c_str());
    }

    // --- tests/test_m4_pipeline.py --------------------------------------------------------------------------------------

    // test_a_new_agent_resumes_where_the_last_one_stopped: a FakeAgent stopped after 25 work items (art still to be
    // approved) and a second one finishing the book on the same server: the second does not redo what the first did
    // (no item seen by both but those the first saw more than once), every page ends approved and finished, the
    // requests made before are kept, and nothing was asked twice (one request per panel and per character sheet).
    void aNewAgentResumesWhereTheLastOneStopped() {
        if (genko::test::mcp_python().isEmpty()) QSKIP("no Python with the mcp extra: set GENKO_MCP_PYTHON");
        const QString dir = dir_.path() + QStringLiteral("/resume");
        QVERIFY(QDir().mkpath(dir));
        QString problem;
        const Json run = fake_agent_run(dir, "resume", problem);
        QVERIFY2(problem.isEmpty(), qPrintable(problem));
        bool all_art = true;
        for (const Json& ok : run["art_ok_after_first"]) all_art = all_art && ok.get<bool>();
        QVERIFY(!all_art);
        const auto ids = [](const Json& list) {
            std::vector<std::string> out;
            for (const Json& id : list) out.push_back(id.get<std::string>());
            return out;
        };
        const std::vector<std::string> first = ids(run["first_seen"]);
        const std::vector<std::string> second = ids(run["second_seen"]);
        std::set<std::string> repeated;
        for (const std::string& id : first) {
            if (std::count(first.begin(), first.end(), id) > 1) repeated.insert(id);
        }
        const std::set<std::string> first_set(first.begin(), first.end());
        for (const std::string& id : std::set<std::string>(second.begin(), second.end())) {
            QVERIFY2(repeated.count(id) || !first_set.count(id), ("done again by the second agent: " + id).c_str());
        }
        const genko::core::Document doc = genko::service::load_episode(utf8_path(dir + QStringLiteral("/root/story.genko")));
        std::size_t panels = 0;
        for (const auto& page : doc.pages) {
            QVERIFY2(page->art_ok && page->stage == "finish", page->index.repr().c_str());
            panels += page->leaf_frames().size();
        }
        const Json requests = genko::core::py_get(doc.studio, "requests", Json::object());
        for (const Json& before : run["requests_before"]) QVERIFY2(requests.contains(before.get<std::string>()), before.dump().c_str());
        QCOMPARE(requests.size(), panels + doc.bible.characters.size());
    }

    // test_an_agent_that_keeps_failing_asks_a_person_and_moves_on: a FakeAgent whose plan for page 3 is always refused
    // gives up after three tries, asks a person for help once (a help ticket for page 3, assigned to the person) and
    // plans the other pages.
    void anAgentThatKeepsFailingAsksAPersonAndMovesOn() {
        if (genko::test::mcp_python().isEmpty()) QSKIP("no Python with the mcp extra: set GENKO_MCP_PYTHON");
        const QString dir = dir_.path() + QStringLiteral("/broken");
        QVERIFY(QDir().mkpath(dir));
        QString problem;
        const Json run = fake_agent_run(dir, "broken", problem);
        QVERIFY2(problem.isEmpty(), qPrintable(problem));
        int tries = 0;
        for (const Json& line : run["log"]) tries += line.get<std::string>().starts_with("plan_page {'page': 3}") ? 1 : 0;
        QCOMPARE(tries, 3);  // (gave up after three tries instead of looping)
        const genko::core::Document doc = genko::service::load_episode(utf8_path(dir + QStringLiteral("/root/story.genko")));
        std::vector<Json> help;
        for (const Json& t : doc.tickets) {
            if (genko::core::py_get(t, "kind") == Json("help")) help.push_back(t);
        }
        QCOMPARE(help.size(), std::size_t{1});
        QVERIFY(genko::core::py_equals(genko::core::py_get(help[0], "page_index"), Json(3)));
        QCOMPARE(genko::core::py_get(help[0], "assignee"), Json("human"));
        for (const std::size_t i : {std::size_t{0}, std::size_t{1}, std::size_t{3}}) {
            QVERIFY2(doc.page(i).plan && genko::core::py_truthy(*doc.page(i).plan), std::to_string(i).c_str());
        }
    }

    // --- tests/test_m5.py -----------------------------------------------------------------------------------------------

    // test_multi_character_panel_is_fixed_one_person_at_a_time: a panel of two is asked for whole first (the steps say
    // to fix each person by inpaint); focus_character is for fixes only; an inpaint of one person needs the people's
    // regions reported first; then it asks to change that person only, with that person's face and sheet (not the
    // other's), a mask and the source; a face named alone makes another request; a character who is not there is refused.
    void multiCharacterPanelIsFixedOnePersonAtATime() {
        const fs::path root = folder(QStringLiteral("multi"));
        StudioService agent(root, "ai:test");
        const std::string made = make_demo(agent);
        QVERIFY2(made.empty(), made.c_str());
        const fs::path book = root / kBook;
        HumanService human(book, "human:leaf");
        const std::string sheets = approve_sheets(agent, human);
        QVERIFY2(sheets.empty(), sheets.c_str());
        as_person(book, Json::array({Json{{"op", "set_studio"}, {"policy", Json{{"pilot", false}}}}}));
        std::string frame_id;
        const genko::core::Document named = genko::service::load_episode(book);
        for (const genko::core::Frame* f : named.page(3).leaf_frames()) {
            if (f->panel && genko::core::py_get(*f->panel, "characters", Json::array()).size() == 2) {
                frame_id = f->id;
                break;
            }
        }
        QVERIFY(!frame_id.empty());
        const auto ask = [&](Json kwargs) {
            kwargs["page"] = 4;
            kwargs["frame_id"] = frame_id;
            return agent.call("generation_request", Json::array({kBook}), kwargs);
        };
        const Json fresh = ask(Json::object()).data["request"];
        const Json steps = genko::core::py_get(fresh, "steps", Json::array());
        QVERIFY2(steps.is_array() && steps.size() > 2 && has(steps[2], "inpaint"), steps.dump().c_str());
        const ToolResult wrong = ask(Json{{"focus_character", "hina"}});
        QVERIFY(!wrong.ok);
        QCOMPARE(wrong.issues.at(0).path, std::string("/focus_character"));  // (only for fixes)
        std::string problem;
        const std::string cand = make_art(agent, 4, frame_id, problem);
        QVERIFY2(problem.empty(), problem.c_str());
        const ToolResult unreported = ask(Json{{"mode", "inpaint"}, {"parent", cand}, {"focus_character", "hina"}});
        QVERIFY(!unreported.ok && has(unreported.data["error"], "report_regions"));
        const Json figures = agent.call("inspect", Json::array({kBook, "panel", 4, frame_id}), Json::object()).data["panels"][0]["figures"];
        Json regions = Json::array();
        for (const Json& f : figures) regions.push_back(Json{{"kind", "person"}, {"char", f["char"]}, {"rect_mm", f["body_mm"]}});
        for (const Json& f : figures) regions.push_back(Json{{"kind", "face"}, {"char", f["char"]}, {"rect_mm", f["head_mm"]}});
        QVERIFY(agent.call("report_regions", Json::array({kBook, 4, frame_id, regions}), Json::object()).ok);
        const ToolResult fix = ask(Json{{"mode", "inpaint"}, {"parent", cand}, {"focus_character", "hina"}});
        QVERIFY2(fix.ok, fix.to_dict().dump().c_str());
        const Json request = fix.data["request"];
        QCOMPARE(request["focus_character"], Json("hina"));
        QVERIFY(request["prompt"]["ja"].get<std::string>().starts_with("直すのは日向ひなだけ"));
        QVERIFY(has_reference(request, "refs/hina_face.png") && has_reference(request, "refs/hina_sheet.png"));
        QVERIFY(!any_reference_starting(request, "refs/sora_"));
        QCOMPARE(request["files"]["mask"], Json("mask.png"));
        QCOMPARE(request["files"]["source"], Json("source.png"));
        const ToolResult face_only = ask(Json{{"mode", "inpaint"}, {"parent", cand}, {"regions", Json::array({"face:sora"})}});
        QVERIFY(face_only.ok && face_only.data["request"]["id"] != request["id"]);
        // (Python's test reads the request of a reply that has none)
        const ToolResult nobody = ask(Json{{"mode", "inpaint"}, {"parent", cand}, {"focus_character", "nobody"}});
        QVERIFY(!nobody.ok && !nobody.data.contains("request"));
    }

    // test_audit_proves_only_people_approved_and_redo_cannot_sneak_one_in: the audit counts the person's four name
    // approvals; the person undoes them, an agent may not redo them (only a person), the person redoes them and the
    // audit holds; an approval saved around the ops by an agent (a hand-edited book) shows as a violation by that actor.
    void auditProvesOnlyPeopleApprovedAndRedoCannotSneakOneIn() {
        const fs::path root = folder(QStringLiteral("audit"));
        StudioService agent(root, "ai:test");
        const std::string made = make_demo(agent);
        QVERIFY2(made.empty(), made.c_str());
        const fs::path book = root / kBook;
        Json report = genko::service::evaluate::audit(book);
        QVERIFY2(report.value("ok", false), report.dump().c_str());
        QVERIFY(genko::core::py_equals(report["changes"], Json(4)));
        QVERIFY(genko::core::py_equals(report["changes_by_actor"], Json{{"human:leaf", 4}}));
        const genko::test::Run undone = genko::test::run_genko({QStringLiteral("undo"), q(book), QStringLiteral("--as"), QStringLiteral("human:leaf")});
        QVERIFY2(undone.exit_code == 0, qPrintable(undone.described()));
        QVERIFY(!genko::service::load_episode(book).page(3).name_ok);
        std::string refused;
        try {
            genko::storage::restore(book, "ai:test", true, false);
        } catch (const genko::core::Error& error) {
            refused = error.what();
        }
        QVERIFY2(has(refused, "only a person"), refused.c_str());
        const genko::test::Run redone = genko::test::run_genko({QStringLiteral("redo"), q(book), QStringLiteral("--as"), QStringLiteral("human:leaf")});
        QVERIFY2(redone.exit_code == 0, qPrintable(redone.described()));
        QVERIFY(genko::service::load_episode(book).page(3).name_ok);
        QVERIFY(genko::service::evaluate::audit(book).value("ok", false));
        // an approval written around the ops (a bug, or a hand-edited file) shows up as a violation
        {
            genko::storage::ProjectLock lock(book, "ai:rogue");
            lock.try_acquire();
            genko::core::Document doc = genko::service::load_episode(book);
            doc.edit_page(0).art_ok = true;
            genko::storage::SaveRequest request;
            request.actor = "ai:rogue";
            request.base_revision = doc.revision;
            genko::storage::Saver(lock).save(doc, request);
        }
        report = genko::service::evaluate::audit(book);
        QVERIFY2(!report.value("ok", true), report.dump().c_str());
        QCOMPARE(report["violations"].at(0)["actor"], Json("ai:rogue"));
    }

    // test_skill_copy_matches_the_packaged_one: the skill this build serves (genko://guide/skill, the packaged
    // SKILL.md) is the one shipped for Hermes (integrations/hermes/genko-manga/SKILL.md), and every MCP tool it names
    // is one of this build's.
    void skillCopyMatchesThePackagedOne() {
        const auto lf = [](std::string text) {
            for (std::size_t at = text.find("\r\n"); at != std::string::npos; at = text.find("\r\n", at)) text.erase(at, 1);
            return text;
        };
        const std::string packaged = lf(genko::service::data::skill());
        const std::string shipped = lf(genko::storage::read_file(utf8_path(genko::test::repo_root()) / "integrations" / "hermes" / "genko-manga" / "SKILL.md"));
        QVERIFY(packaged == shipped);
        std::set<std::string> tools;
        for (const genko::api::mcp::ToolSpec& tool : genko::api::mcp::tool_specs()) tools.insert(std::string(tool.name));
        const std::regex named(R"(mcp__genko__(\w+))");
        std::size_t seen = 0;
        for (std::sregex_iterator it(packaged.begin(), packaged.end(), named), end; it != end; ++it, ++seen) {
            QVERIFY2(tools.count((*it)[1].str()), (*it)[1].str().c_str());
        }
        QVERIFY(seen > 0);
    }

    // --- tests/test_m6.py ------------------------------------------------------------------------------------------------

    // test_pack_defaults_to_the_spec_dpi: the submission pack without a resolution takes the book's (a B4 comic's 600
    // dpi): its TIFF and PNG pages are drawn at 600 dpi and list.csv says 600.
    void packDefaultsToTheSpecDpi() {
        const fs::path out = folder(QStringLiteral("pack"));
        const genko::core::Document ep = genko::core::new_episode("t", genko::core::Num(1), 1, genko::core::PageSpec::b4_comic());
        QCOMPARE(ep.spec.dpi.value(), 600.0);
        const std::vector<fs::path> written = genko::formats::export_pack(ep, out);
        const render::Size page{render::mm_to_px(ep.page(0).spec.width_mm.value(), 600), render::mm_to_px(ep.page(0).spec.height_mm.value(), 600)};
        int pictures = 0;
        for (const fs::path& file : written) {
            const std::string ext = genko::core::path_to_utf8(file.extension());
            if (ext != ".png" && ext != ".tif" && ext != ".tiff") continue;
            ++pictures;
            const render::Size size = render::picture_size(genko::storage::read_file(file));
            QVERIFY2(size == page, genko::core::path_to_utf8(file).c_str());
        }
        QCOMPARE(pictures, 2);  // (the TIFF and the PNG, both at 600 dpi)
        const std::string csv = genko::storage::read_file(out / "list.csv");
        const std::size_t row = csv.find('\n');
        QVERIFY(row != std::string::npos);
        std::vector<std::string> cells;
        std::size_t start = row + 1;
        for (std::size_t comma = csv.find(',', start); comma != std::string::npos && cells.size() < 3; comma = csv.find(',', start)) {
            cells.push_back(csv.substr(start, comma - start));
            start = comma + 1;
        }
        QVERIFY(cells.size() == 3);
        QCOMPARE(cells[2], std::string("600"));
    }

    // --- tests/test_m8.py ------------------------------------------------------------------------------------------------

    // test_xycut_recovers_panels_of_hand_drawn_pages: on Python's synthetic hand-drawn pages (every layout, two seeds)
    // the XY-cut finds at least 90% of the panels within 5 mm on every edge, at 150 and 300 dpi.
    void xycutRecoversPanelsOfHandDrawnPages_data() {
        QTest::addColumn<int>("dpi");
        QTest::newRow("150 dpi") << 150;
        QTest::newRow("300 dpi") << 300;
    }
    void xycutRecoversPanelsOfHandDrawnPages() {
        QFETCH(int, dpi);
        int hits = 0;
        int total = 0;
        for (std::size_t i = 0; i < synth::layouts().size(); ++i) {
            for (const std::int64_t seed : {1, 2}) {
                const synth::Scan scan = synth::draw_page(synth::layouts()[i], dpi, seed * 100 + static_cast<std::int64_t>(i));
                const genko::service::xycut::Detection found =
                    genko::service::xycut::detect(scan.image, {0, 0, synth::kPageW, synth::kPageH}, genko::service::xycut::Params{}, true);
                hits += matched(found.leaves_mm, scan.truth);
                total += static_cast<int>(scan.truth.size());
            }
        }
        QVERIFY2(static_cast<double>(hits) / total >= 0.9, (std::to_string(hits) + " of " + std::to_string(total)).c_str());
    }

    // test_detection_gives_tiers_and_reading_order: a page of tiers 1, 2 and 1 is found as those tiers, the middle one's
    // columns listed right to left (the right one 0.55 wide); the reading order takes the right column before the left;
    // every panel and the whole detection are confident.
    void detectionGivesTiersAndReadingOrder() {
        const synth::Scan scan = synth::draw_page(synth::layouts()[0], 150, 7);
        const genko::service::xycut::Detection found =
            genko::service::xycut::detect(scan.image, {0, 0, synth::kPageW, synth::kPageH}, genko::service::xycut::Params{}, true);
        QVERIFY2(found.tiers.is_array() && found.tiers.size() == 3, found.tiers.dump().c_str());
        std::vector<std::size_t> columns;
        for (const Json& tier : found.tiers) columns.push_back(tier["cols"].size());
        QVERIFY((columns == std::vector<std::size_t>{1, 2, 1}));
        QVERIFY(std::abs(genko::core::to_float(found.tiers[1]["cols"][0]["w"]) - 0.55) < 0.03);  // (columns right to left, as in the name DSL)
        QVERIFY(found.leaves_mm.size() >= 3);
        QVERIFY(genko::core::to_float(found.leaves_mm[1]["rect_mm"][0]) > genko::core::to_float(found.leaves_mm[2]["rect_mm"][0]));
        for (const Json& panel : found.leaves_mm) {
            const double confidence = genko::core::to_float(panel["confidence"]);
            QVERIFY(0 < confidence && confidence <= 1);
        }
        QVERIFY(found.confidence > 0.6);
    }

    // test_d8_evaluation_command: `genko studio eval-atari` on a truth file of every synthetic layout (aligned to the
    // page) passes, at 90% or more, and reports each page.
    void d8EvaluationCommand() {
        const fs::path dir = folder(QStringLiteral("d8"));
        Json pages = Json::array();
        for (std::size_t i = 0; i < synth::layouts().size(); ++i) {
            const synth::Scan scan = synth::draw_page(synth::layouts()[i], 150, 500 + static_cast<std::int64_t>(i));
            const std::string name = "s" + std::to_string(i) + ".png";
            write_file(dir / name, genko::formats::png_bytes(scan.image));
            Json truth = Json::array();
            for (const synth::Rect& r : scan.truth) truth.push_back(Json::array({r[0], r[1], r[2], r[3]}));
            pages.push_back(Json{{"scan", name}, {"panels", truth}});
        }
        write_file(dir / "truth.json", genko::core::dump_python(Json{{"align", "page"}, {"pages", pages}}));
        const genko::test::Run ran = genko::test::run_genko({QStringLiteral("studio"), QStringLiteral("eval-atari"), q(dir / "truth.json")});
        const Json result = last_line(ran.out);
        QVERIFY2(result.is_object() && result.value("ok", false) && result.value("passed", false), qPrintable(ran.described()));
        QVERIFY(genko::core::to_float(result["rate"]) >= 0.9);
        QCOMPARE(result["pages"].size(), synth::layouts().size());
    }

    // test_a_person_accepts_the_layout_and_the_lines: an agent imports a scan (only a proposal), proposes the lines it
    // reads (a line without text refused at its place) and cannot confirm its own proposals; the person accepts the
    // layout, then the lines: the page gets the panels drawn on the scan and the lines, each in the panel it sits in;
    // then the work is the panels' briefs, and once they are written, the name's approval.
    void aPersonAcceptsTheLayoutAndTheLines() {
        const fs::path root = folder(QStringLiteral("accept"));
        StudioService agent(root, "ai:test");
        Scans scans;
        const std::string made = atari_project(agent, root, 2, scans);
        QVERIFY2(made.empty(), made.c_str());
        const fs::path book = root / kBook;
        StudioService importer(root, "ai:test");
        importer.set_confine(false);
        const ToolResult agent_import = importer.call("import_name", Json::array({kBook, Json::array({genko::core::path_to_utf8(scans.files[0])})}),
                                                      Json{{"align", "page"}});
        QVERIFY2(agent_import.ok, agent_import.to_dict().dump().c_str());  // (an agent may import too: the proposal still needs a person)
        Json layout;
        const Json imported_proposals = genko::core::py_get(genko::service::load_episode(book).studio, "proposals", Json::object());
        for (const auto& [id, proposal] : imported_proposals.items()) {
            if (proposal.value("kind", std::string()) == "layout") {
                layout = proposal;
                break;
            }
        }
        QVERIFY(layout.is_object());
        const Json first = layout["panels"][0]["rect_mm"];
        const double fx = genko::core::to_float(first[0]);
        const double fy = genko::core::to_float(first[1]);
        const double fw = genko::core::to_float(first[2]);
        const Json lines = Json::array({Json{{"text", "5年前の今日\nここで約束した"}, {"balloon", "narration"}, {"x_mm", fx + fw - 20}, {"y_mm", fy + 4}},
                                        Json{{"text", "…来るわけないよね"}, {"box01", Json::array({0.5, 0.45, 0.04, 0.1})}}});
        const ToolResult bad = agent.call("propose_lines", Json::array({kBook, 1, Json::array({Json{{"text", ""}}})}), Json::object());
        QVERIFY(!bad.ok);
        QCOMPARE(bad.issues.at(0).path, std::string("/lines/0"));
        const ToolResult proposed = agent.call("propose_lines", Json::array({kBook, 1, lines}), Json::object());
        QVERIFY2(proposed.ok && !proposed.images.empty(), proposed.to_dict().dump().c_str());
        QVERIFY(genko::service::load_episode(book).story_for_page(genko::core::Num(1)).empty());  // (nothing on the page yet)
        // the agent cannot confirm its own proposals
        for (const Json& op : {Json{{"op", "resolve_proposal"}, {"id", layout["id"]}, {"status", "accepted"}}, Json{{"op", "set_layout"}, {"page", 1}, {"tree", layout["tree"]}}}) {
            QVERIFY2(!agent.call("apply_ops", Json::array({kBook, Json::array({op})}), Json{{"commit", true}}).ok, op.dump().c_str());
        }
        QCOMPARE(genko::service::load_episode(book).studio["proposals"][layout["id"].get<std::string>()]["status"], Json("open"));
        // the person accepts the layout, then the lines
        for (const Json& id : {layout["id"], proposed.data["proposal"]}) {
            const genko::test::Run ran = genko::test::run_genko(
                {QStringLiteral("studio"), QStringLiteral("accept"), q(book), QString::fromStdString(id.get<std::string>()), QStringLiteral("--as"), QStringLiteral("human:leaf")});
            QVERIFY2(last_line(ran.out).value("ok", false), qPrintable(ran.described()));
        }
        const genko::core::Document doc = genko::service::load_episode(book);
        const genko::core::Page& page = doc.page(0);
        const auto leaves = page.leaf_frames();
        QCOMPARE(leaves.size(), scans.truths[0].size());
        std::set<std::string> leaf_ids;
        for (const genko::core::Frame* f : leaves) leaf_ids.insert(f->id);
        for (const synth::Rect& t : scans.truths[0]) {
            bool found = false;
            for (const genko::core::Frame* f : leaves) {
                const Json rect = Json::array({f->rect.x.value(), f->rect.y.value(), f->rect.width.value(), f->rect.height.value()});
                found = found || edge_error(rect, t) <= 5;
            }
            QVERIFY(found);
        }
        const auto added = doc.story_for_page(genko::core::Num(1));
        QCOMPARE(added.size(), std::size_t{2});
        QCOMPARE(added[0]->text, std::string("5年前の今日\nここで約束した"));
        QCOMPARE(added[1]->text, std::string("…来るわけないよね"));
        for (const genko::core::StoryLine* line : added) QVERIFY(line->frame_id && leaf_ids.count(*line->frame_id));  // (each in the panel it sits in)
        std::set<std::string> statuses;
        const Json all_proposals = genko::core::py_get(doc.studio, "proposals", Json::object());
        for (const auto& [id, proposal] : all_proposals.items()) {
            if (proposal.value("page_id", std::string()) == page.id) statuses.insert(proposal.value("status", std::string()));
        }
        QVERIFY((statuses == std::set<std::string>{"accepted"}));
        // next: briefs for the panels, then the name approval
        QVERIFY((kinds_of(agent.call("next", Json::array({kBook}), Json{{"limit", 20}}).data["items"], 1) == std::set<std::string>{"brief_panels"}));
        Json ops = Json::array();
        for (const genko::core::Frame* f : leaves) ops.push_back(Json{{"op", "set_panel"}, {"page", 1}, {"frame_id", f->id}, {"set", Json{{"shot", "MS"}, {"angle", "eye"}}}});
        const std::string briefed = commit_ops(agent, ops);
        QVERIFY2(briefed.empty(), briefed.c_str());
        QVERIFY(contains_py(agent.call("next", Json::array({kBook}), Json{{"limit", 20}}).data["waiting_for"], Json{{"gate", "name"}, {"pages", Json::array({1})}}));
    }

    // test_rejecting_reanalysing_and_user_regions: a person rejects the layout proposed for an imported scan, and the
    // agent is told the layout still needs a proposal; the same analysis again reopens the proposal; once accepted,
    // analysing the scan again only proposes (the person's region stays); a different layout over a page that has one
    // needs force.
    void rejectingReanalysingAndUserRegions() {
        const fs::path root = folder(QStringLiteral("reject"));
        StudioService agent(root, "ai:test");
        Scans scans;
        const std::string made = atari_project(agent, root, 1, scans);
        QVERIFY2(made.empty(), made.c_str());
        const fs::path book = root / kBook;
        const genko::test::Run imported =
            genko::test::run_genko({QStringLiteral("studio"), QStringLiteral("import-name"), q(book), q(scans.files[0]), QStringLiteral("--align"), QStringLiteral("page")});
        QVERIFY2(last_line(imported.out).value("ok", false), qPrintable(imported.described()));
        const Json proposals = genko::service::load_episode(book).studio["proposals"];
        QVERIFY(proposals.is_object() && !proposals.empty());
        const std::string first = proposals.begin().value()["id"].get<std::string>();
        const genko::test::Run rejected = genko::test::run_genko({QStringLiteral("studio"), QStringLiteral("reject"), q(book), QString::fromStdString(first),
                                                                  QStringLiteral("--note"), QStringLiteral("コマが多すぎる"), QStringLiteral("--as"),
                                                                  QStringLiteral("human:leaf")});
        QVERIFY2(last_line(rejected.out).value("ok", false), qPrintable(rejected.described()));
        QVERIFY(kinds_of(agent.call("next", Json::array({kBook}), Json{{"limit", 20}}).data["items"]).count("atari_layout"));  // (the layout still needs a proposal)
        const ToolResult again = agent.call("analyze_name", Json::array({kBook, 1, Json{{"min_panel_mm", 20}}}), Json::object());
        QVERIFY2(again.ok && !again.images.empty(), again.to_dict().dump().c_str());
        const std::string proposal_id = again.data["proposal"].get<std::string>();
        QCOMPARE(genko::service::load_episode(book).studio["proposals"][proposal_id]["status"], Json("open"));  // (reopened if identical)
        HumanService human(book, "human:leaf");
        const Json accepted_now = human.accept_proposal(Json(proposal_id));
        QVERIFY2(accepted_now.value("ok", false), accepted_now.dump().c_str());
        const genko::core::Frame frame = first_leaf(book, 1);
        as_person(book, Json::array({Json{{"op", "add_region"}, {"page", 1}, {"frame_id", frame.id},
                                          {"region", Json{{"kind", "keep"}, {"rect_mm", Json::array({frame.rect.x.value() + 5, frame.rect.y.value() + 5, 20, 20})}}}}}));
        // analysing the scan again only proposes: the person's regions stay
        QVERIFY(agent.call("analyze_name", Json::array({kBook, 1}), Json::object()).ok);
        {
            const genko::core::Document doc = genko::service::load_episode(book);
            const genko::core::Frame* kept = doc.page(0).find_frame(frame.id);
            QVERIFY(kept != nullptr && kept->panel);
            std::vector<std::string> sources;
            for (const Json& r : genko::core::py_get(*kept->panel, "regions", Json::array())) sources.push_back(r.value("source", std::string()));
            QVERIFY((sources == std::vector<std::string>{"user"}));
        }
        // the same analysis again adds nothing new; a different layout over a page that has one needs force
        const Json accepted = genko::service::load_episode(book).studio["proposals"][proposal_id];
        Json other = Json{{"kind", accepted["kind"]}, {"tree", accepted["tree"]}, {"panels", accepted["panels"]}};
        other["tree"]["rect_mm"][3] = genko::core::to_float(other["tree"]["rect_mm"][3]) - 1.0;
        other["id"] = "pr_other";
        other["page"] = 1;
        as_person(book, Json::array({Json{{"op", "propose"}, {"proposal", other}}}));
        const Json result = human.accept_proposal(Json("pr_other"));
        QVERIFY2(!result.value("ok", true) && has(genko::core::py_get(result, "error"), "force"), result.dump().c_str());
        QVERIFY(human.accept_proposal(Json("pr_other"), true).value("ok", false));
    }

    // test_mcp_import_and_proposal_tools: over MCP, import_name refuses a file outside the root and imports a scan from
    // the book's inbox (its proposal has the page's panels); proposals lists the layout; render shows the atari overlay;
    // propose_lines answers with its picture.
    void mcpImportAndProposalTools() {
        const fs::path root = folder(QStringLiteral("mcp-atari"));
        StudioService agent(root, "ai:test");
        Scans scans;
        const std::string made = atari_project(agent, root, 1, scans);
        QVERIFY2(made.empty(), made.c_str());
        write_file(root / kBook / "studio" / "inbox" / "scans" / "p1.png", genko::storage::read_file(scans.files[0]));
        const genko::api::mcp::Tools tools(root, "ai:hermes");
        const auto data = [](const genko::api::mcp::CallOutcome& outcome) {
            return genko::core::parse_python_json(outcome.content.at(0)["text"].get<std::string>());
        };
        const Json outside = data(tools.call("import_name", Json{{"project", kBook}, {"files", Json::array({"/etc/hosts"})}}));
        QVERIFY2(!outside.value("ok", true), outside.dump().c_str());
        const Json imported =
            data(tools.call("import_name", Json{{"project", kBook}, {"files", Json::array({"demo.genko/studio/inbox/scans/p1.png"})}, {"align", "page"}}));
        QVERIFY2(imported.value("ok", false), imported.dump().c_str());
        QVERIFY(genko::core::py_equals(imported["proposals"][0]["panels"], Json(scans.truths[0].size())));
        const Json listed = data(tools.call("proposals", Json{{"project", kBook}}));
        QVERIFY2(listed.value("ok", false) && !genko::core::py_get(listed, "proposals", Json::array()).empty(), listed.dump().c_str());
        QCOMPARE(listed["proposals"][0]["kind"], Json("layout"));
        const genko::api::mcp::CallOutcome shown = tools.call("render", Json{{"project", kBook}, {"page", 1}, {"kind", "atari"}, {"max_px", 500}});
        QVERIFY2(shown.content.size() > 1 && shown.content[1]["type"] == Json("image"), shown.content.dump().substr(0, 2000).c_str());
        const genko::api::mcp::CallOutcome proposed = tools.call(
            "propose_lines",
            Json{{"project", kBook}, {"page", 1}, {"lines", Json::array({Json{{"text", "待って！"}, {"balloon", "shout"}, {"box01", Json::array({0.6, 0.1, 0.05, 0.1})}}})}});
        QVERIFY2(data(proposed).value("ok", false), proposed.content.dump().substr(0, 2000).c_str());
        QVERIFY(proposed.content.size() > 1 && proposed.content[1]["type"] == Json("image"));
    }

    // --- tests/test_m9.py ------------------------------------------------------------------------------------------------

    // test_mannequin_in_a_panel_becomes_the_pose_guide: a mannequin a person put in a panel becomes the request's pose
    // guide (guides/pose.png, drawn with black) with a note to follow it (not there before); a monochrome page's request
    // is not in colour; the mannequin is in that panel's rect and not in one beside it.
    void mannequinInAPanelBecomesThePoseGuide() {
        const fs::path root = folder(QStringLiteral("mannequin"));
        StudioService agent(root, "ai:test");
        const std::string made = make_demo(agent);
        QVERIFY2(made.empty(), made.c_str());
        const fs::path book = root / kBook;
        const genko::core::Frame frame = first_leaf(book, 1);
        const Json before = agent.call("generation_request", Json::array({kBook}), Json{{"page", 1}, {"frame_id", frame.id}}).data;
        const double rx = frame.rect.x.value();
        const double ry = frame.rect.y.value();
        const double rw = frame.rect.width.value();
        const double rh = frame.rect.height.value();
        as_person(book, Json::array({Json{{"op", "add_mannequin"}, {"page", 1}, {"pos", Json::array({rx + rw / 2, ry + rh * 0.6, 0})},
                                          {"height_mm", rh * 0.6}, {"preset", "point"}, {"id", "pose1"}}}));
        const Json after = agent.call("generation_request", Json::array({kBook}), Json{{"page", 1}, {"frame_id", frame.id}}).data;
        QVERIFY2(before.contains("request") && after.contains("request"), after.dump().c_str());
        QCOMPARE(after["request"]["files"]["pose"], Json("guides/pose.png"));
        QCOMPARE(after["request"]["color"], Json(false));
        QVERIFY(any_note(after["request"], "マネキン"));
        QVERIFY(!any_note(before["request"], "マネキン"));
        QVERIFY(after.contains("dir"));
        const render::Image guide = render::read_png(genko::storage::read_file(utf8_path(after["dir"]) / "guides" / "pose.png")).convert("L");
        QCOMPARE(guide.getextrema().at(0).first, 0.0);
        // a mannequin elsewhere on the page is not this panel's pose
        const genko::core::Document other = genko::service::load_episode(book);
        const Json& prim = other.page(0).prims.back();
        QVERIFY(genko::core::mannequin::in_rect(prim, {rx, ry, rw, rh}));
        QVERIFY(!genko::core::mannequin::in_rect(prim, {rx + rw + 50, ry, 10, 10}));
    }

    // test_colour_pages_get_the_colour_vocabulary: the colour phrase book does not avoid 色 and uses the colour style
    // words (the monochrome one avoids 色 and says monochrome); a request for a colour page avoids no 色 and starts its
    // prompt with the colour style, a monochrome page's request avoids 色. (Python's test calls genreq._avoid; here the
    // request genreq builds, whose avoid list that is.)
    void colourPagesGetTheColourVocabulary() {
        namespace genreq = genko::service::genreq;
        const Json mono = genreq::vocab("mono");
        const Json color = genreq::vocab("color");
        QVERIFY(contains_py(mono["avoid"], Json("色")) && !contains_py(color["avoid"], Json("色")));
        QCOMPARE(color["style"], color["style_color"]);
        std::string en = color["style"]["en"].get<std::string>();
        std::transform(en.begin(), en.end(), en.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        QVERIFY(has(en, "colour"));
        QVERIFY(has(mono["style"]["tags"], "monochrome"));
        const fs::path project = folder(QStringLiteral("colour"));
        const genko::core::Document ep = genko::core::new_episode("t", genko::core::Num(1), 1, genko::core::PageSpec::a4_mono());
        genko::core::Document coloured = ep;
        coloured.edit_page(0).spec.expression = "color";
        const auto request_for = [&project](const genko::core::Document& doc) {
            genreq::Options options;
            options.page = Json(1);
            options.frame_id = Json(doc.page(0).leaf_frames().at(0)->id);
            return genreq::build(doc, project, options).request;
        };
        const Json colour_request = request_for(coloured);
        QVERIFY(!contains_py(colour_request["avoid"], Json("色")));
        QCOMPARE(colour_request["color"], Json(true));
        QVERIFY(colour_request["prompt"]["ja"].get<std::string>().starts_with(color["style"]["ja"].get<std::string>()));
        QVERIFY(contains_py(request_for(ep)["avoid"], Json("色")));
    }

    // test_claude_code_skill_copy_matches_the_packaged_skill: the skill shipped for Claude Code is the packaged one with
    // Claude Code's tool names (mcp__genko__…), and its MCP example starts `genko mcp` as an agent (ai:…).
    void claudeCodeSkillCopyMatchesThePackagedSkill() {
        const auto lf = [](std::string text) {
            for (std::size_t at = text.find("\r\n"); at != std::string::npos; at = text.find("\r\n", at)) text.erase(at, 1);
            return text;
        };
        std::string packaged = lf(genko::service::data::skill());
        for (std::size_t at = packaged.find("mcp_genko_"); at != std::string::npos; at = packaged.find("mcp_genko_", at + 12)) {
            packaged.replace(at, 10, "mcp__genko__");
        }
        const fs::path claude = utf8_path(genko::test::repo_root()) / "integrations" / "claude-code";
        QVERIFY(lf(genko::storage::read_file(claude / "skills" / "genko-manga" / "SKILL.md")) == packaged);
        const Json config = genko::core::parse_python_json(genko::storage::read_file(claude / "mcp.example.json"));
        const Json args = config["mcpServers"]["genko"]["args"];
        bool mcp = false;
        bool agent = false;
        for (const Json& a : args) {
            mcp = mcp || a == Json("mcp");
            agent = agent || (a.is_string() && a.get<std::string>().starts_with("ai:"));
        }
        QVERIFY(mcp && agent);
    }

    // test_generic_mcp_client_example_runs: integrations/generic/mcp_client_example.py, on this build's `genko mcp`,
    // makes a book, writes its bible, script and name, gets work from next and reads the skill.
    void genericMcpClientExampleRuns() {
        if (genko::test::mcp_python().isEmpty()) QSKIP("no Python with the mcp extra: set GENKO_MCP_PYTHON");
        const QString dir = dir_.path() + QStringLiteral("/example");
        QVERIFY(QDir().mkpath(dir));
        const QString driver = dir + QStringLiteral("/example_run.py");
        genko::test::write_bytes(driver, kExampleRun);
        const genko::test::Run run = genko::test::run(
            genko::test::mcp_python(),
            {driver, genko::test::repo_root() + QStringLiteral("/integrations/generic/mcp_client_example.py"), genko::test::genko_cli(), dir + QStringLiteral("/root")},
            mcp_env(), 600 * 1000);
        const QString problem = run_problem(run);
        QVERIFY2(problem.isEmpty(), qPrintable(problem));
        const Json report = last_line(run.out);
        QVERIFY2(report.is_object(), run.out.constData());
        for (const char* key : {"create", "bible", "script", "name"}) QVERIFY2(genko::core::py_truthy(report.value(key, Json())), key);
        const Json next = genko::core::py_get(report, "next");
        QVERIFY(next.is_array() && !next.empty());
        QVERIFY(report.value("skill_chars", 0) > 1000);
    }
};

QTEST_GUILESS_MAIN(TestLedgerServiceB)
#include "test_ledger_service_b.moc"
