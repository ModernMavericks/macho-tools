#!/bin/sh
# tests/leaf-tool-crashes.sh — regression coverage for two heap-overflow
# crashes a code review found in add_version_min and retag_swift_classes
# after Task 1 of the toolkit convergence plan converted them onto
# src/image.h.
#
# mi_open validates every LOAD COMMAND (magic, cmdsize bounds/alignment,
# LC_SEGMENT_64/nsects agreement) but nothing about a SECTION's file range,
# and nothing about an address later derived from one -- that was never its
# job (see image.h's own file header: "what is in here?", nothing about
# what a section's bytes point at). Both fixtures below are built to pass
# mi_open, and `macho9 verify`, cleanly, while still containing an
# out-of-bounds reference these tools used to dereference unconditionally:
#
#   nosect.macho      one LC_SEGMENT_64, nsects=0 -- no section anywhere has
#                      a nonzero file offset, so add_version_min's "is there
#                      room before the first section" check never found a
#                      bound and wrote LC_VERSION_MIN_MACOSX 16 bytes past a
#                      buffer whose allocation was exactly file-sized.
#   oobsection.macho   one LC_SEGMENT_64/__DATA with one section,
#                      __objc_classlist, whose offset/size (0x7000/0x8000)
#                      point entirely past this tiny file -- retag_swift_
#                      classes indexed the class list at that offset
#                      directly, with no check against the file's actual
#                      size.
#
# Host-portability: both fixtures are hand-built byte-for-byte (no compiler
# invoked to produce Mach-O structure, just a throwaway C helper -- same
# idiom as change_dylib_test.sh's ordinal_of.c/has_lc.c -- writing the struct
# layout directly), so this asks the same question on every host regardless
# of toolchain version; see tests/README.md.
#
#   sh tests/leaf-tool-crashes.sh <bindir>
set -eu
BIN="${1:?usage: leaf-tool-crashes.sh <bindir>}"
[ -x "$BIN/add_version_min" ] || { echo "leaf-tool-crashes: $BIN/add_version_min not found" >&2; exit 1; }
[ -x "$BIN/retag_swift_classes" ] || { echo "leaf-tool-crashes: $BIN/retag_swift_classes not found" >&2; exit 1; }

CC="${CC:-clang}"
T="${TMPDIR:-/tmp}/leaf-tool-crashes.$$"
mkdir -p "$T"
trap 'rm -rf "$T"' EXIT INT TERM

fails=0
ok()   { echo "PASS $1"; }
bad()  { echo "FAIL $1: $2"; fails=$((fails + 1)); }
skip() { echo "SKIP $1: $2"; }

cat > "$T/mkfixture.c" <<'EOF'
/* Writes one of two tiny, deliberately malformed-past-load-commands Mach-O
 * fixtures. Byte layout only -- see leaf-tool-crashes.sh for what each is
 * shaped to trigger. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <mach-o/loader.h>

int main(int argc, char **argv) {
    if (argc != 3) { fprintf(stderr, "usage: %s nosect|oobsection out\n", argv[0]); return 1; }

    static uint8_t buf[512];
    memset(buf, 0, sizeof buf);
    struct mach_header_64 *h = (struct mach_header_64 *)buf;
    h->magic = MH_MAGIC_64;
    h->ncmds = 1;
    struct segment_command_64 *seg = (struct segment_command_64 *)(buf + sizeof *h);
    seg->cmd = LC_SEGMENT_64;
    strcpy(seg->segname, "__DATA");

    size_t fsize;
    if (strcmp(argv[1], "nosect") == 0) {
        seg->cmdsize = sizeof(*seg);
        seg->nsects = 0;
        h->sizeofcmds = seg->cmdsize;
        fsize = sizeof(*h) + seg->cmdsize;
    } else if (strcmp(argv[1], "oobsection") == 0) {
        seg->cmdsize = sizeof(*seg) + sizeof(struct section_64);
        seg->nsects = 1;
        h->sizeofcmds = seg->cmdsize;
        struct section_64 *s = (struct section_64 *)((uint8_t *)seg + sizeof *seg);
        strcpy(s->sectname, "__objc_classlist");
        strcpy(s->segname, "__DATA");
        s->offset = 0x7000;
        s->size   = 0x8000;
        fsize = sizeof(*h) + seg->cmdsize;
    } else {
        fprintf(stderr, "unknown kind: %s\n", argv[1]);
        return 1;
    }

    FILE *f = fopen(argv[2], "wb");
    if (!f) { perror("fopen"); return 1; }
    if (fwrite(buf, 1, fsize, f) != fsize) { perror("fwrite"); fclose(f); return 1; }
    fclose(f);
    return 0;
}
EOF
"$CC" -O2 -o "$T/mkfixture" "$T/mkfixture.c"

"$T/mkfixture" nosect "$T/nosect.macho"
"$T/mkfixture" oobsection "$T/oobsection.macho"

# --- add_version_min -------------------------------------------------------
cp "$T/nosect.macho" "$T/av.macho"
rc=0
"$BIN/add_version_min" "$T/av.macho" >"$T/av.out" 2>"$T/av.err" || rc=$?
if [ "$rc" -gt 127 ]; then
    bad "add_version_min: nosect fixture" "killed by a signal (exit $rc) -- the heap overflow this fixture exists to catch"
elif [ "$rc" -eq 1 ] && grep -q "no room for LC_VERSION_MIN_MACOSX" "$T/av.err"; then
    ok "add_version_min: refuses (not crashes) a file with no sectioned segment"
else
    bad "add_version_min: nosect fixture" "expected exit 1 + 'no room' message, got exit $rc: $(cat "$T/av.err")"
fi

if [ -f /usr/lib/libgmalloc.dylib ]; then
    cp "$T/nosect.macho" "$T/av_gm.macho"
    rc=0
    DYLD_INSERT_LIBRARIES=/usr/lib/libgmalloc.dylib \
        "$BIN/add_version_min" "$T/av_gm.macho" >"$T/av_gm.out" 2>"$T/av_gm.err" || rc=$?
    if [ "$rc" -gt 127 ]; then
        bad "add_version_min: nosect fixture (libgmalloc)" "killed by a signal (exit $rc) under libgmalloc -- the heap overflow this fixture exists to catch"
    else
        ok "add_version_min: nosect fixture (libgmalloc): completed without crashing (exit $rc)"
    fi
else
    skip "add_version_min: nosect fixture (libgmalloc)" "no /usr/lib/libgmalloc.dylib on this host"
fi

# --- retag_swift_classes ----------------------------------------------------
cp "$T/oobsection.macho" "$T/rt.macho"
rc=0
"$BIN/retag_swift_classes" "$T/rt.macho" >"$T/rt.out" 2>"$T/rt.err" || rc=$?
if [ "$rc" -gt 127 ]; then
    bad "retag_swift_classes: oobsection fixture" "killed by a signal (exit $rc) -- the heap overflow this fixture exists to catch"
elif [ "$rc" -eq 0 ] && grep -q "^total: 0 class record(s) retagged$" "$T/rt.out"; then
    ok "retag_swift_classes: refuses (not crashes) an out-of-bounds section range"
else
    bad "retag_swift_classes: oobsection fixture" "expected exit 0 + 'total: 0' output, got exit $rc: $(cat "$T/rt.out") $(cat "$T/rt.err")"
fi

if [ -f /usr/lib/libgmalloc.dylib ]; then
    cp "$T/oobsection.macho" "$T/rt_gm.macho"
    rc=0
    DYLD_INSERT_LIBRARIES=/usr/lib/libgmalloc.dylib \
        "$BIN/retag_swift_classes" "$T/rt_gm.macho" >"$T/rt_gm.out" 2>"$T/rt_gm.err" || rc=$?
    if [ "$rc" -gt 127 ]; then
        bad "retag_swift_classes: oobsection fixture (libgmalloc)" "killed by a signal (exit $rc) under libgmalloc -- the heap overflow this fixture exists to catch"
    else
        ok "retag_swift_classes: oobsection fixture (libgmalloc): completed without crashing (exit $rc)"
    fi
else
    skip "retag_swift_classes: oobsection fixture (libgmalloc)" "no /usr/lib/libgmalloc.dylib on this host"
fi

echo "leaf-tool-crashes: $fails failure(s)"
[ "$fails" -eq 0 ]
