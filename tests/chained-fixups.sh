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
LC_DYLD_CHAINED_FIXUPS=0x80000034
LC_DYLD_INFO_ONLY=0x22

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
