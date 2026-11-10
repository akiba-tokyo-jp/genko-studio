# vcpkg's x64-windows-static-md (every library static, the C runtime as a DLL) with one exception (PC4 F-e): LibRaw
# (LGPL-2.1-only OR CDDL-1.0) is built as a DLL of its own (bin\raw_r.dll beside genko.exe: native/package/LIBRAW-LGPL-ja.md),
# never linked statically into the program. What it needs (JasPer, Little CMS, zlib, libjpeg-turbo) stays static inside
# that DLL. CMakePresets.json's windows presets name this folder in VCPKG_OVERLAY_TRIPLETS.
set(VCPKG_TARGET_ARCHITECTURE x64)
set(VCPKG_CRT_LINKAGE dynamic)
set(VCPKG_LIBRARY_LINKAGE static)

if(PORT STREQUAL "libraw")
  set(VCPKG_LIBRARY_LINKAGE dynamic)
endif()
