#include "opsupport.hpp"

#include <QByteArray>
#include <QCryptographicHash>

#include "core/actor.hpp"
#include "core/command_bus.hpp"
#include "core/error.hpp"
#include "core/ids.hpp"
#include "storage/snapshot.hpp"
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
            core::ApplyResult result = core::CommandBus().apply(doc, step["ops"], core::Actor(agent), dry_run);
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

}  // namespace genko::test
