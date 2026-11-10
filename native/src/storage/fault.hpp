#pragma once

#include <cstddef>
#include <filesystem>
#include <string_view>

// Fault injection for the save tests (docs/cpp-migration/FIXTURES.md F5, ACCEPTANCE.md AC-SAVE 5 and 10).
//
//   GENKO_FAULT=<stage>:<action>[,<stage>:<action>…]
//
// stages (schema-v4 §4.2): assets (new assets: strokes, pictures, states, op records), prepare (the prepare line),
// project (project.json), audit (studio/audit.jsonl), commit (the commit line; also the commit and abort lines
// a repair writes). The repair's audit lines are in the audit stage.
// actions:
//   fail   every write of the stage fails with ENOSPC before anything is written (a full disk)
//   late   the write is made, then fails with ENOSPC (as when the fsync of the file or its folder fails)
//   crash  the process stops (std::_Exit(77)) just after the write
//   torn   half of the bytes are written, then the process stops (std::_Exit(77)): a cut journal line
//
// Built only with the CMake option GENKO_FAULT_INJECTION (on for Debug and ASan builds, off for Release): a release
// build ignores the variable and every function here does nothing.

namespace genko::storage::fault {

enum class Stage { none, assets, prepare, project, audit, commit };

// Whether this build has fault injection.
bool compiled_in();

// The stage of the writes this thread makes while the scope lives.
class StageScope {
public:
    explicit StageScope(Stage stage);
    ~StageScope();
    StageScope(const StageScope&) = delete;
    StageScope& operator=(const StageScope&) = delete;

private:
    Stage previous_;
};

// Called by the write functions: before a write (fail: throws core::Error("io") with ENOSPC's text for `target`)…
void before_write(const std::filesystem::path& target);
// …how many of `size` bytes to write (torn: half; the caller then calls after_write, which stops the process)…
std::size_t bytes_to_write(std::size_t size);
// …and after the write (late: throws core::Error("io"); crash and torn: std::_Exit(77)).
void after_write(const std::filesystem::path& target);

// Tests: the specification to use instead of GENKO_FAULT, in this process (empty: no faults). Throws
// core::Error("value") for a specification it does not understand, as GENKO_FAULT does on the first write.
void set_for_testing(std::string_view spec);

}  // namespace genko::storage::fault
