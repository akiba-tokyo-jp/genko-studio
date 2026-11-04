# genko-plugin-host (PC5 J-f, plugins:WASM-ABI-v1, docs/PLUGIN_WASM.md): the WebAssembly plugins' host — PhotoCraft
# v0.2.0's crates/plugins (wasmi 2.0.0) copied as Rust into native/wasm-host, a small program of its own that Genko starts
# for each run and speaks to over its plugin protocol 2. Built with cargo (crates from crates.io, pinned and checked by
# native/wasm-host/Cargo.lock: --locked) into the build folder, and copied beside genko (render::plugins::host_program
# finds it there). Without cargo, or when cargo cannot build it (no network for the crates the first time), only the
# WebAssembly plugins are missing: Genko says so when one is chosen; every other part builds as before.
#
#   GENKO_BUILD_PLUGIN_HOST  ON (default) | OFF
#
# Windows: the same cargo build (the MSVC toolchain); checked on a Windows machine after the merge.

option(GENKO_BUILD_PLUGIN_HOST "Build genko-plugin-host (the WebAssembly plugins' host, Rust: cargo)" ON)

set(GENKO_PLUGIN_HOST_NAME "")  # (the program's name beside genko when it is built: tests and the distribution)
if(NOT GENKO_BUILD_PLUGIN_HOST)
  message(STATUS "genko-plugin-host: off (GENKO_BUILD_PLUGIN_HOST=OFF): WebAssembly plugins will not run")
  return()
endif()
find_program(GENKO_CARGO cargo HINTS "$ENV{CARGO_HOME}/bin" "$ENV{HOME}/.cargo/bin")
if(NOT GENKO_CARGO)
  message(STATUS "genko-plugin-host: cargo was not found: WebAssembly plugins will not run (Genko says so when one is chosen)")
  return()
endif()

set(_genko_host_src "${GENKO_SOURCE_ROOT}/wasm-host")
set(_genko_host_target_dir "${CMAKE_BINARY_DIR}/wasm-host")
set(_genko_host_name "genko-plugin-host${CMAKE_EXECUTABLE_SUFFIX}")
set(_genko_host_stamp "${_genko_host_target_dir}/genko-plugin-host.stamp")
file(GLOB _genko_host_sources CONFIGURE_DEPENDS "${_genko_host_src}/src/*.rs")
add_custom_command(
  OUTPUT "${_genko_host_stamp}"
  COMMAND "${CMAKE_COMMAND}" -DCARGO=${GENKO_CARGO} -DSOURCE=${_genko_host_src} -DTARGET_DIR=${_genko_host_target_dir}
          -DNAME=${_genko_host_name} -DDEST=$<TARGET_FILE_DIR:genko> -DSTAMP=${_genko_host_stamp}
          -P "${GENKO_SOURCE_ROOT}/cmake/genko_plugin_host_build.cmake"
  DEPENDS ${_genko_host_sources} "${_genko_host_src}/Cargo.toml" "${_genko_host_src}/Cargo.lock"
          "${GENKO_SOURCE_ROOT}/cmake/genko_plugin_host_build.cmake"
  COMMENT "genko-plugin-host: cargo build --release --locked"
  VERBATIM)
add_custom_target(genko_plugin_host ALL DEPENDS "${_genko_host_stamp}")
set(GENKO_PLUGIN_HOST_NAME "${_genko_host_name}")
