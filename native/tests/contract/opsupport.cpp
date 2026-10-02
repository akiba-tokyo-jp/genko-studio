#include "opsupport.hpp"

#include <QByteArray>
#include <QCryptographicHash>

#include "core/actor.hpp"
#include "core/command_bus.hpp"
#include "core/error.hpp"
#include "core/ids.hpp"
#include "render/ops_registry.hpp"
#include "storage/lock.hpp"
#include "storage/reader.hpp"
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

StepOutcome state_of(const core::Document& doc, storage::AssetStore& store) {
    StepOutcome out;
    out.full = storage::snapshot(doc, true);
    out.payload = as_v3_payload(storage::project_payload_v4(doc, store));
    return out;
}

std::vector<StepOutcome> run_steps(core::Document doc, const Json& steps, std::uint64_t first_id, storage::AssetStore& store,
                                   bool digest, core::Document* last) {
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
        out.push_back(std::move(outcome));
    }
    if (last != nullptr) *last = std::move(doc);
    return out;
}

std::string compare_step(const StepOutcome& cpp, const Json& python) {
    std::string where;
    const Json& py_reply = python["reply"];
    if (py_reply.contains("uncaught")) {
        // (Python's command line prints a ValueError's message as its error, and stops with a traceback on the others)
        const std::string type = py_reply["uncaught"].get<std::string>();
        const std::string message = py_reply["error"].get<std::string>();
        const std::string want = type + ": " + message;
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
