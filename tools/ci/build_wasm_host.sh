#!/usr/bin/env bash
# genko-plugin-host for Genko's WebAssembly plugins (PC5 J-f, docs/PLUGIN_WASM.md, docs/cpp-migration/THIRD_PARTY.md):
# native/wasm-host — PhotoCraft v0.2.0's crates/plugins copied as Rust, on wasmi 2.0.0 — its crates from crates.io at the
# versions and checksums of native/wasm-host/Cargo.lock (--locked: a crate whose checksum differs is refused), its own
# tests run (the plug-in ABI end to end through Genko's protocol 2), and the program built. CMake builds the same program
# beside genko (native/cmake/GenkoPluginHost.cmake); this script is the CI's check of the host itself.
#
#   tools/ci/build_wasm_host.sh [TARGET_DIR]
#
# TARGET_DIR: cargo's target folder (default: build/wasm-host under the repository). Windows: the same commands with
# the MSVC toolchain (checked on a Windows machine after the merge).
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
TARGET=${1:-$ROOT/build/wasm-host}
export CARGO_TARGET_DIR=$TARGET
cargo --version
cargo test --release --locked --manifest-path "$ROOT/native/wasm-host/Cargo.toml"
cargo build --release --locked --manifest-path "$ROOT/native/wasm-host/Cargo.toml"
"$TARGET/release/genko-plugin-host" --version
