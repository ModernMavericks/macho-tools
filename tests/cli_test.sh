#!/bin/sh
# tests/cli_test.sh — exercises the macho9 CLI itself: --capabilities, and
# each verb this build actually implements.
#
# What this does NOT re-prove: change_dylib_test.sh already runs real dylib
# renumbering, -insert/-delete ordinal correctness, and header-growth end to
# end, through change_dylib. dylib/rpath/lc/minos here call the very same
# code -- mr_apply_file/mv_add_version_min in src/, which is all change_dylib
# and add_version_min are too (see cli/macho9.c's file header) -- so this
# asserts the TRANSLATION and DISPATCH are correct, one exemplar op per verb,
# not the underlying rewrite a second time.
#
# Host-portability, per the task's own hard-won rules:
#   - every fixture is built with -mmacosx-version-min=10.9, so a modern
#     linker's LC_DYLD_CHAINED_FIXUPS default can't sneak in and ask a
#     different question on the cross runner than it asks natively here.
#   - nothing here parses otool/nm text. Facts about a binary come either
#     from `macho9 info`'s own stable output, or from a tiny C reader built
#     alongside the fixtures (same trick change_dylib_test.sh's ordinal_of.c
#     uses), never from a format Apple's tools are free to reformat.
set -eu
BIN="${1:?usage: cli_test.sh <bindir>}"
MACHO9="$BIN/macho9"
[ -x "$MACHO9" ] || { echo "cli_test: $MACHO9 not found or not executable" >&2; exit 1; }
# macho9 needs NOTHING else in $BIN: dylib/rpath/lc/minos used to run
# change_dylib/add_version_min as subprocesses found next to it, and this
# script used to refuse to start without them. The "macho9 alone in an empty
# directory" assertions below are what replaced that requirement -- they check
# the property the requirement existed for, from the outside, instead of
# taking it on trust.

CC="${CC:-clang}"
FIXTURE_FLAGS="-mmacosx-version-min=10.9"
T="${TMPDIR:-/tmp}/cli_test.$$"
mkdir -p "$T"
trap 'rm -rf "$T"' EXIT INT TERM

fails=0
ok()   { echo "PASS $1"; }
bad()  { echo "FAIL $1: $2"; fails=$((fails + 1)); }
# Not a failure: the assertion could not be exercised on this host. Printed
# loudly and distinctly from PASS/FAIL, per-assertion, rather than silently
# omitted -- a silent skip is how coverage rots. Does not touch $fails.
skip() { echo "SKIP $1: $2"; }

# `set -e` means any bare command that exits nonzero kills the WHOLE script
# immediately -- which has already happened for real (a helper's exit
# convention bug took every verb's tests after it down silently, with only
# a generic CTest error to show for it: twenty-plus assertions never ran,
# and nothing said so). This does not remove `set -e` -- the fix stays
# targeted -- but an early death is no longer silent: reached_end is set to
# 1 only at the very end, right before the summary line, so an EXIT trap
# firing while it is still 0 means the script did NOT reach its own
# summary, and says so loudly, with the exit code that killed it.
reached_end=0
trap 'rc=$?; if [ "$reached_end" -eq 0 ]; then
    echo "cli_test: FATAL -- aborted early (a command exited $rc under set -e); the suite did NOT run to completion, and everything after the last PASS/FAIL/SKIP line above never ran" >&2
fi' EXIT

# --- fixtures --------------------------------------------------------------
cat > "$T/a.c" <<'EOF'
int a_sym(void) { return 11; }
EOF
cat > "$T/main.c" <<'EOF'
#include <stdio.h>
int a_sym(void);
int main(void) { return a_sym() == 11 ? 0 : 1; }
EOF
"$CC" -dynamiclib -O2 $FIXTURE_FLAGS -install_name "@loader_path/liba.dylib" \
    "$T/a.c" -o "$T/liba.dylib"

build_main() {
    # $2 (optional): an extra -Xlinker -rpath search path baked in at link time.
    if [ -n "${2:-}" ]; then
        "$CC" -O2 $FIXTURE_FLAGS -Xlinker -rpath -Xlinker "$2" \
            "$T/main.c" "$T/liba.dylib" -o "$1"
    else
        "$CC" -O2 $FIXTURE_FLAGS "$T/main.c" "$T/liba.dylib" -o "$1"
    fi
}

# A fixture GUARANTEED not to carry LC_BUILD_VERSION, on any host.
#
# The three "-delete build-version is a miss" assertions below used to build
# a plain build_main fixture and rely on a comment claiming "-mmacosx-version
# -min=10.9 clang never emits build-version (confirmed empirically)". That
# was confirmed on 10.9 only, and it is FALSE on the cross/CI runner, whose
# modern linker emits LC_BUILD_VERSION anyway: the delete then SUCCEEDS, no
# miss is reported, and all three assertions fail. Green on the target,
# red on the runner -- the same host-toolchain dependence that bit the
# rpath -insert headerpad fixture.
#
# So stop asserting what a linker emits and MAKE the premise true: strip the
# kind first, unconditionally. A no-op where it was already absent.
#
# This uses macho9 to set up a macho9 test, which is circular only in
# appearance: if the strip silently did nothing, the delete under test would
# FIND build-version and report no miss, and the assertions fail loudly. The
# setup cannot mask the defect it is setting up for.
build_main_without_build_version() {
    build_main "$1"
    "$MACHO9" lc "$1" -delete build-version >/dev/null 2>&1 || true
    # Assert the precondition rather than trusting the strip. otool, not
    # macho9, so a macho9 defect cannot certify its own setup. Without this
    # the test would pass on 10.9 for the OLD reason (the linker never
    # emitted it) and silently stop testing anything the day it does.
    if otool -l "$1" 2>/dev/null | grep -q LC_BUILD_VERSION; then
        bad "fixture setup" "build_main_without_build_version left LC_BUILD_VERSION in $1"
    fi
}

# ---------------------------------------------------------------------------
# Host capability: can this host run a Mach-O binary that was modified
# in-place after being signed at link time, AT ALL?
#
# This must be established WITHOUT running macho9 on the probe binary. The
# grow/lc "still runs" assertions below rewrite a fixture with macho9 and
# then run it; if this host's kernel kills any modified binary, that proves
# nothing about macho9 -- but if the probe used to detect that ALSO goes
# through macho9, a real macho9 regression that corrupts its output looks
# IDENTICAL to a host that kills modified binaries: same symptom (the child
# doesn't run), same wrong conclusion ("host policy, not a macho9 defect"),
# and a genuine defect ships as a green, honest-looking SKIP. That is worse
# than no check at all.
#
# So this probe never calls macho9. It builds a plain fixture, flips ONE
# byte inside the existing header pad (unused space between the end of the
# load commands and the first section's file data -- computed here by an
# independent read, not by calling into macho9/image.h, for the same
# non-circularity reason strip_version_min.c below is self-contained) via a
# throwaway C program, and tries to run the result. If the kernel/dyld kills
# THAT, this host enforces code-signing on any post-link modification,
# unconditionally of what changed or which tool changed it -- an honest,
# independently-established fact the grow/lc sections can trust. If it
# still runs, this host does NOT enforce that, and a failure to run
# macho9's OWN rewritten fixture later is no longer explainable by host
# policy -- it must be treated as a real defect (FAIL), not silently
# skipped.
cat > "$T/perturb_pad.c" <<'EOF'
/* Flip one byte inside a Mach-O's header pad (the unused space between the
 * end of the load commands and the first section's file data) -- content
 * no code path reads, so this is semantically inert, but it still changes
 * the file's bytes, which is all a code-signature hash cares about.
 * Exit 0 = flipped one byte, 4 = no pad available, 2 = error. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <mach-o/loader.h>

int main(int argc, char **argv) {
    if (argc != 2) { fprintf(stderr, "usage: %s FILE\n", argv[0]); return 2; }
    int fd = open(argv[1], O_RDWR);
    if (fd < 0) { perror("open"); return 2; }
    struct stat st;
    if (fstat(fd, &st) != 0) { perror("fstat"); close(fd); return 2; }
    size_t size = (size_t)st.st_size;
    uint8_t *buf = malloc(size);
    if (!buf || read(fd, buf, size) != (ssize_t)size) {
        fprintf(stderr, "read failed\n"); close(fd); return 2;
    }
    struct mach_header_64 *hdr = (struct mach_header_64 *)buf;
    if (hdr->magic != MH_MAGIC_64) { fprintf(stderr, "not a 64-bit Mach-O\n"); return 2; }

    uint32_t first_sect_off = UINT32_MAX;
    uint8_t *lcp = buf + sizeof(*hdr);
    for (uint32_t i = 0; i < hdr->ncmds; i++) {
        struct load_command *lc = (struct load_command *)lcp;
        if (lc->cmd == LC_SEGMENT_64) {
            struct segment_command_64 *seg = (struct segment_command_64 *)lcp;
            struct section_64 *sect = (struct section_64 *)(lcp + sizeof(*seg));
            for (uint32_t j = 0; j < seg->nsects; j++)
                if (sect[j].offset && sect[j].offset < first_sect_off)
                    first_sect_off = sect[j].offset;
        }
        lcp += lc->cmdsize;
    }
    uint32_t lc_end = (uint32_t)sizeof(*hdr) + hdr->sizeofcmds;
    if (first_sect_off == UINT32_MAX || first_sect_off <= lc_end) {
        fprintf(stderr, "no header pad available to perturb\n");
        return 4;
    }
    buf[lc_end] ^= 0xFF;   /* the first pad byte; never read by any load command */

    lseek(fd, 0, SEEK_SET);
    if (write(fd, buf, size) != (ssize_t)size) { perror("write"); return 2; }
    close(fd);
    return 0;
}
EOF
"$CC" -O2 -o "$T/perturb_pad" "$T/perturb_pad.c"

build_main "$T/signing_probe"
if "$T/perturb_pad" "$T/signing_probe" >"$T/perturb.out" 2>&1; then
    if (cd "$T" && ./signing_probe) >"$T/signing_probe.out" 2>&1; then
        signing_probe_rc=0
    else
        signing_probe_rc=$?
    fi
else
    signing_probe_rc=$?   # 4 = no pad (fixture too tight -- treat as "can't determine")
fi
if [ "$signing_probe_rc" -eq 0 ]; then
    signing_enforced=0
    ok "host probe: a trivially-perturbed binary still runs (macho9-independent)"
elif [ "$signing_probe_rc" -eq 137 ]; then
    signing_enforced=1
    ok "host probe: a trivially-perturbed binary is SIGKILLed (137) -- code-signing enforcement, independent of macho9"
else
    # Neither a clean run nor the specific signal we know how to explain.
    # Per the coordinator: do not guess. Anything unrecognized here means the
    # grow/lc sections below cannot trust EITHER conclusion, so they must not
    # silently skip. That is fully achieved by leaving signing_enforced at 0:
    # the grow/lc sections below gate their run-assertions on
    # `signing_enforced -eq 1` (skip only when enforcement is POSITIVELY
    # confirmed), so 0 here already means "treat as real, don't skip" for
    # this unrecognized case exactly as it does for the confirmed-unenforced
    # one -- a separate signing_probe_unknown flag was tracked alongside this
    # for a time but nothing downstream ever read it (confirmed: no other
    # reference to it in this file), so it added a state without adding
    # behavior. Removed rather than left to imply a distinction that wasn't
    # there.
    signing_enforced=0
    bad "host probe" "unrecognized outcome (exit $signing_probe_rc: $(head -1 "$T/signing_probe.out" 2>/dev/null || cat "$T/perturb.out" 2>/dev/null || echo 'no output')) -- cannot determine whether this host enforces code-signing on modified binaries; treating grow/lc run-assertions as real rather than risking a masked defect"
fi

# ---------------------------------------------------------------------------
# Is this host the product's actual target platform (Mac OS X 10.9, Darwin
# 13.x)? This is the one place in this file a version check is the right
# tool rather than a capability probe: the question isn't "can this host DO
# X" (that's what signing_enforced answers, above), it's "does the PRODUCT
# even promise X here at all". mg_grow_header's whole trick -- donating
# __PAGEZERO bytes and lowering __TEXT's vmaddr -- is something 10.9's dyld
# accepts by design; nothing in this repo, the proposal, or the plan
# promises a grown binary also loads on a newer dyld, so "does it run here"
# is only a hard requirement ON the target, everywhere else it's a bonus
# worth recording but not asserting on.
darwin_major=$(uname -r | cut -d. -f1)
is_target_platform=0
[ "$darwin_major" = "13" ] && is_target_platform=1

# ============================================================================
# --capabilities
# ============================================================================
caps=$("$MACHO9" --capabilities) || bad "capabilities: exit" "nonzero"
case "$caps" in
    "format 1"*) ok "capabilities: starts with format line" ;;
    *) bad "capabilities: format line" "got: $(echo "$caps" | head -1)" ;;
esac
# exitcodes documents EX_REFUSED (see cli/macho9.c) so a caller can tell
# "macho9 examined FILE and declined" apart from "macho9 itself failed"
# without scraping stderr text. Assert the line exists, names refused=1,
# and that a real refusal (verify on a non-Mach-O file) actually exits with
# that code -- not just some nonzero value. The corrected scheme is 0 ok, 1
# refused, 2 error -- backwards from what shipped, and deliberately so:
# diff/grep/cmp all reserve 2 for "something went wrong" and 1 for "a
# normal, expected, non-success answer". Nothing outside this repo had ever
# run the compat wrappers, so this was the last chance to fix it.
echo "$caps" | grep -q "^exitcodes ok=0 refused=1 failed=2$" \
    && ok "capabilities: exitcodes line documents refused=1" \
    || bad "capabilities: exitcodes line" "missing or wrong: $(echo "$caps" | grep '^exitcodes')"
echo 'not a mach-o' > "$T/not-a-macho-in-cli-test"
rc=0
"$MACHO9" verify "$T/not-a-macho-in-cli-test" >/dev/null 2>&1 || rc=$?
[ "$rc" -eq 1 ] \
    && ok "capabilities: a real refusal (verify on a non-Mach-O) actually exits 1" \
    || bad "capabilities: exitcodes vs reality" "verify on a non-Mach-O exited $rc, not the documented 1"

for v in verify info grow minos lc dylib rpath segment retag-swift declassify; do
    if echo "$caps" | grep -q "^verb $v"; then
        ok "capabilities: advertises $v"
    else
        bad "capabilities: $v" "not listed"
    fi
done
# declassify used to be the one verb --capabilities deliberately omitted,
# because it was a stub. It is implemented now (Task 0.6b), so it is in the
# loop above -- and the assertions further down are what make that
# advertisement honest, per print_capabilities' own "never advertise one that
# errors out" contract. usage() must have stopped disclaiming it too: that
# line is the other place a caller reads about what this build can do.
"$MACHO9" >/dev/null 2>"$T/usage.err" || true
if grep -q "declassify" "$T/usage.err"; then
    if grep "declassify" "$T/usage.err" | grep -qi "not implemented"; then
        bad "usage: declassify" "still says 'not implemented'"
    else
        ok "usage: declassify listed without a 'not implemented' disclaimer"
    fi
else
    bad "usage: declassify" "not mentioned at all"
fi
# rpath -insert IS implemented now; capabilities must claim it. A wrapper has
# no other way to learn this build can place a search path FIRST, which
# docs/PROPOSAL.md calls a new capability change_dylib never had.
if echo "$caps" | grep "^verb rpath" | grep -q "insert"; then
    ok "capabilities: rpath insert advertised"
else
    bad "capabilities: rpath insert" "implemented but not advertised"
fi

# --fatal-warnings promotes "an operation matched nothing" from a stderr
# report to a refusal (dylib/rpath/lc only -- segment and retag-swift take no
# list of operations that could miss). A wrapper has no other way to learn
# this build can do that than by probing this line, the same reason
# --allow-grow is advertised.
for v in dylib rpath lc; do
    if echo "$caps" | grep "^verb $v" | grep -q "flags=.*fatal-warnings"; then
        ok "capabilities: $v advertises fatal-warnings"
    else
        bad "capabilities: $v fatal-warnings" "not listed"
    fi
done
for v in segment retag-swift; do
    if echo "$caps" | grep "^verb $v" | grep -q "fatal-warnings"; then
        bad "capabilities: $v fatal-warnings" "advertised, but $v takes no list of operations that could miss"
    else
        ok "capabilities: $v correctly does not advertise fatal-warnings"
    fi
done

# ----------------------------------------------------------------------------
# capabilities vocabulary must match what the parsers actually accept.
#
# --capabilities' "kinds=" and "ops=" lists and the cmd_lc/cmd_dylib_or_rpath
# parsers that decide what a real invocation accepts are now both built from
# ONE table each (LC_STRIP_KINDS, DYLIB_OPS in cli/macho9.c) precisely so
# they cannot say different things -- before this they were three
# hand-copied lists (change_dylib's strippable[], macho9's own LC_KINDS[],
# and a hardcoded "kinds=..." string) that a review found had already drifted
# apart in spirit even where the values still matched by luck. This does not
# re-derive the table (it can't see the C source); it drives macho9 itself
# with every name --capabilities claims and confirms none of them is refused
# as unrecognized -- which is exactly what would happen if a name were ever
# added to (or dropped from) one list and not the other.
build_main "$T/vocab_fixture"
kinds=$(echo "$caps" | sed -n 's/^verb lc .*kinds=\([^ ]*\).*/\1/p')
[ -n "$kinds" ] || bad "capabilities vocab" "no kinds= on the lc line"
oldifs="$IFS"; IFS=','
vocab_kind_fail=0
for kind in $kinds; do
    "$MACHO9" lc "$T/vocab_fixture" -delete "$kind" >"$T/vocab_kind.out" 2>&1 || true
    if grep -qi "unknown KIND" "$T/vocab_kind.out"; then
        bad "capabilities vocab: kind '$kind'" "advertised but lc -delete refused it as unknown: $(cat "$T/vocab_kind.out")"
        vocab_kind_fail=1
    fi
done
IFS="$oldifs"
[ "$vocab_kind_fail" -eq 0 ] && ok "capabilities vocab: every advertised lc kind is accepted by lc -delete"
# And the inverse: a KIND that is plainly not real must still be refused --
# otherwise this check could trivially "pass" by lc accepting everything.
"$MACHO9" lc "$T/vocab_fixture" -delete not-a-real-kind >"$T/vocab_bogus.out" 2>&1 \
    && bad "capabilities vocab: bogus kind" "lc -delete accepted a KIND that isn't in any table" \
    || { grep -qi "unknown KIND" "$T/vocab_bogus.out" \
         && ok "capabilities vocab: an unadvertised kind is refused as unknown" \
         || bad "capabilities vocab: bogus kind" "refused, but not with 'unknown KIND': $(cat "$T/vocab_bogus.out")"; }

# Same idea for dylib/rpath ops=: every op --capabilities advertises for a
# verb must be recognized by that verb's own parser (never "unknown or
# incomplete operation"), and rpath must still refuse an op that belongs to
# dylib's vocabulary but not its own (-reexport: LC_RPATH has only one kind
# -- see DYLIB_OPS in cli/macho9.c).
vocab_ops_fail=0
check_ops_accepted() {
    # $1=verb (dylib|rpath)  $2=ops csv from capabilities
    verb="$1"; oldifs2="$IFS"; IFS=','
    for op in $2; do
        IFS="$oldifs2"   # restore default (whitespace) splitting for the command below
        case "$op" in
            replace) "$MACHO9" "$verb" "$T/vocab_fixture" "-$op" /no/such/old /no/such/new \
                         >"$T/vocab_op.out" 2>&1 || true ;;
            *)       "$MACHO9" "$verb" "$T/vocab_fixture" "-$op" /no/such/path \
                         >"$T/vocab_op.out" 2>&1 || true ;;
        esac
        if grep -q "unknown or incomplete operation" "$T/vocab_op.out"; then
            bad "capabilities vocab: $verb -$op" "advertised but the parser called it unknown/incomplete: $(cat "$T/vocab_op.out")"
            vocab_ops_fail=1
        fi
        IFS=','
    done
    IFS="$oldifs2"
}
dylib_ops=$(echo "$caps" | sed -n 's/^verb dylib .*ops=\([^ ]*\).*/\1/p')
rpath_ops=$(echo "$caps" | sed -n 's/^verb rpath .*ops=\([^ ]*\).*/\1/p')
[ -n "$dylib_ops" ] && [ -n "$rpath_ops" ] || bad "capabilities vocab" "missing ops= on dylib or rpath line"
check_ops_accepted dylib "$dylib_ops"
check_ops_accepted rpath "$rpath_ops"
[ "$vocab_ops_fail" -eq 0 ] && ok "capabilities vocab: every advertised dylib/rpath op is accepted by its own parser"
# rpath's ops= must not include reexport: LC_RPATH has exactly one kind, so
# there is nothing for a reexport to promote it to. (insert used to be listed
# here too; it is a real operation now, and the assertion further up requires
# it to be advertised.) If reexport ever appeared the parser would refuse it
# and the case above would catch that, but this also confirms capabilities
# didn't just stop advertising it for an unrelated reason.
case ",$rpath_ops," in
    *,reexport,*)
        bad "capabilities vocab: rpath ops=" "unexpectedly advertises reexport: $rpath_ops" ;;
    *)
        ok "capabilities vocab: rpath ops= correctly omits reexport" ;;
esac

# ============================================================================
# declassify: chained fixups -> LC_DYLD_INFO_ONLY
#
# The conversion lives in src/declassify.c (Task 0.6b lifted it out of
# compat/patch_macho.c's main, before that file became a shell wrapper); this
# verb is the only C front-end over it now, and the `patch_macho` name reaches
# this very verb through compat/patch_macho.sh. What is asserted here is the OBSERVABLE result -- the two
# quadwords in __DATA the conversion rewrites, which load commands survived,
# where the new LC_DYLD_INFO_ONLY points, and how far __LINKEDIT now reaches --
# not "it exited 0".
#
# THE FIXTURE IS HAND-BUILT, and it has to be: chained fixups are a 2021
# format, 10.9's linker predates them by a decade, and tests/chained-fixups.sh
# (which asks the HOST linker for one) therefore SKIPs entirely on the machine
# this toolkit is actually for. A fixture written byte by byte asks the same
# question on every host -- the reasoning tests/README.md's host-portability
# section gives, and the idiom leaf-tool-crashes.sh's mkfixture.c and the
# mkswift helper below already use.
SRC_DIR=$(CDPATH= cd -- "$(dirname -- "$0")/../src" && pwd)
cat > "$T/mkchained.c" <<'EOF'
/* mkchained make|make-weak|make-big OUT
 *                        -- write a tiny 64-bit Mach-O that uses CHAINED
 *                          FIXUPS, the format `declassify`/patch_macho exists
 *                          to lower. No linker on any host this repo supports
 *                          can be asked to emit one on demand (10.9's predates
 *                          the format by a decade), so the bytes are laid out
 *                          by hand -- the same idiom tests/leaf-tool-crashes.sh
 *                          and cli_test.sh's mkswift already use.
 *
 * mkchained check FILE   -- print, one per line, what the conversion is
 *                          supposed to have DONE to it. Structure read
 *                          directly out of the file; never otool text.
 *
 * The image: three segments (__TEXT, __DATA, __LINKEDIT), one section in each
 * of the first two, an LC_DYLD_CHAINED_FIXUPS pointing at a hand-built fixups
 * blob in __LINKEDIT, an LC_DYLD_EXPORTS_TRIE, and an LC_BUILD_VERSION -- the
 * three commands the conversion strips. __DATA holds a two-link chain: slot 0
 * is a REBASE of a base-relative target (0x1000), slot 1 (8 bytes later) is a
 * BIND of import 0, "_mkchained_sym", from library ordinal 1.
 *
 * After conversion, then, slot 0 must hold image_base + 0x1000 and slot 1 must
 * hold 0 (dyld fills a bind slot in at load time). Those two quadwords are the
 * whole point: they are the arithmetic the conversion does that nothing else
 * in this repo does, and they are observable in the output file's bytes.
 *
 * make-weak differs in ONE field: the import's library ordinal is -3
 * (BIND_SPECIAL_DYLIB_WEAK_LOOKUP), which 10.9's dyld rejects outright with
 * "bad special ordinal". The conversion has to remap it to flat lookup (-2)
 * plus the weak-import flag, and that remap is visible in the emitted opcodes.
 *
 * make-big differs in SIZE: a 2MB __DATA whose every quadword is one link of a
 * single rebase chain, ~262k fixups. At about 5 opcode bytes each that is well
 * past the 1MB the conversion buffers, so it must REFUSE. Before the bound
 * existed this fixture walked straight off the end of a 1MB malloc.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <mach-o/loader.h>
#include "mach_compat.h"

#define TEXT_VMADDR  0x100000000ULL
#define SECT_OFF     0x400          /* first section's file offset: the bound
                                     * the conversion checks for the 48 bytes
                                     * LC_DYLD_INFO_ONLY needs */
#define DATA_OFF     0x1000
#define DATA_SIZE    0x1000
#define BIG_DATA_SIZE 0x200000
#define TRIE_SIZE    0x10
#define FIXUPS_SIZE  0x100

#define CF_PTR_64_OFFSET 6
#define REBASE_TARGET    0x1000ULL  /* base-relative, so the converted slot
                                     * must read TEXT_VMADDR + this */
#define BIND_SLOT_OFF    8
#define SYMNAME          "_mkchained_sym"

enum { MK_PLAIN, MK_WEAK, MK_BIG };

/* segname/sectname are char[16] and need NOT be NUL-terminated; see
 * tests/README.md's host-portability section for why strcpy is wrong here. */
static void set_name16(char *field, const char *name) {
    size_t len = strlen(name);
    if (len > 16) len = 16;
    memset(field, 0, 16);
    memcpy(field, name, len);
}

static struct segment_command_64 *put_seg(uint8_t *p, const char *name,
                                          uint64_t vmaddr, uint64_t vmsize,
                                          uint64_t fileoff, uint64_t filesize,
                                          uint32_t nsects) {
    struct segment_command_64 *s = (struct segment_command_64 *)p;
    s->cmd = LC_SEGMENT_64;
    s->cmdsize = (uint32_t)(sizeof *s + nsects * sizeof(struct section_64));
    set_name16(s->segname, name);
    s->vmaddr = vmaddr; s->vmsize = vmsize;
    s->fileoff = fileoff; s->filesize = filesize;
    s->maxprot = 7; s->initprot = 3;
    s->nsects = nsects; s->flags = 0;
    return s;
}

static void put_sect(struct segment_command_64 *seg, int i, const char *sect,
                     const char *segname, uint64_t addr, uint64_t size,
                     uint32_t offset) {
    struct section_64 *s = (struct section_64 *)(seg + 1) + i;
    memset(s, 0, sizeof *s);
    set_name16(s->sectname, sect);
    set_name16(s->segname, segname);
    s->addr = addr; s->size = size; s->offset = offset;
}

static int make(const char *path, int mode) {
    uint64_t data_size = (mode == MK_BIG) ? BIG_DATA_SIZE : DATA_SIZE;
    uint64_t linkedit_off = DATA_OFF + data_size;
    uint64_t fixups_off = linkedit_off;
    uint64_t trie_off = linkedit_off + FIXUPS_SIZE;
    size_t fsize = (size_t)(linkedit_off + 0x1000);

    uint8_t *buf = calloc(1, fsize);
    if (!buf) return 2;

    struct mach_header_64 *h = (struct mach_header_64 *)buf;
    h->magic = MH_MAGIC_64;
    h->cputype = CPU_TYPE_X86_64;
    h->cpusubtype = CPU_SUBTYPE_X86_64_ALL;
    h->filetype = MH_DYLIB;
    h->flags = MH_NOUNDEFS | MH_DYLDLINK | MH_TWOLEVEL;

    uint8_t *p = buf + sizeof *h;

    struct segment_command_64 *text = put_seg(p, "__TEXT", TEXT_VMADDR, 0x1000, 0, 0x1000, 1);
    put_sect(text, 0, "__text", "__TEXT", TEXT_VMADDR + SECT_OFF, 4, SECT_OFF);
    p += text->cmdsize;

    struct segment_command_64 *data = put_seg(p, "__DATA", TEXT_VMADDR + DATA_OFF, data_size,
                                              DATA_OFF, data_size, 1);
    put_sect(data, 0, "__data", "__DATA", TEXT_VMADDR + DATA_OFF, data_size, DATA_OFF);
    p += data->cmdsize;

    struct segment_command_64 *le = put_seg(p, "__LINKEDIT", TEXT_VMADDR + linkedit_off, 0x1000,
                                            linkedit_off, 0x1000, 0);
    p += le->cmdsize;

    struct linkedit_data_command *cf = (struct linkedit_data_command *)p;
    cf->cmd = LC_DYLD_CHAINED_FIXUPS; cf->cmdsize = sizeof *cf;
    cf->dataoff = (uint32_t)fixups_off; cf->datasize = FIXUPS_SIZE;
    p += cf->cmdsize;

    struct linkedit_data_command *tr = (struct linkedit_data_command *)p;
    tr->cmd = LC_DYLD_EXPORTS_TRIE; tr->cmdsize = sizeof *tr;
    tr->dataoff = (uint32_t)trie_off; tr->datasize = TRIE_SIZE;
    p += tr->cmdsize;

    /* LC_BUILD_VERSION by hand: 10.9's <mach-o/loader.h> has no
     * build_version_command struct, only the command number mach_compat.h
     * supplies. cmd, cmdsize, platform, minos, sdk, ntools. */
    uint32_t *bv = (uint32_t *)p;
    bv[0] = LC_BUILD_VERSION; bv[1] = 24; bv[2] = 1;
    bv[3] = 0x000C0000; bv[4] = 0x000C0000; bv[5] = 0;
    p += 24;

    h->ncmds = 6;
    h->sizeofcmds = (uint32_t)(p - (buf + sizeof *h));

    /* The chain in __DATA. Every link uses pointer format 6
     * (DYLD_CHAINED_PTR_64_OFFSET): bit 63 selects bind over rebase, bits
     * [62:51] are the distance to the next link in 4-byte strides, and the low
     * bits are a base-relative target (rebase) or an import ordinal (bind). */
    uint64_t *slot = (uint64_t *)(buf + DATA_OFF);
    if (mode == MK_BIG) {
        uint64_t n = data_size / 8;
        for (uint64_t i = 0; i < n; i++)
            slot[i] = REBASE_TARGET | ((i + 1 < n) ? ((uint64_t)2 << 51) : 0);
    } else {
        slot[0] = REBASE_TARGET | ((uint64_t)(BIND_SLOT_OFF / 4) << 51);
        slot[1] = (1ULL << 63) | 0ULL;   /* bind import 0, next = 0 = end of chain */
    }

    /* The fixups blob: header, starts-image, one starts-segment for __DATA
     * (segment index 1), one import, one symbol name. */
    uint8_t *fx = buf + fixups_off;
    uint32_t *fh = (uint32_t *)fx;
    fh[0] = 0;      /* fixups_version */
    fh[1] = 0x20;   /* starts_offset */
    fh[2] = 0x60;   /* imports_offset */
    fh[3] = 0x80;   /* symbols_offset */
    fh[4] = 1;      /* imports_count */
    fh[5] = 1;      /* imports_format: DYLD_CHAINED_IMPORT */
    fh[6] = 0;      /* symbols_format: uncompressed */

    uint32_t *starts = (uint32_t *)(fx + 0x20);
    starts[0] = 3;      /* seg_count: __TEXT, __DATA, __LINKEDIT */
    starts[1] = 0;      /* __TEXT: no fixups */
    starts[2] = 0x10;   /* __DATA: its starts-segment, relative to starts */
    starts[3] = 0;      /* __LINKEDIT: no fixups */

    uint8_t *ss = fx + 0x30;
    *(uint32_t *)(ss + 0)  = 24;                /* size */
    *(uint16_t *)(ss + 4)  = 0x1000;            /* page_size */
    *(uint16_t *)(ss + 6)  = CF_PTR_64_OFFSET;  /* pointer_format */
    *(uint64_t *)(ss + 8)  = DATA_OFF;          /* segment_offset */
    *(uint32_t *)(ss + 16) = 0;                 /* max_valid_pointer */
    *(uint16_t *)(ss + 20) = 1;                 /* page_count: one page START,
                                                 * whose chain may run on past
                                                 * that page -- which is what
                                                 * make-big's does */
    *(uint16_t *)(ss + 22) = 0;                 /* page_start[0]: chain at +0 */

    /* import 0. lib_ordinal 1 normally; 0xFD reads back as the signed -3 that
     * means BIND_SPECIAL_DYLIB_WEAK_LOOKUP, the ordinal 10.9's dyld refuses. */
    *(uint32_t *)(fx + 0x60) = (mode == MK_WEAK) ? 0xFDu : 1u;
    memcpy(fx + 0x80, SYMNAME, sizeof SYMNAME);

    memset(buf + trie_off, 0, TRIE_SIZE);

    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0755);
    if (fd < 0) { perror("create"); free(buf); return 2; }
    if (write(fd, buf, fsize) != (ssize_t)fsize) { perror("write"); close(fd); free(buf); return 2; }
    close(fd);
    free(buf);
    return 0;
}

static int has_cmd(uint8_t *buf, uint32_t want) {
    struct mach_header_64 *h = (struct mach_header_64 *)buf;
    uint8_t *p = buf + sizeof *h;
    for (uint32_t i = 0; i < h->ncmds; i++) {
        struct load_command *lc = (struct load_command *)p;
        if (lc->cmd == want) return 1;
        p += lc->cmdsize;
    }
    return 0;
}

static int check(const char *path) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) { perror("open"); return 2; }
    struct stat st;
    if (fstat(fd, &st) != 0) { perror("fstat"); close(fd); return 2; }
    uint8_t *buf = malloc((size_t)st.st_size);
    if (!buf || read(fd, buf, (size_t)st.st_size) != (ssize_t)st.st_size) {
        fprintf(stderr, "read failed\n"); close(fd); return 2;
    }
    close(fd);
    struct mach_header_64 *h = (struct mach_header_64 *)buf;
    if (h->magic != MH_MAGIC_64) { fprintf(stderr, "not a 64-bit Mach-O\n"); free(buf); return 2; }

    uint64_t *slot = (uint64_t *)(buf + DATA_OFF);
    printf("size=%llu\n", (unsigned long long)st.st_size);
    printf("slot0=0x%llx\n", (unsigned long long)slot[0]);
    printf("slot1=0x%llx\n", (unsigned long long)slot[1]);
    printf("chained=%d\n", has_cmd(buf, LC_DYLD_CHAINED_FIXUPS));
    printf("trie=%d\n", has_cmd(buf, LC_DYLD_EXPORTS_TRIE));
    printf("buildver=%d\n", has_cmd(buf, LC_BUILD_VERSION));
    printf("dyldinfo=%d\n", has_cmd(buf, LC_DYLD_INFO_ONLY));

    /* The LC_DYLD_INFO_ONLY the conversion added, and the __LINKEDIT it had to
     * extend to cover what that command points at. */
    uint8_t *p = buf + sizeof *h;
    for (uint32_t i = 0; i < h->ncmds; i++) {
        struct load_command *lc = (struct load_command *)p;
        if (lc->cmd == LC_DYLD_INFO_ONLY) {
            struct dyld_info_command *di = (struct dyld_info_command *)lc;
            printf("rebase=%u+%u\n", di->rebase_off, di->rebase_size);
            printf("bind=%u+%u\n", di->bind_off, di->bind_size);
            printf("export=%u+%u\n", di->export_off, di->export_size);
            /* The bind stream's first three opcodes are SET_TYPE_IMM, the
             * dylib-ordinal opcode and SET_SYMBOL_TRAILING_FLAGS_IMM, each one
             * byte, and the symbol name follows the third as a NUL-terminated
             * string. Reading them back is how this proves the BIND link was
             * translated and not merely counted, and it is what makes the -3
             * weak remap observable: the second byte says which dylib opcode
             * was chosen and the third carries the weak-import flag. Reading
             * at a FIXED offset is deliberate: if the emitted opcode sequence
             * ever changes shape, that is a behaviour change in the conversion
             * and this must fail rather than adapt. */
            if (di->bind_size > 4 && di->bind_off + di->bind_size <= (uint32_t)st.st_size) {
                uint8_t *b = buf + di->bind_off;
                printf("bindops=%02x,%02x,%02x\n", b[0], b[1], b[2]);
                printf("bindsym=%.*s\n", (int)(di->bind_size - 3), (char *)b + 3);
            }
        }
        if (lc->cmd == LC_SEGMENT_64) {
            struct segment_command_64 *s = (struct segment_command_64 *)lc;
            if (strncmp(s->segname, "__LINKEDIT", 16) == 0)
                printf("linkedit=%llu+%llu\n", (unsigned long long)s->fileoff,
                       (unsigned long long)s->filesize);
        }
        p += lc->cmdsize;
    }
    free(buf);
    return 0;
}

int main(int argc, char **argv) {
    if (argc != 3) { fprintf(stderr, "usage: mkchained make|make-weak|make-big|check FILE\n"); return 2; }
    if (strcmp(argv[1], "make") == 0) return make(argv[2], MK_PLAIN);
    if (strcmp(argv[1], "make-weak") == 0) return make(argv[2], MK_WEAK);
    if (strcmp(argv[1], "make-big") == 0) return make(argv[2], MK_BIG);
    if (strcmp(argv[1], "check") == 0) return check(argv[2]);
    fprintf(stderr, "usage: mkchained make|make-weak|make-big|check FILE\n");
    return 2;
}
EOF
"$CC" -O2 -I "$SRC_DIR" -o "$T/mkchained" "$T/mkchained.c"
"$T/mkchained" make "$T/chained.in"

# What the fixture is, before anything touches it. If this ever stops holding,
# every assertion below is asking the wrong question and would "pass" for the
# wrong reason -- so it is checked, not assumed.
chk=$("$T/mkchained" check "$T/chained.in")
if echo "$chk" | grep -q "^chained=1" && echo "$chk" | grep -q "^dyldinfo=0"; then
    ok "declassify: fixture really uses chained fixups and has no LC_DYLD_INFO_ONLY"
else
    bad "declassify: fixture" "not the shape this suite expects: $(echo "$chk" | tr '\n' ' ')"
fi

"$MACHO9" declassify "$T/chained.in" "$T/chained.out" >"$T/dcl.out" 2>"$T/dcl.err" && rc=0 || rc=$?
[ "$rc" -eq 0 ] && ok "declassify: converts a chained-fixups binary (exit 0)" \
    || bad "declassify: exit code" "exited $rc on a chained-fixups binary: $(cat "$T/dcl.err")"

# The conversion's whole job, read back out of the output file's bytes.
#   slot0  a REBASE of base-relative 0x1000 in a __TEXT based at 0x100000000,
#          so the classic rebase (which adds the slide, not slide+base) has to
#          find the absolute 0x100001000 already in the slot;
#   slot1  a BIND, which must be zeroed for dyld to fill in;
#   the three modern load commands must be gone, LC_DYLD_INFO_ONLY present,
#   and __LINKEDIT must now cover the appended opcode streams -- otherwise dyld
#   would not read the very bytes the new command points at.
dcl_out=$("$T/mkchained" check "$T/chained.out")
dcl_val() { echo "$dcl_out" | sed -n "s/^$1=//p"; }
dcl_fail=0
dcl_expect() {
    got=$(dcl_val "$1")
    [ "$got" = "$2" ] || { bad "declassify: $1" "expected $2, got '$got'"; dcl_fail=1; }
}
dcl_expect slot0 0x100001000
dcl_expect slot1 0x0
dcl_expect chained 0
dcl_expect trie 0
dcl_expect buildver 0
dcl_expect dyldinfo 1
dcl_expect bindsym _mkchained_sym
# The three bind opcodes, read back as bytes: SET_TYPE_IMM|POINTER (0x51),
# SET_DYLIB_ORDINAL_IMM|1 (0x11), SET_SYMBOL_TRAILING_FLAGS_IMM with no flags
# (0x40). mkchained prints them; nothing asserted them until now, which left
# "the bind was translated" resting on the symbol name alone. It is also the
# baseline the weak-ordinal case below is a deviation from.
dcl_expect bindops 51,11,40
[ "$dcl_fail" -eq 0 ] && ok "declassify: rebase rewritten, bind zeroed and named, modern commands stripped"

# __LINKEDIT has to end exactly where the file now does: the conversion
# appends the rebase/bind streams past its old end and extends it to cover
# them. This is one of the two sanctioned exceptions to "never move a byte",
# so it gets its own assertion rather than riding along with the others.
le=$(dcl_val linkedit); le_off=${le%%+*}; le_size=${le##*+}
osize=$(dcl_val size)
if [ "$((le_off + le_size))" -eq "$osize" ]; then
    ok "declassify: __LINKEDIT extended to cover the appended opcode streams"
else
    bad "declassify: __LINKEDIT" "ends at $((le_off + le_size)), file is $osize bytes"
fi
rebase=$(dcl_val rebase); bind=$(dcl_val bind)
if [ "${rebase##*+}" -gt 0 ] && [ "${bind##*+}" -gt 0 ]; then
    ok "declassify: LC_DYLD_INFO_ONLY points at non-empty rebase and bind streams"
else
    bad "declassify: LC_DYLD_INFO_ONLY" "empty stream(s): rebase=$rebase bind=$bind"
fi

# THE -3 WEAK-LOOKUP REMAP, which nothing in this repo asserted and which
# fails at LOAD TIME on the real product when it is wrong: a bind whose library
# ordinal is -3 (BIND_SPECIAL_DYLIB_WEAK_LOOKUP, emitted by every modern
# toolchain) is rejected by 10.9's dyld with "bad special ordinal". The
# conversion has to rewrite it as flat lookup (-2) plus the weak-import flag,
# and the rewrite is visible in the opcodes: 0x3e is
# SET_DYLIB_SPECIAL_IMM|(-2 & 0x0F) where the plain fixture has 0x11
# (SET_DYLIB_ORDINAL_IMM|1), and 0x41 is SET_SYMBOL_TRAILING_FLAGS_IMM with
# BIND_SYMBOL_FLAGS_WEAK_IMPORT where the plain fixture has 0x40.
"$T/mkchained" make-weak "$T/weak.in"
"$MACHO9" declassify "$T/weak.in" "$T/weak.out" >/dev/null 2>"$T/weak.err" && rc=0 || rc=$?
if [ "$rc" -eq 0 ]; then
    dcl_out=$("$T/mkchained" check "$T/weak.out")
    dcl_fail=0
    dcl_expect bindops 51,3e,41
    dcl_expect bindsym _mkchained_sym
    dcl_expect slot1 0x0
    [ "$dcl_fail" -eq 0 ] && ok "declassify: a -3 weak-lookup ordinal becomes flat lookup + weak-import"
else
    bad "declassify: weak ordinal" "exited $rc: $(cat "$T/weak.err")"
fi

# THE OPCODE BUFFER'S BOUND. The conversion emits into two fixed 1MB buffers
# (declassify.h's LIMITS). A binary with more fixups than that used to walk
# straight off the end of the allocation -- ~5 bytes per rebase, so about 200k
# fixups reaches it, which a large modern binary genuinely carries. This
# fixture is a 2MB __DATA that is one 262k-link rebase chain: the conversion
# must REFUSE it, say so, and write nothing.
"$T/mkchained" make-big "$T/big.in"
rm -f "$T/big.out"
"$MACHO9" declassify "$T/big.in" "$T/big.out" >/dev/null 2>"$T/big.err" && rc=0 || rc=$?
[ "$rc" -eq 1 ] && ok "declassify: refuses a binary with more fixups than the opcode buffer holds" \
    || bad "declassify: opcode overflow" "expected EX_REFUSED (1), got $rc: $(cat "$T/big.err")"
grep -q "opcode buffer" "$T/big.err" \
    && ok "declassify: names the opcode buffer as the reason" \
    || bad "declassify: opcode overflow" "no reason on stderr: $(cat "$T/big.err")"
[ -e "$T/big.out" ] && bad "declassify: opcode overflow" "wrote an output for an input it refused" \
    || ok "declassify: an over-large input produces no output file"
if [ -x "$BIN/patch_macho" ]; then
    rm -f "$T/big.pm"
    "$BIN/patch_macho" "$T/big.in" "$T/big.pm" >/dev/null 2>&1 && rc=0 || rc=$?
    [ "$rc" -eq 1 ] && ok "declassify: patch_macho refuses the same over-large input with its flat 1" \
        || bad "declassify: opcode overflow (patch_macho)" "expected 1, got $rc"
fi

# BYTE-IDENTITY WITH patch_macho, the strongest available proof that lifting
# the conversion into src/declassify.c did not change it: the two front-ends
# are handed the same buffer by md_declassify and must write the same bytes.
# Not a hard requirement of THIS script (macho9 stands alone, and $BIN need
# not hold anything else), so its absence is a SKIP, not a failure.
if [ -x "$BIN/patch_macho" ]; then
    "$BIN/patch_macho" "$T/chained.in" "$T/chained.pm" >/dev/null 2>&1
    if cmp -s "$T/chained.out" "$T/chained.pm"; then
        ok "declassify: byte-identical to patch_macho's output"
    else
        bad "declassify: byte-identity" "macho9 and patch_macho produced different bytes"
    fi
    # patch_macho returns a flat 1 for everything that goes wrong; this verb
    # distinguishes "examined it and declined" (EX_REFUSED=1) from an
    # operational failure (EX_FAIL=2). For THIS refusal the two numbers
    # happen to agree (both 1) -- that is a coincidence of the corrected
    # numbering, not a design goal -- but a Task 2 wrapper still has real
    # mapping work to do for the EX_FAIL=2 case, where the numbers diverge;
    # compat/patch_macho.sh's own header covers both.
    "$BIN/patch_macho" "$T/not-a-macho-in-cli-test" "$T/nope_pm" >/dev/null 2>&1 && pm_rc=0 || pm_rc=$?
    [ "$pm_rc" -eq 1 ] && ok "declassify: patch_macho's flat 1 and this verb's EX_REFUSED agree on this refusal" \
        || bad "declassify: patch_macho exit" "expected the historical flat 1, got $pm_rc"
else
    skip "declassify: byte-identity with patch_macho" "no patch_macho in $BIN"
fi

# IDEMPOTENCY, which install.sh's wrapper depends on: running the conversion
# over an already-converted binary passes it through unchanged instead of
# failing on the fixups that are no longer there.
"$MACHO9" declassify "$T/chained.out" "$T/chained.again" >"$T/again.out" 2>"$T/again.err" && rc=0 || rc=$?
if [ "$rc" -eq 0 ]; then
    if cmp -s "$T/chained.out" "$T/chained.again"; then
        ok "declassify: an already-converted binary passes through byte-for-byte"
    else
        bad "declassify: idempotency" "a second conversion changed the bytes"
    fi
    grep -q "Already patched" "$T/again.out" \
        && ok "declassify: says it passed through" \
        || bad "declassify: pass-through message" "no 'Already patched' line: $(cat "$T/again.out")"
else
    bad "declassify: idempotency" "exited $rc on an already-converted binary: $(cat "$T/again.err")"
fi

# IN and OUT may be the same path: the whole image is in memory before a byte
# is written, so this is an in-place conversion and must land the same bytes.
cp "$T/chained.in" "$T/inplace"
"$MACHO9" declassify "$T/inplace" "$T/inplace" >/dev/null 2>"$T/inplace.err" && rc=0 || rc=$?
if [ "$rc" -eq 0 ]; then
    cmp -s "$T/inplace" "$T/chained.out" \
        && ok "declassify: IN and OUT may be the same file" \
        || bad "declassify: IN == OUT" "in-place output differs from the two-file output"
else
    bad "declassify: IN == OUT" "exited $rc : $(cat "$T/inplace.err")"
fi

# Refusals. Each is a decision macho9 made about the INPUT, so each is
# EX_REFUSED (1), never EX_FAIL (2), which means "something went wrong running
# macho9" -- that distinction is what --capabilities' exitcodes line promises.
"$MACHO9" declassify "$T/not-a-macho-in-cli-test" "$T/nope" >/dev/null 2>"$T/nm.err" && rc=0 || rc=$?
[ "$rc" -eq 1 ] && ok "declassify: refuses a non-Mach-O with EX_REFUSED" \
    || bad "declassify: non-Mach-O" "expected 1, got $rc"
[ -e "$T/nope" ] && bad "declassify: non-Mach-O" "wrote an output file for an input it refused" \
    || ok "declassify: a refused input produces no output file"

# A 64-bit Mach-O with NEITHER chained fixups NOR LC_DYLD_INFO_ONLY -- a plain
# object file is exactly that -- is not idempotent-pass-through material and
# not convertible either. It must say so and refuse, not quietly copy.
"$CC" -c -O2 $FIXTURE_FLAGS "$T/main.c" -o "$T/plain.o"
"$MACHO9" declassify "$T/plain.o" "$T/plain.out" >/dev/null 2>"$T/plain.err" && rc=0 || rc=$?
[ "$rc" -eq 1 ] && ok "declassify: refuses a Mach-O with no chained fixups and no LC_DYLD_INFO_ONLY" \
    || bad "declassify: no fixups" "expected 1, got $rc"
grep -q "No chained fixups found" "$T/plain.err" \
    && ok "declassify: says why it refused" \
    || bad "declassify: no fixups" "no reason on stderr: $(cat "$T/plain.err")"

# An OUT that cannot be written is an OPERATIONAL failure, not a refusal: the
# input was fine and macho9 declined nothing. It must exit 2 (EX_FAIL), and
# this is the assertion that keeps EX_REFUSED from decaying into "any nonzero".
"$MACHO9" declassify "$T/chained.in" "$T/no/such/dir/out" >/dev/null 2>"$T/unwritable.err" && rc=0 || rc=$?
[ "$rc" -eq 2 ] && ok "declassify: an unwritable OUT is a failure (2), not a refusal (1)" \
    || bad "declassify: unwritable OUT" "expected 2, got $rc"

# ============================================================================
# macho9 stands alone
#
# dylib/rpath/lc/minos used to fork and exec change_dylib/add_version_min,
# located next to macho9 on disk, and --capabilities hid those four verbs
# whenever the sibling was missing. Both are gone: the rewrite is linked in
# (src/rewrite.c, src/version_min.c). That is the whole point of the
# extraction -- it is what lets change_dylib become a wrapper AROUND macho9
# without a cycle -- so prove it from the outside rather than by reading the
# source: copy ONLY macho9 into an empty directory and make it do real work
# there. A regression that restored the subprocess would fail here even
# though every other assertion in this file, run from a full bindir, would
# still pass.
# ============================================================================
mkdir -p "$T/alone"
cp "$MACHO9" "$T/alone/macho9"
alone_caps=$("$T/alone/macho9" --capabilities)
alone_missing=""
for v in verify info grow minos lc dylib rpath segment retag-swift; do
    echo "$alone_caps" | grep -q "^verb $v" || alone_missing="$alone_missing $v"
done
[ -z "$alone_missing" ] && ok "alone: --capabilities still advertises every verb with no sibling present" \
    || bad "alone: capabilities" "verbs missing when macho9 stands alone:$alone_missing"

build_main "$T/alone/fixture"
if "$T/alone/macho9" dylib "$T/alone/fixture" -append "@loader_path/libalone.dylib" \
        >"$T/alone_dylib.out" 2>&1; then
    ok "alone: dylib -append works with no change_dylib anywhere near macho9"
else
    bad "alone: dylib -append" "$(cat "$T/alone_dylib.out")"
fi
"$T/alone/macho9" info "$T/alone/fixture" | grep -qF "path=@loader_path/libalone.dylib" \
    && ok "alone: the append really landed in the file" \
    || bad "alone: dylib -append result" "new dependency not in info output"

if "$T/alone/macho9" lc "$T/alone/fixture" -delete uuid >"$T/alone_lc.out" 2>&1; then
    ok "alone: lc -delete works with no change_dylib anywhere near macho9"
else
    bad "alone: lc -delete" "$(cat "$T/alone_lc.out")"
fi

# minos is the one verb that was gated on add_version_min rather than
# change_dylib, so it needs its own standalone run -- a regression that
# restored only THAT subprocess would sail past the two assertions above.
#
# Which of mv_add_version_min's two success paths runs here depends on the
# host's linker: a 10.9 ld emits LC_VERSION_MIN_MACOSX itself (so this is the
# "already present" path), a 2026 one emits LC_BUILD_VERSION instead (so this
# actually appends). Both are exit 0 and both prove the point, so accept
# either MESSAGE rather than asserting which -- what must not happen is
# macho9 failing because a binary it no longer needs isn't there. (Note the
# fixture is deliberately NOT stripped of its version-min first: the helper
# that does that is built further down, and this assertion is about reaching
# the driver at all, not about which branch of it ran.)
if "$T/alone/macho9" minos "$T/alone/fixture" 10.9 >"$T/alone_minos.out" 2>&1; then
    ok "alone: minos works with no add_version_min anywhere near macho9"
else
    bad "alone: minos" "$(cat "$T/alone_minos.out")"
fi
if grep -q "LC_VERSION_MIN_MACOSX" "$T/alone_minos.out"; then
    ok "alone: minos reached the version-min driver in-process (said what it did)"
else
    bad "alone: minos output" "exited 0 but said nothing about LC_VERSION_MIN_MACOSX: $(cat "$T/alone_minos.out")"
fi
"$T/alone/macho9" info "$T/alone/fixture" | grep -q "LC_VERSION_MIN_MACOSX" \
    && ok "alone: the fixture carries LC_VERSION_MIN_MACOSX afterward" \
    || bad "alone: minos result" "no LC_VERSION_MIN_MACOSX in info output after minos"

# ============================================================================
# verify
# ============================================================================
build_main "$T/verify_ok"
if "$MACHO9" verify "$T/verify_ok" >"$T/verify_ok.out"; then
    ok "verify: accepts a real binary"
else
    bad "verify: real binary" "refused: $(cat "$T/verify_ok.out")"
fi
# `: OK` and not `OK`: cmd_verify's verdict line is "<path>: OK", and a bare
# two-character substring would also be satisfied by a path, a diagnostic or a
# future line that happens to contain them. Paired with the exit-status check
# above it could not silently pass today, but the tighter pattern costs
# nothing and is what every other verify assertion in this file uses.
grep -q ': OK' "$T/verify_ok.out" && ok "verify: reports OK" || bad "verify: OK text" "missing"

echo 'not a mach-o' > "$T/verify_bad"
if "$MACHO9" verify "$T/verify_bad" >/dev/null 2>&1; then
    bad "verify: garbage file" "should have been refused"
else
    ok "verify: refuses a non-Mach-O file"
fi

# A dylib links at image base 0, and mg_plausible used to read that 0 as
# mi_text_base's "no segment maps the header" sentinel and refuse before
# checking anything -- so the gate refused every dylib on the machine for a
# reason that had nothing to do with plausibility. The tell was
# `FAILED (see above)` with nothing above it: no entry was ever checked.
# Built here, not found on the host: a test that scans /usr/lib for a
# suitable dylib skips itself away on the cross runner, where those dylibs
# live in the dyld shared cache. FIXTURE_FLAGS for the usual reason, and no
# -headerpad is needed because neither assertion below grows the header.
"$CC" -dynamiclib -O2 $FIXTURE_FLAGS -install_name "@loader_path/fixture.dylib" \
    "$T/a.c" -o "$T/fixture.dylib"
if "$MACHO9" verify "$T/fixture.dylib" >"$T/dylibverify.out" 2>&1; then
    ok "verify: a dylib gets a real verdict"
else
    bad "verify: a dylib gets a real verdict" "refused: $(cat "$T/dylibverify.out")"
fi
grep -q ': OK' "$T/dylibverify.out" && ok "verify: and says OK" \
    || bad "verify: and says OK" "missing: $(cat "$T/dylibverify.out")"
if grep -q "FAILED (see above)" "$T/dylibverify.out"; then
    bad "verify: not the contentless failure" \
        "the precondition bail is back: $(cat "$T/dylibverify.out")"
else
    ok "verify: not the contentless failure"
fi

# The gate refusing every dylib meant no dylib could be rewritten at all:
# mr_process_thin gates on mg_plausible, so macho9 dylib/rpath/lc -- and
# change_dylib, which is the same code -- refused every dylib outright.
cp "$T/fixture.dylib" "$T/dylibrw"
if "$MACHO9" lc "$T/dylibrw" -delete uuid >"$T/dylibrw.out" 2>&1; then
    ok "lc -delete: a dylib is rewritable"
else
    bad "lc -delete: a dylib is rewritable" "refused: $(cat "$T/dylibrw.out")"
fi

# ============================================================================
# info
# ============================================================================
build_main "$T/info_fixture"
info_out=$("$MACHO9" info "$T/info_fixture") || bad "info: exit" "nonzero"
echo "$info_out" | grep -q "LC_SEGMENT_64" && ok "info: shows LC_SEGMENT_64" \
    || bad "info: segments" "not found in output"
echo "$info_out" | grep -q "segname=__TEXT" && ok "info: shows __TEXT segment" \
    || bad "info: __TEXT" "not found in output"
echo "$info_out" | grep -q "ordinal=1 path=.*liba.dylib" && ok "info: shows liba as ordinal 1" \
    || bad "info: ordinal" "not found in output: $info_out"
echo "$info_out" | grep -q "header pad:" && ok "info: shows header pad line" \
    || bad "info: header pad" "not found in output"

# ============================================================================
# grow
# ============================================================================
build_main "$T/grow_fixture"
before=$(wc -c < "$T/grow_fixture")
"$MACHO9" grow "$T/grow_fixture" 4096 >"$T/grow.out" || bad "grow: exit" "$(cat "$T/grow.out")"
after=$(wc -c < "$T/grow_fixture")
if [ "$after" -eq "$((before + 4096))" ]; then
    ok "grow: file grew by exactly the page-aligned request"
else
    bad "grow: size" "before=$before after=$after (expected +4096)"
fi
"$MACHO9" verify "$T/grow_fixture" >/dev/null && ok "grow: result still verifies" \
    || bad "grow: post-grow verify" "failed"

# grow now replaces its target via wa_write_atomic (src/atomic_write.h,
# mkstemp+rename) -- the same path change_dylib uses -- instead of
# ftruncate()+write() straight into the open file. Prove the symlink-safety
# that buys: growing THROUGH a symlink must rewrite the REAL target (fresh
# inode, since rename() always creates one) and leave the symlink itself
# intact, not replace the symlink with a plain file the way a naive rename
# of the symlink PATH itself would.
build_main "$T/grow_link_target"
ln -sf grow_link_target "$T/grow_link"
target_ino_before=$(stat -f %i "$T/grow_link_target")
"$MACHO9" grow "$T/grow_link" 4096 >"$T/grow_link.out" 2>&1 \
    || bad "grow: symlink" "exit failed: $(cat "$T/grow_link.out")"
if [ -L "$T/grow_link" ]; then
    ok "grow: growing through a symlink leaves the symlink a symlink"
else
    bad "grow: symlink" "the symlink itself got replaced by a plain file"
fi
target_ino_after=$(stat -f %i "$T/grow_link_target")
if [ "$target_ino_after" != "$target_ino_before" ]; then
    ok "grow: the real target was replaced via mkstemp+rename (fresh inode = atomicity kept)"
else
    bad "grow: symlink" "target inode unchanged -- wrote in place, not atomically"
fi
readlink "$T/grow_link" | grep -q "^grow_link_target$" \
    && ok "grow: symlink still points at the same name" \
    || bad "grow: symlink" "symlink target changed: $(readlink "$T/grow_link")"

# Whether a GROWN binary can be EXECUTED, ruling (settled after evidence: a
# prior round's host-capability probe showed the cross runner runs a
# trivially-perturbed binary FINE but specifically refuses a GROWN one --
# so this is not the code-signing question $signing_enforced answers; it is
# a genuinely different dyld objection to the grow transformation itself):
#
# These tools exist to produce binaries loadable by Mac OS X 10.9. That is
# the product's contract. mg_grow_header works by LOWERING the image base
# -- donating bytes from __PAGEZERO and dropping __TEXT's vmaddr -- an
# exotic transformation 10.9's dyld accepts by design. A grown binary is
# NOT required to also load on a newer macOS, and asserting that it must
# would be a stronger requirement than the product makes. So: on the
# target platform (Darwin 13.x / Mac OS X 10.9) this stays a HARD
# assertion, unconditionally. Everywhere else, a run failure is scoped out
# with a SKIP -- but never a hand-waved "host policy": it carries whatever
# diagnostic this host's own loader actually gave, captured here (exit
# status/signal plus DYLD_PRINT_LIBRARIES=1 output and any dyld stderr
# text), so the record says exactly what a newer dyld objects to rather
# than guessing. A future reader deciding whether this is "the product
# doesn't promise this" versus "mg_grow_header has a real bug" should be
# able to read that text and judge for themselves.
if (cd "$T" && DYLD_PRINT_LIBRARIES=1 ./grow_fixture) >"$T/grow_run.out" 2>&1; then
    ok "grow: grown binary still runs"
else
    grow_run_rc=$?
    grow_run_diag=$(cat "$T/grow_run.out" 2>/dev/null | tr '\n' ' ' | cut -c1-800)
    if [ "$is_target_platform" -eq 1 ]; then
        bad "grow: run" "grown binary failed to execute ON THE TARGET PLATFORM ITSELF (Darwin 13 / Mac OS X 10.9) -- this is a real macho9 defect, not a portability question. exit $grow_run_rc: $grow_run_diag"
    else
        skip "grow: grown binary still runs" \
            "not the product's target platform (Darwin $darwin_major; the target is Darwin 13 / Mac OS X 10.9) -- mg_grow_header's image-base-lowering trick is only promised to load there. This host's loader says: exit $grow_run_rc: $grow_run_diag"
    fi
fi
# N=0 is refused, not silently a no-op.
if "$MACHO9" grow "$T/grow_fixture" 0 >/dev/null 2>&1; then
    bad "grow: N=0" "should be refused"
else
    ok "grow: N=0 refused"
fi

# ============================================================================
# minos
# ============================================================================
# build_main's FIXTURE_FLAGS (-mmacosx-version-min=10.9) makes the linker
# emit LC_VERSION_MIN_MACOSX itself -- so a fixture built that way already
# HAS the load command macho9 minos is supposed to add, and the "happy
# path" below would pass even with cmd_minos's body replaced by `return 0`
# (confirmed by doing exactly that -- see the commit message).
#
# A first fix tried -Wl,-no_version_load_command to suppress it at link
# time. That is 10.9-ld-only: it linked here and broke the whole suite on
# the cross runner ("ld: unknown options: -no_version_load_command"),
# trading one host dependency for a worse one -- a hard link failure
# instead of one weak assertion. Fixed properly this time: build the
# fixture NORMALLY (portable -- every fixture in this file does this) and
# then remove the load command ourselves, by direct Mach-O structure
# surgery, with a tiny throwaway C program compiled by plain $CC with no
# special flags -- the same "read/write the structure directly" idiom
# change_dylib_test.sh's ordinal_of.c already uses, so nothing here depends
# on a specific ld/clang version, and nothing here depends on macho9 or
# change_dylib's own strip machinery either (their -strip-lc/`lc -delete`
# vocabulary doesn't cover LC_VERSION_MIN_MACOSX today, and reusing the
# tool under test to build that test's own fixture would be circular
# regardless). The fixture is therefore test-tool-constructed, not
# linker-constructed, for this one load command only.
cat > "$T/strip_version_min.c" <<'EOF'
/* Remove the FIRST LC_VERSION_MIN_MACOSX load command from a Mach-O file,
 * in place: memmove the load commands after it down over it, zero the
 * freed tail bytes (they become header pad), and fix up ncmds/sizeofcmds.
 *
 * The GOAL is a fixture that LACKS LC_VERSION_MIN_MACOSX, not "removed one".
 * A 2026 linker emits LC_BUILD_VERSION instead of LC_VERSION_MIN_MACOSX in
 * the first place (a 10.9-era linker emits the latter), so on a cross host
 * there is nothing to strip -- the goal is already met. That is SUCCESS,
 * not an error: exit 0 either way. Only a genuine failure to remove one
 * that IS present is exit 2. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <mach-o/loader.h>

int main(int argc, char **argv) {
    if (argc != 2) { fprintf(stderr, "usage: %s FILE\n", argv[0]); return 2; }
    int fd = open(argv[1], O_RDWR);
    if (fd < 0) { perror("open"); return 2; }
    struct stat st;
    if (fstat(fd, &st) != 0) { perror("fstat"); close(fd); return 2; }
    size_t size = (size_t)st.st_size;
    uint8_t *buf = malloc(size);
    if (!buf || read(fd, buf, size) != (ssize_t)size) {
        fprintf(stderr, "read failed\n"); close(fd); return 2;
    }
    struct mach_header_64 *hdr = (struct mach_header_64 *)buf;
    if (hdr->magic != MH_MAGIC_64) { fprintf(stderr, "not a 64-bit Mach-O\n"); return 2; }

    uint8_t *lcp = buf + sizeof(*hdr);
    uint32_t found_off = 0, found_size = 0;
    for (uint32_t i = 0; i < hdr->ncmds; i++) {
        struct load_command *lc = (struct load_command *)lcp;
        if (lc->cmd == LC_VERSION_MIN_MACOSX) {
            found_off = (uint32_t)(lcp - buf);
            found_size = lc->cmdsize;
            break;
        }
        lcp += lc->cmdsize;
    }
    if (!found_size) { printf("no LC_VERSION_MIN_MACOSX present; nothing to strip (goal already met)\n"); return 0; }

    uint32_t lc_end = (uint32_t)sizeof(*hdr) + hdr->sizeofcmds;
    uint32_t after = found_off + found_size;
    memmove(buf + found_off, buf + after, lc_end - after);
    memset(buf + lc_end - found_size, 0, found_size);
    hdr->ncmds -= 1;
    hdr->sizeofcmds -= found_size;

    lseek(fd, 0, SEEK_SET);
    if (write(fd, buf, size) != (ssize_t)size) { perror("write"); return 2; }
    close(fd);
    return 0;
}
EOF
"$CC" -O2 -o "$T/strip_version_min" "$T/strip_version_min.c"

build_main "$T/minos_fixture"
# A BARE invocation here would let `set -e` kill the WHOLE script the
# instant this ever exits nonzero -- which used to happen legitimately
# (before the exit-0-on-"already absent" fix above) and took every later
# verb's tests down with it, silently, with only a generic CTest error to
# show for it. Wrapped in `if` so a genuine failure is reported as ONE
# assertion (via bad(), below) and the suite keeps running.
if "$T/strip_version_min" "$T/minos_fixture" >"$T/strip_version_min.out"; then
    strip_rc=0
else
    strip_rc=$?
fi
if [ "$strip_rc" -ne 0 ]; then
    bad "minos: fixture setup" "strip_version_min exited $strip_rc: $(cat "$T/strip_version_min.out")"
fi
# The precondition is specifically "no LC_VERSION_MIN_MACOSX" -- that is the
# ONE load command macho9 minos adds, and the thing the "present after"
# assertion below checks for. LC_BUILD_VERSION is a DIFFERENT load command a
# modern linker emits instead (add_version_min.c only ever looks for
# LC_VERSION_MIN_MACOSX, so LC_BUILD_VERSION's presence is orthogonal to
# this test, not a disqualifier) -- asserting its absence too would be
# asserting something about LC_BUILD_VERSION this test does not need and
# cannot always get.
before_minos=$("$MACHO9" info "$T/minos_fixture")
if echo "$before_minos" | grep -q "LC_VERSION_MIN_MACOSX"; then
    bad "minos: precondition" "fixture still carries LC_VERSION_MIN_MACOSX"
else
    ok "minos: fixture genuinely has no LC_VERSION_MIN_MACOSX before"
fi

"$MACHO9" minos "$T/minos_fixture" 10.9 >"$T/minos.out" || bad "minos: exit" "$(cat "$T/minos.out")"
minos_info=$("$MACHO9" info "$T/minos_fixture")
echo "$minos_info" | grep -q "LC_VERSION_MIN_MACOSX" && ok "minos: LC_VERSION_MIN_MACOSX present after" \
    || bad "minos: version-min" "not found in info output"
# Running it again must not error (add_version_min's own "already present" path).
if "$MACHO9" minos "$T/minos_fixture" 10.9 >/dev/null 2>&1; then
    ok "minos: idempotent re-run does not error"
else
    bad "minos: re-run" "errored on an already-minos'd file"
fi
# Any other version is refused up front -- this build can only target 10.9.
if "$MACHO9" minos "$T/minos_fixture" 10.10 >/dev/null 2>&1; then
    bad "minos: wrong version" "10.10 should be refused"
else
    ok "minos: non-10.9 version refused"
fi

# ============================================================================
# lc -delete
# ============================================================================
build_main "$T/lc_fixture"
before_info=$("$MACHO9" info "$T/lc_fixture")
echo "$before_info" | grep -q "LC_UUID" && ok "lc: fixture has LC_UUID before" \
    || bad "lc: precondition" "fixture has no LC_UUID to delete"
"$MACHO9" lc "$T/lc_fixture" -delete uuid >"$T/lc.out" || bad "lc: exit" "$(cat "$T/lc.out")"
after_info=$("$MACHO9" info "$T/lc_fixture")
if echo "$after_info" | grep -q "LC_UUID"; then
    bad "lc: delete uuid" "LC_UUID still present"
else
    ok "lc: delete uuid removed it"
fi
# Whether a binary that's had its LC_UUID deleted can still be EXECUTED
# turns on TWO independent host facts, not on macho9: (a) kernel
# code-signing enforcement, killing ANY binary modified since it was
# signed -- see $signing_enforced, established above without ever running
# macho9; (b) modern dyld separately refusing to load an image with no
# LC_UUID at all ("missing LC_UUID load command"), which 10.9's dyld does
# not require. These showed up as genuinely different failure modes on the
# cross runner that motivated this (grow got SIGKILLed outright; this got
# far enough for dyld itself to abort on the missing UUID).
#
# signing_enforced already answers (a) honestly. For (b), run lc_fixture
# for real and read its OWN failure, rather than inferring it from a
# separate macho9-produced probe (the same masking risk as grow's old
# probe): only a failure whose message literally names the missing-LC_UUID
# refusal is treated as (b) and skipped; anything else, with signing
# already ruled out, is a real defect and FAILS.
if [ "$signing_enforced" -eq 1 ]; then
    skip "lc: binary still runs after uuid deletion" \
        "this host SIGKILLs any binary modified since it was signed at link time (established independently of macho9 by the host probe above); a host policy, not a macho9 defect, and exercised for real on 10.9"
else
    if (cd "$T" && ./lc_fixture) >"$T/lc_fixture_run.out" 2>&1; then
        ok "lc: binary still runs after uuid deletion"
    else
        lc_run_rc=$?
        if grep -qi "missing LC_UUID" "$T/lc_fixture_run.out" 2>/dev/null; then
            skip "lc: binary still runs after uuid deletion" \
                "modern dyld refuses to load any image with no LC_UUID at all ('missing LC_UUID load command'); 10.9's dyld has no such requirement. Code-signing enforcement was independently ruled out above (a trivially-perturbed binary DID run on this host), so this is dyld's own content-driven refusal, not a masked macho9 defect"
        else
            bad "lc: run" "binary failed to execute after uuid deletion (exit $lc_run_rc: $(head -1 "$T/lc_fixture_run.out" 2>/dev/null || echo 'no output')), this host DOES run a trivially-perturbed binary fine (see host probe above), and dyld did not report its missing-LC_UUID message -- code-signing and the known dyld requirement are both ruled out, so this looks like a real macho9 defect"
        fi
    fi
fi
# Unknown KIND is refused with this verb's own message, before the rewriter
# is ever called -- so a bad KIND never reaches (or is diagnosed by) code
# shared with change_dylib.
if "$MACHO9" lc "$T/lc_fixture" -delete bogus-kind >/dev/null 2>"$T/lc_bad.err"; then
    bad "lc: bad kind" "should be refused"
else
    ok "lc: unknown KIND refused"
fi
grep -q "unknown KIND" "$T/lc_bad.err" && ok "lc: bad kind message" \
    || bad "lc: bad kind message" "missing 'unknown KIND'"
# lc -delete naming a KIND the file does not carry: the strip-cmds twin of
# the dylib -replace miss report above. The absence is MADE true by
# build_main_without_build_version rather than assumed from what the host's
# linker happens to emit -- see that helper for why the old assumption was
# false on the cross runner. So this is a guaranteed miss, not a maybe.
build_main_without_build_version "$T/lc_miss_fixture"
"$MACHO9" lc "$T/lc_miss_fixture" -delete build-version \
    >"$T/lc_miss.out" 2>"$T/lc_miss.err" && lc_miss_rc=0 || lc_miss_rc=$?
[ "$lc_miss_rc" -eq 0 ] && ok "lc: -delete of an absent kind still exits 0" \
    || bad "lc: -delete of an absent kind" "expected 0, got $lc_miss_rc: $(cat "$T/lc_miss.err")"
grep -q "no load command of kind build-version to delete" "$T/lc_miss.err" \
    && ok "lc: names the kind that matched nothing" \
    || bad "lc: -delete of an absent kind" "expected the 'no load command of kind build-version to delete' message on stderr, got: $(cat "$T/lc_miss.err")"

# A duplicated -delete for a kind the file DOES carry ("uuid" -- every build
# has one) must not report the SECOND occurrence as unmatched: the load
# command it names was struck by the very same strip pass the first
# occurrence's match credits. This is the strip_cmds twin of the dylib
# -replace+-delete-same-path case above -- see the "no `break`" comment on
# the strip-kind loop in src/rewrite.c for why a duplicate used to be able
# to make this false.
build_main "$T/lc_dup_fixture"
"$MACHO9" lc "$T/lc_dup_fixture" -delete uuid -delete uuid \
    >/dev/null 2>"$T/lc_dup.err" || bad "lc: duplicate -delete uuid" "$(cat "$T/lc_dup.err")"
if grep -q "no load command of kind uuid to delete" "$T/lc_dup.err"; then
    bad "lc: duplicate -delete uuid" "a uuid load command WAS present and WAS stripped, but the duplicate -delete was reported as a miss: $(cat "$T/lc_dup.err")"
else
    ok "lc: a duplicate -delete for a kind that IS present is not reported as a miss"
fi

# ============================================================================
# lc --fatal-warnings: the same "matched nothing" report as -delete
# build-version above (the fixture is stripped of it first, not assumed to
# lack it), turned
# into a refusal instead of just a stderr note. EX_REFUSED (1), the same
# code a deliberate refusal uses elsewhere (cmd_verify, cmd_grow, cmd_minos),
# because mr_apply_file returns MR_REFUSED for this and MR_REFUSED is
# defined (src/rewrite.h) to equal EX_REFUSED.
# ============================================================================
build_main_without_build_version "$T/lc_fw_fixture"
"$MACHO9" lc "$T/lc_fw_fixture" --fatal-warnings -delete build-version \
    >/dev/null 2>"$T/lc_fw.err" && lc_fw_rc=0 || lc_fw_rc=$?
[ "$lc_fw_rc" -eq 1 ] && ok "lc: --fatal-warnings refuses when a KIND matched nothing (EX_REFUSED)" \
    || bad "lc: --fatal-warnings refusal" "expected exit 1, got $lc_fw_rc: $(cat "$T/lc_fw.err")"
grep -q "no load command of kind build-version to delete" "$T/lc_fw.err" \
    && ok "lc: --fatal-warnings still names the KIND that matched nothing" \
    || bad "lc: --fatal-warnings refusal message" "expected 'no load command of kind build-version to delete', got: $(cat "$T/lc_fw.err")"
# Without --fatal-warnings, the identical invocation still succeeds -- so the
# flag is what changed the answer, not something else about this fixture.
build_main_without_build_version "$T/lc_fw_lax_fixture"
"$MACHO9" lc "$T/lc_fw_lax_fixture" -delete build-version \
    >/dev/null 2>/dev/null && lc_fw_lax_rc=0 || lc_fw_lax_rc=$?
[ "$lc_fw_lax_rc" -eq 0 ] && ok "lc: without --fatal-warnings the same unmatched KIND still succeeds" \
    || bad "lc: no --fatal-warnings" "expected 0, got $lc_fw_lax_rc"
# And when every -delete DOES match, --fatal-warnings must not refuse a run
# that had nothing to complain about.
build_main "$T/lc_fw_ok_fixture"
"$MACHO9" lc "$T/lc_fw_ok_fixture" --fatal-warnings -delete uuid \
    >/dev/null 2>"$T/lc_fw_ok.err" && lc_fw_ok_rc=0 || lc_fw_ok_rc=$?
[ "$lc_fw_ok_rc" -eq 0 ] && ok "lc: --fatal-warnings succeeds when the KIND matched" \
    || bad "lc: --fatal-warnings (matched)" "expected 0, got $lc_fw_ok_rc: $(cat "$T/lc_fw_ok.err")"

# ============================================================================
# dylib: --allow-grow alone (no operation) must be refused with MACHO9's
# OWN usage, not change_dylib's.
#
# --allow-grow used to count toward the "need at least one operation" guard
# (k, which included it), so this exact invocation fell through to
# change_dylib and printed ITS usage -- leaking the -change/-add/-strip-lc/
# -add-rpath spellings this grammar deliberately does not offer (see
# cmd_dylib_or_rpath's `nops` counter in cli/macho9.c). Regression test for
# that fix: no cli_test.sh assertion existed for it before.
# ============================================================================
build_main "$T/dylib_noop_fixture"
if "$MACHO9" dylib "$T/dylib_noop_fixture" --allow-grow >/dev/null 2>"$T/dylib_noop.err"; then
    bad "dylib: --allow-grow alone" "should be refused (no operation given)"
else
    ok "dylib: --allow-grow alone is refused"
fi
grep -q "need at least one operation" "$T/dylib_noop.err" && ok "dylib: --allow-grow alone prints macho9's own usage" \
    || bad "dylib: --allow-grow alone message" "missing macho9's 'need at least one operation'"
if grep -qE -- "-strip-lc|-add-rpath" "$T/dylib_noop.err"; then
    bad "dylib: --allow-grow alone" "leaked change_dylib's usage (-strip-lc/-add-rpath) in: $(cat "$T/dylib_noop.err")"
else
    ok "dylib: --allow-grow alone does not leak change_dylib's spellings"
fi

# ============================================================================
# dylib -replace  (and --allow-grow forcing a real header growth)
# ============================================================================
build_main "$T/dylib_fixture"
newpath="@loader_path/renamed-liba.dylib"
"$MACHO9" dylib "$T/dylib_fixture" -replace "@loader_path/liba.dylib" "$newpath" \
    >"$T/dylib.out" || bad "dylib: -replace exit" "$(cat "$T/dylib.out")"
dylib_info=$("$MACHO9" info "$T/dylib_fixture")
echo "$dylib_info" | grep -q "path=$newpath" && ok "dylib: -replace changed the path" \
    || bad "dylib: -replace" "new path not found in info output"

# A path long enough to overflow the header pad: refused without
# --allow-grow, accepted with it -- proving the flag actually reaches
# change_dylib's -grow rather than being silently dropped.
build_main "$T/dylib_grow_fixture"
# The default linker leaves a generous header pad (observed: ~2.6KB on this
# host), so the replacement has to overflow comfortably past that on any
# plausible linker default -- not just squeak past this host's own number --
# or the "without --allow-grow" half of this test is not actually exercising
# the refusal path.
longpath="@loader_path/$(printf 'x%.0s' $(seq 1 3500)).dylib"
if "$MACHO9" dylib "$T/dylib_grow_fixture" -replace "@loader_path/liba.dylib" "$longpath" \
    >/dev/null 2>"$T/dylib_grow.err"; then
    bad "dylib: long path without --allow-grow" "should have been refused"
else
    ok "dylib: long path without --allow-grow is refused"
fi
if "$MACHO9" dylib "$T/dylib_grow_fixture" --allow-grow -replace "@loader_path/liba.dylib" "$longpath" \
    >"$T/dylib_grow.out"; then
    ok "dylib: --allow-grow lets the same replace through"
else
    bad "dylib: --allow-grow" "$(cat "$T/dylib_grow.out")"
fi
grown_info=$("$MACHO9" info "$T/dylib_grow_fixture")
echo "$grown_info" | grep -qF "path=$longpath" && ok "dylib: --allow-grow result has the long path" \
    || bad "dylib: --allow-grow result" "long path not found"

# ============================================================================
# dylib -replace naming a path the image does not have matched nothing, and
# the tool used to say so NOWHERE: stdout reported only the -replace that DID
# fire, and the exit code was 0. Ask for two, get one, no way to tell -- the
# silent partial success docs/PROPOSAL.md's "verify" section exists to rule
# out ("Every defect found in this code has been a silent success.").
#
# The report has to land on stderr, not stdout: the compat/ wrappers need
# stdout byte-identical to the tools they replaced (tests/known-callers.sh,
# tests/wrapper_test.sh), so anything new has to go where those gates don't
# look.
# ============================================================================
build_main "$T/unmatched_fixture"
unmatched_out=$("$MACHO9" dylib "$T/unmatched_fixture" \
        -replace /usr/lib/libSystem.B.dylib /tmp/new.dylib \
        -replace /nope/absent.dylib /also/absent.dylib \
        2>"$T/unmatched.err") && unmatched_rc=0 || unmatched_rc=$?
[ "$unmatched_rc" -eq 0 ] && ok "dylib: unmatched -replace still exits 0" \
    || bad "dylib: unmatched -replace exit" "expected 0, got $unmatched_rc: $(cat "$T/unmatched.err")"
grep -qF "/nope/absent.dylib" "$T/unmatched.err" && ok "dylib: names the -replace that matched nothing" \
    || bad "dylib: unmatched -replace" "expected /nope/absent.dylib on stderr, got: $(cat "$T/unmatched.err")"
grep -q "matched nothing" "$T/unmatched.err" && ok "dylib: says it matched nothing" \
    || bad "dylib: unmatched -replace message" "expected 'matched nothing' on stderr, got: $(cat "$T/unmatched.err")"
if echo "$unmatched_out" | grep -q "matched nothing"; then
    bad "dylib: unmatched -replace" "'matched nothing' leaked onto stdout: $unmatched_out"
else
    ok "dylib: the unmatched report is not on stdout"
fi
echo "$unmatched_out" | grep -qF "libSystem.B.dylib -> /tmp/new.dylib" \
    && ok "dylib: the -replace that DID match is still reported" \
    || bad "dylib: matched -replace" "expected 'libSystem.B.dylib -> /tmp/new.dylib' on stdout, got: $unmatched_out"
# The inverse of the two checks above: an implementation that reported EVERY
# operation as a miss (hit and miss inverted) would still pass every
# assertion so far -- inverting hit/miss is exactly the failure this feature
# exists to prevent, so it has to be checked for directly, not just inferred
# from the positive cases passing.
if grep -qF "/usr/lib/libSystem.B.dylib" "$T/unmatched.err"; then
    bad "dylib: matched -replace" "the -replace that DID match was reported as a miss: $(cat "$T/unmatched.err")"
else
    ok "dylib: the -replace that matched is NOT reported as a miss"
fi

# A -delete anywhere wins over a conflicting -change for the SAME old_path,
# regardless of argument order (see the comment on mr_is_deleted in
# src/rewrite.c) -- so a -replace and a -delete naming the identical path
# both match the SAME load command. Crediting only the first entry that
# matched (the old behaviour) would report the SECOND -- here, the -delete,
# the operation that actually removed the load command -- as having matched
# nothing, which is false. This is exactly tests/change_dylib_test.sh's
# historical-bug regression case and compat/translate.sh's accumulation of
# -change/-delete into one macho9 invocation, reached through this same
# code path.
build_main "$T/dylib_conflict_fixture"
conflict_path="@loader_path/libconflict.dylib"
"$MACHO9" dylib "$T/dylib_conflict_fixture" -append "$conflict_path" \
    >/dev/null || bad "dylib: conflict fixture setup" "-append of $conflict_path failed"
"$MACHO9" dylib "$T/dylib_conflict_fixture" \
        -replace "$conflict_path" /also/absent.dylib \
        -delete "$conflict_path" \
        >"$T/conflict.out" 2>"$T/conflict.err" && conflict_rc=0 || conflict_rc=$?
[ "$conflict_rc" -eq 0 ] && ok "dylib: -replace and -delete on the same path still exits 0" \
    || bad "dylib: -replace+-delete same path" "expected 0, got $conflict_rc: $(cat "$T/conflict.err")"
conflict_info=$("$MACHO9" info "$T/dylib_conflict_fixture")
if echo "$conflict_info" | grep -qF "path=$conflict_path"; then
    bad "dylib: -replace+-delete same path" "expected the -delete to win (dependency removed), still present in: $conflict_info"
else
    ok "dylib: -replace+-delete same path: the -delete won, as documented"
fi
if grep -q "matched nothing" "$T/conflict.err"; then
    bad "dylib: -replace+-delete same path" "one of the two operations that both matched the SAME load command was reported as a miss: $(cat "$T/conflict.err")"
else
    ok "dylib: -replace+-delete same path: neither operation is reported as a miss"
fi

# ============================================================================
# dylib --fatal-warnings: turns the "matched nothing" report just above from
# a stderr note into a refusal. Two -replace ops, one of which matches and
# one of which cannot -- so this also proves the file is STILL WRITTEN when
# --fatal-warnings refuses: this mode reports AFTER the rewrite, it does not
# roll it back (see mr_ops.fatal_unmatched's own comment in src/rewrite.h).
# ============================================================================
build_main "$T/dylib_fw_fixture"
"$MACHO9" dylib "$T/dylib_fw_fixture" --fatal-warnings \
        -replace "@loader_path/liba.dylib" "@loader_path/renamed-fw.dylib" \
        -replace /nope/absent-fw.dylib /also/absent-fw.dylib \
        >"$T/dylib_fw.out" 2>"$T/dylib_fw.err" && dylib_fw_rc=0 || dylib_fw_rc=$?
[ "$dylib_fw_rc" -eq 1 ] && ok "dylib: --fatal-warnings refuses an unmatched op (EX_REFUSED)" \
    || bad "dylib: --fatal-warnings refusal" "expected exit 1, got $dylib_fw_rc: $(cat "$T/dylib_fw.err")"
grep -qF "/nope/absent-fw.dylib" "$T/dylib_fw.err" && ok "dylib: --fatal-warnings still names the op that matched nothing" \
    || bad "dylib: --fatal-warnings refusal message" "expected /nope/absent-fw.dylib on stderr, got: $(cat "$T/dylib_fw.err")"
dylib_fw_info=$("$MACHO9" info "$T/dylib_fw_fixture")
echo "$dylib_fw_info" | grep -qF "path=@loader_path/renamed-fw.dylib" \
    && ok "dylib: --fatal-warnings still wrote the file -- the op that DID match was applied despite the refusal" \
    || bad "dylib: --fatal-warnings file-still-written" "expected the matched -replace to have landed anyway, got: $dylib_fw_info"
# Without --fatal-warnings, the identical invocation still succeeds -- so the
# flag is what changed the answer, not something else about this fixture.
build_main "$T/dylib_fw_lax_fixture"
"$MACHO9" dylib "$T/dylib_fw_lax_fixture" \
        -replace "@loader_path/liba.dylib" "@loader_path/renamed-fw-lax.dylib" \
        -replace /nope/absent-fw.dylib /also/absent-fw.dylib \
        >/dev/null 2>/dev/null && dylib_fw_lax_rc=0 || dylib_fw_lax_rc=$?
[ "$dylib_fw_lax_rc" -eq 0 ] && ok "dylib: without --fatal-warnings the same unmatched op still succeeds" \
    || bad "dylib: no --fatal-warnings" "expected 0, got $dylib_fw_lax_rc"
# And when every op DOES match, --fatal-warnings must not refuse a run that
# had nothing to complain about.
build_main "$T/dylib_fw_ok_fixture"
"$MACHO9" dylib "$T/dylib_fw_ok_fixture" --fatal-warnings \
        -replace "@loader_path/liba.dylib" "@loader_path/renamed-fw-ok.dylib" \
        >"$T/dylib_fw_ok.out" 2>"$T/dylib_fw_ok.err" && dylib_fw_ok_rc=0 || dylib_fw_ok_rc=$?
[ "$dylib_fw_ok_rc" -eq 0 ] && ok "dylib: --fatal-warnings succeeds when nothing is unmatched" \
    || bad "dylib: --fatal-warnings (matched)" "expected 0, got $dylib_fw_ok_rc: $(cat "$T/dylib_fw_ok.err")"

# The brief's own canonical example: EVERY operation matches nothing, not
# just one of several. mr_process_thin's "nothing to change" early return
# never sets *out_modified in that case, so mr_apply_file never attempts
# the write at all -- there is nothing here for --fatal-warnings to have
# left in place. This is the assertion the mixed-op test above does NOT
# cover (there, one op DOES match, so the file legitimately changes): only
# an all-miss run proves the file is untouched, not merely unrolled-back.
build_main "$T/dylib_fw_allmiss_fixture"
cp "$T/dylib_fw_allmiss_fixture" "$T/dylib_fw_allmiss_before"
"$MACHO9" dylib "$T/dylib_fw_allmiss_fixture" --fatal-warnings \
        -replace /nope/absent-fw-allmiss.dylib /also/absent-fw-allmiss.dylib \
        >/dev/null 2>"$T/dylib_fw_allmiss.err" && dylib_fw_allmiss_rc=0 || dylib_fw_allmiss_rc=$?
[ "$dylib_fw_allmiss_rc" -eq 1 ] && ok "dylib: --fatal-warnings refuses when EVERY op matched nothing" \
    || bad "dylib: --fatal-warnings (all miss)" "expected exit 1, got $dylib_fw_allmiss_rc: $(cat "$T/dylib_fw_allmiss.err")"
cmp -s "$T/dylib_fw_allmiss_fixture" "$T/dylib_fw_allmiss_before" \
    && ok "dylib: --fatal-warnings left the file byte-for-byte untouched when nothing at all matched" \
    || bad "dylib: --fatal-warnings (all miss)" "the file was modified despite every operation matching nothing"

# segment and retag-swift take no list of operations that could miss, so
# neither parses --fatal-warnings at all -- passing it lands as an extra
# positional argument and is refused the same way any wrong argument count
# is, with each verb's own usage line, not a --fatal-warnings-specific
# message (there is nothing to be specific about: the flag was never seen).
build_main "$T/segment_fw_fixture"
"$MACHO9" segment "$T/segment_fw_fixture" __DATA __DATA_R9 --fatal-warnings \
    >/dev/null 2>"$T/segment_fw.err" \
    && bad "segment: --fatal-warnings" "should be refused (segment takes exactly FILE OLD NEW)" \
    || { grep -q "usage:" "$T/segment_fw.err" \
         && ok "segment: does not accept --fatal-warnings (refused as a usage error)" \
         || bad "segment: --fatal-warnings" "refused, but not with a usage message: $(cat "$T/segment_fw.err")"; }
"$MACHO9" retag-swift "$T/segment_fw_fixture" --fatal-warnings \
    >/dev/null 2>"$T/retag_fw.err" \
    && bad "retag-swift: --fatal-warnings" "should be refused (retag-swift takes exactly FILE)" \
    || { grep -q "usage:" "$T/retag_fw.err" \
         && ok "retag-swift: does not accept --fatal-warnings (refused as a usage error)" \
         || bad "retag-swift: --fatal-warnings" "refused, but not with a usage message: $(cat "$T/retag_fw.err")"; }

# ============================================================================
# dylib -append / -insert / -delete / -reexport
#
# -replace and --allow-grow (above) exercise only two of change_dylib's
# translation targets. The mapping itself -- macho9's flag to change_dylib's
# -- is the only new logic dylib/rpath add, so every op needs its own
# observable check, not just an exit code: a swapped mapping (say -append
# landing on change_dylib's -insert) would ship silently and INVERT dylib
# initialization order, which is the whole reason -insert exists (see
# docs/PROPOSAL.md "Why these names"). None of these dylibs need to exist on
# disk -- only the load-command rewrite is being checked here, via `macho9
# info`, never by running the binary.
# ============================================================================
spare="@loader_path/libspare.dylib"

# -append places the new dependency LAST -- after every existing one,
# INCLUDING the implicit libSystem.B.dylib the linker adds on its own, which
# is why this checks "highest ordinal in the file" rather than a hardcoded
# number (build_main's plain main.c still needs libSystem for _start/crt,
# so liba=1, libSystem=2, and spare correctly lands at 3, not 2).
build_main "$T/dylib_append_fixture"
before_append_info=$("$MACHO9" info "$T/dylib_append_fixture")
last_ordinal_before=$(echo "$before_append_info" | grep -o "ordinal=[0-9]*" | sed 's/ordinal=//' | sort -n | tail -1)
"$MACHO9" dylib "$T/dylib_append_fixture" -append "$spare" \
    >"$T/dylib_append.out" || bad "dylib: -append exit" "$(cat "$T/dylib_append.out")"
append_info=$("$MACHO9" info "$T/dylib_append_fixture")
expect_ordinal=$((last_ordinal_before + 1))
echo "$append_info" | grep -qF "ordinal=$expect_ordinal path=$spare" \
    && ok "dylib: -append put the new dep last (ordinal $expect_ordinal)" \
    || bad "dylib: -append" "expected ordinal=$expect_ordinal path=$spare in: $append_info"
echo "$append_info" | grep -qF "ordinal=1 path=@loader_path/liba.dylib" && ok "dylib: -append left liba at ordinal 1" \
    || bad "dylib: -append (liba)" "expected liba still at ordinal 1 in: $append_info"

# -insert places the new dependency FIRST (ordinal 1), pushing liba to 2 --
# the mapping that specifically must not become -append, since load order is
# dyld INITIALIZATION order (docs/PROPOSAL.md).
build_main "$T/dylib_insert_fixture"
"$MACHO9" dylib "$T/dylib_insert_fixture" -insert "$spare" \
    >"$T/dylib_insert.out" || bad "dylib: -insert exit" "$(cat "$T/dylib_insert.out")"
insert_info=$("$MACHO9" info "$T/dylib_insert_fixture")
echo "$insert_info" | grep -qF "ordinal=1 path=$spare" && ok "dylib: -insert put the new dep at ordinal 1 (first)" \
    || bad "dylib: -insert" "expected ordinal=1 path=$spare in: $insert_info"
echo "$insert_info" | grep -qF "ordinal=2 path=@loader_path/liba.dylib" && ok "dylib: -insert renumbered liba to ordinal 2" \
    || bad "dylib: -insert (liba)" "expected liba renumbered to ordinal 2 in: $insert_info"

# -delete removes the dependency and renumbers survivors; reuses the
# -append fixture above (liba=1, spare=2) so deleting the UNUSED spare
# (never called, so nothing binds to it -- change_dylib refuses a -delete
# that would orphan a bound symbol) proves removal without disturbing liba.
"$MACHO9" dylib "$T/dylib_append_fixture" -delete "$spare" \
    >"$T/dylib_delete.out" || bad "dylib: -delete exit" "$(cat "$T/dylib_delete.out")"
delete_info=$("$MACHO9" info "$T/dylib_append_fixture")
if echo "$delete_info" | grep -qF "path=$spare"; then
    bad "dylib: -delete" "spare still present in: $delete_info"
else
    ok "dylib: -delete removed the spare dependency"
fi
echo "$delete_info" | grep -qF "ordinal=1 path=@loader_path/liba.dylib" && ok "dylib: -delete left liba at ordinal 1" \
    || bad "dylib: -delete (liba)" "expected liba still at ordinal 1 in: $delete_info"

# -reexport promotes LC_LOAD_DYLIB -> LC_REEXPORT_DYLIB for an EXISTING
# dependency; check the load-command KIND changed, not just that the path
# is still there (it would be, for -replace too).
build_main "$T/dylib_reexport_fixture"
"$MACHO9" dylib "$T/dylib_reexport_fixture" -reexport "@loader_path/liba.dylib" \
    >"$T/dylib_reexport.out" || bad "dylib: -reexport exit" "$(cat "$T/dylib_reexport.out")"
reexport_info=$("$MACHO9" info "$T/dylib_reexport_fixture")
echo "$reexport_info" | grep -A1 "LC_REEXPORT_DYLIB" | grep -qF "path=@loader_path/liba.dylib" \
    && ok "dylib: -reexport promoted liba to LC_REEXPORT_DYLIB" \
    || bad "dylib: -reexport" "no LC_REEXPORT_DYLIB naming liba in: $reexport_info"

# ============================================================================
# rpath -append
# ============================================================================
build_main "$T/rpath_fixture" "/tmp/cli_test_original_rpath"
before_rp=$("$MACHO9" info "$T/rpath_fixture")
echo "$before_rp" | grep -q "rpath=/tmp/cli_test_original_rpath" && ok "rpath: fixture has original rpath" \
    || bad "rpath: precondition" "original rpath missing from info output"
"$MACHO9" rpath "$T/rpath_fixture" -append "/tmp/cli_test_appended_rpath" \
    >"$T/rpath.out" || bad "rpath: -append exit" "$(cat "$T/rpath.out")"
after_rp=$("$MACHO9" info "$T/rpath_fixture")
echo "$after_rp" | grep -q "rpath=/tmp/cli_test_original_rpath" && \
echo "$after_rp" | grep -q "rpath=/tmp/cli_test_appended_rpath" && \
    ok "rpath: -append kept the original and added the new one" || \
    bad "rpath: -append" "expected both rpaths in: $after_rp"

# -replace rewrites a search path in place; -delete removes one outright --
# neither was exercised above (only -append was), and each maps to a
# distinct change_dylib flag (-change-rpath / -delete-rpath) that a swapped
# mapping could silently confuse with the dylib family's -change/-delete.
build_main "$T/rpath_replace_fixture" "/tmp/cli_test_replace_before"
"$MACHO9" rpath "$T/rpath_replace_fixture" -replace "/tmp/cli_test_replace_before" "/tmp/cli_test_replace_after" \
    >"$T/rpath_replace.out" || bad "rpath: -replace exit" "$(cat "$T/rpath_replace.out")"
replace_info=$("$MACHO9" info "$T/rpath_replace_fixture")
if echo "$replace_info" | grep -q "rpath=/tmp/cli_test_replace_before"; then
    bad "rpath: -replace" "old rpath still present in: $replace_info"
else
    ok "rpath: -replace removed the old search path"
fi
echo "$replace_info" | grep -q "rpath=/tmp/cli_test_replace_after" && ok "rpath: -replace added the new search path" \
    || bad "rpath: -replace (new)" "new rpath not found in: $replace_info"

build_main "$T/rpath_delete_fixture" "/tmp/cli_test_delete_me"
"$MACHO9" rpath "$T/rpath_delete_fixture" -delete "/tmp/cli_test_delete_me" \
    >"$T/rpath_delete.out" || bad "rpath: -delete exit" "$(cat "$T/rpath_delete.out")"
delete_rp_info=$("$MACHO9" info "$T/rpath_delete_fixture")
if echo "$delete_rp_info" | grep -q "^  rpath="; then
    bad "rpath: -delete" "an rpath is still present in: $delete_rp_info"
else
    ok "rpath: -delete removed the search path"
fi

# rpath -replace naming a search path the file does not have: the rpath twin
# of the dylib -replace miss report above.
build_main "$T/rpath_miss_fixture" "/tmp/cli_test_rpath_present"
"$MACHO9" rpath "$T/rpath_miss_fixture" -replace "/tmp/cli_test_rpath_absent" "/tmp/cli_test_rpath_new" \
    >"$T/rpath_miss.out" 2>"$T/rpath_miss.err" && rpath_miss_rc=0 || rpath_miss_rc=$?
[ "$rpath_miss_rc" -eq 0 ] && ok "rpath: unmatched -replace still exits 0" \
    || bad "rpath: unmatched -replace" "expected 0, got $rpath_miss_rc: $(cat "$T/rpath_miss.err")"
grep -q "rpath /tmp/cli_test_rpath_absent matched nothing" "$T/rpath_miss.err" \
    && ok "rpath: names the -replace that matched nothing" \
    || bad "rpath: unmatched -replace" "expected 'rpath /tmp/cli_test_rpath_absent matched nothing' on stderr, got: $(cat "$T/rpath_miss.err")"
rpath_miss_info=$("$MACHO9" info "$T/rpath_miss_fixture")
echo "$rpath_miss_info" | grep -q "rpath=/tmp/cli_test_rpath_present" \
    && ok "rpath: an untouched rpath is left alone by the unmatched -replace" \
    || bad "rpath: unmatched -replace" "the ORIGINAL rpath disappeared: $rpath_miss_info"

# rpath --fatal-warnings: the same miss report just above, turned into a
# refusal, the rpath twin of the dylib --fatal-warnings block above.
build_main "$T/rpath_fw_fixture" "/tmp/cli_test_rpath_fw_present"
"$MACHO9" rpath "$T/rpath_fw_fixture" --fatal-warnings \
        -replace "/tmp/cli_test_rpath_fw_absent" "/tmp/cli_test_rpath_fw_new" \
        >/dev/null 2>"$T/rpath_fw.err" && rpath_fw_rc=0 || rpath_fw_rc=$?
[ "$rpath_fw_rc" -eq 1 ] && ok "rpath: --fatal-warnings refuses an unmatched op (EX_REFUSED)" \
    || bad "rpath: --fatal-warnings refusal" "expected exit 1, got $rpath_fw_rc: $(cat "$T/rpath_fw.err")"
grep -q "rpath /tmp/cli_test_rpath_fw_absent matched nothing" "$T/rpath_fw.err" \
    && ok "rpath: --fatal-warnings still names the op that matched nothing" \
    || bad "rpath: --fatal-warnings refusal message" "expected the miss message on stderr, got: $(cat "$T/rpath_fw.err")"
# Without --fatal-warnings, the identical invocation still succeeds.
build_main "$T/rpath_fw_lax_fixture" "/tmp/cli_test_rpath_fw_lax_present"
"$MACHO9" rpath "$T/rpath_fw_lax_fixture" \
        -replace "/tmp/cli_test_rpath_fw_absent" "/tmp/cli_test_rpath_fw_new" \
        >/dev/null 2>/dev/null && rpath_fw_lax_rc=0 || rpath_fw_lax_rc=$?
[ "$rpath_fw_lax_rc" -eq 0 ] && ok "rpath: without --fatal-warnings the same unmatched op still succeeds" \
    || bad "rpath: no --fatal-warnings" "expected 0, got $rpath_fw_lax_rc"
# And when the op DOES match, --fatal-warnings must not refuse.
build_main "$T/rpath_fw_ok_fixture" "/tmp/cli_test_rpath_fw_ok_present"
"$MACHO9" rpath "$T/rpath_fw_ok_fixture" --fatal-warnings \
        -replace "/tmp/cli_test_rpath_fw_ok_present" "/tmp/cli_test_rpath_fw_ok_new" \
        >"$T/rpath_fw_ok.out" 2>"$T/rpath_fw_ok.err" && rpath_fw_ok_rc=0 || rpath_fw_ok_rc=$?
[ "$rpath_fw_ok_rc" -eq 0 ] && ok "rpath: --fatal-warnings succeeds when nothing is unmatched" \
    || bad "rpath: --fatal-warnings (matched)" "expected 0, got $rpath_fw_ok_rc: $(cat "$T/rpath_fw_ok.err")"

# ============================================================================
# rpath -insert: the search path lands FIRST, not last
# ============================================================================
# THE ONLY THING WORTH ASSERTING HERE IS ORDER. dyld takes the first rpath
# that resolves, so an -insert whose result merely CONTAINS the new path is
# indistinguishable from an -append that silently stood in for it -- which is
# exactly the wrong answer this operation exists to rule out
# (docs/PROPOSAL.md: "flipping their order flips which one loads"). Every
# assertion below therefore compares POSITIONS in `macho9 info`'s rpath list,
# never mere presence.
#
# `grep -n` over info's own stable "  rpath=" lines gives those positions
# without parsing otool.
rpath_positions() { "$MACHO9" info "$1" | grep -n "^  rpath=" | sed 's/:.*rpath=/ /'; }
rpath_first() { rpath_positions "$1" | head -1 | sed 's/^[0-9]* //'; }
rpath_last()  { rpath_positions "$1" | tail -1 | sed 's/^[0-9]* //'; }

# Two rpaths baked in at link time, so "first" is a real position among
# several rather than the only one there is.
"$CC" -O2 $FIXTURE_FLAGS \
    -Xlinker -rpath -Xlinker "/tmp/cli_test_ins_existing_one" \
    -Xlinker -rpath -Xlinker "/tmp/cli_test_ins_existing_two" \
    "$T/main.c" "$T/liba.dylib" -o "$T/rpath_insert_fixture"
[ "$(rpath_first "$T/rpath_insert_fixture")" = "/tmp/cli_test_ins_existing_one" ] \
    && ok "rpath -insert: fixture starts with the linker's first rpath" \
    || bad "rpath -insert: precondition" "expected /tmp/cli_test_ins_existing_one first, got: $(rpath_positions "$T/rpath_insert_fixture")"

"$MACHO9" rpath "$T/rpath_insert_fixture" -insert "/tmp/cli_test_inserted_rpath" \
    >"$T/rpath_insert.out" 2>&1 || bad "rpath: -insert exit" "$(cat "$T/rpath_insert.out")"
[ "$(rpath_first "$T/rpath_insert_fixture")" = "/tmp/cli_test_inserted_rpath" ] \
    && ok "rpath: -insert put the new search path FIRST" \
    || bad "rpath: -insert" "inserted path is not first: $(rpath_positions "$T/rpath_insert_fixture")"
# ...and did not eat either existing one, in either order.
ins_all=$(rpath_positions "$T/rpath_insert_fixture" | sed 's/^[0-9]* //' | tr '\n' ' ')
[ "$ins_all" = "/tmp/cli_test_inserted_rpath /tmp/cli_test_ins_existing_one /tmp/cli_test_ins_existing_two " ] \
    && ok "rpath: -insert kept both existing search paths, in their original order, behind it" \
    || bad "rpath: -insert order" "unexpected rpath order: $ins_all"

# THE ASSERTION THAT MAKES THE ONE ABOVE MEAN SOMETHING: -append on the SAME
# fixture must put its path LAST. If -insert were quietly implemented as
# -append, this pair could not both hold -- and the "first" assertion alone
# would pass against a fixture whose new path happened to sort first.
"$CC" -O2 $FIXTURE_FLAGS \
    -Xlinker -rpath -Xlinker "/tmp/cli_test_ins_existing_one" \
    -Xlinker -rpath -Xlinker "/tmp/cli_test_ins_existing_two" \
    "$T/main.c" "$T/liba.dylib" -o "$T/rpath_append_cmp_fixture"
"$MACHO9" rpath "$T/rpath_append_cmp_fixture" -append "/tmp/cli_test_inserted_rpath" \
    >"$T/rpath_append_cmp.out" 2>&1 || bad "rpath: -append (comparison) exit" "$(cat "$T/rpath_append_cmp.out")"
[ "$(rpath_last "$T/rpath_append_cmp_fixture")" = "/tmp/cli_test_inserted_rpath" ] \
    && [ "$(rpath_first "$T/rpath_append_cmp_fixture")" = "/tmp/cli_test_ins_existing_one" ] \
    && ok "rpath: -append put the very same path LAST -- so -insert is not -append in disguise" \
    || bad "rpath: -append vs -insert" "append did not land last: $(rpath_positions "$T/rpath_append_cmp_fixture")"

# An image with NO existing rpath still honours -insert; there is simply
# nothing to be in front of. Combined with an -append in the same run, the
# inserted one must still come out first -- the case where a naive
# implementation that emits inserts after appends gets it backwards.
#
# Built with an explicit -headerpad MINIMUM rather than through build_main,
# because the default pad is a property of the LINKER, not of this test: 10.9's
# leaves ~3.1KB, while the modern cross runner's leaves 56 bytes, and this case
# adds two whole LC_RPATHs where the rest of the rpath cases only rewrite
# existing ones. It failed on the cross runner alone for exactly that reason
# ("new LCs (1432 bytes) don't fit in header pad (56 avail)"), which is
# tests/README.md's host-portability lesson arriving in a new place.
#
# -headerpad sets a FLOOR, so this is a no-op wherever the default already
# exceeds it -- measured on this 10.9 host: 0x800 changed nothing, 0x2000 moved
# the pad from 3128 to 11320. The value is deliberately well clear of what two
# rpaths need. It must NOT go into FIXTURE_FLAGS: the --allow-grow case below
# depends on a 3500-character path overflowing whatever pad the linker left, so
# padding every fixture would silently disarm that refusal.
"$CC" -O2 $FIXTURE_FLAGS -Wl,-headerpad,0x2000 \
    "$T/main.c" "$T/liba.dylib" -o "$T/rpath_insert_empty"
"$MACHO9" info "$T/rpath_insert_empty" | grep -q "^  rpath=" \
    && bad "rpath -insert: empty precondition" "fixture unexpectedly already has an rpath" \
    || ok "rpath -insert: empty-case fixture has no rpath to start with"
"$MACHO9" rpath "$T/rpath_insert_empty" -insert "/tmp/cli_test_empty_ins" -append "/tmp/cli_test_empty_app" \
    >"$T/rpath_insert_empty.out" 2>&1 || bad "rpath: -insert (no existing) exit" "$(cat "$T/rpath_insert_empty.out")"
empty_all=$(rpath_positions "$T/rpath_insert_empty" | sed 's/^[0-9]* //' | tr '\n' ' ')
[ "$empty_all" = "/tmp/cli_test_empty_ins /tmp/cli_test_empty_app " ] \
    && ok "rpath: -insert lands before -append even in an image that had no rpaths" \
    || bad "rpath: -insert (no existing)" "unexpected order: $empty_all"

# The rewritten binary still runs: an LC_RPATH inserted in the wrong place, or
# one that desynchronized the load-command table, shows up here as a dyld
# failure rather than as a passing byte comparison.
if [ "$signing_enforced" -eq 1 ]; then
    skip "rpath: -insert result still runs" \
        "this host SIGKILLs any binary modified since it was signed at link time (established independently of macho9 by the host probe above)"
elif (cd "$T" && ./rpath_insert_fixture) >"$T/rpath_insert_run.out" 2>&1; then
    ok "rpath: -insert result still runs"
else
    bad "rpath: -insert result" "the binary no longer runs: $(cat "$T/rpath_insert_run.out")"
fi

# ============================================================================
# segment: rename every matching LC_SEGMENT_64, and its sections' copy
# ============================================================================
# `macho9 info` prints a segment's segname but NOT the copy of that name each
# section_64 carries, and the section copies are half of what this verb must
# change (getsectiondata matches on the section's copy -- see
# src/segname.h's header comment). segread below is a purpose-built
# reader for exactly that, in the same spirit as change_dylib_test.sh's
# ordinal_of/fatcheck: nothing here parses otool.
#
# It doubles as the fat-container half of this section. Building the fat file
# by hand rather than with lipo is change_dylib_test.sh's rule and its reason:
# what lipo will accept is not this suite's to pin. Slice 0 is the real
# 64-bit binary; slice 1 is a non-Mach-O blob that mr_apply_file must pass
# through byte for byte -- which is also the "only SOME slices match" case.
cat > "$T/segread.c" <<'EOF'
/* segread <mode> <file> [args]
 *
 *   segs  FILE          print "SEG <segname>" and "SECT <segname>/<sectname>"
 *                       for every LC_SEGMENT_64, in load order
 *   wrap  OUT THIN BLOB [CT1]
 *                       build a classic (32-bit fat_arch) fat container:
 *                       slice 0 = THIN, slice 1 = BLOB. CT1 is slice 1's
 *                       cputype and defaults to 7 (CPU_TYPE_X86). It does
 *                       NOT decide whether the rewriter skips the slice --
 *                       the slice's own bytes do -- it decides the
 *                       "arch N (cputype 0x...)" label the refusal names,
 *                       so pass 16777223 (CPU_TYPE_X86_64) when BLOB really
 *                       is a 64-bit Mach-O and the label should say so.
 *   dump  FILE IDX OUT  write fat slice IDX to OUT
 *
 * Names are char[16] and need not be NUL-terminated; printed with %.16s and
 * compared nowhere, so a 16-byte name comes out whole. Big-endian fat header
 * fields are written/read by hand -- FAT_MAGIC on disk is always big-endian.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <mach-o/loader.h>
#include <mach-o/fat.h>

static uint32_t be32(uint32_t v) {
    return ((v & 0xffu) << 24) | ((v & 0xff00u) << 8) |
           ((v & 0xff0000u) >> 8) | ((v >> 24) & 0xffu);
}

static uint8_t *slurp(const char *path, size_t *n) {
    FILE *f = fopen(path, "rb");
    if (!f) { perror(path); exit(2); }
    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t *b = malloc((size_t)sz);
    if (!b || fread(b, 1, (size_t)sz, f) != (size_t)sz) { fprintf(stderr, "read %s\n", path); exit(2); }
    fclose(f);
    *n = (size_t)sz;
    return b;
}

static void spit(const char *path, const uint8_t *b, size_t n) {
    FILE *f = fopen(path, "wb");
    if (!f) { perror(path); exit(2); }
    if (fwrite(b, 1, n, f) != n) { perror("fwrite"); exit(2); }
    fclose(f);
}

int main(int argc, char **argv) {
    if (argc < 3) { fprintf(stderr, "usage: segread segs|wrap|dump ...\n"); return 2; }

    if (strcmp(argv[1], "segs") == 0) {
        size_t n; uint8_t *b = slurp(argv[2], &n);
        const struct mach_header_64 *h = (const struct mach_header_64 *)b;
        if (n < sizeof *h || h->magic != MH_MAGIC_64) { fprintf(stderr, "not a thin 64-bit Mach-O\n"); return 2; }
        const uint8_t *p = b + sizeof *h;
        for (uint32_t i = 0; i < h->ncmds; i++) {
            const struct load_command *lc = (const struct load_command *)p;
            if (lc->cmd == LC_SEGMENT_64) {
                const struct segment_command_64 *sg = (const struct segment_command_64 *)lc;
                printf("SEG %.16s\n", sg->segname);
                const struct section_64 *sc = (const struct section_64 *)(sg + 1);
                for (uint32_t k = 0; k < sg->nsects; k++)
                    printf("SECT %.16s/%.16s\n", sc[k].segname, sc[k].sectname);
            }
            p += lc->cmdsize;
        }
        return 0;
    }

    if (strcmp(argv[1], "wrap") == 0) {
        if (argc != 5 && argc != 6) { fprintf(stderr, "usage: segread wrap OUT THIN BLOB [CT1]\n"); return 2; }
        uint32_t ct1 = (argc == 6) ? (uint32_t)strtoul(argv[5], NULL, 0) : 7u;
        size_t tn, bn;
        uint8_t *tb = slurp(argv[3], &tn), *bb = slurp(argv[4], &bn);
        const struct mach_header_64 *h = (const struct mach_header_64 *)tb;
        if (tn < sizeof *h || h->magic != MH_MAGIC_64) { fprintf(stderr, "slice 0 is not a thin 64-bit Mach-O\n"); return 2; }
        uint32_t align = 12;                       /* 4096, what real fat files use */
        uint32_t hdrlen = (uint32_t)(sizeof(struct fat_header) + 2 * sizeof(struct fat_arch));
        uint32_t off0 = (hdrlen + 4095u) & ~4095u;
        uint32_t off1 = (uint32_t)((off0 + tn + 4095u) & ~4095u);
        size_t total = off1 + bn;
        uint8_t *out = calloc(1, total);
        struct fat_header *fh = (struct fat_header *)out;
        fh->magic = be32(FAT_MAGIC);
        fh->nfat_arch = be32(2);
        struct fat_arch *ar = (struct fat_arch *)(out + sizeof *fh);
        ar[0].cputype = be32((uint32_t)h->cputype);
        ar[0].cpusubtype = be32((uint32_t)h->cpusubtype);
        ar[0].offset = be32(off0); ar[0].size = be32((uint32_t)tn); ar[0].align = be32(align);
        ar[1].cputype = be32(ct1); /* default CPU_TYPE_X86: a slice this rewriter skips */
        ar[1].cpusubtype = be32(3);
        ar[1].offset = be32(off1); ar[1].size = be32((uint32_t)bn); ar[1].align = be32(align);
        memcpy(out + off0, tb, tn);
        memcpy(out + off1, bb, bn);
        spit(argv[2], out, total);
        return 0;
    }

    if (strcmp(argv[1], "dump") == 0) {
        if (argc != 5) { fprintf(stderr, "usage: segread dump FILE IDX OUT\n"); return 2; }
        size_t n; uint8_t *b = slurp(argv[2], &n);
        const struct fat_header *fh = (const struct fat_header *)b;
        if (n < sizeof *fh || be32(fh->magic) != FAT_MAGIC) { fprintf(stderr, "not a fat file\n"); return 2; }
        uint32_t narch = be32(fh->nfat_arch);
        uint32_t idx = (uint32_t)strtoul(argv[3], NULL, 10);
        if (idx >= narch) { fprintf(stderr, "slice %u of %u\n", idx, narch); return 2; }
        const struct fat_arch *ar = (const struct fat_arch *)(b + sizeof *fh);
        uint32_t off = be32(ar[idx].offset), sz = be32(ar[idx].size);
        if ((size_t)off + sz > n) { fprintf(stderr, "slice out of bounds\n"); return 2; }
        spit(argv[4], b + off, sz);
        return 0;
    }

    fprintf(stderr, "unknown mode: %s\n", argv[1]);
    return 2;
}
EOF
"$CC" -O2 -o "$T/segread" "$T/segread.c"

# A fixture with a real __DATA segment whose sections therefore carry
# "__DATA" in their own segname fields. It is built to have MORE THAN ONE
# such section -- an initialised global (__data), a zero-initialised one
# (__bss/__common) and a call through libSystem (__la_symbol_ptr) -- because
# the rename has to walk every section of the matched segment, and a
# single-section fixture cannot tell "renames the sections" from "renames the
# first section".
cat > "$T/segmain.c" <<'EOF'
#include <string.h>
int g_counter = 7;
int g_zero;
char g_buf[64];
int main(void) {
    g_counter++;
    memset(g_buf, 'x', sizeof g_buf);
    g_zero = g_buf[0] == 'x';
    return (g_counter == 8 && g_zero) ? 0 : 1;
}
EOF
"$CC" -O2 $FIXTURE_FLAGS "$T/segmain.c" -o "$T/segment_fixture"
"$T/segread" segs "$T/segment_fixture" > "$T/segs_before"
grep -q "^SEG __DATA$" "$T/segs_before" && grep -q "^SECT __DATA/" "$T/segs_before" \
    && ok "segment: fixture has a __DATA segment whose sections name it" \
    || bad "segment: precondition" "no __DATA segment/section in: $(cat "$T/segs_before")"

"$MACHO9" segment "$T/segment_fixture" __DATA __DATA_R9 \
    >"$T/segment.out" 2>&1 || bad "segment: exit" "$(cat "$T/segment.out")"
"$T/segread" segs "$T/segment_fixture" > "$T/segs_after"
grep -q "^SEG __DATA$" "$T/segs_after" \
    && bad "segment: the segment itself" "a segment is still named __DATA: $(cat "$T/segs_after")" \
    || ok "segment: renamed the LC_SEGMENT_64 itself"
grep -q "^SEG __DATA_R9$" "$T/segs_after" \
    && ok "segment: the new name is what landed" \
    || bad "segment: new name" "no __DATA_R9 segment in: $(cat "$T/segs_after")"
# The half `macho9 info` cannot see: every section's own copy of the name.
if grep -q "^SECT __DATA/" "$T/segs_after"; then
    bad "segment: section segnames" "a section still names __DATA: $(cat "$T/segs_after")"
else
    ok "segment: renamed each section's copy of the segment name too"
fi
# `|| true`: grep -c exits 1 when the count is 0, and a bare command exiting
# nonzero under this script's `set -e` would kill the whole suite (see the
# reached_end guard at the top). 0 is a legitimate -- indeed the interesting
# -- answer here, so it must reach the comparison rather than the trap.
nsect_before=$(grep -c "^SECT __DATA/" "$T/segs_before" || true)
nsect_after=$(grep -c "^SECT __DATA_R9/" "$T/segs_after" || true)
[ "$nsect_before" -gt 0 ] && [ "$nsect_before" -eq "$nsect_after" ] \
    && ok "segment: all $nsect_before section(s) moved to the new name, none lost" \
    || bad "segment: section count" "$nsect_before sections named __DATA before, $nsect_after named __DATA_R9 after"
# Nothing else moved: __TEXT and its sections are untouched.
grep -q "^SEG __TEXT$" "$T/segs_after" && grep -q "^SECT __TEXT/__text$" "$T/segs_after" \
    && ok "segment: left every non-matching segment alone" \
    || bad "segment: collateral" "__TEXT changed: $(cat "$T/segs_after")"
if [ "$signing_enforced" -eq 1 ]; then
    skip "segment: the renamed binary still runs" \
        "this host SIGKILLs any binary modified since it was signed at link time (established independently of macho9 by the host probe above)"
elif (cd "$T" && ./segment_fixture) >"$T/segment_run.out" 2>&1; then
    ok "segment: the renamed binary still runs"
else
    bad "segment: result" "the renamed binary no longer runs: $(cat "$T/segment_run.out")"
fi

# A NEW name of exactly 16 bytes fills the field with no room for a
# terminator -- the boundary rename_segment has always accepted, and the one a
# strcpy-based implementation gets wrong by writing a 17th byte.
"$CC" -O2 $FIXTURE_FLAGS "$T/segmain.c" -o "$T/segment_16_fixture"
"$MACHO9" segment "$T/segment_16_fixture" __DATA ABCDEFGHIJKLMNOP \
    >"$T/segment16.out" 2>&1 || bad "segment: 16-byte name exit" "$(cat "$T/segment16.out")"
"$T/segread" segs "$T/segment_16_fixture" | grep -q "^SEG ABCDEFGHIJKLMNOP$" \
    && ok "segment: accepts a NEW name of exactly 16 bytes and writes it whole" \
    || bad "segment: 16-byte name" "got: $("$T/segread" segs "$T/segment_16_fixture")"
# 17 is one too many, and must be refused before any I/O.
"$CC" -O2 $FIXTURE_FLAGS "$T/segmain.c" -o "$T/segment_17_fixture"
cp "$T/segment_17_fixture" "$T/segment_17_before"
rc=0
"$MACHO9" segment "$T/segment_17_fixture" __DATA ABCDEFGHIJKLMNOPQ \
    >"$T/segment17.out" 2>&1 || rc=$?
[ "$rc" -eq 1 ] && ok "segment: refuses a 17-byte NEW name with the documented refusal code" \
    || bad "segment: 17-byte name" "expected exit 1, got $rc: $(cat "$T/segment17.out")"
cmp -s "$T/segment_17_fixture" "$T/segment_17_before" \
    && ok "segment: a refused rename left the file byte-for-byte unchanged" \
    || bad "segment: 17-byte name" "the file was modified despite the refusal"

# A segment name nothing matches must leave the file alone -- and say so.
"$CC" -O2 $FIXTURE_FLAGS "$T/segmain.c" -o "$T/segment_nomatch_fixture"
cp "$T/segment_nomatch_fixture" "$T/segment_nomatch_before"
"$MACHO9" segment "$T/segment_nomatch_fixture" __NOSUCHSEG __OTHER \
    >"$T/segment_nomatch.out" 2>&1 || bad "segment: no-match exit" "$(cat "$T/segment_nomatch.out")"
cmp -s "$T/segment_nomatch_fixture" "$T/segment_nomatch_before" \
    && ok "segment: a rename that matched nothing did not touch the file" \
    || bad "segment: no-match" "the file changed although no segment matched"

# --- segment on a FAT container --------------------------------------------
# The case the retirement plan singles out: fix_macho's -rename_seg is
# fat-capable and folds into this verb, so this verb has to be too. The
# non-Mach-O second slice must come back byte for byte.
"$CC" -O2 $FIXTURE_FLAGS "$T/segmain.c" -o "$T/segment_fat_slice"
printf 'not a mach-o at all, just bytes to be preserved verbatim.\n' > "$T/segment_fat_blob"
"$T/segread" wrap "$T/segment_fat" "$T/segment_fat_slice" "$T/segment_fat_blob"
"$MACHO9" segment "$T/segment_fat" __DATA __DATA_R9 \
    >"$T/segment_fat.out" 2>&1 || bad "segment: fat exit" "$(cat "$T/segment_fat.out")"
"$T/segread" dump "$T/segment_fat" 0 "$T/segment_fat_slice0"
"$T/segread" segs "$T/segment_fat_slice0" > "$T/segs_fat"
grep -q "^SEG __DATA_R9$" "$T/segs_fat" && ! grep -q "^SEG __DATA$" "$T/segs_fat" \
    && ok "segment: renamed the 64-bit slice of a fat container" \
    || bad "segment: fat slice 0" "expected __DATA_R9 and no __DATA in: $(cat "$T/segs_fat")"
grep -q "^SECT __DATA/" "$T/segs_fat" \
    && bad "segment: fat slice 0 sections" "a section still names __DATA: $(cat "$T/segs_fat")" \
    || ok "segment: renamed the fat slice's section segnames too"
"$T/segread" dump "$T/segment_fat" 1 "$T/segment_fat_blob_after"
cmp -s "$T/segment_fat_blob" "$T/segment_fat_blob_after" \
    && ok "segment: passed the non-Mach-O fat slice through byte for byte" \
    || bad "segment: fat slice 1" "the slice this rewriter cannot read was modified"

# ---- segment does NOT meet the mg_plausible gate ---------------------------
#
# mr_apply_file's last gate before writing (src/rewrite.c) asks whether the
# image's initializers and compact-unwind entries still name functions
# LC_FUNCTION_STARTS knows about. That is an OFFSET question, and a segment
# rename moves no offset -- it writes characters into segname/sectname fields.
# So mr_process_thin skips the gate for a rename-only operation set, and these
# are the assertions that it really does, and that it still runs for everything
# else.
#
# The input is tests/mkimplausible.c's committed, hand-built fixture, not a
# scan of /usr/lib. An earlier version did scan for a dylib the gate refused,
# and SKIPped when it found nothing -- which passes on 10.9 and covers nothing
# on the cross runner, where those dylibs live only in the shared cache. Those
# refusals were also not the heuristic getting real dylibs wrong: it never ran
# on them (see mkimplausible.c's header), so the scan would come up empty
# today. The fixture trips the gate on its merits; its header says how.
"$CC" -O2 -Wall -Wextra -I "$SRC_DIR" -o "$T/mkimplausible" "$SRC_DIR/../tests/mkimplausible.c"
"$T/mkimplausible" "$T/implausible"

# `|| true`: a refusal is the expected outcome and this suite runs under set -e.
"$MACHO9" verify "$T/implausible" >/dev/null 2>"$T/imp_verify.err" || true
grep -q 'implausible' "$T/imp_verify.err" \
    && ok "segment: the fixture really is one mg_plausible rejects" \
    || bad "segment: mg_plausible fixture" "macho9 verify did not call it implausible: $(cat "$T/imp_verify.err")"

# An ordinary operation on it still meets the gate and is refused, with the
# input left alone -- so the skip below is narrow, not a hole.
cp "$T/implausible" "$T/imp_lc"
imp_before=$(shasum -a 256 < "$T/imp_lc" | cut -d' ' -f1)
if "$MACHO9" lc "$T/imp_lc" -delete uuid >/dev/null 2>"$T/imp_lc.err"; then
    bad "segment: mg_plausible scope" "lc -delete uuid was NOT refused, so the gate is gone"
else
    grep -q 'no known function' "$T/imp_lc.err" \
        && ok "segment: an operation that CAN move an offset still meets the gate" \
        || bad "segment: mg_plausible scope" "lc -delete refused for another reason: $(cat "$T/imp_lc.err")"
fi
[ "$(shasum -a 256 < "$T/imp_lc" | cut -d' ' -f1)" = "$imp_before" ] \
    && ok "segment: that refusal left the input untouched" \
    || bad "segment: mg_plausible scope" "the refused input was modified"

# ---- an EMPTY LC_FUNCTION_STARTS is "nothing to check", not a refusal ------
#
# The same fixture with three bytes changed: its 8-byte LC_FUNCTION_STARTS
# blob is all zeros, so it declares no function starts. That is the shape a
# dylib with NO CODE has -- a stub written to satisfy a link is the everyday
# example; the one instance on this host is libswiftObjectiveC.dylib, which
# is neither stock 10.9 nor a shipped product (tests/mkimplausible.c has the
# measured provenance note) -- and it is the same fact about an image as
# carrying no LC_FUNCTION_STARTS at all, which mg_plausible has always
# accepted. It folded the two apart for a while -- ns == 0 fell into a
# composite `ns <= 0 ||` refusal -- which refused
# that dylib with a contentless `FAILED (see above)` from verify and, through
# the rewrite path, with a message about base-relative offsets naming no known
# function when the image had no function starts for anything to name.
"$T/mkimplausible" "$T/emptystarts" -empty-starts
if "$MACHO9" verify "$T/emptystarts" >"$T/es_verify.out" 2>&1; then
    ok "verify: an image declaring no function starts is accepted"
else
    bad "verify: empty LC_FUNCTION_STARTS" \
        "refused an image with nothing to check against: $(cat "$T/es_verify.out")"
fi
grep -q ': OK' "$T/es_verify.out" && ok "verify: and says OK about it" \
    || bad "verify: empty LC_FUNCTION_STARTS" "no OK verdict: $(cat "$T/es_verify.out")"

# ...and it is rewritable, which is the half the rewrite path got wrong: the
# gate sits in mr_process_thin, so a refusal here refused the operation too.
cp "$T/emptystarts" "$T/es_lc"
if "$MACHO9" lc "$T/es_lc" -delete uuid >/dev/null 2>"$T/es_lc.err"; then
    ok "lc -delete: an image declaring no function starts is rewritable"
else
    bad "lc -delete: empty LC_FUNCTION_STARTS" "refused: $(cat "$T/es_lc.err")"
fi

# ...while its twin, differing only in those three bytes, is still refused --
# so the acceptance above is about declaring no function starts, not about the
# gate having stopped asking.
cmp -s "$T/implausible" "$T/emptystarts" \
    && bad "verify: empty LC_FUNCTION_STARTS" "the two fixtures are identical; the flag did nothing" \
    || ok "verify: the accepted and refused fixtures really are different files"

# ...and a rename of the very same file goes through, and really renames.
cp "$T/implausible" "$T/imp_seg"
if "$MACHO9" segment "$T/imp_seg" __DATA __DATA_R9 >/dev/null 2>"$T/imp_seg.err"; then
    "$T/segread" segs "$T/imp_seg" > "$T/imp_segs"
    grep -q "^SEG __DATA_R9$" "$T/imp_segs" && ! grep -q "^SEG __DATA$" "$T/imp_segs" \
        && ok "segment: renames a binary mg_plausible rejects for other operations" \
        || bad "segment: mg_plausible scope" "exited 0 but did not rename: $(cat "$T/imp_segs")"
    grep -q "^SECT __DATA/" "$T/imp_segs" \
        && bad "segment: mg_plausible scope" "a section still names __DATA" \
        || ok "segment: and renames that binary's section segnames too"
else
    bad "segment: mg_plausible scope" "refused the fixture: $(cat "$T/imp_seg.err")"
fi

# ---- MR_ERROR: one bad slice refuses the WHOLE fat file --------------------
#
# mr_process_fat (src/rewrite.c) splits per-slice failure in two: MR_SKIP for
# a slice that is not a 64-bit Mach-O -- left alone, other slices still
# rewritten, exit 0 -- and MR_ERROR for a slice that IS one and whose edit was
# refused, which aborts the whole file. Only MR_ERROR is a divergence from the
# tool this replaced (compat/fix_macho.sh's divergence 4, where it is stated
# most emphatically: fix_macho printed "Skipping arch %u" for BOTH and exited
# 0, having shipped a partially converted universal binary as a success).
#
# The asymmetry used to run the wrong way: MR_SKIP had an assertion (through
# the fat wrap just above and through fix_macho in wrapper_test.sh) and
# MR_ERROR had none, because building a hermetic bad slice looked like it
# needed a scan of the host. It does not: it needs a slice that IS a 64-bit
# Mach-O and whose edit mg_plausible refuses, which is exactly what
# tests/mkimplausible.c already builds, wrapped at CPU_TYPE_X86_64 so
# mr_process_thin reaches it instead of skipping it.
"$T/segread" wrap "$T/mrerr_fat" "$T/segment_fat_slice" "$T/implausible" 16777223
mrerr_before=$(shasum -a 256 < "$T/mrerr_fat" | cut -d' ' -f1)
if "$MACHO9" lc "$T/mrerr_fat" -delete uuid >"$T/mrerr.out" 2>"$T/mrerr.err"; then
    bad "lc: MR_ERROR fat slice" "exited 0; a partial rewrite was reported as success"
else
    ok "lc: a fat slice whose edit is refused refuses the whole file (nonzero exit)"
fi
grep -q 'refusing the whole fat file -- a partial rewrite would leave its slices inconsistent' \
    "$T/mrerr.err" \
    && ok "lc: and says so, naming the partial-rewrite reason" \
    || bad "lc: MR_ERROR message" "expected mr_process_fat's refusal, got: $(cat "$T/mrerr.err")"
grep -q 'arch 1 (cputype 0x1000007)' "$T/mrerr.err" \
    && ok "lc: and names which slice it was" \
    || bad "lc: MR_ERROR slice label" "expected 'arch 1 (cputype 0x1000007)', got: $(cat "$T/mrerr.err")"
grep -q 'no known function' "$T/mrerr.err" \
    && ok "lc: and the underlying per-slice refusal is still on stderr too" \
    || bad "lc: MR_ERROR per-slice reason" "the slice's own refusal was swallowed: $(cat "$T/mrerr.err")"
# The whole point of refusing: slice 0 WAS editable, so an abort that wrote
# anything would leave exactly the inconsistent file the message names.
[ "$(shasum -a 256 < "$T/mrerr_fat" | cut -d' ' -f1)" = "$mrerr_before" ] \
    && ok "lc: and left the fat file byte-for-byte unchanged, slice 0 included" \
    || bad "lc: MR_ERROR atomicity" "the refused fat file was modified"
# MEASURED WHILE WRITING THIS, and worth pinning: the fat table's cputype is
# NOT what decides MR_SKIP vs MR_ERROR. Wrap the very same bad slice at
# CPU_TYPE_X86 and it is still MR_ERROR -- mr_process_thin asks the slice's
# own bytes whether they are a 64-bit Mach-O, and the cputype only supplies
# the "arch N (cputype 0x...)" label. So the MR_SKIP assertions (the fat wrap
# above, and fix_macho's in wrapper_test.sh) cover a slice that really is not
# a Mach-O, which is the only thing that reaches that path.
"$T/segread" wrap "$T/mrskip_fat" "$T/segment_fat_slice" "$T/implausible" 7
if "$MACHO9" lc "$T/mrskip_fat" -delete uuid >"$T/mrskip.out" 2>"$T/mrskip.err"; then
    bad "lc: MR_SKIP/MR_ERROR split" "a Mach-O slice at cputype 0x7 was skipped, not refused"
else
    grep -q 'arch 1 (cputype 0x7): refusing the whole fat file' "$T/mrskip.err" \
        && ok "lc: the slice's own bytes decide MR_ERROR, not the fat table's cputype" \
        || bad "lc: MR_SKIP/MR_ERROR split" "refused for another reason: $(cat "$T/mrskip.err")"
fi

# ============================================================================
# retag-swift: the is-Swift tag moves from the stable-ABI bit to the legacy one
# ============================================================================
# The observable is the two low bits of each class record's data word, so the
# fixture is a hand-built Mach-O with a known layout and the reader is the
# same program that wrote it -- the leaf-tool-crashes.sh pattern. Nothing on
# this host can emit a real Swift binary (10.9 predates Swift entirely), and a
# fixture whose bits nothing in the repo chose would prove less, not more.
cat > "$T/mkswift.c" <<'EOF'
/* mkswift make OUT   -- write a tiny 64-bit Mach-O with one __DATA segment
 *                       holding __objc_classlist -> one class record, whose
 *                       isa points at a metaclass record. Both records carry
 *                       the STABLE-ABI is-Swift tag (low bits == 2).
 * mkswift tags FILE   -- print "class <low2> <word>" then "meta <low2> <word>"
 *                       for the two records this layout puts at fixed offsets.
 *
 * Fixed layout (file offsets == vm offsets; the segment maps at vmaddr
 * VMBASE with fileoff 0, so file_off(va) == va - VMBASE):
 *   0x800  __objc_classlist: one 8-byte VA, pointing at the class record
 *   0x900  class record:     +0 isa -> metaclass VA, +32 data word
 *   0x940  metaclass record: +0 isa == 0,            +32 data word
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <mach-o/loader.h>

#define FSIZE      0x1000
#define VMBASE     0x100000000ULL
#define LISTOFF    0x800
#define CLASSOFF   0x900
#define METAOFF    0x940
#define DATAOFF    32
#define PAYLOAD    0x00000001000009c0ULL   /* plausible non-tag bits, preserved */

static void set_name16(char *field, const char *name) {
    size_t len = strlen(name);
    if (len > 16) len = 16;
    memset(field, 0, 16);
    memcpy(field, name, len);
}

static uint8_t *slurp(const char *path, size_t *n) {
    FILE *f = fopen(path, "rb");
    if (!f) { perror(path); exit(2); }
    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t *b = malloc((size_t)sz);
    if (!b || fread(b, 1, (size_t)sz, f) != (size_t)sz) { fprintf(stderr, "read %s\n", path); exit(2); }
    fclose(f);
    *n = (size_t)sz;
    return b;
}

int main(int argc, char **argv) {
    if (argc != 3) { fprintf(stderr, "usage: mkswift make|tags FILE\n"); return 2; }

    if (strcmp(argv[1], "tags") == 0) {
        size_t n; uint8_t *b = slurp(argv[2], &n);
        if (n < FSIZE) { fprintf(stderr, "fixture truncated\n"); return 2; }
        uint64_t c = *(uint64_t *)(b + CLASSOFF + DATAOFF);
        uint64_t m = *(uint64_t *)(b + METAOFF + DATAOFF);
        printf("class %llu 0x%llx\n", (unsigned long long)(c & 3), (unsigned long long)c);
        printf("meta %llu 0x%llx\n", (unsigned long long)(m & 3), (unsigned long long)m);
        return 0;
    }
    if (strcmp(argv[1], "make") != 0) { fprintf(stderr, "unknown mode\n"); return 2; }

    uint8_t *buf = calloc(1, FSIZE);
    struct mach_header_64 *h = (struct mach_header_64 *)buf;
    h->magic = MH_MAGIC_64;
    h->cputype = CPU_TYPE_X86_64;
    h->cpusubtype = 3;
    h->filetype = MH_EXECUTE;
    h->ncmds = 1;

    struct segment_command_64 *sg = (struct segment_command_64 *)(buf + sizeof *h);
    sg->cmd = LC_SEGMENT_64;
    sg->cmdsize = (uint32_t)(sizeof *sg + 2 * sizeof(struct section_64));
    set_name16(sg->segname, "__DATA");
    sg->vmaddr = VMBASE;
    sg->vmsize = FSIZE;
    sg->fileoff = 0;
    sg->filesize = FSIZE;
    sg->nsects = 2;
    h->sizeofcmds = sg->cmdsize;

    struct section_64 *sc = (struct section_64 *)(sg + 1);
    set_name16(sc[0].sectname, "__objc_classlist");
    set_name16(sc[0].segname, "__DATA");
    sc[0].addr = VMBASE + LISTOFF;
    sc[0].size = 8;
    sc[0].offset = LISTOFF;
    set_name16(sc[1].sectname, "__objc_data");
    set_name16(sc[1].segname, "__DATA");
    sc[1].addr = VMBASE + CLASSOFF;
    sc[1].size = 0x100;
    sc[1].offset = CLASSOFF;

    *(uint64_t *)(buf + LISTOFF) = VMBASE + CLASSOFF;
    *(uint64_t *)(buf + CLASSOFF) = VMBASE + METAOFF;   /* class->isa */
    *(uint64_t *)(buf + CLASSOFF + DATAOFF) = PAYLOAD | 2;
    *(uint64_t *)(buf + METAOFF) = 0;                   /* metaclass->isa */
    *(uint64_t *)(buf + METAOFF + DATAOFF) = PAYLOAD | 2;

    FILE *f = fopen(argv[2], "wb");
    if (!f) { perror("fopen"); return 1; }
    if (fwrite(buf, 1, FSIZE, f) != FSIZE) { perror("fwrite"); fclose(f); return 1; }
    fclose(f);
    return 0;
}
EOF
"$CC" -O2 -o "$T/mkswift" "$T/mkswift.c"
"$T/mkswift" make "$T/swift_fixture"
tags_before=$("$T/mkswift" tags "$T/swift_fixture")
[ "$tags_before" = "class 2 0x1000009c2
meta 2 0x1000009c2" ] \
    && ok "retag-swift: fixture starts with both records on the stable-ABI bit" \
    || bad "retag-swift: precondition" "unexpected starting tags: $tags_before"

"$MACHO9" retag-swift "$T/swift_fixture" >"$T/retag.out" 2>&1 \
    || bad "retag-swift: exit" "$(cat "$T/retag.out")"
tags_after=$("$T/mkswift" tags "$T/swift_fixture")
[ "$tags_after" = "class 1 0x1000009c1
meta 1 0x1000009c1" ] \
    && ok "retag-swift: moved both tags to the legacy bit, leaving every other bit alone" \
    || bad "retag-swift" "expected both records tagged 1 with 0x...9c1, got: $tags_after"
# Both halves of the pair: the class AND the metaclass its isa points at. The
# count in the message is how we know the metaclass was reached at all.
grep -q "retagged 2 class record(s)" "$T/retag.out" \
    && ok "retag-swift: reported both the class and its metaclass" \
    || bad "retag-swift: count" "expected 2 records, got: $(cat "$T/retag.out")"

# Idempotent: a second run finds nothing on the stable bit, says 0, and does
# not flip anything back.
cp "$T/swift_fixture" "$T/swift_twice_before"
"$MACHO9" retag-swift "$T/swift_fixture" >"$T/retag2.out" 2>&1 \
    || bad "retag-swift: second run exit" "$(cat "$T/retag2.out")"
grep -q "retagged 0 class record(s)" "$T/retag2.out" \
    && ok "retag-swift: a second run retags nothing" \
    || bad "retag-swift: idempotence" "expected 0 records, got: $(cat "$T/retag2.out")"
cmp -s "$T/swift_fixture" "$T/swift_twice_before" \
    && ok "retag-swift: a run with nothing to do left the file untouched" \
    || bad "retag-swift: idempotence" "the file changed on a no-op run"

# Handed something it cannot read, this verb SAYS SO rather than exiting 0
# with no output -- the whole reason it does not just forward the old tool's
# bare "return 0". A fat container is the realistic case: this verb is
# thin-only, exactly like retag_swift_classes.
#
# The container is built HERE, out of the very fixture the assertions above
# just retagged successfully, rather than borrowed from the `segment` section
# ~150 lines up. Two reasons: a borrowed fixture means deleting or renaming
# that section silently breaks a retag-swift assertion, and wrapping THIS
# fixture makes the pair a control -- the same bytes are retaggable thin and
# refused fat, so the refusal is provably about the container, not the
# content. The one thing still shared is the segread helper that assembles
# it, and that dependency is checked rather than assumed.
if [ ! -x "$T/segread" ]; then
    bad "retag-swift: fat" "the segread helper (built in the segment section above) is missing, so the fat-refusal assertions could not be built"
else
    printf 'not a mach-o at all, just bytes.\n' > "$T/retag_fat_blob"
    "$T/segread" wrap "$T/retag_fat" "$T/swift_fixture" "$T/retag_fat_blob"
    rc=0
    "$MACHO9" retag-swift "$T/retag_fat" >"$T/retag_fat.out" 2>"$T/retag_fat.err" || rc=$?
    [ "$rc" -eq 1 ] && ok "retag-swift: refuses a fat container with the documented refusal code" \
        || bad "retag-swift: fat" "expected exit 1, got $rc: $(cat "$T/retag_fat.out") $(cat "$T/retag_fat.err")"
    grep -q "not a readable 64-bit Mach-O" "$T/retag_fat.err" \
        && ok "retag-swift: says why it refused, instead of silently doing nothing" \
        || bad "retag-swift: fat message" "no explanation on stderr: $(cat "$T/retag_fat.err")"
    # The control: the same class records, thin, ARE reachable. Without this
    # the assertions above would also pass against a verb that refused
    # everything.
    cp "$T/swift_fixture" "$T/retag_thin_control"
    rc=0
    "$MACHO9" retag-swift "$T/retag_thin_control" >"$T/retag_thin_control.out" 2>&1 || rc=$?
    [ "$rc" -eq 0 ] \
        && ok "retag-swift: the same bytes, thin, are accepted -- the refusal is about the container" \
        || bad "retag-swift: thin control" "expected exit 0, got $rc: $(cat "$T/retag_thin_control.out")"
fi

# MSWIFT_ERROR (a path that cannot even be opened) must be EX_FAIL (2) -- a
# genuine operational failure, NOT the EX_REFUSED the unreadable-input case
# gets, and certainly not 0. This is the assertion that covers cmd_retag_swift's
# by-name test of the negative codes: collapse those branches and one of
# these two exit codes moves.
rc=0
"$MACHO9" retag-swift "$T/no-such-file-for-retag" >"$T/retag_missing.out" 2>"$T/retag_missing.err" || rc=$?
[ "$rc" -eq 2 ] \
    && ok "retag-swift: an unopenable path is a failure (2), not a refusal (1) and not silent success" \
    || bad "retag-swift: missing path" "expected exit 2, got $rc: $(cat "$T/retag_missing.out") $(cat "$T/retag_missing.err")"

reached_end=1
echo "cli_test: $fails failure(s)"
[ "$fails" -eq 0 ]
