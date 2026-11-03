# Builds genko-plugin-host (cmake -P, from GenkoPluginHost.cmake): `cargo build --release --locked` with its target
# folder in the build folder, the program copied beside genko. A build that fails is said (a warning) and leaves no
# program there (and no stamp: tried again at the next build): only the WebAssembly plugins are missing, the rest of the
# build goes on.
#   -DCARGO= -DSOURCE= -DTARGET_DIR= -DNAME= -DDEST= -DSTAMP=
cmake_minimum_required(VERSION 3.24)
execute_process(
  COMMAND "${CMAKE_COMMAND}" -E env "CARGO_TARGET_DIR=${TARGET_DIR}" "${CARGO}" build --release --locked
          --manifest-path "${SOURCE}/Cargo.toml"
  RESULT_VARIABLE _rc)
if(_rc EQUAL 0 AND EXISTS "${TARGET_DIR}/release/${NAME}")
  file(MAKE_DIRECTORY "${DEST}")
  file(COPY_FILE "${TARGET_DIR}/release/${NAME}" "${DEST}/${NAME}" ONLY_IF_DIFFERENT)
  file(TOUCH "${STAMP}")
else()
  # (no stamp: tried again at the next build)
  message(WARNING "genko-plugin-host could not be built (cargo: ${_rc}): WebAssembly plugins will not run")
  file(REMOVE "${DEST}/${NAME}")
endif()
