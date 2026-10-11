#include "service/jobs.hpp"

#include <QCoreApplication>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
#include <utility>
#include <vector>

#include "core/clock.hpp"
#include "core/command_bus.hpp"
#include "core/error.hpp"
#include "core/ids.hpp"
#include "core/pyconv.hpp"
#include "core/pynum.hpp"
#include "core/pyops.hpp"
#include "render/not_yet_ported.hpp"
#include "storage/fsutil.hpp"
#include "studio/files.hpp"

namespace genko::service::jobs {

namespace fs = std::filesystem;

namespace {

double now() { return std::chrono::duration<double>(std::chrono::system_clock::now().time_since_epoch()).count(); }

fs::path folder_of(const fs::path& project) { return project / "studio" / "jobs"; }

// _write(project, record): the record replaced whole (beside it first).
void write_record(const fs::path& project, const Json& record) {
    const fs::path folder = folder_of(project);
    fs::create_directories(folder);
    storage::write_atomic(folder / core::path_from_utf8(record["id"].get<std::string>() + ".json"), studio::text_mode(core::dump_python(record)));
}

// One job's state, shared by the thread that waits for it, its worker and its beat.
struct Shared {
    std::mutex lock;
    std::condition_variable changed;
    bool ended = false;
    std::optional<Json> reply;
};

// The threads this process started (joined by wait_all, or when the process ends).
class Threads {
public:
    ~Threads() { join_all(); }

    void add(std::thread thread, std::shared_ptr<std::atomic<bool>> done) {
        std::lock_guard<std::mutex> guard(lock_);
        // (threads that finished are joined as new ones come, so a long-running server does not keep them)
        for (auto it = running_.begin(); it != running_.end();) {
            if (it->second->load()) {
                it->first.join();
                it = running_.erase(it);
            } else {
                ++it;
            }
        }
        running_.emplace_back(std::move(thread), std::move(done));
    }

    void join_all() {
        for (;;) {
            std::vector<std::pair<std::thread, std::shared_ptr<std::atomic<bool>>>> taken;
            {
                std::lock_guard<std::mutex> guard(lock_);
                taken.swap(running_);
            }
            if (taken.empty()) return;
            for (auto& [thread, done] : taken) {
                (void)done;
                if (thread.joinable()) thread.join();
            }
        }
    }

private:
    std::mutex lock_;
    // (each flag is set by its thread as its last step: join then waits only for the thread's return)
    std::vector<std::pair<std::thread, std::shared_ptr<std::atomic<bool>>>> running_;
};

Threads& threads() {
    static Threads all;
    return all;
}

// A thread that sets its flag when it ends.
void launch(std::function<void()> body) {
    auto done = std::make_shared<std::atomic<bool>>(false);
    std::thread thread([body = std::move(body), done]() mutable {
        try {
            body();
        } catch (...) {  // (each body says what broke itself; nothing may leave a thread)
        }
        body = nullptr;
        done->store(true);
    });
    threads().add(std::move(thread), std::move(done));
}

Json with(const Json& record, std::initializer_list<std::pair<const char*, Json>> changes) {
    Json out = record;
    for (const auto& [key, value] : changes) out[key] = value;
    return out;
}

}  // namespace

std::string exception_text(const std::exception_ptr& caught) {
    const auto named = [](const std::string& type, const std::string& message) { return type + ": " + message; };
    try {
        std::rethrow_exception(caught);
    } catch (const core::PyUncaught& exc) {
        return named(exc.type(), exc.what());
    } catch (const core::PyValueError& exc) {
        return named("ValueError", exc.what());
    } catch (const core::PyTypeError& exc) {
        return named("TypeError", exc.what());
    } catch (const core::ApplyError& exc) {
        return named("ApplyError", exc.what());
    } catch (const core::OpError& exc) {
        return named("ApplyError", exc.what());
    } catch (const render::NotYetPorted& exc) {
        return named("NotYetPorted", exc.what());
    } catch (const core::Error& exc) {
        const std::string code = exc.code();
        if (code == "not_found") return named("FileNotFoundError", exc.what());
        if (code == "json") return named("JSONDecodeError", exc.what());
        if (code == "memory") return named("MemoryError", exc.what());
        return named(code == "io" ? "OSError" : "RuntimeError", exc.what());
    } catch (const std::bad_alloc& exc) {
        return named("MemoryError", exc.what());
    } catch (const std::exception& exc) {
        return named("RuntimeError", exc.what());
    } catch (...) {
        return "RuntimeError: unknown";
    }
}

Json start(const fs::path& project, const std::string& kind, std::function<Json()> work, const std::string& actor, double wait) {
    // (the id is always random, as uuid.uuid4 is: tests that count ids count only the book's)
    const std::string job_id = "job_" + core::local_time_text("%Y%m%d-%H%M%S") + "_" + core::new_txn_id().substr(0, 6);
    const double started = now();
    const Json record = Json::object({{"id", job_id}, {"kind", kind}, {"actor", actor}, {"status", "running"}, {"started", started},
                                      {"beat", started}, {"pid", static_cast<std::int64_t>(QCoreApplication::applicationPid())}});
    write_record(project, record);
    auto shared = std::make_shared<Shared>();

    launch([project, record, shared] {  // (says the work is still alive, until it ends)
        std::unique_lock<std::mutex> guard(shared->lock);
        while (!shared->changed.wait_for(guard, std::chrono::duration<double>(kBeatS), [&shared] { return shared->ended; })) {
            try {
                write_record(project, with(record, {{"beat", now()}}));
            } catch (const std::exception&) {  // (OSError: the next beat tries again)
            }
        }
    });
    launch([project, record, shared, work = std::move(work)] {
        Json reply;
        std::string status;
        try {
            reply = work();
            status = core::py_truthy(core::py_get(reply, "ok", Json(true))) ? "done" : "failed";
        } catch (...) {  // (whatever broke, the job says so instead of staying "running")
            reply = Json::object({{"ok", false}, {"error", exception_text(std::current_exception())}});
            status = "failed";
        }
        std::lock_guard<std::mutex> guard(shared->lock);
        shared->reply = reply;
        shared->ended = true;
        shared->changed.notify_all();
        try {
            const double finished = now();
            write_record(project, with(record, {{"status", status}, {"beat", finished}, {"finished", finished}, {"result", reply}}));
        } catch (const std::exception&) {  // (the reply is given all the same; the record says running until it is lost)
        }
    });

    std::unique_lock<std::mutex> guard(shared->lock);
    shared->changed.wait_for(guard, std::chrono::duration<double>(std::max(0.0, wait)), [&shared] { return shared->reply.has_value(); });
    if (shared->reply) {
        Json out = *shared->reply;
        if (!out.is_object()) out = Json::object();
        out["job"] = job_id;
        return out;
    }
    return Json::object({{"ok", true}, {"job", job_id}, {"status", "running"}, {"kind", kind},
                         {"hint", "まだ書き出している。しばらく（1〜5 分）待って export_status に job を渡すと結果が返る"}});
}

Json status(const fs::path& project, const Json& job_id) {
    const Json missing = Json::object({{"ok", false}, {"error", "job " + core::py_str(job_id) + " はない"}});
    if (!core::py_truthy(job_id)) return missing;
    std::string id;
    if (job_id.is_string()) {
        id = job_id.get<std::string>();
        if (id.find('/') != std::string::npos || id.find('\\') != std::string::npos || id.front() == '.') return missing;
    } else if (job_id.is_array() || job_id.is_object()) {
        for (const char* bad : {"/", "\\"}) {
            const bool in = job_id.is_object() ? job_id.contains(bad)
                                               : std::any_of(job_id.begin(), job_id.end(), [bad](const Json& v) { return core::py_equals(v, Json(bad)); });
            if (in) return missing;
        }
        core::raise_attribute_error(job_id, "startswith");
    } else {
        throw core::PyTypeError("argument of type '" + core::py_type_name(job_id) + "' is not iterable");
    }
    Json record;
    try {
        record = core::parse_python_json(storage::read_file(folder_of(project) / core::path_from_utf8(id + ".json")), nullptr,
                                         core::ParseOptions{false, true});
    } catch (const core::Error&) {  // (OSError, ValueError)
        return missing;
    }
    if (!record.is_object()) core::raise_attribute_error(record, "get");
    const double t = now();
    const double started = core::to_float(core::py_get(record, "started", Json(0)));
    const double elapsed = t - started;
    const double silent = t - core::to_float(core::py_get(record, "beat", core::py_get(record, "started", Json(0))));
    const Json state = [&record] {
        if (!record.contains("status")) throw core::PyUncaught("KeyError", "'status'");
        return record["status"];
    }();
    if (core::py_equals(state, Json("running")) && (elapsed > kLostS || (record.contains("beat") && silent > kStaleS))) record["status"] = "lost";
    Json out = Json::object({{"ok", true}, {"job", job_id}, {"status", record["status"]}, {"kind", core::py_get(record, "kind")},
                             {"seconds", core::py_round(elapsed, 1)}});
    if (core::is_one_of(record["status"], {"done", "failed"})) {
        out["seconds"] = core::py_round(core::to_float(core::py_get(record, "finished", Json(now()))) - started, 1);
        out["result"] = core::py_get(record, "result");
    }
    if (core::py_equals(record["status"], Json("lost"))) {
        out["hint"] = "書き出しの途中で、書き出していた Genko（MCP サーバ）が止まった。もう一度 export を呼び、終わるまで同じつなぎのまま export_status で待つ";
    }
    return out;
}

void wait_all() { threads().join_all(); }

}  // namespace genko::service::jobs
