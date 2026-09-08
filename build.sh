#!/bin/sh
# Build every tool and run every test.
#
#   ./build.sh              build into ./build, then run both suites
#   OUT=/tmp/x ./build.sh   build somewhere else
#   CC=gcc ./build.sh       use a different compiler
#
# No -std: patch_macho.c assigns to an anonymous struct, which clang rejects
# under -std=c11 and accepts in its default gnu dialect. Everything here builds
# with the stock 10.9 toolchain and has no dependencies beyond libc.
set -e
CC="${CC:-clang}"
OUT="${OUT:-build}"
mkdir -p "$OUT"

echo "[1] tools..."
for t in patch_macho change_dylib add_version_min fix_macho rename_segment retag_swift_classes; do
    "$CC" -O2 -Wall -o "$OUT/$t" "$t.c"
    echo "    $OUT/$t"
done

echo "[2] macho_grow_test -- hermetic: the grow, every re-baser, verify, plausibility..."
"$CC" -O2 -Wno-unused-function -o "$OUT/macho_grow_test" macho_grow_test.c
"$OUT/macho_grow_test"

echo "[3] change_dylib_test -- builds real dylibs, rewrites a real binary, RUNS it..."
sh change_dylib_test.sh

echo "OK"
