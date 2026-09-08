#!/bin/sh
# Cover patch_macho's conversion, which tests/fixture.macho cannot reach.
#
# That fixture was built on 10.9, whose linker predates chained fixups by a
# decade, so it has none to convert -- leaving the heaviest transform in the
# pipeline (94,900 rebases on a real binary) untested by the characterization.
# This builds a binary that DOES use them, on a host that can emit them, and
# runs the conversion over it.
#
# Exit 77 = SKIP where chained fixups cannot be produced, which includes 10.9
# itself. That is the honest outcome: the check did not run, rather than passed.
set -eu
BIN="${1:?usage: chained-fixups.sh <bindir>}"
T=$(mktemp -d /tmp/chained-fixups.XXXXXX)
trap 'rm -rf "$T"' EXIT INT TERM

cat > "$T/t.c" <<'CEOF'
#include <stdio.h>
int main(void) { printf("chained\n"); return 0; }
CEOF

# CMake exports SDKROOT / MACOSX_DEPLOYMENT_TARGET for the 10.9 cross build, and
# inheriting those here would defeat the point: we want the HOST's own defaults,
# which is what makes its linker choose chained fixups. Unset rather than
# override, so the host decides.
unset SDKROOT MACOSX_DEPLOYMENT_TARGET CMAKE_OSX_SYSROOT CMAKE_OSX_DEPLOYMENT_TARGET 2>/dev/null || true
if ! "${CC:-cc}" -O0 -o "$T/in" "$T/t.c" 2>/dev/null; then
    echo "chained-fixups: host cc cannot build a plain binary — SKIP"
    exit 77
fi
if ! otool -l "$T/in" | grep -q LC_DYLD_CHAINED_FIXUPS; then
    echo "chained-fixups: host linker emitted no LC_DYLD_CHAINED_FIXUPS — SKIP"
    exit 77
fi
echo "chained-fixups: input uses LC_DYLD_CHAINED_FIXUPS, converting"

"$BIN/patch_macho" "$T/in" "$T/out" >/dev/null

if otool -l "$T/out" | grep -q LC_DYLD_CHAINED_FIXUPS; then
    echo "chained-fixups: FAIL — output still carries LC_DYLD_CHAINED_FIXUPS" >&2
    exit 1
fi
if ! otool -l "$T/out" | grep -q LC_DYLD_INFO_ONLY; then
    echo "chained-fixups: FAIL — output has no LC_DYLD_INFO_ONLY" >&2
    exit 1
fi
echo "chained-fixups: converted to LC_DYLD_INFO_ONLY"

# The rest of the pipeline must accept what patch_macho produced. Before this,
# change_dylib refused chained-fixups input outright, so a conversion that
# produced something it still would not touch could pass unnoticed.
"$BIN/add_version_min" "$T/out" >/dev/null
"$BIN/change_dylib" "$T/out" -strip-lc uuid -strip-lc codesig \
    -change "/usr/lib/libSystem.B.dylib" "@loader_path/../S.dylib" >/dev/null
echo "chained-fixups: the converted image survives the rest of the pipeline"

# Determinism, the property the characterization rests on: same input twice,
# identical bytes. Checked here too because this path never runs on 10.9.
"$BIN/patch_macho" "$T/in" "$T/out2" >/dev/null
if ! cmp -s "$T/out" "$T/out2"; then
    : # out has been further edited; re-run the same edits before comparing
    "$BIN/add_version_min" "$T/out2" >/dev/null
    "$BIN/change_dylib" "$T/out2" -strip-lc uuid -strip-lc codesig \
        -change "/usr/lib/libSystem.B.dylib" "@loader_path/../S.dylib" >/dev/null
fi
if cmp -s "$T/out" "$T/out2"; then
    echo "chained-fixups: deterministic"
else
    echo "chained-fixups: FAIL — same input produced different output" >&2
    exit 1
fi
echo "chained-fixups: OK"
