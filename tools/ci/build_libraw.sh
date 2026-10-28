#!/usr/bin/env bash
# LibRaw for Genko's camera RAW (PC4 F-e, docs/cpp-migration/THIRD_PARTY.md): libraw.org's own source of LibRaw 0.22.2,
# checked by its sha256, built as a shared library (LGPL-2.1 or CDDL-1.0: linked dynamically, never statically) without
# OpenMP, JasPer or Little CMS, its libraries and headers put in PREFIX. CMake finds it there: -DGENKO_LIBRAW_ROOT=PREFIX
# (or the environment's GENKO_LIBRAW_ROOT). Windows takes vcpkg's libraw instead (native/cmake/triplets).
#
#   tools/ci/build_libraw.sh PREFIX [TARBALL]
#
# TARBALL: LibRaw-0.22.2.tar.gz already downloaded (else it is downloaded from https://www.libraw.org/data/).
set -euo pipefail
VERSION=0.22.2
SHA256=de86b035655accff8d4010f1a221fdf50d353cb7b1422ba26f14a0db92612cfa
PREFIX=${1:?usage: build_libraw.sh PREFIX [TARBALL]}
TARBALL=${2:-}
mkdir -p "$PREFIX"
PREFIX=$(cd "$PREFIX" && pwd)
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT
if [ -z "$TARBALL" ]; then
  TARBALL="$WORK/LibRaw-$VERSION.tar.gz"
  curl -fsSL -o "$TARBALL" "https://www.libraw.org/data/LibRaw-$VERSION.tar.gz"
fi
echo "$SHA256  $TARBALL" | sha256sum -c -
mkdir "$WORK/src" "$WORK/obj"
tar -xzf "$TARBALL" -C "$WORK/src"
cd "$WORK/obj"
"$WORK/src/LibRaw-$VERSION/configure" --prefix="$PREFIX" --enable-shared --disable-static --disable-openmp --disable-jasper --disable-lcms \
  --disable-examples --enable-zlib --enable-jpeg CFLAGS="-O2" CXXFLAGS="-O2"
make -j"$(nproc)"
make install
# (the licence texts beside it: LibRaw's own COPYRIGHT, LICENSE.LGPL and LICENSE.CDDL)
mkdir -p "$PREFIX/share/doc/libraw"
cp "$WORK/src/LibRaw-$VERSION/COPYRIGHT" "$WORK/src/LibRaw-$VERSION/LICENSE.LGPL" "$WORK/src/LibRaw-$VERSION/LICENSE.CDDL" "$PREFIX/share/doc/libraw/"
