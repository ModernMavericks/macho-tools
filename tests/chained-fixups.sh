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
SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
SRC_DIR="$SCRIPT_DIR/../src"
T=$(mktemp -d /tmp/chained-fixups.XXXXXX)
trap 'rm -rf "$T"' EXIT INT TERM

cat > "$T/t.c" <<'CEOF'
#include <stdio.h>
int main(void) { printf("chained\n"); return 0; }
CEOF

# `otool -l | grep -q LC_...` is exactly the oracle tests/README.md's lessons
# warn against: it violates lesson two (parsing otool's human-readable text
# as an oracle for Mach-O structure) and would FALSE-PASS on 10.9, whose
# otool predates both LC_DYLD_CHAINED_FIXUPS and LC_DYLD_INFO_ONLY and prints
# them as "?(0x8...) Unknown load command" / a numeric cmd — neither of which
# `grep -q LC_DYLD_...` would ever match, so a "not present" check built this
# way would ALWAYS say "not present" on 10.9, regardless of what the file
# actually carries. Self-guarded today only because this whole script SKIPs
# on 10.9 (no chained fixups to emit there), but that guard is a property of
# THIS host, not of the check itself -- the repo already has a tiny
# structure-reading helper for exactly this (has_lc.c, in
# change_dylib_test.sh), so build one here rather than lean on otool's text.
cat > "$T/has_lc.c" <<'CEOF'
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <mach-o/loader.h>
int main(int argc, char **argv) {
    if (argc != 3) { fprintf(stderr, "usage: %s file cmd-hex\n", argv[0]); return 2; }
    uint32_t want = (uint32_t)strtoul(argv[2], NULL, 16);
    int fd = open(argv[1], O_RDONLY);
    if (fd < 0) { perror("open"); return 2; }
    struct stat st; fstat(fd, &st);
    uint8_t *buf = malloc((size_t)st.st_size);
    if (!buf || read(fd, buf, (size_t)st.st_size) != (ssize_t)st.st_size) {
        fprintf(stderr, "read failed\n"); return 2;
    }
    close(fd);
    struct mach_header_64 *hdr = (struct mach_header_64 *)buf;
    if (hdr->magic != MH_MAGIC_64) { fprintf(stderr, "not a 64-bit Mach-O\n"); return 2; }
    uint8_t *lcp = buf + sizeof(struct mach_header_64);
    for (uint32_t i = 0; i < hdr->ncmds; i++) {
        struct load_command *lc = (struct load_command *)lcp;
        if (lc->cmd == want) return 0;
        lcp += lc->cmdsize;
    }
    return 1;
}
CEOF
"${CC:-cc}" -O2 -o "$T/has_lc" "$T/has_lc.c"

# The bug this replaced: LC_DYLD_INFO_ONLY was hand-typed here as 0x22,
# dropping the LC_REQ_DYLD bit ((0x22|LC_REQ_DYLD) = 0x80000022) that
# `#define LC_DYLD_INFO_ONLY` in <mach-o/loader.h> carries. The OLD
# otool-text oracle never had this bug because it matched on the NAME otool
# prints, so the bit never entered into it -- converting to a numeric
# comparison (this wave's own fix, see git history) made the bit load-
# bearing, and a hand-copied literal dropped it. has_lc's `==` is exact, so
# this silently searched for the wrong command and reported "absent" on a
# file that had one -- caught only on a cross runner where the host linker
# actually emits LC_DYLD_INFO_ONLY (10.9's does not, so this SKIPs here
# regardless, and the bug was invisible on this host).
#
# Fix, and fix the CLASS: never hand-type an LC_REQ_DYLD-bearing constant
# in shell again. Read both values out of the same headers this project's
# own C source trusts (src/mach_compat.h over <mach-o/loader.h>) via a
# throwaway C program, so a shell constant can no longer drift from the
# header that defines it -- the identical reasoning tests/README.md's
# "never parse otool text" lesson gives, applied one level deeper: don't
# hand-transcribe a NUMBER out of a header either, read it back out of the
# header via the compiler instead.
cat > "$T/lc_const.c" <<'CEOF'
#include <stdio.h>
#include <mach-o/loader.h>
#include "mach_compat.h"
int main(void) {
    printf("%#x %#x\n", LC_DYLD_CHAINED_FIXUPS, LC_DYLD_INFO_ONLY);
    return 0;
}
CEOF
"${CC:-cc}" -O2 -I "$SRC_DIR" -o "$T/lc_const" "$T/lc_const.c"
lc_const_out=$("$T/lc_const")
LC_DYLD_CHAINED_FIXUPS=${lc_const_out%% *}
LC_DYLD_INFO_ONLY=${lc_const_out##* }

# This assertion runs on EVERY host, including 10.9 where the rest of this
# script SKIPs -- it is the one part of this fix that a 10.9-only run can
# actually prove, since the end-to-end conversion path below never executes
# here. 0x80000034 and 0x80000022 are this project's own long-standing
# values (mach_compat.h's LC_DYLD_CHAINED_FIXUPS comment, and
# <mach-o/loader.h>'s LC_DYLD_INFO_ONLY); if the header ever changes them,
# this is meant to fail loudly, not silently track a moving target.
[ "$LC_DYLD_CHAINED_FIXUPS" = "0x80000034" ] || {
    echo "chained-fixups: LC_DYLD_CHAINED_FIXUPS read as $LC_DYLD_CHAINED_FIXUPS, expected 0x80000034 -- header definition changed?" >&2
    exit 1
}
[ "$LC_DYLD_INFO_ONLY" = "0x80000022" ] || {
    echo "chained-fixups: LC_DYLD_INFO_ONLY read as $LC_DYLD_INFO_ONLY, expected 0x80000022 -- header definition changed?" >&2
    exit 1
}

# CMake exports SDKROOT / MACOSX_DEPLOYMENT_TARGET for the 10.9 cross build, and
# inheriting those here would defeat the point: we want the HOST's own defaults,
# which is what makes its linker choose chained fixups. Unset rather than
# override, so the host decides.
unset SDKROOT MACOSX_DEPLOYMENT_TARGET CMAKE_OSX_SYSROOT CMAKE_OSX_DEPLOYMENT_TARGET 2>/dev/null || true
if ! "${CC:-cc}" -O0 -o "$T/in" "$T/t.c" 2>/dev/null; then
    echo "chained-fixups: host cc cannot build a plain binary — SKIP"
    exit 77
fi
if ! "$T/has_lc" "$T/in" "$LC_DYLD_CHAINED_FIXUPS"; then
    echo "chained-fixups: host linker emitted no LC_DYLD_CHAINED_FIXUPS — SKIP"
    exit 77
fi
echo "chained-fixups: input uses LC_DYLD_CHAINED_FIXUPS, converting"

"$BIN/patch_macho" "$T/in" "$T/out" >/dev/null

if "$T/has_lc" "$T/out" "$LC_DYLD_CHAINED_FIXUPS"; then
    echo "chained-fixups: FAIL — output still carries LC_DYLD_CHAINED_FIXUPS" >&2
    exit 1
fi
if ! "$T/has_lc" "$T/out" "$LC_DYLD_INFO_ONLY"; then
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
