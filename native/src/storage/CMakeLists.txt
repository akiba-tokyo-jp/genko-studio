genko_library(genko_storage
  SOURCES
    fsutil.cpp
    asset_store.cpp
    reader.cpp
    writer.cpp
    snapshot.cpp
  PUBLIC_DEPS genko_core Qt6::Core)
