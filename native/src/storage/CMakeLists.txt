genko_library(genko_storage
  SOURCES
    fsutil.cpp
    fault.cpp
    asset_store.cpp
    reader.cpp
    writer.cpp
    snapshot.cpp
    lock.cpp
    state.cpp
    journal.cpp
    transaction.cpp
    undo.cpp
    gc.cpp
    doctor.cpp
    migrate.cpp
  PUBLIC_DEPS genko_core Qt6::Core)

# Fault injection (storage/fault.hpp): 1 only when the option is on and the configuration is not a release one (also
# with multi-config generators). Public, so the tests know whether they can inject faults.
target_compile_definitions(genko_storage PUBLIC
  GENKO_FAULT_INJECTION=$<IF:$<AND:$<BOOL:${GENKO_FAULT_INJECTION}>,$<NOT:$<CONFIG:Release,RelWithDebInfo,MinSizeRel>>>,1,0>)
