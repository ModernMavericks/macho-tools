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
[ -x "$BIN/patch_macho" ] || { echo "leaf-tool-crashes: $BIN/patch_macho not found" >&2; exit 1; }

CC="${CC:-clang}"
SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
SRC_DIR="$SCRIPT_DIR/../src"
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

/* segname/sectname are char[16], NOT required to be NUL-terminated -- a
 * 16-character name fills the field completely, with no room for a
 * terminator (src/image.c's name_eq comment, and now tests/README.md's
 * host-portability section, explain why the real tools compare these
 * fields with strncmp rather than strcmp/strlen). strcpy'ing a 16-character
 * name into one of these fields writes a 17th byte -- the NUL -- past the
 * field, into whatever struct member follows. 10.9's clang lets that
 * happen silently; a modern clang's _FORTIFY_SOURCE turns strcpy into
 * __strcpy_chk, which detects the overflow and aborts (SIGTRAP) before this
 * helper ever gets to write the fixture file, failing this test on the
 * cross runner while it passes natively. memcpy with an explicit,
 * field-width-capped length has no such trap: it is also just the CORRECT
 * operation for a fixed-width, not-necessarily-terminated field. */
static void set_name16(char *field, const char *name) {
    size_t len = strlen(name);
    if (len > 16) len = 16;
    memset(field, 0, 16);
    memcpy(field, name, len);
}

int main(int argc, char **argv) {
    if (argc != 3) { fprintf(stderr, "usage: %s nosect|oobsection out\n", argv[0]); return 1; }

    static uint8_t buf[512];
    memset(buf, 0, sizeof buf);
    struct mach_header_64 *h = (struct mach_header_64 *)buf;
    h->magic = MH_MAGIC_64;
    h->ncmds = 1;
    struct segment_command_64 *seg = (struct segment_command_64 *)(buf + sizeof *h);
    seg->cmd = LC_SEGMENT_64;
    set_name16(seg->segname, "__DATA");

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
        /* "__objc_classlist" is exactly 16 characters -- the case that
         * actually exposed this: strcpy's 17th byte (the NUL) had nowhere
         * to go but into s->segname, the very next field. */
        set_name16(s->sectname, "__objc_classlist");
        set_name16(s->segname, "__DATA");
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

# --- patch_macho: pm_collect_ctx's to_remove[] must refuse, not overflow ----
#
# Task 2a moved patch_macho.c's collecting walk into an mi_each_lc callback
# and put its fixed-size `to_remove[]` array (originally sized [4]) into the
# SAME context struct as `int n_remove`, with n_remove declared immediately
# after the array -- same layout hazard as segs[32]/nsegs just above it in
# that struct, but without the matching `>= 32` style bound. With the array
# at its original size [4], a 5th push (any mix of LC_DYLD_EXPORTS_TRIE/
# LC_DYLD_CHAINED_FIXUPS/LC_BUILD_VERSION -- this fixture uses LC_BUILD_
# VERSION because it is trivial to repeat N times) wrote to_remove[4], one
# element past the array, landing on n_remove itself; every push after that
# walked further off the struct into main()'s locals. A malformed/
# pathological input the pre-Task-2a tool declined cleanly went from a clean
# refusal to a crash mid-run in a tool install.sh points at user binaries.
#
# The array was then enlarged from [4] to [16] (see patch_macho.c's own
# comment on struct pm_collect_ctx): review found that [4] left ZERO margin
# on a real, legitimate input -- a zippered (Mac Catalyst) binary carries two
# LC_BUILD_VERSION commands plus at most one each of LC_DYLD_EXPORTS_TRIE/
# LC_DYLD_CHAINED_FIXUPS, for 1+1+2 = 4, exactly the old cap. This fixture's
# N values target the CURRENT [16] boundary, not the original [4] one the
# bug was found at, so the test keeps testing the actual edge rather than an
# arbitrary interior point.
#
# N=15/N=16 stay at-or-under the cap and must still succeed structurally
# (this fixture has no chained fixups, so patch_macho's own "No chained
# fixups found" refusal fires afterward -- exit 1, but a CLEAN one, not a
# crash). N=17/N=18 sit one and two past the cap: pre-the-[4]-fix, values in
# this shape corrupted n_remove into something that still looked like a
# small int (free() on a bogus pointer -> exit 134, "pointer being freed was
# not allocated") or into something that didn't (-> exit 139, SIGSEGV);
# post-fix, every N at or past the cap must refuse cleanly (exit 1, naming
# the overflow) with the file left untouched, exactly like segs[32]'s
# existing refusal just above.
cat > "$T/mkmanylc.c" <<'EOF'
/* Writes a Mach-O with N x LC_BUILD_VERSION load commands (ntools=0, so each
 * is a fixed 24 bytes -- already 8-aligned, satisfying mi_validate's cmdsize
 * alignment check) and nothing else. No LC_SEGMENT_64 at all: patch_macho's
 * mi_find_segment(&im, "__TEXT") lookup tolerates that (returns NULL,
 * image_base_vmaddr stays 0), so this exercises pm_collect_lc's to_remove[]
 * pushes in isolation, the same way leaf-tool-crashes' other fixtures target
 * one specific hazard each. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <mach-o/loader.h>
#include "mach_compat.h"   /* LC_BUILD_VERSION: not in the 10.9 SDK's own headers */

struct bvc { uint32_t cmd, cmdsize, platform, minos, sdk, ntools; };

int main(int argc, char **argv) {
    if (argc != 3) { fprintf(stderr, "usage: %s N out\n", argv[0]); return 1; }
    int n = atoi(argv[1]);
    if (n < 1 || n > 64) { fprintf(stderr, "N out of range\n"); return 1; }

    size_t fsize = sizeof(struct mach_header_64) + (size_t)n * sizeof(struct bvc);
    uint8_t *buf = calloc(1, fsize);
    struct mach_header_64 *h = (struct mach_header_64 *)buf;
    h->magic = MH_MAGIC_64;
    h->ncmds = (uint32_t)n;
    h->sizeofcmds = (uint32_t)(n * sizeof(struct bvc));

    struct bvc *c = (struct bvc *)(buf + sizeof(*h));
    for (int i = 0; i < n; i++) {
        c[i].cmd = LC_BUILD_VERSION;
        c[i].cmdsize = sizeof(struct bvc);
    }

    FILE *f = fopen(argv[2], "wb");
    if (!f) { perror("fopen"); return 1; }
    if (fwrite(buf, 1, fsize, f) != fsize) { perror("fwrite"); fclose(f); return 1; }
    fclose(f);
    return 0;
}
EOF
"$CC" -O2 -I "$SRC_DIR" -o "$T/mkmanylc" "$T/mkmanylc.c"

pm_manylc_case() {
    n="$1"; expect="$2"   # expect: "clean" (refuses/errors without crashing) or "cap" (refuses, names the cap)
    "$T/mkmanylc" "$n" "$T/manylc_$n.macho"
    before_md5=$(md5 -q "$T/manylc_$n.macho" 2>/dev/null || md5sum "$T/manylc_$n.macho" | awk '{print $1}')
    rc=0
    "$BIN/patch_macho" "$T/manylc_$n.macho" "$T/manylc_${n}_out.macho" \
        >"$T/manylc_$n.out" 2>"$T/manylc_$n.err" || rc=$?
    after_md5=$(md5 -q "$T/manylc_$n.macho" 2>/dev/null || md5sum "$T/manylc_$n.macho" | awk '{print $1}')

    if [ "$rc" -gt 127 ]; then
        bad "patch_macho: N=$n LC_BUILD_VERSION" "killed by a signal (exit $rc) -- the to_remove[] overflow this fixture exists to catch"
        return
    fi
    if [ "$expect" = "cap" ]; then
        if [ "$rc" -eq 1 ] && grep -q "more than 16 load commands to strip" "$T/manylc_$n.err"; then
            ok "patch_macho: N=$n LC_BUILD_VERSION refuses, naming the to_remove[] cap"
        else
            bad "patch_macho: N=$n LC_BUILD_VERSION" "expected exit 1 + cap message, got exit $rc: $(cat "$T/manylc_$n.err")"
        fi
        [ "$before_md5" = "$after_md5" ] \
            && ok "patch_macho: N=$n LC_BUILD_VERSION leaves the input untouched on refusal" \
            || bad "patch_macho: N=$n LC_BUILD_VERSION" "input was modified despite the refusal"
    else
        [ "$rc" -eq 1 ] \
            && ok "patch_macho: N=$n LC_BUILD_VERSION completes without crashing (exit $rc, under the cap)" \
            || bad "patch_macho: N=$n LC_BUILD_VERSION" "expected a clean exit 1 (no chained fixups), got exit $rc: $(cat "$T/manylc_$n.err")"
    fi
}

pm_manylc_case 15 clean
pm_manylc_case 16 clean
pm_manylc_case 17 cap
pm_manylc_case 18 cap

if [ -f /usr/lib/libgmalloc.dylib ]; then
    for n in 17 18; do
        "$T/mkmanylc" "$n" "$T/manylc_gm_$n.macho"
        rc=0
        DYLD_INSERT_LIBRARIES=/usr/lib/libgmalloc.dylib \
            "$BIN/patch_macho" "$T/manylc_gm_$n.macho" "$T/manylc_gm_${n}_out.macho" \
            >"$T/manylc_gm_$n.out" 2>"$T/manylc_gm_$n.err" || rc=$?
        if [ "$rc" -gt 127 ]; then
            bad "patch_macho: N=$n LC_BUILD_VERSION (libgmalloc)" "killed by a signal (exit $rc) under libgmalloc"
        else
            ok "patch_macho: N=$n LC_BUILD_VERSION (libgmalloc): completed without crashing (exit $rc)"
        fi
    done
else
    skip "patch_macho: N=17/18 LC_BUILD_VERSION (libgmalloc)" "no /usr/lib/libgmalloc.dylib on this host"
fi

echo "leaf-tool-crashes: $fails failure(s)"
[ "$fails" -eq 0 ]
