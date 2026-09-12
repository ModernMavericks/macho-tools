#!/bin/sh
# tests/cli_test.sh — exercises the machotool CLI itself: --capabilities, and
# each verb this build actually implements.
#
# What this does NOT re-prove: change_dylib_test.sh already runs real dylib
# renumbering, -insert/-delete ordinal correctness, and header-growth end to
# end, through change_dylib. dylib/rpath/lc/minos here call the very same
# code -- mr_apply_file/mv_add_version_min in src/, which is all change_dylib
# and add_version_min are too (see cli/machotool.c's file header) -- so this
# asserts the TRANSLATION and DISPATCH are correct, one exemplar op per verb,
# not the underlying rewrite a second time.
#
# Host-portability, per the task's own hard-won rules:
#   - every fixture is built with -mmacosx-version-min=10.9, so a modern
#     linker's LC_DYLD_CHAINED_FIXUPS default can't sneak in and ask a
#     different question on the cross runner than it asks natively here.
#   - nothing here parses otool/nm text. Facts about a binary come either
#     from `machotool info`'s own stable output, or from a tiny C reader built
#     alongside the fixtures (same trick change_dylib_test.sh's ordinal_of.c
#     uses), never from a format Apple's tools are free to reformat.
set -eu
BIN="${1:?usage: cli_test.sh <bindir>}"
MACHOTOOL="$BIN/machotool"
[ -x "$MACHOTOOL" ] || { echo "cli_test: $MACHOTOOL not found or not executable" >&2; exit 1; }
[ -x "$BIN/makefat" ] && [ -x "$BIN/fatcheck" ] || { echo "cli_test: need makefat and fatcheck in $BIN" >&2; exit 1; }
# machotool needs NOTHING else in $BIN: dylib/rpath/lc/minos used to run
# change_dylib/add_version_min as subprocesses found next to it, and this
# script used to refuse to start without them. The "machotool alone in an empty
# directory" assertions below are what replaced that requirement -- they check
# the property the requirement existed for, from the outside, instead of
# taking it on trust.

CC="${CC:-clang}"
# This script's own directory, for the fixture-builder C sources that live
# beside it (tests/strip_version_min.c).
HERE=$(cd "$(dirname "$0")" && pwd)
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
# A file's content digest, for "untouched"/"unchanged" assertions -- the same
# shasum invocation already used a few times below, named once so the `edit`
# section (which needs it three times) doesn't repeat the pipeline.
sha()  { shasum -a 256 < "$1" | cut -d' ' -f1; }

# mtip VERB FILE ARG...  -- run a rewriting verb and leave its result AT FILE.
#
# dylib, rpath, lc and segment take `FILE OUT` and never write FILE. Most of
# the assertions below were written when those verbs rewrote FILE, and they are
# about the REWRITE -- which load command moved, which ordinal was renumbered,
# what the run printed, what it refused -- not about which path the bytes land
# in. So they keep asking their own question, of a file this helper puts the
# result back into: run the verb with a temp beside FILE as OUT, then mv the
# temp over FILE, which is precisely the two steps the compat wrappers take
# (compat/machotool-compat.sh's install path). The verb's stdout and exit status
# are passed through unchanged, less the "Wrote <temp>" line, which names a
# path no assertion here asked about.
#
# WHAT THIS DOES NOT HIDE: that FILE is never written by machotool itself is
# asserted directly, per verb, in "the rewriting verbs never write their input"
# below -- against FILE's bytes AND its inode, with no helper in the way. This
# one is an ergonomic for everything else, not a stand-in for that.
mtip() {
    mtip_verb=$1; mtip_file=$2; shift 2
    mtip_tmp="$mtip_file.mtip"
    rm -f "$mtip_tmp"
    mtip_rc=0
    "$MACHOTOOL" "$mtip_verb" "$mtip_file" "$mtip_tmp" "$@" >"$T/mtip.out" || mtip_rc=$?
    # Through the environment, not `awk -v`: that escape-processes what it
    # assigns, so a $T containing a backslash would leave the line unsuppressed.
    # compat/machotool-compat.sh's mw_run_to_tmp, which this mirrors, has the
    # measurement.
    MTIP_PREFIX="Wrote $mtip_tmp (" awk 'index($0, ENVIRON["MTIP_PREFIX"]) != 1' "$T/mtip.out"
    if [ "$mtip_rc" -eq 0 ]; then
        mv -f "$mtip_tmp" "$mtip_file" || return 2
    else
        rm -f "$mtip_tmp"
    fi
    return "$mtip_rc"
}

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

# Two dylibs ahead of the libSystem clang appends, in THIS link order:
# libb (ordinal 1), which nothing binds to, then liba (ordinal 2), which
# main's a_sym binds to; libSystem is 3. Both halves of that are needed by
# the `edit` follow-up assertions below. Nothing may bind to libb,
# or `dylib delete` refuses it outright. And libb must come BEFORE a dylib
# with binds, or deleting it renumbers nothing and every count it reports is
# a vacuous zero. libb gets no -install_name, so its install name is the path
# it was linked from, "$T/libb.dylib" -- the path a script names to delete it.
# The linker keeps an unreferenced dylib unless told to dead-strip them.
cat > "$T/b.c" <<'EOF'
int b_sym(void) { return 22; }
EOF
"$CC" -dynamiclib -O2 $FIXTURE_FLAGS "$T/b.c" -o "$T/libb.dylib"
# A third dylib, bound to, for proving the reported counts follow the input.
cat > "$T/c.c" <<'EOF'
int c_sym(void) { return 33; }
EOF
"$CC" -dynamiclib -O2 $FIXTURE_FLAGS -install_name "@loader_path/libc3.dylib" \
    "$T/c.c" -o "$T/libc3.dylib"
cat > "$T/main3.c" <<'EOF'
int a_sym(void);
int c_sym(void);
int main(void) { return a_sym() == 11 && c_sym() == 33 ? 0 : 1; }
EOF

# The ordinals above are the premise, so check them rather than trust the
# linker: `machotool info`'s own stable output, as the header says.
fixture_ordinals() {
    fo_info=$("$MACHOTOOL" info "$1")
    shift
    for fo_want in "$@"; do
        echo "$fo_info" | grep -qF "$fo_want" \
            || bad "fixture setup" "expected '$fo_want' in: $fo_info"
    done
}
build_main_two_dylibs() {
    "$CC" -O2 $FIXTURE_FLAGS "$T/main.c" "$T/libb.dylib" "$T/liba.dylib" -o "$1"
    fixture_ordinals "$1" "ordinal=1 path=$T/libb.dylib" \
        "ordinal=2 path=@loader_path/liba.dylib" "ordinal=3 path=/usr/lib/libSystem.B.dylib"
}
# The same, with a third dylib that main also binds to after liba:
# libb=1, liba=2, libc3=3, libSystem=4.
build_main_three_dylibs() {
    "$CC" -O2 $FIXTURE_FLAGS "$T/main3.c" "$T/libb.dylib" "$T/liba.dylib" \
        "$T/libc3.dylib" -o "$1"
    fixture_ordinals "$1" "ordinal=1 path=$T/libb.dylib" \
        "ordinal=2 path=@loader_path/liba.dylib" "ordinal=3 path=@loader_path/libc3.dylib" \
        "ordinal=4 path=/usr/lib/libSystem.B.dylib"
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
# This uses machotool to set up a machotool test, which is circular only in
# appearance: if the strip silently did nothing, the delete under test would
# FIND build-version and report no miss, and the assertions fail loudly. The
# setup cannot mask the defect it is setting up for.
build_main_without_build_version() {
    build_main "$1"
    mtip lc "$1" -delete build-version >/dev/null 2>&1 || true
    # Assert the precondition rather than trusting the strip. otool, not
    # machotool, so a machotool defect cannot certify its own setup. Without this
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
# This must be established WITHOUT running machotool on the probe binary. The
# grow/lc "still runs" assertions below rewrite a fixture with machotool and
# then run it; if this host's kernel kills any modified binary, that proves
# nothing about machotool -- but if the probe used to detect that ALSO goes
# through machotool, a real machotool regression that corrupts its output looks
# IDENTICAL to a host that kills modified binaries: same symptom (the child
# doesn't run), same wrong conclusion ("host policy, not a machotool defect"),
# and a genuine defect ships as a green, honest-looking SKIP. That is worse
# than no check at all.
#
# So this probe never calls machotool. It builds a plain fixture, flips ONE
# byte inside the existing header pad (unused space between the end of the
# load commands and the first section's file data -- computed here by an
# independent read, not by calling into machotool/image.h, for the same
# non-circularity reason tests/strip_version_min.c is self-contained) via a
# throwaway C program, and tries to run the result. If the kernel/dyld kills
# THAT, this host enforces code-signing on any post-link modification,
# unconditionally of what changed or which tool changed it -- an honest,
# independently-established fact the grow/lc sections can trust. If it
# still runs, this host does NOT enforce that, and a failure to run
# machotool's OWN rewritten fixture later is no longer explainable by host
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
    ok "host probe: a trivially-perturbed binary still runs (machotool-independent)"
elif [ "$signing_probe_rc" -eq 137 ]; then
    signing_enforced=1
    ok "host probe: a trivially-perturbed binary is SIGKILLed (137) -- code-signing enforcement, independent of machotool"
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
caps=$("$MACHOTOOL" --capabilities) || bad "capabilities: exit" "nonzero"
case "$caps" in
    "format 1"*) ok "capabilities: starts with format line" ;;
    *) bad "capabilities: format line" "got: $(echo "$caps" | head -1)" ;;
esac
# exitcodes documents EX_REFUSED (see cli/machotool.c) so a caller can tell
# "machotool examined FILE and declined" apart from "machotool itself failed"
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
"$MACHOTOOL" verify "$T/not-a-macho-in-cli-test" >/dev/null 2>&1 || rc=$?
[ "$rc" -eq 1 ] \
    && ok "capabilities: a real refusal (verify on a non-Mach-O) actually exits 1" \
    || bad "capabilities: exitcodes vs reality" "verify on a non-Mach-O exited $rc, not the documented 1"

# "output positional=2 never-writes-input" is the shape every rewriting verb's
# positionals take: a wrapper checks for this line rather than assume it.
echo "$caps" | grep -qx "output positional=2 never-writes-input" \
    && ok "capabilities: output line documents FILE OUT, never-writes-input" \
    || bad "capabilities: output line" "missing or wrong: $(echo "$caps" | grep '^output')"

for v in verify info grow minos lc dylib rpath segment retag-swift declassify edit; do
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
"$MACHOTOOL" >/dev/null 2>"$T/usage.err" || true
if grep -q "declassify" "$T/usage.err"; then
    if grep "declassify" "$T/usage.err" | grep -qi "not implemented"; then
        bad "usage: declassify" "still says 'not implemented'"
    else
        ok "usage: declassify listed without a 'not implemented' disclaimer"
    fi
else
    bad "usage: declassify" "not mentioned at all"
fi
grep -q "edit" "$T/usage.err" \
    && ok "usage: edit listed" \
    || bad "usage: edit" "not mentioned at all"
# rpath -insert IS implemented now; capabilities must claim it. A wrapper has
# no other way to learn this build can place a search path FIRST, which
# docs/PROPOSAL.md calls a new capability change_dylib never had.
if echo "$caps" | grep "^verb rpath" | grep -q "insert"; then
    ok "capabilities: rpath insert advertised"
else
    bad "capabilities: rpath insert" "implemented but not advertised"
fi
# edit's own line carries NO flags= field, because `edit` accepts no flags.
# `--output` became the OUT positional, `--dry-run` went with it (a scratch OUT
# is the same run), and `--verbose` went when the report stopped being optional:
# advertising any of them would tell a wrapper it may pass a flag this build
# refuses. Whole-line equality, because "not advertised" is the claim.
echo "$caps" | grep -qxF "verb edit" \
    && ok "capabilities: edit advertises no flags at all" \
    || bad "capabilities: edit flags" "expected a bare 'verb edit': $(echo "$caps" | grep '^verb edit')"

# --capabilities' statement lines are generated from MS_TABLE (src/script.c)
# by looping ms_table_row, not hand-copied. The spec's statement vocabulary
# has 14 <kind,op> pairs, and `target 10.9` -- whose profile occupies the op
# column -- makes 15; tests/script_test.c's
# test_capabilities_table_round_trips separately walks ms_table_row directly
# and confirms MS_TABLE itself has those 15 rows, each of which round-trips
# through ms_parse. This assertion checks the other half of the same claim
# from here, reusing the $caps already captured above: that
# print_capabilities' loop over ms_table_row actually emitted 15 "statement "
# lines, with none dropped, none extra, and none duplicated. Together the
# two catch the generator and the table going out of step with each other.
n_statements=$(echo "$caps" | grep -c '^statement ' || true)
n_unique=$(echo "$caps" | grep '^statement ' | sort -u | wc -l | tr -d ' ')
[ "$n_statements" -eq 15 ] && [ "$n_unique" -eq 15 ] \
    && ok "capabilities: exactly 15 unique statement lines" \
    || bad "capabilities statement count" "got $n_statements line(s), $n_unique unique: $(echo "$caps" | grep '^statement')"
# The profile vocabulary is advertised from that same table, so a wrapper can
# see which targets this build knows rather than guess. `target 10.9 0`: no
# operands after the profile.
echo "$caps" | grep -qxF "statement target 10.9 0" \
    && ok "capabilities: the target profile is advertised" \
    || bad "capabilities statements" "no 'statement target 10.9 0' line: $(echo "$caps" | grep '^statement')"
echo "$caps" | grep -q "statement dylib replace 2" \
    && ok "capabilities: statement table is advertised" \
    || bad "capabilities statements" "no 'statement dylib replace 2' line: $(echo "$caps" | grep '^statement')"

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
# ONE table each (LC_STRIP_KINDS, DYLIB_OPS in cli/machotool.c) precisely so
# they cannot say different things -- before this they were three
# hand-copied lists (change_dylib's strippable[], machotool's own LC_KINDS[],
# and a hardcoded "kinds=..." string) that a review found had already drifted
# apart in spirit even where the values still matched by luck. This does not
# re-derive the table (it can't see the C source); it drives machotool itself
# with every name --capabilities claims and confirms none of them is refused
# as unrecognized -- which is exactly what would happen if a name were ever
# added to (or dropped from) one list and not the other.
build_main "$T/vocab_fixture"
kinds=$(echo "$caps" | sed -n 's/^verb lc .*kinds=\([^ ]*\).*/\1/p')
[ -n "$kinds" ] || bad "capabilities vocab" "no kinds= on the lc line"
oldifs="$IFS"; IFS=','
vocab_kind_fail=0
for kind in $kinds; do
    mtip lc "$T/vocab_fixture" -delete "$kind" >"$T/vocab_kind.out" 2>&1 || true
    if grep -qi "unknown KIND" "$T/vocab_kind.out"; then
        bad "capabilities vocab: kind '$kind'" "advertised but lc -delete refused it as unknown: $(cat "$T/vocab_kind.out")"
        vocab_kind_fail=1
    fi
done
IFS="$oldifs"
[ "$vocab_kind_fail" -eq 0 ] && ok "capabilities vocab: every advertised lc kind is accepted by lc -delete"
# And the inverse: a KIND that is plainly not real must still be refused --
# otherwise this check could trivially "pass" by lc accepting everything.
mtip lc "$T/vocab_fixture" -delete not-a-real-kind >"$T/vocab_bogus.out" 2>&1 \
    && bad "capabilities vocab: bogus kind" "lc -delete accepted a KIND that isn't in any table" \
    || { grep -qi "unknown KIND" "$T/vocab_bogus.out" \
         && ok "capabilities vocab: an unadvertised kind is refused as unknown" \
         || bad "capabilities vocab: bogus kind" "refused, but not with 'unknown KIND': $(cat "$T/vocab_bogus.out")"; }

# Same idea for dylib/rpath ops=: every op --capabilities advertises for a
# verb must be recognized by that verb's own parser (never "unknown or
# incomplete operation"), and rpath must still refuse an op that belongs to
# dylib's vocabulary but not its own (-reexport: LC_RPATH has only one kind
# -- see DYLIB_OPS in cli/machotool.c).
vocab_ops_fail=0
check_ops_accepted() {
    # $1=verb (dylib|rpath)  $2=ops csv from capabilities
    verb="$1"; oldifs2="$IFS"; IFS=','
    for op in $2; do
        IFS="$oldifs2"   # restore default (whitespace) splitting for the command below
        case "$op" in
            replace) mtip "$verb" "$T/vocab_fixture" "-$op" /no/such/old /no/such/new \
                         >"$T/vocab_op.out" 2>&1 || true ;;
            *)       mtip "$verb" "$T/vocab_fixture" "-$op" /no/such/path \
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
# section gives, and the idiom leaf-tool-crashes.sh's mkfixture.c and
# tests/mkswift.c already use.
#
# The program is tests/mkchained.c, a FILE rather than a here-document because
# tests/wrapper_test.sh needs exactly the same fixture for exactly the same
# reason -- `patch_macho`'s converting path -- and one copy of it is enough.
# Same arrangement as tests/strip_version_min.c and tests/mkswift.c.
SRC_DIR=$(CDPATH= cd -- "$(dirname -- "$0")/../src" && pwd)
"$CC" -O2 -I "$SRC_DIR" -o "$T/mkchained" "$HERE/mkchained.c"
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

"$MACHOTOOL" declassify "$T/chained.in" "$T/chained.out" >"$T/dcl.out" 2>"$T/dcl.err" && rc=0 || rc=$?
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
"$MACHOTOOL" declassify "$T/weak.in" "$T/weak.out" >/dev/null 2>"$T/weak.err" && rc=0 || rc=$?
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
"$MACHOTOOL" declassify "$T/big.in" "$T/big.out" >/dev/null 2>"$T/big.err" && rc=0 || rc=$?
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

# THE HEADER PAD'S BOUND. The new LC_DYLD_INFO_ONLY goes after the load
# commands and must end before the first section's file data. With no section
# data (make-nosect) there is no such bound, and the conversion used to assume
# 4096; with the first section past the end of the file (make-sectpast) the
# bound lies outside the image. Either way it must refuse, say why, and write
# nothing.
for dcl_mode in nosect sectpast; do
    "$T/mkchained" make-$dcl_mode "$T/$dcl_mode.in"
    rm -f "$T/$dcl_mode.out"
    "$MACHOTOOL" declassify "$T/$dcl_mode.in" "$T/$dcl_mode.out" >/dev/null 2>"$T/$dcl_mode.err" \
        && rc=0 || rc=$?
    case $dcl_mode in
        nosect)   dcl_why="no section data bounds the header pad" ;;
        sectpast) dcl_why="lies past the end of the image" ;;
    esac
    [ "$rc" -eq 1 ] && grep -q "$dcl_why" "$T/$dcl_mode.err" \
        && ok "declassify: $dcl_mode: refuses (1), saying '$dcl_why'" \
        || bad "declassify: $dcl_mode" "expected 1 + '$dcl_why', got $rc: $(cat "$T/$dcl_mode.err")"
    [ -e "$T/$dcl_mode.out" ] && bad "declassify: $dcl_mode" "wrote an output for an input it refused" \
        || ok "declassify: $dcl_mode: produces no output file"
done

# BYTE-IDENTITY WITH patch_macho, the strongest available proof that lifting
# the conversion into src/declassify.c did not change it: the two front-ends
# are handed the same buffer by md_declassify and must write the same bytes.
# Not a hard requirement of THIS script (machotool stands alone, and $BIN need
# not hold anything else), so its absence is a SKIP, not a failure.
if [ -x "$BIN/patch_macho" ]; then
    "$BIN/patch_macho" "$T/chained.in" "$T/chained.pm" >/dev/null 2>&1
    if cmp -s "$T/chained.out" "$T/chained.pm"; then
        ok "declassify: byte-identical to patch_macho's output"
    else
        bad "declassify: byte-identity" "machotool and patch_macho produced different bytes"
    fi
    # patch_macho returns a flat 1 for everything that goes wrong; this verb
    # distinguishes "examined it and declined" (EX_REFUSED=1) from an
    # operational failure (EX_FAIL=2). For THIS refusal the two numbers
    # happen to agree (both 1) -- that is a coincidence of the corrected
    # numbering, not a design goal -- but a wrapper that must look like
    # patch_macho still has real mapping work to do for the EX_FAIL=2 case,
    # where the numbers diverge; compat/patch_macho.sh's own header covers
    # both.
    "$BIN/patch_macho" "$T/not-a-macho-in-cli-test" "$T/nope_pm" >/dev/null 2>&1 && pm_rc=0 || pm_rc=$?
    [ "$pm_rc" -eq 1 ] && ok "declassify: patch_macho's flat 1 and this verb's EX_REFUSED agree on this refusal" \
        || bad "declassify: patch_macho exit" "expected the historical flat 1, got $pm_rc"
else
    skip "declassify: byte-identity with patch_macho" "no patch_macho in $BIN"
fi

# IDEMPOTENCY, which install.sh's wrapper depends on: running the conversion
# over an already-converted binary passes it through unchanged instead of
# failing on the fixups that are no longer there.
"$MACHOTOOL" declassify "$T/chained.out" "$T/chained.again" >"$T/again.out" 2>"$T/again.err" && rc=0 || rc=$?
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

# IN AND OUT MAY NO LONGER BE THE SAME PATH. This verb used to allow it (the
# whole image is in memory before a byte is written, so it worked), and now
# refuses it UP FRONT -- before any read -- because machotool never writes its
# input. The same four facts every other converted verb is held to: refused
# with 2, IN untouched in bytes AND inode, the refusal is the up-front one, and
# a symlink to IN is caught too. `patch_macho IN IN` still converts IN: its
# wrapper runs this verb into a temp beside OUT and installs that.
cp "$T/chained.in" "$T/inplace"
dcl_sha=$(sha "$T/inplace"); dcl_ino=$(stat -f %i "$T/inplace")
rc=0
"$MACHOTOOL" declassify "$T/inplace" "$T/inplace" >/dev/null 2>"$T/inplace.err" || rc=$?
[ "$rc" -eq 2 ] && [ "$(sha "$T/inplace")" = "$dcl_sha" ] \
    && [ "$(stat -f %i "$T/inplace")" = "$dcl_ino" ] \
    && ok "declassify: an OUT that is IN is refused (2), IN untouched" \
    || bad "declassify: OUT=IN" "rc $rc, or IN changed"
grep -q "never writes its input" "$T/inplace.err" \
    && ok "declassify: ... refused up front, before any work" \
    || bad "declassify: OUT=IN" "not the up-front refusal: $(cat "$T/inplace.err")"
rm -f "$T/inplace_link"; ln -s "$T/inplace" "$T/inplace_link"
rc=0
"$MACHOTOOL" declassify "$T/inplace" "$T/inplace_link" >/dev/null 2>&1 || rc=$?
[ "$rc" -eq 2 ] && ok "declassify: an OUT that is a symlink to IN is refused (2)" \
    || bad "declassify: OUT=link" "rc $rc"

# OUT TAKES IN'S MODE, not the fixed 0755 this verb's own open() used to ask
# for: wa_write_new copies the input's, like every other converted verb. The C
# tool's 0755-masked-by-umask is now compat/patch_macho.sh's to restore, and
# tests/wrapper_test.sh is where that is asserted.
chmod 640 "$T/inplace"
rm -f "$T/dcl_mode_out"
rc=0
"$MACHOTOOL" declassify "$T/inplace" "$T/dcl_mode_out" >/dev/null 2>"$T/dcl_mode.err" || rc=$?
[ "$rc" -eq 0 ] && [ "$(stat -f %Lp "$T/dcl_mode_out")" = 640 ] \
    && ok "declassify: OUT is created with IN's mode" \
    || bad "declassify: OUT mode" "rc $rc, mode $(stat -f %Lp "$T/dcl_mode_out" 2>/dev/null): $(cat "$T/dcl_mode.err")"

# Refusals. Each is a decision machotool made about the INPUT, so each is
# EX_REFUSED (1), never EX_FAIL (2), which means "something went wrong running
# machotool" -- that distinction is what --capabilities' exitcodes line promises.
"$MACHOTOOL" declassify "$T/not-a-macho-in-cli-test" "$T/nope" >/dev/null 2>"$T/nm.err" && rc=0 || rc=$?
[ "$rc" -eq 1 ] && ok "declassify: refuses a non-Mach-O with EX_REFUSED" \
    || bad "declassify: non-Mach-O" "expected 1, got $rc"
[ -e "$T/nope" ] && bad "declassify: non-Mach-O" "wrote an output file for an input it refused" \
    || ok "declassify: a refused input produces no output file"

# A 64-bit Mach-O with NEITHER chained fixups NOR LC_DYLD_INFO_ONLY -- a plain
# object file is exactly that -- is not idempotent-pass-through material and
# not convertible either. It must say so and refuse, not quietly copy.
"$CC" -c -O2 $FIXTURE_FLAGS "$T/main.c" -o "$T/plain.o"
"$MACHOTOOL" declassify "$T/plain.o" "$T/plain.out" >/dev/null 2>"$T/plain.err" && rc=0 || rc=$?
[ "$rc" -eq 1 ] && ok "declassify: refuses a Mach-O with no chained fixups and no LC_DYLD_INFO_ONLY" \
    || bad "declassify: no fixups" "expected 1, got $rc"
grep -q "No chained fixups found" "$T/plain.err" \
    && ok "declassify: says why it refused" \
    || bad "declassify: no fixups" "no reason on stderr: $(cat "$T/plain.err")"

# An OUT that cannot be written is an OPERATIONAL failure, not a refusal: the
# input was fine and machotool declined nothing. It must exit 2 (EX_FAIL), and
# this is the assertion that keeps EX_REFUSED from decaying into "any nonzero".
"$MACHOTOOL" declassify "$T/chained.in" "$T/no/such/dir/out" >/dev/null 2>"$T/unwritable.err" && rc=0 || rc=$?
[ "$rc" -eq 2 ] && ok "declassify: an unwritable OUT is a failure (2), not a refusal (1)" \
    || bad "declassify: unwritable OUT" "expected 2, got $rc"

# ============================================================================
# machotool stands alone
#
# dylib/rpath/lc/minos used to fork and exec change_dylib/add_version_min,
# located next to machotool on disk, and --capabilities hid those four verbs
# whenever the sibling was missing. Both are gone: the rewrite is linked in
# (src/rewrite.c, src/version_min.c). That is the whole point of the
# extraction -- it is what lets change_dylib become a wrapper AROUND machotool
# without a cycle -- so prove it from the outside rather than by reading the
# source: copy ONLY machotool into an empty directory and make it do real work
# there. A regression that restored the subprocess would fail here even
# though every other assertion in this file, run from a full bindir, would
# still pass.
# ============================================================================
mkdir -p "$T/alone"
cp "$MACHOTOOL" "$T/alone/machotool"
alone_caps=$("$T/alone/machotool" --capabilities)
alone_missing=""
for v in verify info grow minos lc dylib rpath segment retag-swift; do
    echo "$alone_caps" | grep -q "^verb $v" || alone_missing="$alone_missing $v"
done
[ -z "$alone_missing" ] && ok "alone: --capabilities still advertises every verb with no sibling present" \
    || bad "alone: capabilities" "verbs missing when machotool stands alone:$alone_missing"

build_main "$T/alone/fixture"
if "$T/alone/machotool" dylib "$T/alone/fixture" "$T/alone/fixture.out" \
        -append "@loader_path/libalone.dylib" >"$T/alone_dylib.out" 2>&1; then
    ok "alone: dylib -append works with no change_dylib anywhere near machotool"
else
    bad "alone: dylib -append" "$(cat "$T/alone_dylib.out")"
fi
"$T/alone/machotool" info "$T/alone/fixture.out" | grep -qF "path=@loader_path/libalone.dylib" \
    && ok "alone: the append really landed in the output" \
    || bad "alone: dylib -append result" "new dependency not in info output"

if "$T/alone/machotool" lc "$T/alone/fixture.out" "$T/alone/fixture.out2" -delete uuid \
        >"$T/alone_lc.out" 2>&1; then
    ok "alone: lc -delete works with no change_dylib anywhere near machotool"
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
# machotool failing because a binary it no longer needs isn't there. (Note the
# fixture is deliberately NOT stripped of its version-min first: the helper
# that does that is built further down, and this assertion is about reaching
# the driver at all, not about which branch of it ran.)
if "$T/alone/machotool" minos "$T/alone/fixture" "$T/alone/fixture.minos" 10.9 >"$T/alone_minos.out" 2>&1; then
    ok "alone: minos works with no add_version_min anywhere near machotool"
else
    bad "alone: minos" "$(cat "$T/alone_minos.out")"
fi
if grep -q "LC_VERSION_MIN_MACOSX" "$T/alone_minos.out"; then
    ok "alone: minos reached the version-min driver in-process (said what it did)"
else
    bad "alone: minos output" "exited 0 but said nothing about LC_VERSION_MIN_MACOSX: $(cat "$T/alone_minos.out")"
fi
"$T/alone/machotool" info "$T/alone/fixture.minos" | grep -q "LC_VERSION_MIN_MACOSX" \
    && ok "alone: the output carries LC_VERSION_MIN_MACOSX afterward" \
    || bad "alone: minos result" "no LC_VERSION_MIN_MACOSX in info output after minos"

# ============================================================================
# verify
# ============================================================================
build_main "$T/verify_ok"
if "$MACHOTOOL" verify "$T/verify_ok" >"$T/verify_ok.out"; then
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
if "$MACHOTOOL" verify "$T/verify_bad" >/dev/null 2>&1; then
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
if "$MACHOTOOL" verify "$T/fixture.dylib" >"$T/dylibverify.out" 2>&1; then
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
# mr_process_thin gates on mg_plausible, so machotool dylib/rpath/lc -- and
# change_dylib, which is the same code -- refused every dylib outright.
cp "$T/fixture.dylib" "$T/dylibrw"
if mtip lc "$T/dylibrw" -delete uuid >"$T/dylibrw.out" 2>&1; then
    ok "lc -delete: a dylib is rewritable"
else
    bad "lc -delete: a dylib is rewritable" "refused: $(cat "$T/dylibrw.out")"
fi

# ============================================================================
# info
# ============================================================================
build_main "$T/info_fixture"
info_out=$("$MACHOTOOL" info "$T/info_fixture") || bad "info: exit" "nonzero"
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
# mtip because this verb reads FILE and writes OUT now, and the assertions
# below (and the "does it still run?" one further down) are about the grown
# image being AT $T/grow_fixture: the helper runs the verb into a temp beside
# the file and mv's it over, which is what every caller that wants the old
# in-place behaviour has to do.
mtip grow "$T/grow_fixture" 4096 >"$T/grow.out" || bad "grow: exit" "$(cat "$T/grow.out")"
after=$(wc -c < "$T/grow_fixture")
if [ "$after" -eq "$((before + 4096))" ]; then
    ok "grow: file grew by exactly the page-aligned request"
else
    bad "grow: size" "before=$before after=$after (expected +4096)"
fi
"$MACHOTOOL" verify "$T/grow_fixture" >/dev/null && ok "grow: result still verifies" \
    || bad "grow: post-grow verify" "failed"

# grow NEVER WRITES ITS INPUT: `grow FILE OUT N`. The same five facts every
# other converted verb is held to (see the nwi block further down, whose
# wording this follows), with grow's own success line -- `Grew OUT: ...`, not
# `Wrote OUT (...)` -- and its own proof that OUT carries the change: OUT is
# exactly N bytes bigger.
build_main "$T/gnwi"
gnwi_sha=$(sha "$T/gnwi"); gnwi_ino=$(stat -f %i "$T/gnwi")
gnwi_before=$(wc -c < "$T/gnwi")
rm -f "$T/gnwi_out"
"$MACHOTOOL" grow "$T/gnwi" "$T/gnwi_out" 4096 >"$T/gnwi.out" 2>"$T/gnwi.err" \
    && ok "grow FILE OUT N: succeeds" \
    || bad "grow FILE OUT N" "$(cat "$T/gnwi.err")"
[ "$(sha "$T/gnwi")" = "$gnwi_sha" ] && [ "$(stat -f %i "$T/gnwi")" = "$gnwi_ino" ] \
    && ok "grow FILE OUT N: FILE is untouched, bytes and inode" \
    || bad "grow FILE OUT N" "FILE changed"
[ "$(wc -c < "$T/gnwi_out")" -eq "$((gnwi_before + 4096))" ] \
    && ok "grow FILE OUT N: OUT is the grown image" \
    || bad "grow FILE OUT N" "OUT is $(wc -c < "$T/gnwi_out") bytes, want $((gnwi_before + 4096))"
grep -q "^Grew $T/gnwi_out: " "$T/gnwi.out" \
    && ok "grow FILE OUT N: says what it wrote, naming OUT" \
    || bad "grow FILE OUT N" "no Grew line naming OUT: $(cat "$T/gnwi.out")"
rc=0
"$MACHOTOOL" grow "$T/gnwi" "$T/gnwi" 4096 >/dev/null 2>"$T/gnwi_same.err" || rc=$?
[ "$rc" -eq 2 ] && [ "$(sha "$T/gnwi")" = "$gnwi_sha" ] \
    && ok "grow: an OUT that is FILE is refused (2), FILE untouched" \
    || bad "grow OUT=FILE" "rc $rc"
grep -q "never writes its input" "$T/gnwi_same.err" \
    && ok "grow: ... refused up front, before any work" \
    || bad "grow OUT=FILE" "not the up-front refusal: $(cat "$T/gnwi_same.err")"
rm -f "$T/gnwi_link"; ln -s "$T/gnwi" "$T/gnwi_link"
rc=0
"$MACHOTOOL" grow "$T/gnwi" "$T/gnwi_link" 4096 >/dev/null 2>&1 || rc=$?
[ "$rc" -eq 2 ] && ok "grow: an OUT that is a symlink to FILE is refused (2)" \
    || bad "grow OUT=link" "rc $rc"
rc=0
"$MACHOTOOL" grow "$T/gnwi" 4096 >/dev/null 2>&1 || rc=$?
[ "$rc" -eq 2 ] && ok "grow: a missing OUT is an error (2)" \
    || bad "grow no OUT" "rc $rc"

# grow writes OUT through wa_write_new (src/atomic_write.h, mkstemp+rename).
# Prove the symlink-safety that buys, which now belongs to OUT rather than to
# FILE: an OUT that is a symlink is FOLLOWED -- the real target gets the new
# bytes (a fresh inode, since rename() always creates one) and the symlink
# stays a symlink, where a naive rename onto the symlink's own path would
# replace the link with a plain file.
build_main "$T/grow_link_in"
build_main "$T/grow_link_target"
ln -sf grow_link_target "$T/grow_link"
target_ino_before=$(stat -f %i "$T/grow_link_target")
target_size_before=$(wc -c < "$T/grow_link_target")
"$MACHOTOOL" grow "$T/grow_link_in" "$T/grow_link" 4096 >"$T/grow_link.out" 2>&1 \
    || bad "grow: symlinked OUT" "exit failed: $(cat "$T/grow_link.out")"
if [ -L "$T/grow_link" ]; then
    ok "grow: an OUT that is a symlink stays a symlink"
else
    bad "grow: symlinked OUT" "the symlink itself got replaced by a plain file"
fi
target_ino_after=$(stat -f %i "$T/grow_link_target")
if [ "$target_ino_after" != "$target_ino_before" ]; then
    ok "grow: the real target was replaced via mkstemp+rename (fresh inode = atomicity kept)"
else
    bad "grow: symlinked OUT" "target inode unchanged -- wrote in place, not atomically"
fi
[ "$(wc -c < "$T/grow_link_target")" -ne "$target_size_before" ] \
    && ok "grow: ... and it is the target that got the grown image" \
    || bad "grow: symlinked OUT" "the target's size did not change"
readlink "$T/grow_link" | grep -q "^grow_link_target$" \
    && ok "grow: symlink still points at the same name" \
    || bad "grow: symlinked OUT" "symlink target changed: $(readlink "$T/grow_link")"

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
        bad "grow: run" "grown binary failed to execute ON THE TARGET PLATFORM ITSELF (Darwin 13 / Mac OS X 10.9) -- this is a real machotool defect, not a portability question. exit $grow_run_rc: $grow_run_diag"
    else
        skip "grow: grown binary still runs" \
            "not the product's target platform (Darwin $darwin_major; the target is Darwin 13 / Mac OS X 10.9) -- mg_grow_header's image-base-lowering trick is only promised to load there. This host's loader says: exit $grow_run_rc: $grow_run_diag"
    fi
fi
# N=0 is refused, not silently a no-op -- and with a real OUT, so this asks
# about N rather than about the argument count.
rm -f "$T/grow_zero_out"
if "$MACHOTOOL" grow "$T/grow_fixture" "$T/grow_zero_out" 0 >/dev/null 2>&1; then
    bad "grow: N=0" "should be refused"
else
    ok "grow: N=0 refused"
fi
[ -e "$T/grow_zero_out" ] && bad "grow: N=0" "wrote an output for a request it refused" \
    || ok "grow: N=0 produces no output file"

# ============================================================================
# minos
# ============================================================================
# build_main's FIXTURE_FLAGS (-mmacosx-version-min=10.9) makes the linker
# emit LC_VERSION_MIN_MACOSX itself -- so a fixture built that way already
# HAS the load command machotool minos is supposed to add, and the "happy
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
# on a specific ld/clang version, and nothing here depends on machotool or
# change_dylib's own strip machinery either (their -strip-lc/`lc -delete`
# vocabulary doesn't cover LC_VERSION_MIN_MACOSX today, and reusing the
# tool under test to build that test's own fixture would be circular
# regardless). The fixture is therefore test-tool-constructed, not
# linker-constructed, for this one load command only.
#
# The program that does it is tests/strip_version_min.c, a file rather than a
# here-document because tests/wrapper_test.sh needs exactly the same fixture
# for exactly the same reason, and one copy of it is enough.
"$CC" -O2 -o "$T/strip_version_min" "$HERE/strip_version_min.c"

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
# ONE load command machotool minos adds, and the thing the "present after"
# assertion below checks for. LC_BUILD_VERSION is a DIFFERENT load command a
# modern linker emits instead (add_version_min.c only ever looks for
# LC_VERSION_MIN_MACOSX, so LC_BUILD_VERSION's presence is orthogonal to
# this test, not a disqualifier) -- asserting its absence too would be
# asserting something about LC_BUILD_VERSION this test does not need and
# cannot always get.
before_minos=$("$MACHOTOOL" info "$T/minos_fixture")
if echo "$before_minos" | grep -q "LC_VERSION_MIN_MACOSX"; then
    bad "minos: precondition" "fixture still carries LC_VERSION_MIN_MACOSX"
else
    ok "minos: fixture genuinely has no LC_VERSION_MIN_MACOSX before"
fi

"$MACHOTOOL" minos "$T/minos_fixture" "$T/minos_out" 10.9 >"$T/minos.out" \
    || bad "minos: exit" "$(cat "$T/minos.out")"
minos_info=$("$MACHOTOOL" info "$T/minos_out")
echo "$minos_info" | grep -q "LC_VERSION_MIN_MACOSX" && ok "minos: LC_VERSION_MIN_MACOSX present after" \
    || bad "minos: version-min" "not found in info output"
# Running it again must not error (add_version_min's own "already present"
# path) -- this time reading the output of the run above, which HAS the
# command, so the second run really takes that branch.
if "$MACHOTOOL" minos "$T/minos_out" "$T/minos_out2" 10.9 >/dev/null 2>&1; then
    ok "minos: idempotent re-run does not error"
else
    bad "minos: re-run" "errored on an already-minos'd file"
fi
# Any other version is refused up front -- this build can only target 10.9.
if "$MACHOTOOL" minos "$T/minos_fixture" "$T/minos_out3" 10.10 >/dev/null 2>&1; then
    bad "minos: wrong version" "10.10 should be refused"
else
    ok "minos: non-10.9 version refused"
fi

# minos never writes its input: FILE OUT, and an OUT that is FILE is refused.
build_main "$T/mo_in"; "$T/strip_version_min" "$T/mo_in" >/dev/null
mo_before=$(sha "$T/mo_in"); mo_ino=$(stat -f %i "$T/mo_in")
"$MACHOTOOL" minos "$T/mo_in" "$T/mo_out" 10.9 >"$T/mo.out" 2>"$T/mo.err" \
    && ok "minos FILE OUT: succeeds" || bad "minos FILE OUT" "$(cat "$T/mo.err")"
[ "$(sha "$T/mo_in")" = "$mo_before" ] && [ "$(stat -f %i "$T/mo_in")" = "$mo_ino" ] \
    && ok "minos FILE OUT: FILE is untouched" || bad "minos FILE OUT" "FILE changed"
"$MACHOTOOL" info "$T/mo_out" | grep -q LC_VERSION_MIN_MACOSX \
    && ok "minos FILE OUT: OUT has the command" || bad "minos FILE OUT" "OUT lacks it"
grep -q "^Wrote $T/mo_out (" "$T/mo.out" \
    && ok "minos FILE OUT: says what it wrote" || bad "minos FILE OUT" "no Wrote line: $(cat "$T/mo.out")"
rc=0; "$MACHOTOOL" minos "$T/mo_in" "$T/mo_in" 10.9 >/dev/null 2>"$T/mo_same.err" || rc=$?
[ "$rc" -eq 2 ] && [ "$(sha "$T/mo_in")" = "$mo_before" ] \
    && ok "minos: OUT that is FILE is refused (2), FILE untouched" || bad "minos OUT=FILE" "rc $rc"
grep -q "never writes its input" "$T/mo_same.err" \
    && ok "minos: ... refused up front, before any work" \
    || bad "minos OUT=FILE" "not the up-front refusal: $(cat "$T/mo_same.err")"
ln -s "$T/mo_in" "$T/mo_link"
rc=0; "$MACHOTOOL" minos "$T/mo_in" "$T/mo_link" 10.9 >/dev/null 2>&1 || rc=$?
[ "$rc" -eq 2 ] && ok "minos: OUT that is a symlink to FILE is refused (2)" || bad "minos OUT=link" "rc $rc"
rc=0; "$MACHOTOOL" minos "$T/mo_in" 10.9 >/dev/null 2>&1 || rc=$?
[ "$rc" -eq 2 ] && ok "minos: a missing OUT is a usage error (2)" || bad "minos no OUT" "rc $rc"

# ============================================================================
# lc -delete
# ============================================================================
build_main "$T/lc_fixture"
before_info=$("$MACHOTOOL" info "$T/lc_fixture")
echo "$before_info" | grep -q "LC_UUID" && ok "lc: fixture has LC_UUID before" \
    || bad "lc: precondition" "fixture has no LC_UUID to delete"
mtip lc "$T/lc_fixture" -delete uuid >"$T/lc.out" || bad "lc: exit" "$(cat "$T/lc.out")"
after_info=$("$MACHOTOOL" info "$T/lc_fixture")
if echo "$after_info" | grep -q "LC_UUID"; then
    bad "lc: delete uuid" "LC_UUID still present"
else
    ok "lc: delete uuid removed it"
fi
# Whether a binary that's had its LC_UUID deleted can still be EXECUTED
# turns on TWO independent host facts, not on machotool: (a) kernel
# code-signing enforcement, killing ANY binary modified since it was
# signed -- see $signing_enforced, established above without ever running
# machotool; (b) modern dyld separately refusing to load an image with no
# LC_UUID at all ("missing LC_UUID load command"), which 10.9's dyld does
# not require. These showed up as genuinely different failure modes on the
# cross runner that motivated this (grow got SIGKILLed outright; this got
# far enough for dyld itself to abort on the missing UUID).
#
# signing_enforced already answers (a) honestly. For (b), run lc_fixture
# for real and read its OWN failure, rather than inferring it from a
# separate machotool-produced probe (the same masking risk as grow's old
# probe): only a failure whose message literally names the missing-LC_UUID
# refusal is treated as (b) and skipped; anything else, with signing
# already ruled out, is a real defect and FAILS.
if [ "$signing_enforced" -eq 1 ]; then
    skip "lc: binary still runs after uuid deletion" \
        "this host SIGKILLs any binary modified since it was signed at link time (established independently of machotool by the host probe above); a host policy, not a machotool defect, and exercised for real on 10.9"
else
    if (cd "$T" && ./lc_fixture) >"$T/lc_fixture_run.out" 2>&1; then
        ok "lc: binary still runs after uuid deletion"
    else
        lc_run_rc=$?
        if grep -qi "missing LC_UUID" "$T/lc_fixture_run.out" 2>/dev/null; then
            skip "lc: binary still runs after uuid deletion" \
                "modern dyld refuses to load any image with no LC_UUID at all ('missing LC_UUID load command'); 10.9's dyld has no such requirement. Code-signing enforcement was independently ruled out above (a trivially-perturbed binary DID run on this host), so this is dyld's own content-driven refusal, not a masked machotool defect"
        else
            bad "lc: run" "binary failed to execute after uuid deletion (exit $lc_run_rc: $(head -1 "$T/lc_fixture_run.out" 2>/dev/null || echo 'no output')), this host DOES run a trivially-perturbed binary fine (see host probe above), and dyld did not report its missing-LC_UUID message -- code-signing and the known dyld requirement are both ruled out, so this looks like a real machotool defect"
        fi
    fi
fi
# Unknown KIND is refused with this verb's own message, before the rewriter
# is ever called -- so a bad KIND never reaches (or is diagnosed by) code
# shared with change_dylib.
if mtip lc "$T/lc_fixture" -delete bogus-kind >/dev/null 2>"$T/lc_bad.err"; then
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
mtip lc "$T/lc_miss_fixture" -delete build-version \
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
mtip lc "$T/lc_dup_fixture" -delete uuid -delete uuid \
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
mtip lc "$T/lc_fw_fixture" --fatal-warnings -delete build-version \
    >/dev/null 2>"$T/lc_fw.err" && lc_fw_rc=0 || lc_fw_rc=$?
[ "$lc_fw_rc" -eq 1 ] && ok "lc: --fatal-warnings refuses when a KIND matched nothing (EX_REFUSED)" \
    || bad "lc: --fatal-warnings refusal" "expected exit 1, got $lc_fw_rc: $(cat "$T/lc_fw.err")"
grep -q "no load command of kind build-version to delete" "$T/lc_fw.err" \
    && ok "lc: --fatal-warnings still names the KIND that matched nothing" \
    || bad "lc: --fatal-warnings refusal message" "expected 'no load command of kind build-version to delete', got: $(cat "$T/lc_fw.err")"
# Without --fatal-warnings, the identical invocation still succeeds -- so the
# flag is what changed the answer, not something else about this fixture.
build_main_without_build_version "$T/lc_fw_lax_fixture"
mtip lc "$T/lc_fw_lax_fixture" -delete build-version \
    >/dev/null 2>/dev/null && lc_fw_lax_rc=0 || lc_fw_lax_rc=$?
[ "$lc_fw_lax_rc" -eq 0 ] && ok "lc: without --fatal-warnings the same unmatched KIND still succeeds" \
    || bad "lc: no --fatal-warnings" "expected 0, got $lc_fw_lax_rc"
# And when every -delete DOES match, --fatal-warnings must not refuse a run
# that had nothing to complain about.
build_main "$T/lc_fw_ok_fixture"
mtip lc "$T/lc_fw_ok_fixture" --fatal-warnings -delete uuid \
    >/dev/null 2>"$T/lc_fw_ok.err" && lc_fw_ok_rc=0 || lc_fw_ok_rc=$?
[ "$lc_fw_ok_rc" -eq 0 ] && ok "lc: --fatal-warnings succeeds when the KIND matched" \
    || bad "lc: --fatal-warnings (matched)" "expected 0, got $lc_fw_ok_rc: $(cat "$T/lc_fw_ok.err")"

# ============================================================================
# dylib: --allow-grow alone (no operation) must be refused with MACHOTOOL's
# OWN usage, not change_dylib's.
#
# --allow-grow used to count toward the "need at least one operation" guard
# (k, which included it), so this exact invocation fell through to
# change_dylib and printed ITS usage -- leaking the -change/-add/-strip-lc/
# -add-rpath spellings this grammar deliberately does not offer (see
# cmd_dylib_or_rpath's `nops` counter in cli/machotool.c). Regression test for
# that fix: no cli_test.sh assertion existed for it before.
# ============================================================================
build_main "$T/dylib_noop_fixture"
if mtip dylib "$T/dylib_noop_fixture" --allow-grow >/dev/null 2>"$T/dylib_noop.err"; then
    bad "dylib: --allow-grow alone" "should be refused (no operation given)"
else
    ok "dylib: --allow-grow alone is refused"
fi
grep -q "need at least one operation" "$T/dylib_noop.err" && ok "dylib: --allow-grow alone prints machotool's own usage" \
    || bad "dylib: --allow-grow alone message" "missing machotool's 'need at least one operation'"
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
mtip dylib "$T/dylib_fixture" -replace "@loader_path/liba.dylib" "$newpath" \
    >"$T/dylib.out" || bad "dylib: -replace exit" "$(cat "$T/dylib.out")"
dylib_info=$("$MACHOTOOL" info "$T/dylib_fixture")
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
rc=0
mtip dylib "$T/dylib_grow_fixture" -replace "@loader_path/liba.dylib" "$longpath" \
    >/dev/null 2>"$T/dylib_grow.err" || rc=$?
# A considered refusal (mr_process_thin examined the header pad, decided
# the new load commands do not fit, and declined without --allow-grow to
# widen it) is MR_REFUSED, forwarded verbatim as EX_REFUSED.
[ "$rc" -eq 1 ] && ok "dylib: long path without --allow-grow is refused (EX_REFUSED)" \
    || bad "dylib: long path without --allow-grow" "expected exit 1, got $rc: $(cat "$T/dylib_grow.err")"
if mtip dylib "$T/dylib_grow_fixture" --allow-grow -replace "@loader_path/liba.dylib" "$longpath" \
    >"$T/dylib_grow.out"; then
    ok "dylib: --allow-grow lets the same replace through"
else
    bad "dylib: --allow-grow" "$(cat "$T/dylib_grow.out")"
fi
grown_info=$("$MACHOTOOL" info "$T/dylib_grow_fixture")
echo "$grown_info" | grep -qF "path=$longpath" && ok "dylib: --allow-grow result has the long path" \
    || bad "dylib: --allow-grow result" "long path not found"

# ============================================================================
# dylib: pinning the MR_REFUSED/MR_FAIL split (rewrite.h) through mr_apply_file
# and mi_open, which reaching this verb from machotool's own EX_REFUSED/EX_FAIL
# checks never exercised. Without these, reverting the reclassification in
# src/rewrite.c leaves this whole suite green -- confirmed by temporarily
# reverting the 64-bit-fat classification below and watching this section's
# own assertion catch it, then reverting the mutation.
# ============================================================================

# A non-Mach-O file: mi_open reads it fine (no I/O failure at all) and
# mi_validate declines it -- MI_NOT_MACHO, forwarded as MR_REFUSED, EX_REFUSED.
echo 'not a mach-o, just bytes' > "$T/dylib_notmacho"
rc=0
mtip dylib "$T/dylib_notmacho" -replace /usr/lib/libSystem.B.dylib /tmp/x.dylib \
    >/dev/null 2>"$T/dylib_notmacho.err" || rc=$?
[ "$rc" -eq 1 ] && ok "dylib: a non-Mach-O file is refused (EX_REFUSED)" \
    || bad "dylib: non-Mach-O" "expected exit 1, got $rc: $(cat "$T/dylib_notmacho.err")"

# An absent file: mr_apply_file's own open() fails before mi_open is ever
# reached -- a genuine syscall failure, MR_FAIL, EX_FAIL.
rc=0
mtip dylib "$T/no-such-file-for-dylib" -replace /usr/lib/libSystem.B.dylib /tmp/x.dylib \
    >/dev/null 2>"$T/dylib_absent.err" || rc=$?
[ "$rc" -eq 2 ] && ok "dylib: an absent file is a failure, not a refusal (EX_FAIL)" \
    || bad "dylib: absent file" "expected exit 2, got $rc: $(cat "$T/dylib_absent.err")"

# A 64-bit fat container (fat_arch_64 -- FAT_MAGIC_64/FAT_CIGAM_64, arm64e/
# watchOS-style wide offsets). mr_apply_file recognizes this from the first
# 4 bytes alone, before any further read, so the fixture needs nothing past
# that magic to exercise the check -- cheap to build: FAT_MAGIC_64 is
# 0xcafebabf (src/mach_compat.h), and on this host's native byte order that
# is the 4 bytes 0277 0272 0376 0312 (octal), file-order low-to-high.
printf '%b' '\0277\0272\0376\0312' > "$T/dylib_fat64"
rc=0
mtip dylib "$T/dylib_fat64" -replace /usr/lib/libSystem.B.dylib /tmp/x.dylib \
    >/dev/null 2>"$T/dylib_fat64.err" || rc=$?
[ "$rc" -eq 1 ] && ok "dylib: a 64-bit fat container is refused, not merely failed (EX_REFUSED)" \
    || bad "dylib: 64-bit fat" "expected exit 1, got $rc: $(cat "$T/dylib_fat64.err")"
grep -q "fat_arch_64" "$T/dylib_fat64.err" \
    && ok "dylib: names the 64-bit fat container as the reason" \
    || bad "dylib: 64-bit fat message" "no mention of fat_arch_64: $(cat "$T/dylib_fat64.err")"

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
unmatched_out=$(mtip dylib "$T/unmatched_fixture" \
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
# -change/-delete into one machotool invocation, reached through this same
# code path.
build_main "$T/dylib_conflict_fixture"
conflict_path="@loader_path/libconflict.dylib"
mtip dylib "$T/dylib_conflict_fixture" -append "$conflict_path" \
    >/dev/null || bad "dylib: conflict fixture setup" "-append of $conflict_path failed"
mtip dylib "$T/dylib_conflict_fixture" \
        -replace "$conflict_path" /also/absent.dylib \
        -delete "$conflict_path" \
        >"$T/conflict.out" 2>"$T/conflict.err" && conflict_rc=0 || conflict_rc=$?
[ "$conflict_rc" -eq 0 ] && ok "dylib: -replace and -delete on the same path still exits 0" \
    || bad "dylib: -replace+-delete same path" "expected 0, got $conflict_rc: $(cat "$T/conflict.err")"
conflict_info=$("$MACHOTOOL" info "$T/dylib_conflict_fixture")
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
# one of which cannot -- so this also proves that a refused run WRITES
# NOTHING, even though the rewrite itself succeeded: the verdict is decided
# before mr_apply_file's wa_write_new, so OUT is never created and the op that
# DID match lands nowhere.
#
# THIS ASSERTION USED TO SAY THE OPPOSITE. While the verb rewrote FILE, the
# write came last and was conditional on something having changed, so a mixed
# run wrote FILE and then refused; only an all-miss run (below) wrote nothing.
# A verb that writes OUT unconditionally cannot keep that order without
# leaving an output behind on a refusal, which is precisely what "refused"
# must not mean -- so the verdict moved ahead of the write, and both cases now
# give the same answer. Run WITHOUT mtip: whether OUT exists is the point.
# ============================================================================
build_main "$T/dylib_fw_fixture"
cp "$T/dylib_fw_fixture" "$T/dylib_fw_before"
rm -f "$T/dylib_fw_out"
"$MACHOTOOL" dylib "$T/dylib_fw_fixture" "$T/dylib_fw_out" --fatal-warnings \
        -replace "@loader_path/liba.dylib" "@loader_path/renamed-fw.dylib" \
        -replace /nope/absent-fw.dylib /also/absent-fw.dylib \
        >"$T/dylib_fw.out" 2>"$T/dylib_fw.err" && dylib_fw_rc=0 || dylib_fw_rc=$?
[ "$dylib_fw_rc" -eq 1 ] && ok "dylib: --fatal-warnings refuses an unmatched op (EX_REFUSED)" \
    || bad "dylib: --fatal-warnings refusal" "expected exit 1, got $dylib_fw_rc: $(cat "$T/dylib_fw.err")"
grep -qF "/nope/absent-fw.dylib" "$T/dylib_fw.err" && ok "dylib: --fatal-warnings still names the op that matched nothing" \
    || bad "dylib: --fatal-warnings refusal message" "expected /nope/absent-fw.dylib on stderr, got: $(cat "$T/dylib_fw.err")"
[ ! -e "$T/dylib_fw_out" ] \
    && ok "dylib: --fatal-warnings wrote no OUT, though one operation did match" \
    || bad "dylib: --fatal-warnings wrote OUT" "a refused run left $T/dylib_fw_out behind: $("$MACHOTOOL" info "$T/dylib_fw_out")"
cmp -s "$T/dylib_fw_fixture" "$T/dylib_fw_before" \
    && ok "dylib: --fatal-warnings left FILE byte-for-byte untouched" \
    || bad "dylib: --fatal-warnings touched FILE" "FILE changed under a verb that only reads it"
# Without --fatal-warnings, the identical invocation still succeeds -- so the
# flag is what changed the answer, not something else about this fixture.
build_main "$T/dylib_fw_lax_fixture"
mtip dylib "$T/dylib_fw_lax_fixture" \
        -replace "@loader_path/liba.dylib" "@loader_path/renamed-fw-lax.dylib" \
        -replace /nope/absent-fw.dylib /also/absent-fw.dylib \
        >/dev/null 2>/dev/null && dylib_fw_lax_rc=0 || dylib_fw_lax_rc=$?
[ "$dylib_fw_lax_rc" -eq 0 ] && ok "dylib: without --fatal-warnings the same unmatched op still succeeds" \
    || bad "dylib: no --fatal-warnings" "expected 0, got $dylib_fw_lax_rc"
# And when every op DOES match, --fatal-warnings must not refuse a run that
# had nothing to complain about.
build_main "$T/dylib_fw_ok_fixture"
mtip dylib "$T/dylib_fw_ok_fixture" --fatal-warnings \
        -replace "@loader_path/liba.dylib" "@loader_path/renamed-fw-ok.dylib" \
        >"$T/dylib_fw_ok.out" 2>"$T/dylib_fw_ok.err" && dylib_fw_ok_rc=0 || dylib_fw_ok_rc=$?
[ "$dylib_fw_ok_rc" -eq 0 ] && ok "dylib: --fatal-warnings succeeds when nothing is unmatched" \
    || bad "dylib: --fatal-warnings (matched)" "expected 0, got $dylib_fw_ok_rc: $(cat "$T/dylib_fw_ok.err")"

# EVERY operation matches nothing, not just one of several -- the case that
# used to be the only one where a --fatal-warnings refusal wrote nothing,
# because mr_process_thin's "nothing to change" early return left *out_modified
# at 0 and the conditional write never ran. It is no longer the special case:
# the assertion above now says the same thing about a run where one operation
# DID match. Kept, because the two reach the refusal by different routes and
# both must end with no OUT.
build_main "$T/dylib_fw_allmiss_fixture"
cp "$T/dylib_fw_allmiss_fixture" "$T/dylib_fw_allmiss_before"
rm -f "$T/dylib_fw_allmiss_out"
"$MACHOTOOL" dylib "$T/dylib_fw_allmiss_fixture" "$T/dylib_fw_allmiss_out" --fatal-warnings \
        -replace /nope/absent-fw-allmiss.dylib /also/absent-fw-allmiss.dylib \
        >/dev/null 2>"$T/dylib_fw_allmiss.err" && dylib_fw_allmiss_rc=0 || dylib_fw_allmiss_rc=$?
[ "$dylib_fw_allmiss_rc" -eq 1 ] && ok "dylib: --fatal-warnings refuses when EVERY op matched nothing" \
    || bad "dylib: --fatal-warnings (all miss)" "expected exit 1, got $dylib_fw_allmiss_rc: $(cat "$T/dylib_fw_allmiss.err")"
[ ! -e "$T/dylib_fw_allmiss_out" ] \
    && ok "dylib: --fatal-warnings wrote no OUT when nothing at all matched" \
    || bad "dylib: --fatal-warnings (all miss)" "a refused run left an OUT behind"
cmp -s "$T/dylib_fw_allmiss_fixture" "$T/dylib_fw_allmiss_before" \
    && ok "dylib: --fatal-warnings left the file byte-for-byte untouched when nothing at all matched" \
    || bad "dylib: --fatal-warnings (all miss)" "the file was modified despite every operation matching nothing"

# segment and retag-swift take no list of operations that could miss, so
# neither parses --fatal-warnings at all -- passing it lands as an extra
# positional argument and is refused the same way any wrong argument count
# is, with each verb's own usage line, not a --fatal-warnings-specific
# message (there is nothing to be specific about: the flag was never seen).
build_main "$T/segment_fw_fixture"
mtip segment "$T/segment_fw_fixture" __DATA __DATA_R9 --fatal-warnings \
    >/dev/null 2>"$T/segment_fw.err" \
    && bad "segment: --fatal-warnings" "should be refused (segment takes exactly FILE OUT OLD NEW)" \
    || { grep -q "usage:" "$T/segment_fw.err" \
         && ok "segment: does not accept --fatal-warnings (refused as a usage error)" \
         || bad "segment: --fatal-warnings" "refused, but not with a usage message: $(cat "$T/segment_fw.err")"; }
"$MACHOTOOL" retag-swift "$T/segment_fw_fixture" "$T/segment_fw_fixture.rsout" --fatal-warnings \
    >/dev/null 2>"$T/retag_fw.err" \
    && bad "retag-swift: --fatal-warnings" "should be refused (retag-swift takes exactly FILE OUT)" \
    || { grep -q "usage:" "$T/retag_fw.err" \
         && ok "retag-swift: does not accept --fatal-warnings (refused as a usage error)" \
         || bad "retag-swift: --fatal-warnings" "refused, but not with a usage message: $(cat "$T/retag_fw.err")"; }

# ============================================================================
# dylib -append / -insert / -delete / -reexport
#
# -replace and --allow-grow (above) exercise only two of change_dylib's
# translation targets. The mapping itself -- machotool's flag to change_dylib's
# -- is the only new logic dylib/rpath add, so every op needs its own
# observable check, not just an exit code: a swapped mapping (say -append
# landing on change_dylib's -insert) would ship silently and INVERT dylib
# initialization order, which is the whole reason -insert exists (see
# docs/PROPOSAL.md "Why these names"). None of these dylibs need to exist on
# disk -- only the load-command rewrite is being checked here, via `machotool
# info`, never by running the binary.
# ============================================================================
spare="@loader_path/libspare.dylib"

# -append places the new dependency LAST -- after every existing one,
# INCLUDING the implicit libSystem.B.dylib the linker adds on its own, which
# is why this checks "highest ordinal in the file" rather than a hardcoded
# number (build_main's plain main.c still needs libSystem for _start/crt,
# so liba=1, libSystem=2, and spare correctly lands at 3, not 2).
build_main "$T/dylib_append_fixture"
before_append_info=$("$MACHOTOOL" info "$T/dylib_append_fixture")
last_ordinal_before=$(echo "$before_append_info" | grep -o "ordinal=[0-9]*" | sed 's/ordinal=//' | sort -n | tail -1)
mtip dylib "$T/dylib_append_fixture" -append "$spare" \
    >"$T/dylib_append.out" || bad "dylib: -append exit" "$(cat "$T/dylib_append.out")"
append_info=$("$MACHOTOOL" info "$T/dylib_append_fixture")
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
mtip dylib "$T/dylib_insert_fixture" -insert "$spare" \
    >"$T/dylib_insert.out" || bad "dylib: -insert exit" "$(cat "$T/dylib_insert.out")"
insert_info=$("$MACHOTOOL" info "$T/dylib_insert_fixture")
echo "$insert_info" | grep -qF "ordinal=1 path=$spare" && ok "dylib: -insert put the new dep at ordinal 1 (first)" \
    || bad "dylib: -insert" "expected ordinal=1 path=$spare in: $insert_info"
echo "$insert_info" | grep -qF "ordinal=2 path=@loader_path/liba.dylib" && ok "dylib: -insert renumbered liba to ordinal 2" \
    || bad "dylib: -insert (liba)" "expected liba renumbered to ordinal 2 in: $insert_info"

# -delete removes the dependency and renumbers survivors; reuses the
# -append fixture above (liba=1, spare=2) so deleting the UNUSED spare
# (never called, so nothing binds to it -- change_dylib refuses a -delete
# that would orphan a bound symbol) proves removal without disturbing liba.
mtip dylib "$T/dylib_append_fixture" -delete "$spare" \
    >"$T/dylib_delete.out" || bad "dylib: -delete exit" "$(cat "$T/dylib_delete.out")"
delete_info=$("$MACHOTOOL" info "$T/dylib_append_fixture")
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
mtip dylib "$T/dylib_reexport_fixture" -reexport "@loader_path/liba.dylib" \
    >"$T/dylib_reexport.out" || bad "dylib: -reexport exit" "$(cat "$T/dylib_reexport.out")"
reexport_info=$("$MACHOTOOL" info "$T/dylib_reexport_fixture")
echo "$reexport_info" | grep -A1 "LC_REEXPORT_DYLIB" | grep -qF "path=@loader_path/liba.dylib" \
    && ok "dylib: -reexport promoted liba to LC_REEXPORT_DYLIB" \
    || bad "dylib: -reexport" "no LC_REEXPORT_DYLIB naming liba in: $reexport_info"

# ============================================================================
# rpath -append
# ============================================================================
build_main "$T/rpath_fixture" "/tmp/cli_test_original_rpath"
before_rp=$("$MACHOTOOL" info "$T/rpath_fixture")
echo "$before_rp" | grep -q "rpath=/tmp/cli_test_original_rpath" && ok "rpath: fixture has original rpath" \
    || bad "rpath: precondition" "original rpath missing from info output"
mtip rpath "$T/rpath_fixture" -append "/tmp/cli_test_appended_rpath" \
    >"$T/rpath.out" || bad "rpath: -append exit" "$(cat "$T/rpath.out")"
after_rp=$("$MACHOTOOL" info "$T/rpath_fixture")
echo "$after_rp" | grep -q "rpath=/tmp/cli_test_original_rpath" && \
echo "$after_rp" | grep -q "rpath=/tmp/cli_test_appended_rpath" && \
    ok "rpath: -append kept the original and added the new one" || \
    bad "rpath: -append" "expected both rpaths in: $after_rp"

# -replace rewrites a search path in place; -delete removes one outright --
# neither was exercised above (only -append was), and each maps to a
# distinct change_dylib flag (-change-rpath / -delete-rpath) that a swapped
# mapping could silently confuse with the dylib family's -change/-delete.
build_main "$T/rpath_replace_fixture" "/tmp/cli_test_replace_before"
mtip rpath "$T/rpath_replace_fixture" -replace "/tmp/cli_test_replace_before" "/tmp/cli_test_replace_after" \
    >"$T/rpath_replace.out" || bad "rpath: -replace exit" "$(cat "$T/rpath_replace.out")"
replace_info=$("$MACHOTOOL" info "$T/rpath_replace_fixture")
if echo "$replace_info" | grep -q "rpath=/tmp/cli_test_replace_before"; then
    bad "rpath: -replace" "old rpath still present in: $replace_info"
else
    ok "rpath: -replace removed the old search path"
fi
echo "$replace_info" | grep -q "rpath=/tmp/cli_test_replace_after" && ok "rpath: -replace added the new search path" \
    || bad "rpath: -replace (new)" "new rpath not found in: $replace_info"

build_main "$T/rpath_delete_fixture" "/tmp/cli_test_delete_me"
mtip rpath "$T/rpath_delete_fixture" -delete "/tmp/cli_test_delete_me" \
    >"$T/rpath_delete.out" || bad "rpath: -delete exit" "$(cat "$T/rpath_delete.out")"
delete_rp_info=$("$MACHOTOOL" info "$T/rpath_delete_fixture")
if echo "$delete_rp_info" | grep -q "^  rpath="; then
    bad "rpath: -delete" "an rpath is still present in: $delete_rp_info"
else
    ok "rpath: -delete removed the search path"
fi

# rpath -replace naming a search path the file does not have: the rpath twin
# of the dylib -replace miss report above.
build_main "$T/rpath_miss_fixture" "/tmp/cli_test_rpath_present"
mtip rpath "$T/rpath_miss_fixture" -replace "/tmp/cli_test_rpath_absent" "/tmp/cli_test_rpath_new" \
    >"$T/rpath_miss.out" 2>"$T/rpath_miss.err" && rpath_miss_rc=0 || rpath_miss_rc=$?
[ "$rpath_miss_rc" -eq 0 ] && ok "rpath: unmatched -replace still exits 0" \
    || bad "rpath: unmatched -replace" "expected 0, got $rpath_miss_rc: $(cat "$T/rpath_miss.err")"
grep -q "rpath /tmp/cli_test_rpath_absent matched nothing" "$T/rpath_miss.err" \
    && ok "rpath: names the -replace that matched nothing" \
    || bad "rpath: unmatched -replace" "expected 'rpath /tmp/cli_test_rpath_absent matched nothing' on stderr, got: $(cat "$T/rpath_miss.err")"
rpath_miss_info=$("$MACHOTOOL" info "$T/rpath_miss_fixture")
echo "$rpath_miss_info" | grep -q "rpath=/tmp/cli_test_rpath_present" \
    && ok "rpath: an untouched rpath is left alone by the unmatched -replace" \
    || bad "rpath: unmatched -replace" "the ORIGINAL rpath disappeared: $rpath_miss_info"

# rpath --fatal-warnings: the same miss report just above, turned into a
# refusal, the rpath twin of the dylib --fatal-warnings block above.
build_main "$T/rpath_fw_fixture" "/tmp/cli_test_rpath_fw_present"
mtip rpath "$T/rpath_fw_fixture" --fatal-warnings \
        -replace "/tmp/cli_test_rpath_fw_absent" "/tmp/cli_test_rpath_fw_new" \
        >/dev/null 2>"$T/rpath_fw.err" && rpath_fw_rc=0 || rpath_fw_rc=$?
[ "$rpath_fw_rc" -eq 1 ] && ok "rpath: --fatal-warnings refuses an unmatched op (EX_REFUSED)" \
    || bad "rpath: --fatal-warnings refusal" "expected exit 1, got $rpath_fw_rc: $(cat "$T/rpath_fw.err")"
grep -q "rpath /tmp/cli_test_rpath_fw_absent matched nothing" "$T/rpath_fw.err" \
    && ok "rpath: --fatal-warnings still names the op that matched nothing" \
    || bad "rpath: --fatal-warnings refusal message" "expected the miss message on stderr, got: $(cat "$T/rpath_fw.err")"
# Without --fatal-warnings, the identical invocation still succeeds.
build_main "$T/rpath_fw_lax_fixture" "/tmp/cli_test_rpath_fw_lax_present"
mtip rpath "$T/rpath_fw_lax_fixture" \
        -replace "/tmp/cli_test_rpath_fw_absent" "/tmp/cli_test_rpath_fw_new" \
        >/dev/null 2>/dev/null && rpath_fw_lax_rc=0 || rpath_fw_lax_rc=$?
[ "$rpath_fw_lax_rc" -eq 0 ] && ok "rpath: without --fatal-warnings the same unmatched op still succeeds" \
    || bad "rpath: no --fatal-warnings" "expected 0, got $rpath_fw_lax_rc"
# And when the op DOES match, --fatal-warnings must not refuse.
build_main "$T/rpath_fw_ok_fixture" "/tmp/cli_test_rpath_fw_ok_present"
mtip rpath "$T/rpath_fw_ok_fixture" --fatal-warnings \
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
# assertion below therefore compares POSITIONS in `machotool info`'s rpath list,
# never mere presence.
#
# `grep -n` over info's own stable "  rpath=" lines gives those positions
# without parsing otool.
rpath_positions() { "$MACHOTOOL" info "$1" | grep -n "^  rpath=" | sed 's/:.*rpath=/ /'; }
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

mtip rpath "$T/rpath_insert_fixture" -insert "/tmp/cli_test_inserted_rpath" \
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
mtip rpath "$T/rpath_append_cmp_fixture" -append "/tmp/cli_test_inserted_rpath" \
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
"$MACHOTOOL" info "$T/rpath_insert_empty" | grep -q "^  rpath=" \
    && bad "rpath -insert: empty precondition" "fixture unexpectedly already has an rpath" \
    || ok "rpath -insert: empty-case fixture has no rpath to start with"
mtip rpath "$T/rpath_insert_empty" -insert "/tmp/cli_test_empty_ins" -append "/tmp/cli_test_empty_app" \
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
        "this host SIGKILLs any binary modified since it was signed at link time (established independently of machotool by the host probe above)"
elif (cd "$T" && ./rpath_insert_fixture) >"$T/rpath_insert_run.out" 2>&1; then
    ok "rpath: -insert result still runs"
else
    bad "rpath: -insert result" "the binary no longer runs: $(cat "$T/rpath_insert_run.out")"
fi

# ============================================================================
# segment: rename every matching LC_SEGMENT_64, and its sections' copy
# ============================================================================
# `machotool info` prints a segment's segname but NOT the copy of that name each
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

mtip segment "$T/segment_fixture" __DATA __DATA_R9 \
    >"$T/segment.out" 2>&1 || bad "segment: exit" "$(cat "$T/segment.out")"
"$T/segread" segs "$T/segment_fixture" > "$T/segs_after"
grep -q "^SEG __DATA$" "$T/segs_after" \
    && bad "segment: the segment itself" "a segment is still named __DATA: $(cat "$T/segs_after")" \
    || ok "segment: renamed the LC_SEGMENT_64 itself"
grep -q "^SEG __DATA_R9$" "$T/segs_after" \
    && ok "segment: the new name is what landed" \
    || bad "segment: new name" "no __DATA_R9 segment in: $(cat "$T/segs_after")"
# The half `machotool info` cannot see: every section's own copy of the name.
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
        "this host SIGKILLs any binary modified since it was signed at link time (established independently of machotool by the host probe above)"
elif (cd "$T" && ./segment_fixture) >"$T/segment_run.out" 2>&1; then
    ok "segment: the renamed binary still runs"
else
    bad "segment: result" "the renamed binary no longer runs: $(cat "$T/segment_run.out")"
fi

# A NEW name of exactly 16 bytes fills the field with no room for a
# terminator -- the boundary rename_segment has always accepted, and the one a
# strcpy-based implementation gets wrong by writing a 17th byte.
"$CC" -O2 $FIXTURE_FLAGS "$T/segmain.c" -o "$T/segment_16_fixture"
mtip segment "$T/segment_16_fixture" __DATA ABCDEFGHIJKLMNOP \
    >"$T/segment16.out" 2>&1 || bad "segment: 16-byte name exit" "$(cat "$T/segment16.out")"
"$T/segread" segs "$T/segment_16_fixture" | grep -q "^SEG ABCDEFGHIJKLMNOP$" \
    && ok "segment: accepts a NEW name of exactly 16 bytes and writes it whole" \
    || bad "segment: 16-byte name" "got: $("$T/segread" segs "$T/segment_16_fixture")"
# 17 is one too many, and must be refused before any I/O.
"$CC" -O2 $FIXTURE_FLAGS "$T/segmain.c" -o "$T/segment_17_fixture"
cp "$T/segment_17_fixture" "$T/segment_17_before"
rc=0
mtip segment "$T/segment_17_fixture" __DATA ABCDEFGHIJKLMNOPQ \
    >"$T/segment17.out" 2>&1 || rc=$?
[ "$rc" -eq 1 ] && ok "segment: refuses a 17-byte NEW name with the documented refusal code" \
    || bad "segment: 17-byte name" "expected exit 1, got $rc: $(cat "$T/segment17.out")"
cmp -s "$T/segment_17_fixture" "$T/segment_17_before" \
    && ok "segment: a refused rename left the file byte-for-byte unchanged" \
    || bad "segment: 17-byte name" "the file was modified despite the refusal"

# A segment name nothing matches must leave the file alone -- and say so.
"$CC" -O2 $FIXTURE_FLAGS "$T/segmain.c" -o "$T/segment_nomatch_fixture"
cp "$T/segment_nomatch_fixture" "$T/segment_nomatch_before"
mtip segment "$T/segment_nomatch_fixture" __NOSUCHSEG __OTHER \
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
mtip segment "$T/segment_fat" __DATA __DATA_R9 \
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
"$MACHOTOOL" verify "$T/implausible" >/dev/null 2>"$T/imp_verify.err" || true
grep -q 'implausible' "$T/imp_verify.err" \
    && ok "segment: the fixture really is one mg_plausible rejects" \
    || bad "segment: mg_plausible fixture" "machotool verify did not call it implausible: $(cat "$T/imp_verify.err")"

# An ordinary operation on it still meets the gate and is refused, with the
# input left alone -- so the skip below is narrow, not a hole.
cp "$T/implausible" "$T/imp_lc"
imp_before=$(shasum -a 256 < "$T/imp_lc" | cut -d' ' -f1)
if mtip lc "$T/imp_lc" -delete uuid >/dev/null 2>"$T/imp_lc.err"; then
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
if "$MACHOTOOL" verify "$T/emptystarts" >"$T/es_verify.out" 2>&1; then
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
if mtip lc "$T/es_lc" -delete uuid >/dev/null 2>"$T/es_lc.err"; then
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
if mtip segment "$T/imp_seg" __DATA __DATA_R9 >/dev/null 2>"$T/imp_seg.err"; then
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
if mtip lc "$T/mrerr_fat" -delete uuid >"$T/mrerr.out" 2>"$T/mrerr.err"; then
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
if mtip lc "$T/mrskip_fat" -delete uuid >"$T/mrskip.out" 2>"$T/mrskip.err"; then
    bad "lc: MR_SKIP/MR_ERROR split" "a Mach-O slice at cputype 0x7 was skipped, not refused"
else
    grep -q 'arch 1 (cputype 0x7): refusing the whole fat file' "$T/mrskip.err" \
        && ok "lc: the slice's own bytes decide MR_ERROR, not the fat table's cputype" \
        || bad "lc: MR_SKIP/MR_ERROR split" "refused for another reason: $(cat "$T/mrskip.err")"
fi

# ============================================================================
# the rewriting verbs never write their input
# ============================================================================
#
# dylib, rpath, lc and segment all reach mr_apply_file, which reads FILE and
# writes OUT. The same five facts minos and retag-swift are held to above, per
# verb, with no mtip helper in the way: the run succeeds, FILE is untouched in
# BYTES and INODE, OUT carries the change, stdout names what was written, and
# an OUT that is FILE -- or a symlink to FILE, or missing altogether -- is
# refused with 2 before any work.
#
# nwi runs the four that every verb shares; the change OUT is supposed to carry
# is verb-specific, so each verb's own check for that follows.
nwi() {   # VERB then the operands that come after FILE OUT
    nwi_verb=$1; shift
    nwi_in="$T/nwi_$nwi_verb"
    nwi_out="$T/nwi_${nwi_verb}_out"
    build_main "$nwi_in"
    nwi_sha=$(sha "$nwi_in"); nwi_ino=$(stat -f %i "$nwi_in")
    rm -f "$nwi_out"
    "$MACHOTOOL" "$nwi_verb" "$nwi_in" "$nwi_out" "$@" >"$T/nwi.out" 2>"$T/nwi.err" \
        && ok "$nwi_verb FILE OUT: succeeds" \
        || bad "$nwi_verb FILE OUT" "$(cat "$T/nwi.err")"
    [ "$(sha "$nwi_in")" = "$nwi_sha" ] && [ "$(stat -f %i "$nwi_in")" = "$nwi_ino" ] \
        && ok "$nwi_verb FILE OUT: FILE is untouched, bytes and inode" \
        || bad "$nwi_verb FILE OUT" "FILE changed"
    grep -q "^Wrote $nwi_out (" "$T/nwi.out" \
        && ok "$nwi_verb FILE OUT: says what it wrote" \
        || bad "$nwi_verb FILE OUT" "no Wrote line: $(cat "$T/nwi.out")"
    rc=0
    "$MACHOTOOL" "$nwi_verb" "$nwi_in" "$nwi_in" "$@" >/dev/null 2>"$T/nwi_same.err" || rc=$?
    [ "$rc" -eq 2 ] && [ "$(sha "$nwi_in")" = "$nwi_sha" ] \
        && ok "$nwi_verb: OUT that is FILE is refused (2), FILE untouched" \
        || bad "$nwi_verb OUT=FILE" "rc $rc"
    grep -q "never writes its input" "$T/nwi_same.err" \
        && ok "$nwi_verb: ... refused up front, before any work" \
        || bad "$nwi_verb OUT=FILE" "not the up-front refusal: $(cat "$T/nwi_same.err")"
    rm -f "$T/nwi_link"; ln -s "$nwi_in" "$T/nwi_link"
    rc=0
    "$MACHOTOOL" "$nwi_verb" "$nwi_in" "$T/nwi_link" "$@" >/dev/null 2>&1 || rc=$?
    [ "$rc" -eq 2 ] && ok "$nwi_verb: OUT that is a symlink to FILE is refused (2)" \
        || bad "$nwi_verb OUT=link" "rc $rc"
    rc=0
    "$MACHOTOOL" "$nwi_verb" "$nwi_in" "$@" >/dev/null 2>&1 || rc=$?
    [ "$rc" -eq 2 ] && ok "$nwi_verb: a missing OUT is an error (2)" \
        || bad "$nwi_verb no OUT" "rc $rc"
}
nwi dylib -append /nwi/appended.dylib
"$MACHOTOOL" info "$T/nwi_dylib_out" | grep -qF "path=/nwi/appended.dylib" \
    && ok "dylib FILE OUT: OUT carries the appended dependency" \
    || bad "dylib FILE OUT" "OUT lacks the appended dependency"
nwi rpath -append /nwi/appended/rpath
"$MACHOTOOL" info "$T/nwi_rpath_out" | grep -qF "/nwi/appended/rpath" \
    && ok "rpath FILE OUT: OUT carries the appended rpath" \
    || bad "rpath FILE OUT" "OUT lacks the appended rpath"
nwi lc -delete uuid
"$MACHOTOOL" info "$T/nwi_lc_out" | grep -q "LC_UUID" \
    && bad "lc FILE OUT" "OUT still has LC_UUID" \
    || ok "lc FILE OUT: OUT has lost its LC_UUID"
nwi segment __DATA __DATA_NWI
"$MACHOTOOL" info "$T/nwi_segment_out" | grep -q "segname=__DATA_NWI" \
    && ok "segment FILE OUT: OUT carries the renamed segment" \
    || bad "segment FILE OUT" "OUT lacks the renamed segment"

# AN OUT THAT BEGINS WITH '-' IS REFUSED, not created. `dylib FILE
# --allow-grow -append /x` is the flag-first habit from before these verbs took
# an output, and nothing here treats a positional as a flag -- so without the
# check it creates a regular file called "--allow-grow" and exits 0, doing
# something the caller did not ask for. Every verb that takes an OUT gets the
# same answer from the same place (bad_out); these eight are the ones whose
# positionals are at fixed argv indices, and `edit`, whose parser scans for
# them, is asserted in its own section below.
# The operands after OUT are each verb's own, because the argc-exact verbs reach
# their usage line before bad_out if the count is wrong -- which would make
# this pass for the wrong reason.
for nwid_verb in dylib rpath lc segment minos retag-swift declassify grow; do
    case $nwid_verb in
        dylib|rpath)  set -- -append /x ;;
        lc)           set -- -delete uuid ;;
        segment)      set -- __DATA __DATX ;;
        minos)        set -- 10.9 ;;
        retag-swift)  set -- ;;
        declassify)   set -- ;;
        grow)         set -- 4096 ;;
    esac
    build_main "$T/nwid"
    rm -f -- "$T/--nwid-flag"
    rc=0
    # Run IN $T with a bare OUT word, because that is the shape of the mistake:
    # a flag-looking OUT is relative to the caller's directory, and a test that
    # spelled it "$T/--nwid-flag" would be asserting about a path that does not
    # begin with '-' at all. The `cd` is also what keeps the file the check
    # exists to prevent out of the source tree when the check is not there.
    ( cd "$T" && "$MACHOTOOL" "$nwid_verb" nwid --nwid-flag "$@" ) \
        >/dev/null 2>"$T/nwid.err" || rc=$?
    [ "$rc" -eq 2 ] && [ ! -e "$T/--nwid-flag" ] && [ ! -e "$T/-append" ] \
        && grep -q "which begins with '-'" "$T/nwid.err" \
        && ok "$nwid_verb: an OUT beginning with '-' is refused (2), not created" \
        || bad "$nwid_verb OUT=-flag" "rc $rc, exists=$([ -e "$T/--nwid-flag" ] && echo YES || echo no), stderr: $(cat "$T/nwid.err")"
done
# ... and the remedy the message names really does work, so the refusal is not
# a wall in front of a legal path.
build_main "$T/nwid2"
( cd "$T" && "$MACHOTOOL" lc nwid2 ./-nwid-out -delete uuid ) >/dev/null 2>"$T/nwid2.err" \
    && [ -e "$T/-nwid-out" ] \
    && ok "lc: ... and './-name', the remedy the message names, writes that file" \
    || bad "lc OUT=./-name" "$(cat "$T/nwid2.err")"

# A 0 EXIT MUST LEAVE OUT THERE, even when there was nothing to change: OUT is
# the answer, so a caller that got exit 0 and no OUT would have been told the
# work succeeded and handed nothing. Nothing matched here, so OUT has to be a
# byte-for-byte copy of FILE.
build_main "$T/nwi_noop"
rm -f "$T/nwi_noop_out"
"$MACHOTOOL" dylib "$T/nwi_noop" "$T/nwi_noop_out" -delete /not/linked/at/all.dylib \
    >"$T/nwi_noop.out" 2>"$T/nwi_noop.err" && nwi_noop_rc=0 || nwi_noop_rc=$?
[ "$nwi_noop_rc" -eq 0 ] \
    && ok "dylib: an operation that matched nothing still exits 0" \
    || bad "dylib nothing-to-change" "exit $nwi_noop_rc: $(cat "$T/nwi_noop.err")"
cmp -s "$T/nwi_noop" "$T/nwi_noop_out" \
    && ok "dylib: ... and OUT is there, byte-identical to FILE" \
    || bad "dylib nothing-to-change" "OUT is missing or differs from FILE"

# ============================================================================
# retag-swift: the is-Swift tag moves from the stable-ABI bit to the legacy one
# ============================================================================
# The fixture builder is tests/mkswift.c, shared with tests/wrapper_test.sh
# exactly the way tests/strip_version_min.c is -- both suites need a binary
# with real Swift class records to retag, not fixture.macho's usual zero.
"$CC" -O2 -o "$T/mkswift" "$HERE/mkswift.c"
"$T/mkswift" make "$T/swift_fixture"
tags_before=$("$T/mkswift" tags "$T/swift_fixture")
[ "$tags_before" = "class 2 0x1000009c2
meta 2 0x1000009c2" ] \
    && ok "retag-swift: fixture starts with both records on the stable-ABI bit" \
    || bad "retag-swift: precondition" "unexpected starting tags: $tags_before"

swift_fixture_before=$(sha "$T/swift_fixture")
"$MACHOTOOL" retag-swift "$T/swift_fixture" "$T/swift_out1" >"$T/retag.out" 2>&1 \
    || bad "retag-swift: exit" "$(cat "$T/retag.out")"
[ "$(sha "$T/swift_fixture")" = "$swift_fixture_before" ] \
    && ok "retag-swift: FILE is untouched" || bad "retag-swift" "FILE changed"
tags_after=$("$T/mkswift" tags "$T/swift_out1")
[ "$tags_after" = "class 1 0x1000009c1
meta 1 0x1000009c1" ] \
    && ok "retag-swift: moved both tags to the legacy bit, leaving every other bit alone" \
    || bad "retag-swift" "expected both records tagged 1 with 0x...9c1, got: $tags_after"
# Both halves of the pair: the class AND the metaclass its isa points at. The
# count in the message is how we know the metaclass was reached at all.
grep -q "retagged 2 class record(s)" "$T/retag.out" \
    && ok "retag-swift: reported both the class and its metaclass" \
    || bad "retag-swift: count" "expected 2 records, got: $(cat "$T/retag.out")"

# Idempotent: retagging the already-retagged OUT finds nothing on the stable
# bit, says 0, and does not flip anything back.
swift_out1_before=$(sha "$T/swift_out1")
"$MACHOTOOL" retag-swift "$T/swift_out1" "$T/swift_out2" >"$T/retag2.out" 2>&1 \
    || bad "retag-swift: second run exit" "$(cat "$T/retag2.out")"
grep -q "retagged 0 class record(s)" "$T/retag2.out" \
    && ok "retag-swift: a second run retags nothing" \
    || bad "retag-swift: idempotence" "expected 0 records, got: $(cat "$T/retag2.out")"
[ "$(sha "$T/swift_out1")" = "$swift_out1_before" ] \
    && ok "retag-swift: a run with nothing to do left FILE untouched" \
    || bad "retag-swift: idempotence" "FILE changed on a no-op run"
cmp -s "$T/swift_out1" "$T/swift_out2" \
    && ok "retag-swift: a no-op run's OUT still carries the same bytes" \
    || bad "retag-swift: idempotence" "OUT differs from FILE on a no-op run"

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
    "$MACHOTOOL" retag-swift "$T/retag_fat" "$T/retag_fat_out" >"$T/retag_fat.out" 2>"$T/retag_fat.err" || rc=$?
    [ "$rc" -eq 1 ] && ok "retag-swift: refuses a fat container with the documented refusal code" \
        || bad "retag-swift: fat" "expected exit 1, got $rc: $(cat "$T/retag_fat.out") $(cat "$T/retag_fat.err")"
    [ ! -e "$T/retag_fat_out" ] \
        && ok "retag-swift: ... and writes no OUT for a refusal" \
        || bad "retag-swift: fat" "OUT was written despite the refusal"
    grep -q "not a readable 64-bit Mach-O" "$T/retag_fat.err" \
        && ok "retag-swift: says why it refused, instead of silently doing nothing" \
        || bad "retag-swift: fat message" "no explanation on stderr: $(cat "$T/retag_fat.err")"
    # The control: the same class records, thin, ARE reachable. Without this
    # the assertions above would also pass against a verb that refused
    # everything.
    cp "$T/swift_fixture" "$T/retag_thin_control"
    rc=0
    "$MACHOTOOL" retag-swift "$T/retag_thin_control" "$T/retag_thin_control_out" >"$T/retag_thin_control.out" 2>&1 || rc=$?
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
"$MACHOTOOL" retag-swift "$T/no-such-file-for-retag" "$T/retag_missing_out" >"$T/retag_missing.out" 2>"$T/retag_missing.err" || rc=$?
[ "$rc" -eq 2 ] \
    && ok "retag-swift: an unopenable path is a failure (2), not a refusal (1) and not silent success" \
    || bad "retag-swift: missing path" "expected exit 2, got $rc: $(cat "$T/retag_missing.out") $(cat "$T/retag_missing.err")"
# MSWIFT_ERROR's own contract (swift_retag.h) is "already reported" --
# cmd_retag_swift relies on that and prints nothing itself for this code.
# What this guards is the path an absent file actually takes:
# mswift_retag_file's own open() fails first, perror()s, and returns
# MSWIFT_ERROR, so the run must exit 2 with something on stderr. It does NOT
# reach, and cannot guard, the MI_IO_ERROR branch after mswift_retag_file's
# mi_open call -- the one that once returned MSWIFT_ERROR without a print.
# That branch's print is verified by inspection only: once open() and
# fstat() have succeeded, reaching it takes the path being removed or
# replaced mid-run, a malloc failure, or a short read, and this suite stages
# none of those.
[ -s "$T/retag_missing.err" ] \
    && ok "retag-swift: an unopenable path prints something, per MSWIFT_ERROR's contract" \
    || bad "retag-swift: missing path stderr" "exit 2 but stderr was empty -- MSWIFT_ERROR's 'already reported' contract broke"

# retag-swift never writes its input: FILE OUT, and an OUT that is FILE is
# refused. Any 64-bit Mach-O fixture does, whether or not it carries a Swift
# class to retag -- OUT is written either way.
build_main "$T/rs_in"
rs_before=$(sha "$T/rs_in"); rs_ino=$(stat -f %i "$T/rs_in")
"$MACHOTOOL" retag-swift "$T/rs_in" "$T/rs_out" >"$T/rs.out" 2>"$T/rs.err" \
    && ok "retag-swift FILE OUT: succeeds" || bad "retag-swift FILE OUT" "$(cat "$T/rs.err")"
[ "$(sha "$T/rs_in")" = "$rs_before" ] && [ "$(stat -f %i "$T/rs_in")" = "$rs_ino" ] \
    && ok "retag-swift FILE OUT: FILE is untouched" || bad "retag-swift FILE OUT" "FILE changed"
[ -e "$T/rs_out" ] \
    && ok "retag-swift FILE OUT: OUT was written" || bad "retag-swift FILE OUT" "OUT is missing"
grep -q "^Wrote $T/rs_out (" "$T/rs.out" \
    && ok "retag-swift FILE OUT: says what it wrote" || bad "retag-swift FILE OUT" "no Wrote line: $(cat "$T/rs.out")"
rc=0; "$MACHOTOOL" retag-swift "$T/rs_in" "$T/rs_in" >/dev/null 2>"$T/rs_same.err" || rc=$?
[ "$rc" -eq 2 ] && [ "$(sha "$T/rs_in")" = "$rs_before" ] \
    && ok "retag-swift: OUT that is FILE is refused (2), FILE untouched" || bad "retag-swift OUT=FILE" "rc $rc"
grep -q "never writes its input" "$T/rs_same.err" \
    && ok "retag-swift: ... refused up front, before any work" \
    || bad "retag-swift OUT=FILE" "not the up-front refusal: $(cat "$T/rs_same.err")"
rc=0; "$MACHOTOOL" retag-swift "$T/rs_in" >/dev/null 2>&1 || rc=$?
[ "$rc" -eq 2 ] && ok "retag-swift: a missing OUT is a usage error (2)" || bad "retag-swift no OUT" "rc $rc"

# The MI_IO_ERROR branch inside mi_open specifically (not mswift_retag_file's
# own earlier open()/fstat(), which the absent-file case above already
# exercises): verify and declassify are equally cheap to check on an absent
# path, and neither had a numeric-exit-code assertion for one before.
rc=0
"$MACHOTOOL" verify "$T/no-such-file-for-verify" >"$T/verify_missing.out" 2>"$T/verify_missing.err" || rc=$?
[ "$rc" -eq 2 ] && [ -s "$T/verify_missing.err" ] \
    && ok "verify: an absent file is a failure (2), not a refusal, and says something" \
    || bad "verify: missing path" "expected exit 2 with nonempty stderr, got $rc: $(cat "$T/verify_missing.err")"

rc=0
"$MACHOTOOL" declassify "$T/no-such-file-for-declassify" "$T/declassify_missing.out" \
    >/dev/null 2>"$T/declassify_missing.err" || rc=$?
[ "$rc" -eq 2 ] && [ -s "$T/declassify_missing.err" ] \
    && ok "declassify: an absent IN is a failure (2), not a refusal, and says something" \
    || bad "declassify: missing IN" "expected exit 2 with nonempty stderr, got $rc: $(cat "$T/declassify_missing.err")"
[ -e "$T/declassify_missing.out" ] \
    && bad "declassify: missing IN" "wrote an output file for an IN it could not even open" \
    || ok "declassify: an absent IN produces no output file"

# ============================================================================
# edit: parses a script and applies it through me_run in one pass -- the
# three shapes the spec names, plus the property the whole design exists for
# (a parse error costs nothing: the file is never opened for writing).
# ============================================================================

# edit: the production case -- what install.sh does with three tools and
# three full writes of a 208MB binary, in one write. build_main's fixture
# records the install name literally as "@loader_path/liba.dylib"
# (see build_main above), not a path under $T, so the script names that
# install name directly rather than substituting one in -- the same 23
# bytes both before and after, so the replacement fits without growth.
build_main "$T/edit_fixture"
cat >"$T/prod.edits" <<'EOF'
# a comment, and a blank line follow

load-command  delete   uuid
dylib         replace  @loader_path/liba.dylib  @loader_path/../S.dylib
EOF
edit_fixture_before=$(sha "$T/edit_fixture"); edit_fixture_ino=$(stat -f %i "$T/edit_fixture")
rm -f "$T/edit_fixture_out"
"$MACHOTOOL" edit "$T/edit_fixture" "$T/edit_fixture_out" "$T/prod.edits" \
    >"$T/edit.out" 2>"$T/edit.err" && edit_rc=0 || edit_rc=$?
[ "$edit_rc" -eq 0 ] && ok "edit: the production script succeeds" \
    || bad "edit" "expected 0, got $edit_rc: $(cat "$T/edit.err")"
[ "$(sha "$T/edit_fixture")" = "$edit_fixture_before" ] \
    && [ "$(stat -f %i "$T/edit_fixture")" = "$edit_fixture_ino" ] \
    && ok "edit FILE OUT SCRIPT: FILE is untouched, bytes and inode" \
    || bad "edit FILE OUT SCRIPT" "FILE changed"
otool -l "$T/edit_fixture_out" 2>/dev/null | grep -q LC_UUID \
    && bad "edit" "LC_UUID survived the edit script" \
    || ok "edit: applied the load-command delete"
otool -L "$T/edit_fixture_out" 2>/dev/null | grep -q "@loader_path/../S.dylib" \
    && ok "edit: applied the dylib replace" \
    || bad "edit" "the dylib replace did not land: $(otool -L "$T/edit_fixture_out")"

# AN OUT THAT IS FILE IS REFUSED BEFORE THE SCRIPT IS EVEN READ. `edit` reaches
# the same bad_out every other OUT-taking verb does, and it reaches it before
# it opens SCRIPT -- which is why SCRIPT here is a path that does not exist: the
# answer must be the refusal about OUT, not a complaint about the script.
rc=0
"$MACHOTOOL" edit "$T/edit_fixture" "$T/edit_fixture" "$T/no-such-script-at-all" \
    >/dev/null 2>"$T/edit_same.err" || rc=$?
[ "$rc" -eq 2 ] && [ "$(sha "$T/edit_fixture")" = "$edit_fixture_before" ] \
    && ok "edit: OUT that is FILE is refused (2), FILE untouched" \
    || bad "edit OUT=FILE" "rc $rc"
grep -q "never writes its input" "$T/edit_same.err" \
    && ok "edit: ... refused up front, before the script is read" \
    || bad "edit OUT=FILE" "not the up-front refusal: $(cat "$T/edit_same.err")"
rm -f "$T/edit_link"; ln -s "$T/edit_fixture" "$T/edit_link"
rc=0
"$MACHOTOOL" edit "$T/edit_fixture" "$T/edit_link" "$T/prod.edits" >/dev/null 2>&1 || rc=$?
[ "$rc" -eq 2 ] && ok "edit: OUT that is a symlink to FILE is refused (2)" \
    || bad "edit OUT=link" "rc $rc"

# TWO POSITIONALS ARE NO LONGER A COMMAND: `edit FILE SCRIPT` used to rewrite
# FILE, so accepting it now -- with SCRIPT landing where OUT belongs -- would
# write a Mach-O over the script. It is a usage error, and the script survives.
prod_edits_sha=$(sha "$T/prod.edits")
rc=0
"$MACHOTOOL" edit "$T/edit_fixture" "$T/prod.edits" >/dev/null 2>"$T/edit_two.err" || rc=$?
[ "$rc" -eq 2 ] && ok "edit: two positionals (no OUT) is a usage error (2)" \
    || bad "edit no OUT" "expected 2, got $rc: $(cat "$T/edit_two.err")"
[ "$(sha "$T/prod.edits")" = "$prod_edits_sha" ] \
    && ok "edit: ... and the script was not taken for an OUT and written over" \
    || bad "edit no OUT" "the script file was overwritten"

# --dry-run AND --output ARE GONE, both unknown flags now (2). A scratch OUT is
# the same run, so there is nothing --dry-run said that this does not.
rc=0
"$MACHOTOOL" edit --dry-run "$T/edit_fixture" "$T/edit_dry_out" "$T/prod.edits" \
    >/dev/null 2>"$T/edit_dry.err" || rc=$?
[ "$rc" -eq 2 ] && [ ! -e "$T/edit_dry_out" ] \
    && ok "edit: --dry-run is an unknown flag now (2), and writes no OUT" \
    || bad "edit --dry-run gone" "expected 2 and no OUT, got $rc: $(cat "$T/edit_dry.err")"
rc=0
"$MACHOTOOL" edit "$T/edit_fixture" "$T/edit_flag_out" "$T/prod.edits" --output "$T/edit_flag_out2" \
    >/dev/null 2>"$T/edit_output.err" || rc=$?
[ "$rc" -eq 2 ] && [ ! -e "$T/edit_flag_out" ] && [ ! -e "$T/edit_flag_out2" ] \
    && ok "edit: --output is an unknown flag now (2), and neither name is written" \
    || bad "edit --output gone" "expected 2 and no output, got $rc: $(cat "$T/edit_output.err")"

# AN OUT BEGINNING WITH '-' IS REFUSED, not created -- the same answer, from the
# same place (bad_out), as the eight fixed-arity verbs get above. A single
# dash, because a double-dashed OUT is caught as an unknown flag first: this
# verb takes no flags, so every '--' token is refused by name.
build_main "$T/edit_dashout"
rm -f -- "$T/-edit-dashout"
rc=0
( cd "$T" && "$MACHOTOOL" edit edit_dashout -edit-dashout "$T/prod.edits" ) \
    >/dev/null 2>"$T/edit_dashout.err" || rc=$?
[ "$rc" -eq 2 ] && [ ! -e "$T/-edit-dashout" ] \
    && grep -q "which begins with '-'" "$T/edit_dashout.err" \
    && ok "edit: an OUT beginning with '-' is refused (2), not created" \
    || bad "edit OUT=-flag" "rc $rc, stderr: $(cat "$T/edit_dashout.err")"

# A 0 EXIT LEAVES OUT THERE, even when no statement changed anything: OUT is
# the answer, so exit 0 with no OUT would hand a caller nothing.
build_main "$T/edit_noop"
printf 'dylib delete /not/linked/at/all.dylib\n' >"$T/noop.edits"
rm -f "$T/edit_noop_out"
"$MACHOTOOL" edit "$T/edit_noop" "$T/edit_noop_out" "$T/noop.edits" \
    >/dev/null 2>"$T/edit_noop.err" && edit_noop_rc=0 || edit_noop_rc=$?
[ "$edit_noop_rc" -eq 0 ] && ok "edit: a script that changed nothing still exits 0" \
    || bad "edit nothing-to-change" "exit $edit_noop_rc: $(cat "$T/edit_noop.err")"
cmp -s "$T/edit_noop" "$T/edit_noop_out" \
    && ok "edit: ... and OUT is there, byte-identical to FILE" \
    || bad "edit nothing-to-change" "OUT is missing or differs from FILE"

# A REFUSED RUN WRITES NOTHING AT ALL: not FILE, which `edit` never writes,
# and not OUT, which may never have existed. fatal-warnings over an operation
# that matches nothing is the cheapest way to reach a refusal after the image
# has already been read.
build_main "$T/edit_ref"
ref_before=$(sha "$T/edit_ref")
printf 'fatal-warnings\ndylib delete /definitely/not/linked.dylib\n' >"$T/ref.edits"
rm -f "$T/edit_ref_out"
"$MACHOTOOL" edit "$T/edit_ref" "$T/edit_ref_out" "$T/ref.edits" \
    >/dev/null 2>"$T/edit_ref.err" && ref_rc=0 || ref_rc=$?
[ "$ref_rc" -eq 1 ] \
    && ok "edit: an unmatched operation under fatal-warnings is refused (1)" \
    || bad "edit refusal" "expected 1, got $ref_rc: $(cat "$T/edit_ref.err")"
[ "$(sha "$T/edit_ref")" = "$ref_before" ] && [ ! -e "$T/edit_ref_out" ] \
    && ok "edit: ... and the refused run left FILE unchanged and wrote no OUT" \
    || bad "edit refusal" "the refused run modified FILE, or created OUT"
grep -qF "$T/edit_ref_out not written; $T/edit_ref left unmodified" "$T/edit_ref.err" \
    && ok "edit: ... and the refusal says OUT was not written and FILE is unmodified" \
    || bad "edit refusal" "not that wording: $(cat "$T/edit_ref.err")"

# edit FILE OUT - reads the script from stdin, so a generated script needs no
# temp file. Only SCRIPT means stdin: an OUT of "-" begins with a dash and is
# refused above.
build_main "$T/edit_stdin"
rm -f "$T/edit_stdin_out"
printf 'load-command delete uuid\n' | "$MACHOTOOL" edit "$T/edit_stdin" "$T/edit_stdin_out" - \
    >/dev/null 2>"$T/edit_stdin.err" || bad "edit -" "$(cat "$T/edit_stdin.err")"
otool -l "$T/edit_stdin_out" 2>/dev/null | grep -q LC_UUID \
    && bad "edit -" "LC_UUID survived the stdin script" \
    || ok "edit: reads a script from stdin"

# A parse error is reported BEFORE anything is written, and names the line.
# This is what makes a typo in statement 9 of 9 cost nothing.
build_main "$T/edit_bad"
bad_before=$(sha "$T/edit_bad")
printf 'load-command delete uuid\nfrobnicate everything\n' \
    >"$T/bad.edits"
rm -f "$T/edit_bad_out"
"$MACHOTOOL" edit "$T/edit_bad" "$T/edit_bad_out" "$T/bad.edits" \
    >/dev/null 2>"$T/editbad.err" && editbad_rc=0 || editbad_rc=$?
[ "$editbad_rc" -eq 2 ] && ok "edit: a parse error is an error (2), not a refusal" \
    || bad "edit parse error" "expected 2, got $editbad_rc"
grep -q "line 2" "$T/editbad.err" && ok "edit: names the offending line" \
    || bad "edit parse error" "no line number: $(cat "$T/editbad.err")"
[ "$(sha "$T/edit_bad")" = "$bad_before" ] && [ ! -e "$T/edit_bad_out" ] \
    && ok "edit: a parse error left FILE untouched and wrote no OUT" \
    || bad "edit parse error" "the file was modified, or an OUT appeared, despite a parse error"
# `machotool edit: `: every one of src/edit.c's me_say format strings names
# the tool, as every other verb's diagnostics do. No digest protects those
# strings -- tests/EXPECTED and tests/known-callers.sh's sha256s hash
# converted file bytes with the tools' output sent to /dev/null -- so this
# grep is one of the four readers that would actually break if they moved,
# alongside tests/wrapper_test.sh's two unmatched-report assertions and
# compat/rename_segment.sh's `^machotool segment: renamed=N` parser, which is
# production code rather than a test. All four move with what they read.
grep -q "^machotool edit: " "$T/editbad.err" \
    && ok "edit: parse error is prefixed like every other verb's diagnostics" \
    || bad "edit parse error" "no 'machotool edit: ' prefix: $(cat "$T/editbad.err")"

# Usage errors: an unknown flag, too few positionals and too many are all
# EX_FAIL (2) -- never a crash, never silently accepted.
build_main "$T/edit_usage"
rc=0
"$MACHOTOOL" edit "$T/edit_usage" "$T/edit_usage_out" "$T/prod.edits" --bogus-flag \
    >/dev/null 2>"$T/edit_usage1.err" || rc=$?
[ "$rc" -eq 2 ] && ok "edit: an unknown flag is a usage error (2)" \
    || bad "edit usage" "unknown flag: expected 2, got $rc"
rc=0
"$MACHOTOOL" edit "$T/edit_usage" >/dev/null 2>"$T/edit_usage2.err" || rc=$?
[ "$rc" -eq 2 ] && ok "edit: one positional is a usage error (2)" \
    || bad "edit usage" "one positional: expected 2, got $rc"
rc=0
"$MACHOTOOL" edit "$T/edit_usage" "$T/edit_usage_out" "$T/prod.edits" extra \
    >/dev/null 2>"$T/edit_usage3.err" || rc=$?
[ "$rc" -eq 2 ] && ok "edit: a fourth positional is a usage error (2)" \
    || bad "edit usage" "extra positional: expected 2, got $rc"

# A FILE WHOSE NAME STARTS WITH A DASH IS A FILE NAME. Only a double dash is
# refused here, and every other verb takes its FILE positionally without
# examining it, so a single-dash token in FILE's place is a path, not a
# typo'd flag. The compat wrappers reach this: the historical tools open()ed
# whatever argv[1] was, and tests/wrapper_test.sh pins `change_dylib -dashy
# ...` for that reason. OUT is the one positional that refuses a leading dash
# (above), because a mistaken OUT is a file this tool CREATES.
build_main "$T/-edit_dashy"
rc=0
(cd "$T" && "$MACHOTOOL" edit -edit_dashy "$T/edit_dashy_out" "$T/prod.edits") \
    >/dev/null 2>"$T/edit_dash.err" || rc=$?
[ "$rc" -eq 0 ] \
    && ok "edit: a FILE whose name starts with a dash is a file name, not a flag" \
    || bad "edit dash FILE" "expected 0, got $rc: $(head -1 "$T/edit_dash.err")"

# A script file that cannot be read at all -- as opposed to one that parses
# badly -- is also EX_FAIL, reported with the path.
rc=0
"$MACHOTOOL" edit "$T/edit_usage" "$T/edit_usage_out" "$T/no-such-script-for-edit" \
    >/dev/null 2>"$T/edit_noscript.err" || rc=$?
[ "$rc" -eq 2 ] && ok "edit: an unreadable SCRIPT path is a failure (2)" \
    || bad "edit: unreadable script" "expected 2, got $rc"
grep -q "no-such-script-for-edit" "$T/edit_noscript.err" \
    && ok "edit: names the unreadable script path" \
    || bad "edit: unreadable script" "path not named: $(cat "$T/edit_noscript.err")"

# The report must carry the FOLLOW-UP work, not just the statement. A dylib
# delete renumbers every surviving ordinal in the nlist entries AND in the
# SET_DYLIB_ORDINAL* opcodes; PROPOSAL defect #2 was exactly that work not
# happening, and it surfaced as "dyld: library ordinal (4) too big" at
# runtime rather than as anything the tool said. So the log is the only
# place a user can see it.
build_main_two_dylibs "$T/edit_verb"
printf 'dylib delete %s\n' "$T/libb.dylib" >"$T/verb.edits"
"$MACHOTOOL" edit "$T/edit_verb" "$T/edit_verb_out" "$T/verb.edits" \
    >/dev/null 2>"$T/verb.err" || bad "edit report" "$(cat "$T/verb.err")"
grep -q "dylib delete" "$T/verb.err" \
    && ok "edit: names the statement" \
    || bad "edit report" "no statement line: $(cat "$T/verb.err")"
grep -q "renumbered" "$T/verb.err" \
    && ok "edit: reports the ordinal renumbering it did unasked" \
    || bad "edit report" "no renumbering report: $(cat "$T/verb.err")"
grep -q "nlist" "$T/verb.err" \
    && ok "edit: counts the nlist entries it touched" \
    || bad "edit report" "no nlist count: $(cat "$T/verb.err")"
grep -q "SET_DYLIB_ORDINAL" "$T/verb.err" \
    && ok "edit: counts the opcodes it rewrote" \
    || bad "edit report" "no opcode count: $(cat "$T/verb.err")"

# What was removed and where every survivor went, which build_main_two_dylibs
# fixed (and checked): libb was 1, liba 2, libSystem 3.
grep -qF "      removed LC_LOAD_DYLIB (was ordinal 1)" "$T/verb.err" \
    && ok "edit: names the removed command and its ordinal" \
    || bad "edit report" "no 'removed LC_LOAD_DYLIB (was ordinal 1)': $(cat "$T/verb.err")"
grep -qF "      renumbered 2 surviving ordinals: 2->1, 3->2" "$T/verb.err" \
    && ok "edit: lists the renumbering map" \
    || bad "edit report" "no '2->1, 3->2' map: $(cat "$T/verb.err")"

# The counts, as numbers. A "nlist" line saying 0 would satisfy the greps
# above, so these read the figures back: liba's a_sym and libSystem's
# dyld_stub_binder are both undefined symbols whose ordinal moved, and each
# is bound through a SET_DYLIB_ORDINAL opcode. So neither figure can be 0.
vb_nlist() { sed -n 's/^          \([0-9][0-9]*\) nlist entr[a-z]* updated$/\1/p' "$1"; }
vb_ops() { sed -n 's/^          \([0-9][0-9]*\) SET_DYLIB_ORDINAL opcodes\{0,1\} updated.*/\1/p' "$1"; }
nl2=$(vb_nlist "$T/verb.err"); op2=$(vb_ops "$T/verb.err")
[ -n "$nl2" ] && [ "$nl2" -gt 0 ] \
    && ok "edit: a delete that moved bound ordinals counts nlist entries > 0 ($nl2)" \
    || bad "edit report" "nlist count '$nl2' should be > 0: $(cat "$T/verb.err")"
[ -n "$op2" ] && [ "$op2" -gt 0 ] \
    && ok "edit: ... and SET_DYLIB_ORDINAL opcodes > 0 ($op2)" \
    || bad "edit report" "opcode count '$op2' should be > 0: $(cat "$T/verb.err")"

# A count that does not move when the input does is not a count. The same
# delete on a fixture with one more bound dylib after libb (libc3, whose
# c_sym main also calls) renumbers one more ordinal, one more undefined
# symbol, and one more ordinal opcode.
build_main_three_dylibs "$T/edit_verb3"
"$MACHOTOOL" edit "$T/edit_verb3" "$T/edit_verb3_out" "$T/verb.edits" \
    >/dev/null 2>"$T/verb3.err" || bad "edit report (3 dylibs)" "$(cat "$T/verb3.err")"
grep -qF "      renumbered 3 surviving ordinals: 2->1, 3->2, 4->3" "$T/verb3.err" \
    && ok "edit: a third dylib adds its ordinal to the map" \
    || bad "edit report (3 dylibs)" "no '2->1, 3->2, 4->3' map: $(cat "$T/verb3.err")"
nl3=$(vb_nlist "$T/verb3.err"); op3=$(vb_ops "$T/verb3.err")
[ -n "$nl3" ] && [ "$nl3" -gt "${nl2:-0}" ] \
    && ok "edit: the nlist count follows the input ($nl2 -> $nl3)" \
    || bad "edit report (3 dylibs)" "nlist count '$nl3' not above '$nl2': $(cat "$T/verb3.err")"
[ -n "$op3" ] && [ "$op3" -gt "${op2:-0}" ] \
    && ok "edit: the opcode count follows the input ($op2 -> $op3)" \
    || bad "edit report (3 dylibs)" "opcode count '$op3' not above '$op2': $(cat "$T/verb3.err")"

# dylib insert carries the same follow-up: the new command takes ordinal 1
# and every existing one moves up (build_main: liba=1, libSystem=2). A short
# path, so the new 48-byte command fits even the 56-byte header pad the
# modern cross runner's linker leaves (see the rpath -insert fixture above).
build_main "$T/edit_verb_ins"
printf 'dylib insert @loader_path/libn.dylib\n' >"$T/verb_ins.edits"
"$MACHOTOOL" edit "$T/edit_verb_ins" "$T/edit_verb_ins_out" "$T/verb_ins.edits" \
    >/dev/null 2>"$T/verb_ins.err" || bad "edit report (insert)" "$(cat "$T/verb_ins.err")"
grep -qF "      inserted LC_LOAD_DYLIB as ordinal 1" "$T/verb_ins.err" \
    && ok "edit: names the inserted command and its ordinal" \
    || bad "edit report (insert)" "no 'inserted ... as ordinal 1': $(cat "$T/verb_ins.err")"
grep -qF "      renumbered 2 existing ordinals: 1->2, 2->3" "$T/verb_ins.err" \
    && ok "edit: an insert reports the ordinals it pushed up" \
    || bad "edit report (insert)" "no '1->2, 2->3' map: $(cat "$T/verb_ins.err")"
nli=$(vb_nlist "$T/verb_ins.err"); opi=$(vb_ops "$T/verb_ins.err")
[ -n "$nli" ] && [ "$nli" -gt 0 ] && [ -n "$opi" ] && [ "$opi" -gt 0 ] \
    && ok "edit: an insert counts the nlist entries and opcodes it moved ($nli, $opi)" \
    || bad "edit report (insert)" "counts '$nli'/'$opi' should be > 0: $(cat "$T/verb_ins.err")"

# A replace keeps its command's position and ordinal, so it carries no
# follow-up and must not claim one.
build_main "$T/edit_verb_rep"
printf 'dylib replace @loader_path/liba.dylib @loader_path/libz.dylib\n' >"$T/verb_rep.edits"
"$MACHOTOOL" edit "$T/edit_verb_rep" "$T/edit_verb_rep_out" "$T/verb_rep.edits" \
    >/dev/null 2>"$T/verb_rep.err" || bad "edit report (replace)" "$(cat "$T/verb_rep.err")"
grep -q "renumbered" "$T/verb_rep.err" \
    && bad "edit report (replace)" "a replace reported a renumbering: $(cat "$T/verb_rep.err")" \
    || ok "edit: a replace reports no renumbering"

# fixups set classic rebuilds __LINKEDIT's opcode streams wholesale. On an
# image that is already classic (build_main's, linked for 10.9) it passes
# through, and says so rather than staying silent.
build_main "$T/edit_verb_fx"
printf 'fixups set classic\n' >"$T/verb_fx.edits"
"$MACHOTOOL" edit "$T/edit_verb_fx" "$T/edit_verb_fx_out" "$T/verb_fx.edits" \
    >/dev/null 2>"$T/verb_fx.err" || bad "edit report (fixups)" "$(cat "$T/verb_fx.err")"
grep -qF "      already classic (LC_DYLD_INFO_ONLY, no chained fixups): passed through unchanged" \
    "$T/verb_fx.err" \
    && ok "edit: fixups on a classic image reports the pass-through" \
    || bad "edit report (fixups)" "no pass-through report: $(cat "$T/verb_fx.err")"
# ... and on mkchained's hand-built chained-fixups image (declassify's
# fixture, above: one rebase, one bind, and LC_DYLD_CHAINED_FIXUPS,
# LC_DYLD_EXPORTS_TRIE and LC_BUILD_VERSION to strip) it converts, and the
# report carries the conversion's own figures.
"$T/mkchained" make "$T/edit_verb_chained"
"$MACHOTOOL" edit "$T/edit_verb_chained" "$T/edit_verb_chained_out" "$T/verb_fx.edits" \
    >/dev/null 2>"$T/verb_cf.err" || bad "edit report (chained)" "$(cat "$T/verb_cf.err")"
grep -qF "      chained fixups -> LC_DYLD_INFO_ONLY" "$T/verb_cf.err" \
    && ok "edit: fixups on a chained image reports the conversion" \
    || bad "edit report (chained)" "no conversion line: $(cat "$T/verb_cf.err")"
grep -qF "      1 rebase and 1 bind emitted" "$T/verb_cf.err" \
    && ok "edit: reports the rebases and binds the conversion emitted" \
    || bad "edit report (chained)" "no '1 rebase and 1 bind': $(cat "$T/verb_cf.err")"
grep -qF "      stripped LC_DYLD_CHAINED_FIXUPS, LC_DYLD_EXPORTS_TRIE, LC_BUILD_VERSION" \
    "$T/verb_cf.err" \
    && ok "edit: names the commands the conversion stripped, in load order" \
    || bad "edit report (chained)" "no stripped list: $(cat "$T/verb_cf.err")"
grep -q "^      __LINKEDIT extended by [1-9][0-9,]* bytes" "$T/verb_cf.err" \
    && ok "edit: reports extending __LINKEDIT" \
    || bad "edit report (chained)" "no __LINKEDIT line: $(cat "$T/verb_cf.err")"

# swift-abi set legacy reports its retag count: mkswift's fixture has one
# class and its metaclass on the stable-ABI bit, and build_main's has none.
"$T/mkswift" make "$T/edit_verb_swift"
printf 'swift-abi set legacy\n' >"$T/verb_sw.edits"
"$MACHOTOOL" edit "$T/edit_verb_swift" "$T/edit_verb_swift_out" "$T/verb_sw.edits" \
    >/dev/null 2>"$T/verb_sw.err" || bad "edit report (swift-abi)" "$(cat "$T/verb_sw.err")"
grep -qF "      retagged 2 class records" "$T/verb_sw.err" \
    && ok "edit: swift-abi reports the class records it retagged" \
    || bad "edit report (swift-abi)" "no 'retagged 2 class records': $(cat "$T/verb_sw.err")"
build_main "$T/edit_verb_noswift"
"$MACHOTOOL" edit "$T/edit_verb_noswift" "$T/edit_verb_noswift_out" "$T/verb_sw.edits" \
    >/dev/null 2>"$T/verb_nosw.err" || bad "edit report (swift-abi)" "$(cat "$T/verb_nosw.err")"
grep -qF "      nothing to retag" "$T/verb_nosw.err" \
    && ok "edit: swift-abi with no Swift classes says nothing to retag" \
    || bad "edit report (swift-abi)" "no 'nothing to retag': $(cat "$T/verb_nosw.err")"

# THERE IS NO QUIET MODE, so there is no flag. A tool whose job is to make
# edits nobody can see afterwards should not have an option to say nothing
# about them. Anyone who wants silence has 2>/dev/null, which needs no flag
# of ours.
build_main "$T/noverb"
printf 'load-command delete uuid\n' >"$T/nv.edits"
rm -f "$T/noverb_out"
nv_rc=0
"$MACHOTOOL" edit --verbose "$T/noverb" "$T/noverb_out" "$T/nv.edits" \
    >/dev/null 2>"$T/nv.err" || nv_rc=$?
[ "$nv_rc" -ne 0 ] && ok "edit: --verbose is not a flag any more" \
    || bad "no quiet mode" "--verbose was accepted; the flag survives"

# And the report happens anyway, with no flag asked for.
build_main "$T/noverb2"
rm -f "$T/noverb2_out"
"$MACHOTOOL" edit "$T/noverb2" "$T/noverb2_out" "$T/nv.edits" \
    >"$T/nv2.out" 2>"$T/nv2.err" || bad "no quiet mode" "$(cat "$T/nv2.err")"
grep -q "load-command delete" "$T/nv2.err" \
    && ok "edit: reports without being asked" \
    || bad "no quiet mode" "no report on stderr: $(cat "$T/nv2.err")"
# AND IT GOES TO STDERR, which is what makes the report unconditional safe:
# stdout belongs to the operations' own progress lines, and the six compat
# wrappers' stdout is a byte-identical contract with the C tools they replaced.
# So this checks that no report line is on stdout, rather than that stdout is
# empty -- `load-command delete uuid` reaches mr_apply_image, which has always
# printed its own "header pad" and "updated" lines there.
grep -q "load-command delete" "$T/nv2.out" \
    && bad "no quiet mode" "the statement echo went to stdout: $(cat "$T/nv2.out")" \
    || ok "edit: the statement echo is on stderr, not stdout"
grep -q "written (" "$T/nv2.out" \
    && bad "no quiet mode" "the written line went to stdout: $(cat "$T/nv2.out")" \
    || ok "edit: the written line is on stderr, so a wrapper's stdout is untouched"

# Statements run one at a time, so each `dylib insert` goes to the front of
# the image the statement before it left: two insert lines land in the
# REVERSE of the order written, where `machotool dylib -insert A -insert B`
# keeps its order. The README and src/edit.h disclose that; this pins it.
# Two 32-byte commands overflow the 56-byte pad the modern cross runner's
# linker leaves (as the `edit` insert case above notes), so both
# runs free LC_UUID's 24 bytes first.
build_main "$T/edit_ins2"
printf 'load-command delete uuid\ndylib insert /A\ndylib insert /B\n' >"$T/ins2.edits"
"$MACHOTOOL" edit "$T/edit_ins2" "$T/edit_ins2_out" "$T/ins2.edits" >/dev/null 2>"$T/ins2.err" \
    || bad "edit: two inserts" "$(cat "$T/ins2.err")"
ins2=$("$MACHOTOOL" info "$T/edit_ins2_out")
echo "$ins2" | grep -qxF "  ordinal=1 path=/B" && echo "$ins2" | grep -qxF "  ordinal=2 path=/A" \
    && ok "edit: two dylib insert lines leave the second at ordinal 1 and the first at 2" \
    || bad "edit: two inserts" "expected /B at 1 and /A at 2: $(echo "$ins2" | grep 'ordinal=')"
build_main "$T/cli_ins2"
mtip lc "$T/cli_ins2" -delete uuid >/dev/null 2>"$T/cli_ins2.err" \
    || bad "dylib: two inserts" "$(cat "$T/cli_ins2.err")"
mtip dylib "$T/cli_ins2" -insert /A -insert /B >/dev/null 2>"$T/cli_ins2.err" \
    || bad "dylib: two inserts" "$(cat "$T/cli_ins2.err")"
cins2=$("$MACHOTOOL" info "$T/cli_ins2")
echo "$cins2" | grep -qxF "  ordinal=1 path=/A" && echo "$cins2" | grep -qxF "  ordinal=2 path=/B" \
    && ok "dylib: -insert A -insert B keeps A at ordinal 1 and B at 2, unlike two script lines" \
    || bad "dylib: two inserts" "expected /A at 1 and /B at 2: $(echo "$cins2" | grep 'ordinal=')"

# allow-grow through edit, on the riskiest path it has: the header grow
# reallocates the image partway through the script, and the NEXT statement
# must run against the reallocated buffer. build_main's fixture is
# MH_EXECUTE and PIE, the one shape mg_grow_header grows. The appended path
# is sized from the fixture's own pad as `machotool info` reports it, not
# hard-coded, because each host's linker leaves a different pad: an
# LC_LOAD_DYLIB is 24 bytes plus the path and its NUL, so a path longer
# than the pad cannot fit in it. Messages are cut short because the path is
# thousands of bytes long.
build_main "$T/edit_grow"
grow_pad=$("$MACHOTOOL" info "$T/edit_grow" \
    | sed -n 's/^header pad: \([0-9][0-9]*\) bytes available.*/\1/p')
if [ -z "$grow_pad" ]; then
    bad "edit allow-grow: fixture setup" "machotool info reported no header pad"
    grow_pad=0
fi
grow_path="/$(printf "%${grow_pad}s" '' | tr ' ' x)"
printf 'dylib append %s\nload-command delete uuid\n' "$grow_path" >"$T/grow_no.edits"
{ printf 'allow-grow\n'; cat "$T/grow_no.edits"; } >"$T/grow_yes.edits"
grow_before=$(sha "$T/edit_grow")
rm -f "$T/edit_grow_out"
rc=0
"$MACHOTOOL" edit "$T/edit_grow" "$T/edit_grow_out" "$T/grow_no.edits" \
    >/dev/null 2>"$T/grow_no.err" || rc=$?
[ "$rc" -eq 1 ] \
    && ok "edit: a dylib append that overflows the ${grow_pad}-byte pad is refused (1) without allow-grow" \
    || bad "edit allow-grow" "without the directive: expected 1, got $rc: $(cut -c1-160 "$T/grow_no.err")"
[ "$(sha "$T/edit_grow")" = "$grow_before" ] && [ ! -e "$T/edit_grow_out" ] \
    && ok "edit: ... and the refused run left FILE unchanged and wrote no OUT" \
    || bad "edit allow-grow" "the refused run modified FILE, or created OUT"
rc=0
"$MACHOTOOL" edit "$T/edit_grow" "$T/edit_grow_out" "$T/grow_yes.edits" \
    >/dev/null 2>"$T/grow_yes.err" || rc=$?
[ "$rc" -eq 0 ] && ok "edit: with allow-grow, the same script succeeds" \
    || bad "edit allow-grow" "with the directive: expected 0, got $rc: $(cut -c1-160 "$T/grow_yes.err")"
grow_info=$("$MACHOTOOL" info "$T/edit_grow_out")
echo "$grow_info" | grep -qF "path=$grow_path" \
    && ok "edit: allow-grow: the appended dylib is in the written image" \
    || bad "edit allow-grow" "the appended dylib is not in the image"
echo "$grow_info" | grep -q "LC_UUID" \
    && bad "edit allow-grow" "LC_UUID survived: the statement after the grow did not apply" \
    || ok "edit: allow-grow: the statement after the grow applied to the grown image"
"$MACHOTOOL" verify "$T/edit_grow_out" >/dev/null 2>"$T/grow_verify.err" \
    && ok "edit: allow-grow: the result passes machotool verify" \
    || bad "edit allow-grow" "verify refused the result: $(cat "$T/grow_verify.err")"

# version-min set and allow-grow. LC_VERSION_MIN_MACOSX needs 16 bytes of
# header pad, and build_main's pad is far larger, so a fixture that is
# genuinely short has to be made: strip any LC_VERSION_MIN_MACOSX the
# linker emitted (strip_version_min, above), then fill the pad with a dylib
# append whose LC_LOAD_DYLIB is the largest multiple of 8 that fits. An
# LC_LOAD_DYLIB is 24 bytes plus the path and its NUL, rounded up to 8, so a
# path of C-25 bytes makes a command of exactly C, leaving pad % 8 bytes --
# fewer than 16. Sized from `machotool info`, not hard-coded, because each
# host's linker leaves a different pad.
vm_pad_of() {
    "$MACHOTOOL" info "$1" | sed -n 's/^header pad: \([0-9][0-9]*\) bytes available.*/\1/p'
}
build_main "$T/vm_tight"
"$T/strip_version_min" "$T/vm_tight" >/dev/null \
    || bad "version-min allow-grow: fixture setup" "strip_version_min failed"
vm_pad=$(vm_pad_of "$T/vm_tight")
if [ -z "$vm_pad" ] || [ "$vm_pad" -lt 32 ]; then
    bad "version-min allow-grow: fixture setup" "pad '$vm_pad' too small to size a filler"
    vm_pad=32
fi
vm_cmd=$((vm_pad - vm_pad % 8))
vm_fill="/$(printf "%$((vm_cmd - 26))s" '' | tr ' ' v)"
mtip dylib "$T/vm_tight" -append "$vm_fill" >/dev/null 2>"$T/vm_fill.err" \
    || bad "version-min allow-grow: fixture setup" "filler append failed: $(cut -c1-160 "$T/vm_fill.err")"
vm_left=$(vm_pad_of "$T/vm_tight")
[ -n "$vm_left" ] && [ "$vm_left" -lt 16 ] \
    && ok "version-min allow-grow: fixture has ${vm_left} bytes of pad, fewer than the 16 needed" \
    || bad "version-min allow-grow: fixture setup" "expected fewer than 16 bytes of pad, got '$vm_left'"

cp "$T/vm_tight" "$T/vm_e"
vm_before=$(sha "$T/vm_e"); vm_ino=$(stat -f %i "$T/vm_e")
printf 'version-min set 10.9\n' >"$T/vm_no.edits"
printf 'allow-grow\nversion-min set 10.9\n' >"$T/vm_yes.edits"
rm -f "$T/vm_e_out"
rc=0
"$MACHOTOOL" edit "$T/vm_e" "$T/vm_e_out" "$T/vm_no.edits" >/dev/null 2>"$T/vm_no.err" || rc=$?
[ "$rc" -eq 1 ] && ok "edit: version-min set without allow-grow is refused (1) when the pad is short" \
    || bad "edit version-min" "without the directive: expected 1, got $rc: $(cat "$T/vm_no.err")"
[ "$(sha "$T/vm_e")" = "$vm_before" ] && [ "$(stat -f %i "$T/vm_e")" = "$vm_ino" ] \
    && [ ! -e "$T/vm_e_out" ] \
    && ok "edit: ... and the refused run left FILE unchanged and wrote no OUT" \
    || bad "edit version-min" "the refused run modified FILE, or created OUT"
grep -q "growing the header needs allow-grow" "$T/vm_no.err" \
    && ok "edit: ... and the refusal names allow-grow as the remedy" \
    || bad "edit version-min" "no allow-grow remedy in: $(cat "$T/vm_no.err")"
rc=0
"$MACHOTOOL" edit "$T/vm_e" "$T/vm_e_out" "$T/vm_yes.edits" >"$T/vm_yes.out" 2>"$T/vm_yes.err" || rc=$?
[ "$rc" -eq 0 ] && ok "edit: version-min set with allow-grow grows the header and succeeds" \
    || bad "edit version-min" "with the directive: expected 0, got $rc: $(cat "$T/vm_yes.err")"
# The grow lines on stdout are mg_ensure_pad's, labelled with the INPUT's path
# -- the operations run against an image in memory and know nothing about OUT --
# as edit.h's inventory of what the operations print says.
grep -qF "$T/vm_e: grew header pad: " "$T/vm_yes.out" \
    && ok "edit: ... and stdout has 'PATH: grew header pad', naming the input" \
    || bad "edit version-min" "no 'PATH: grew header pad' line on stdout: $(cat "$T/vm_yes.out")"
"$MACHOTOOL" info "$T/vm_e_out" | grep -q "LC_VERSION_MIN_MACOSX" \
    && ok "edit: allow-grow: LC_VERSION_MIN_MACOSX is in the written image" \
    || bad "edit version-min" "no LC_VERSION_MIN_MACOSX after the grow"
"$MACHOTOOL" verify "$T/vm_e_out" >/dev/null 2>"$T/vm_verify.err" \
    && ok "edit: allow-grow: the grown image passes machotool verify" \
    || bad "edit version-min" "verify refused: $(cat "$T/vm_verify.err")"

# machotool minos takes --allow-grow, after the version, as dylib/rpath take
# their flags; without it the verb refuses exactly as before.
cp "$T/vm_tight" "$T/vm_m"
vm_m_before=$(sha "$T/vm_m")
rc=0
rm -f "$T/vm_m_out"
"$MACHOTOOL" minos "$T/vm_m" "$T/vm_m_out" 10.9 >/dev/null 2>"$T/vm_m_no.err" || rc=$?
[ "$rc" -eq 1 ] && ok "minos: without --allow-grow a short pad is refused (1)" \
    || bad "minos --allow-grow" "without the flag: expected 1, got $rc: $(cat "$T/vm_m_no.err")"
[ "$(sha "$T/vm_m")" = "$vm_m_before" ] && [ ! -e "$T/vm_m_out" ] \
    && ok "minos: ... and the refused run left FILE unchanged and wrote no OUT" \
    || bad "minos --allow-grow" "the refused run modified FILE, or created OUT"
grep -q "allow-grow" "$T/vm_m_no.err" \
    && ok "minos: ... and the refusal names allow-grow" \
    || bad "minos --allow-grow" "no allow-grow remedy in: $(cat "$T/vm_m_no.err")"
rc=0
"$MACHOTOOL" minos "$T/vm_m" "$T/vm_m_out" 10.9 --allow-grow >"$T/vm_m_yes.out" 2>"$T/vm_m_yes.err" || rc=$?
[ "$rc" -eq 0 ] && ok "minos: --allow-grow grows the header and adds the command" \
    || bad "minos --allow-grow" "with the flag: expected 0, got $rc: $(cat "$T/vm_m_yes.err")"
vm_m_grows=$(grep -c "grew header pad" "$T/vm_m_yes.out" || true)
[ "$vm_m_grows" -eq 1 ] \
    && ok "minos: --allow-grow: stdout has exactly one 'grew header pad' line" \
    || bad "minos --allow-grow" "expected 1 'grew header pad' line, saw $vm_m_grows: $(cat "$T/vm_m_yes.out")"
"$MACHOTOOL" info "$T/vm_m_out" | grep -q "LC_VERSION_MIN_MACOSX" \
    && ok "minos: --allow-grow: LC_VERSION_MIN_MACOSX is present" \
    || bad "minos --allow-grow" "no LC_VERSION_MIN_MACOSX after the grow"
"$MACHOTOOL" verify "$T/vm_m_out" >/dev/null 2>"$T/vm_m_verify.err" \
    && ok "minos: --allow-grow: the grown file passes machotool verify" \
    || bad "minos --allow-grow" "verify refused: $(cat "$T/vm_m_verify.err")"
rc=0
"$MACHOTOOL" minos "$T/vm_m" "$T/vm_m_out" 10.9 --bogus >/dev/null 2>&1 || rc=$?
[ "$rc" -eq 2 ] && ok "minos: an unknown flag is a usage error (2)" \
    || bad "minos" "an unknown flag: expected 2, got $rc"

# The historical add_version_min never grew, so its wrapper still refuses.
cp "$T/vm_tight" "$T/vm_w"
vm_w_before=$(sha "$T/vm_w")
rc=0
"$BIN/add_version_min" "$T/vm_w" >/dev/null 2>"$T/vm_w.err" || rc=$?
[ "$rc" -eq 1 ] && grep -q "no room for LC_VERSION_MIN_MACOSX" "$T/vm_w.err" \
    && ok "add_version_min: still refuses a short pad (1, no room; never grows)" \
    || bad "add_version_min" "expected 1 and 'no room for LC_VERSION_MIN_MACOSX', got $rc: $(cat "$T/vm_w.err")"
[ "$(sha "$T/vm_w")" = "$vm_w_before" ] \
    && ok "add_version_min: ... and the refused run left the file unchanged" \
    || bad "add_version_min" "the refused run modified the file"

echo "$caps" | grep -q "^verb minos versions=10.9 flags=allow-grow$" \
    && ok "capabilities: minos advertises allow-grow" \
    || bad "capabilities minos" "expected 'verb minos versions=10.9 flags=allow-grow': $(echo "$caps" | grep '^verb minos')"

# ============================================================================
# target 10.9 -- the one statement whose meaning depends on the binary
# ============================================================================
# `target 10.9` expands, in place, into the statements the binary actually
# needs. Detection is EXACT in every case -- a load command is present or it
# is not, a section name begins with __objc_ or it does not, a tag bit is set
# or it is not -- so these assertions pin behaviour, not a heuristic's mood.
#
# EVERY FIXTURE BELOW IS BUILT SO ITS CONDITION IS TRUE BY CONSTRUCTION, and
# the premise is then read back with otool rather than with machotool. Hoping
# the host linker emits the shape a test needs is exactly what made three
# assertions in this file pass here and fail on the cross runner, whose modern
# linker emits LC_BUILD_VERSION where 10.9's does not
# (build_main_without_build_version, above, is the fix that episode produced).
# Where a fixture is set up with machotool itself, that is circular only in
# appearance: the otool check right after is what certifies the premise, and a
# setup that silently did nothing would make the assertion fail loudly rather
# than pass for the wrong reason.
printf 'target 10.9\n' >"$T/tgt.edits"
# FILE OUT SCRIPT with a scratch OUT: a real run, write included, which is
# what this verb offers in place of a prediction.
tgt_run() {
    rm -f "$2"
    "$MACHOTOOL" edit "$1" "$2" "${3:-$T/tgt.edits}" \
        >"$T/tgt.out" 2>"$T/tgt.err"
}

build_main "$T/tgt_plain"
tgt_run "$T/tgt_plain" "$T/tgt_plain.out" || bad "target" "$(cat "$T/tgt.err")"
# The profile line is named in the report whatever the binary turns out to
# need. NOT "a fixture built for 10.9 needs nothing": build_main's fixture
# needs nothing HERE, and on the cross runner carries LC_BUILD_VERSION and so
# derives a delete for it. That is precisely the premise this block's own
# header warns against stating, so it is not stated -- the empty expansion
# gets a fixture built for it, below.
grep -qF "  target 10.9" "$T/tgt.err" \
    && ok "target: the report names the profile line" \
    || bad "target" "no target line in the report: $(cat "$T/tgt.err")"

# AN EMPTY EXPANSION IS AN ANSWER, and it is the profile's whole point: "this
# binary already targets 10.9 correctly" is correct for a profile, unlike for
# an explicit operation, so the run says so and exits 0 rather than reporting
# nothing (which would be indistinguishable from the line having done
# nothing at all).
#
# The fixture needs every one of the five detections to be false on any host,
# which no plain build_main can promise: strip LC_BUILD_VERSION and add
# LC_VERSION_MIN_MACOSX, each already otool-certified by the helper that does
# it. The other three -- chained fixups, a __DATA_CONST, Swift class records
# -- no linker on any host this repo supports can emit at all.
build_main_without_build_version "$T/tgt_empty"
mtip minos "$T/tgt_empty" 10.9 >/dev/null 2>"$T/tgt_empty_minos.err" \
    || bad "target: fixture setup" "minos failed: $(cat "$T/tgt_empty_minos.err")"
otool -l "$T/tgt_empty" 2>/dev/null | grep -q LC_VERSION_MIN_MACOSX \
    || bad "target: fixture setup" "tgt_empty has no LC_VERSION_MIN_MACOSX"
tgt_run "$T/tgt_empty" "$T/tgt_empty.out" && tgt_empty_rc=0 || tgt_empty_rc=$?
[ "$tgt_empty_rc" -eq 0 ] && [ -e "$T/tgt_empty.out" ] \
    && ok "target: a binary that needs nothing is a successful run (0), with OUT written" \
    || bad "target (empty)" "exit $tgt_empty_rc: $(cat "$T/tgt.err")"
grep -qF "    nothing to do: this binary already targets 10.9" "$T/tgt.err" \
    && ok "target: ... and the report says so rather than saying nothing" \
    || bad "target (empty)" "no 'nothing to do' line: $(cat "$T/tgt.err")"

# ROW 1: LC_DYLD_CHAINED_FIXUPS present -> fixups set classic.
# ROW 2: LC_BUILD_VERSION present -> load-command delete build-version.
# mkchained's hand-built image carries both by construction (10.9's linker
# predates chained fixups by a decade, so no host can be asked for one).
# The premise is read by mkchained's own `check`, not by otool: chained
# fixups, the exports trie and LC_BUILD_VERSION all postdate 10.9, and 10.9's
# otool prints them as "Unknown load command", so an otool grep for those
# names could only ever hold on the cross runner. A reader built beside the
# fixture asks the same question on every host -- the reason tests/README.md's
# host-portability section gives for these readers existing -- and it is still
# not machotool, so it cannot certify its own setup.
"$T/mkchained" make "$T/tgt_chained"
tgt_pre=$("$T/mkchained" check "$T/tgt_chained")
echo "$tgt_pre" | grep -q "^chained=1" && echo "$tgt_pre" | grep -q "^buildver=1" \
    || bad "target: fixture setup" "tgt_chained lacks chained fixups or build-version: $(echo "$tgt_pre" | tr '\n' ' ')"
tgt_run "$T/tgt_chained" "$T/tgt_chained.out" || bad "target (chained)" "$(cat "$T/tgt.err")"
grep -qF "    fixups set classic  (LC_DYLD_CHAINED_FIXUPS present)" "$T/tgt.err" \
    && ok "target: chained fixups expand to fixups set classic" \
    || bad "target (chained)" "no fixups line: $(cat "$T/tgt.err")"
grep -qF "    load-command delete build-version  (LC_BUILD_VERSION present)" "$T/tgt.err" \
    && ok "target: LC_BUILD_VERSION expands to load-command delete build-version" \
    || bad "target (chained)" "no build-version line: $(cat "$T/tgt.err")"
# The expansion RAN, it was not merely reported: the output is classic.
tgt_chk=$("$T/mkchained" check "$T/tgt_chained.out")
echo "$tgt_chk" | grep -q "^chained=0" && echo "$tgt_chk" | grep -q "^dyldinfo=1" \
    && echo "$tgt_chk" | grep -q "^buildver=0" \
    && ok "target: ... and the written image is classic, with no LC_BUILD_VERSION" \
    || bad "target (chained)" "not converted: $(echo "$tgt_chk" | tr '\n' ' ')"
"$MACHOTOOL" verify "$T/tgt_chained.out" >/dev/null 2>"$T/tgt_v.err" \
    && ok "target: ... and the result passes machotool verify" \
    || bad "target (chained)" "verify refused: $(cat "$T/tgt_v.err")"
# The inverse, so the detection is not "always emit it": a fixture with no
# LC_BUILD_VERSION derives no delete for one.
#
# Every one of these inverses pairs its negative grep with a positive one: a
# report that stopped being produced at all would satisfy "no build-version
# line" just as well as a correct detection does, and then four assertions
# would pass vacuously, together, for the one reason that ought to fail them.
build_main_without_build_version "$T/tgt_nobv"
tgt_run "$T/tgt_nobv" "$T/tgt_nobv.out" || bad "target (no build-version)" "$(cat "$T/tgt.err")"
# Written as a nested `if` with no `!` anywhere: a !-negated command never
# trips errexit, so the shape is a hazard wherever it is not a test's last
# line, and the family's shell-portability gate refuses it outright.
if grep -qF "  target 10.9" "$T/tgt.err"; then
    if grep -q "load-command delete build-version" "$T/tgt.err"; then
        bad "target (no build-version)" "a delete was derived for an image with no LC_BUILD_VERSION: $(cat "$T/tgt.err")"
    else
        ok "target: an image without LC_BUILD_VERSION derives no delete for it"
    fi
else
    bad "target (no build-version)" "no report at all, so the absent build-version line proves nothing: $(cat "$T/tgt.err")"
fi

# ROW 3: no LC_VERSION_MIN_MACOSX -> version-min set 10.9. strip_version_min
# makes the premise true whatever the host's linker emitted.
build_main "$T/tgt_novm"
"$T/strip_version_min" "$T/tgt_novm" >/dev/null \
    || bad "target: fixture setup" "strip_version_min failed"
if otool -l "$T/tgt_novm" 2>/dev/null | grep -q LC_VERSION_MIN_MACOSX; then
    bad "target: fixture setup" "tgt_novm still has LC_VERSION_MIN_MACOSX"
fi
tgt_run "$T/tgt_novm" "$T/tgt_novm.out" || bad "target (version-min)" "$(cat "$T/tgt.err")"
grep -qF "    version-min set 10.9  (no LC_VERSION_MIN_MACOSX)" "$T/tgt.err" \
    && ok "target: a missing LC_VERSION_MIN_MACOSX expands to version-min set 10.9" \
    || bad "target (version-min)" "no version-min line: $(cat "$T/tgt.err")"
otool -l "$T/tgt_novm.out" 2>/dev/null | grep -q LC_VERSION_MIN_MACOSX \
    && ok "target: ... and the written image has the command" \
    || bad "target (version-min)" "no LC_VERSION_MIN_MACOSX in the output"
# The inverse: one that already has it. minos puts it there whatever the
# linker did, and is a no-op on an image that already had one.
build_main "$T/tgt_hasvm"
mtip minos "$T/tgt_hasvm" 10.9 >/dev/null 2>"$T/tgt_minos.err" \
    || bad "target: fixture setup" "minos failed: $(cat "$T/tgt_minos.err")"
otool -l "$T/tgt_hasvm" 2>/dev/null | grep -q LC_VERSION_MIN_MACOSX \
    || bad "target: fixture setup" "tgt_hasvm has no LC_VERSION_MIN_MACOSX"
tgt_run "$T/tgt_hasvm" "$T/tgt_hasvm.out" || bad "target (has version-min)" "$(cat "$T/tgt.err")"
if grep -qF "  target 10.9" "$T/tgt.err" && ! grep -q "version-min set" "$T/tgt.err"; then
    ok "target: an image that already has LC_VERSION_MIN_MACOSX derives no version-min"
else
    bad "target (has version-min)" "expected a report with no version-min line: $(cat "$T/tgt.err")"
fi

# ROW 4: __DATA_CONST carrying __objc_* sections -> segment rename. No host
# linker here emits __DATA_CONST either (Xcode 10 and later do), so the
# fixture is mkswift's __DATA image with its segment renamed the other way --
# and otool, not machotool, says the premise held.
"$T/mkswift" make "$T/tgt_dc"
mtip segment "$T/tgt_dc" __DATA __DATA_CONST >/dev/null 2>"$T/tgt_seg.err" \
    || bad "target: fixture setup" "segment rename failed: $(cat "$T/tgt_seg.err")"
otool -l "$T/tgt_dc" 2>/dev/null | grep -q "segname __DATA_CONST" \
    && otool -l "$T/tgt_dc" 2>/dev/null | grep -q "sectname __objc_classlist" \
    || bad "target: fixture setup" "tgt_dc is not a __DATA_CONST carrying __objc_ sections"
tgt_run "$T/tgt_dc" "$T/tgt_dc.out" || bad "target (__DATA_CONST)" "$(cat "$T/tgt.err")"
grep -qF "    segment rename __DATA_CONST __DATA  (__DATA_CONST carries __objc_ sections)" \
    "$T/tgt.err" \
    && ok "target: a __DATA_CONST carrying __objc_ sections expands to the rename" \
    || bad "target (__DATA_CONST)" "no segment rename line: $(cat "$T/tgt.err")"
otool -l "$T/tgt_dc.out" 2>/dev/null | grep -q "segname __DATA_CONST" \
    && bad "target (__DATA_CONST)" "__DATA_CONST survived the expansion" \
    || ok "target: ... and the written image has no __DATA_CONST left"
# The inverse: the same fixture before the rename has its __objc_ sections in
# __DATA already, so there is nothing to rename.
"$T/mkswift" make "$T/tgt_nodc"
if otool -l "$T/tgt_nodc" 2>/dev/null | grep -q "segname __DATA_CONST"; then
    bad "target: fixture setup" "tgt_nodc unexpectedly has a __DATA_CONST"
fi
tgt_run "$T/tgt_nodc" "$T/tgt_nodc.out" || bad "target (no __DATA_CONST)" "$(cat "$T/tgt.err")"
if grep -qF "  target 10.9" "$T/tgt.err" && ! grep -q "segment rename" "$T/tgt.err"; then
    ok "target: an image with no __DATA_CONST derives no segment rename"
else
    bad "target (no __DATA_CONST)" "expected a report with no segment rename line: $(cat "$T/tgt.err")"
fi

# ROW 5: class records carrying the stable-ABI Swift tag -> swift-abi set
# legacy. mkswift's records carry tag bit 1 (value 2) by construction, and
# mkswift's own reader -- not machotool -- says so, before and after. The same
# fixture as the row above, run again so this row stands on its own.
tgt_tags=$("$T/mkswift" tags "$T/tgt_nodc")
tgt_run "$T/tgt_nodc" "$T/tgt_nodc.out" || bad "target (swift)" "$(cat "$T/tgt.err")"
echo "$tgt_tags" | grep -qx "class 2 0x1000009c2" \
    && ok "target: fixture setup: the Swift fixture carries the stable-ABI tag" \
    || bad "target: fixture setup" "not the stable-ABI tag: $tgt_tags"
grep -qF "    swift-abi set legacy  (class records carry the stable-ABI Swift tag)" "$T/tgt.err" \
    && ok "target: the stable-ABI Swift tag expands to swift-abi set legacy" \
    || bad "target (swift)" "no swift-abi line: $(cat "$T/tgt.err")"
"$T/mkswift" tags "$T/tgt_nodc.out" | grep -qx "class 1 0x1000009c1" \
    && ok "target: ... and the written image's records carry the legacy tag" \
    || bad "target (swift)" "not retagged: $("$T/mkswift" tags "$T/tgt_nodc.out")"
# The inverse: build_main's fixture has no Objective-C at all.
tgt_run "$T/tgt_plain" "$T/tgt_plain.out" || bad "target (no swift)" "$(cat "$T/tgt.err")"
if grep -qF "  target 10.9" "$T/tgt.err" && ! grep -q "swift-abi set" "$T/tgt.err"; then
    ok "target: an image with no Swift class records derives no swift-abi"
else
    bad "target (no swift)" "expected a report with no swift-abi line: $(cat "$T/tgt.err")"
fi

# IT EXPANDS WHERE IT IS WRITTEN. Position is not cosmetic: fixups set
# classic rewrites __LINKEDIT, which changes the header pad available to
# every dylib replace after it, and this tool does not reorder statements --
# the script is the plan. So the expansion has to land at the target line's
# own position, which the report's order is what shows.
tgt_at() { grep -n "$2" "$1" | head -1 | cut -d: -f1; }
printf 'load-command delete uuid\ntarget 10.9\n' >"$T/tgt_after.edits"
printf 'target 10.9\nload-command delete uuid\n' >"$T/tgt_before.edits"
"$T/mkchained" make "$T/tgt_pos1"
"$T/mkchained" make "$T/tgt_pos2"
tgt_run "$T/tgt_pos1" "$T/tgt_pos1.out" "$T/tgt_after.edits" \
    || bad "target (position)" "$(cat "$T/tgt.err")"
tgt_uuid=$(tgt_at "$T/tgt.err" "^  load-command delete uuid$")
tgt_fx=$(tgt_at "$T/tgt.err" "^    fixups set classic")
[ -n "$tgt_uuid" ] && [ -n "$tgt_fx" ] && [ "$tgt_uuid" -lt "$tgt_fx" ] \
    && ok "target: written last, its expansion is reported last" \
    || bad "target (position)" "uuid at '$tgt_uuid', expansion at '$tgt_fx': $(cat "$T/tgt.err")"
tgt_run "$T/tgt_pos2" "$T/tgt_pos2.out" "$T/tgt_before.edits" \
    || bad "target (position)" "$(cat "$T/tgt.err")"
tgt_uuid=$(tgt_at "$T/tgt.err" "^  load-command delete uuid$")
tgt_fx=$(tgt_at "$T/tgt.err" "^    fixups set classic")
[ -n "$tgt_uuid" ] && [ -n "$tgt_fx" ] && [ "$tgt_fx" -lt "$tgt_uuid" ] \
    && ok "target: written first, its expansion is reported first" \
    || bad "target (position)" "expansion at '$tgt_fx', uuid at '$tgt_uuid': $(cat "$T/tgt.err")"

# TARGET NEVER COUNTS AS UNMATCHED UNDER fatal-warnings. On a chained image
# the expansion derives both `fixups set classic` and `load-command delete
# build-version` -- and the first strips LC_BUILD_VERSION itself, so the
# second finds nothing left to do. "This binary already targets 10.9
# correctly" is a correct answer for a profile, so that is not a miss.
printf 'fatal-warnings\ntarget 10.9\n' >"$T/tgt_fw.edits"
"$T/mkchained" make "$T/tgt_fw"
tgt_run "$T/tgt_fw" "$T/tgt_fw.out" "$T/tgt_fw.edits" && tgt_fw_rc=0 || tgt_fw_rc=$?
[ "$tgt_fw_rc" -eq 0 ] && [ -e "$T/tgt_fw.out" ] \
    && ok "target: a derived statement that matches nothing is not a miss under fatal-warnings" \
    || bad "target (fatal-warnings)" "exit $tgt_fw_rc: $(cat "$T/tgt.err")"
grep -q "matched nothing" "$T/tgt.err" \
    && bad "target (fatal-warnings)" "reported a derived statement as unmatched: $(cat "$T/tgt.err")" \
    || ok "target: ... and nothing is reported as having matched nothing"

# THE OTHER SIDE OF THAT, WHICH IS NOT SPECIAL-CASED: writing `target 10.9`
# AND an explicit statement it would have derived makes the explicit one
# redundant, and fatal-warnings flags it. Same script as above with one line
# added -- the expansion's `fixups set classic` strips LC_BUILD_VERSION, so by
# the time the EXPLICIT `load-command delete build-version` runs there is
# nothing of that kind left. A statement somebody wrote that matched nothing
# is a miss, and under fatal-warnings a refusal. Both halves are documented
# rather than smoothed over, so both halves are pinned.
printf 'fatal-warnings\ntarget 10.9\nload-command delete build-version\n' >"$T/tgt_redundant.edits"
"$T/mkchained" make "$T/tgt_redundant"
tgt_red_before=$(sha "$T/tgt_redundant")
tgt_run "$T/tgt_redundant" "$T/tgt_redundant.out" "$T/tgt_redundant.edits" \
    && tgt_red_rc=0 || tgt_red_rc=$?
[ "$tgt_red_rc" -eq 1 ] && [ ! -e "$T/tgt_redundant.out" ] \
    && [ "$(sha "$T/tgt_redundant")" = "$tgt_red_before" ] \
    && ok "target: an explicit statement the expansion already did is redundant, and fatal-warnings refuses it (1)" \
    || bad "target (redundant)" "expected 1 and no OUT, got $tgt_red_rc: $(cat "$T/tgt.err")"
grep -q "no load command of kind build-version to delete" "$T/tgt.err" \
    && ok "target: ... and the miss reported is the explicit statement's, not the derived one's" \
    || bad "target (redundant)" "no miss reported for the explicit statement: $(cat "$T/tgt.err")"
# Without fatal-warnings the same script is a report and not a refusal, which
# is what makes the line above fatal-warnings' doing rather than target's.
printf 'target 10.9\nload-command delete build-version\n' >"$T/tgt_redlax.edits"
"$T/mkchained" make "$T/tgt_redlax"
tgt_run "$T/tgt_redlax" "$T/tgt_redlax.out" "$T/tgt_redlax.edits" \
    && tgt_redlax_rc=0 || tgt_redlax_rc=$?
[ "$tgt_redlax_rc" -eq 0 ] && [ -e "$T/tgt_redlax.out" ] \
    && ok "target: ... and without fatal-warnings the same redundancy is only reported" \
    || bad "target (redundant)" "expected 0 and an OUT, got $tgt_redlax_rc: $(cat "$T/tgt.err")"

# THE DIRECTIVES STILL GOVERN THE EXPANSION, and a derived statement's
# refusal is reported against the line the operator actually wrote. The
# fixture has no LC_VERSION_MIN_MACOSX and a pad too short for one, so the
# derived `version-min set 10.9` is refused without allow-grow and succeeds
# with it -- the same answer the explicit statement gets.
#
# NOT $T/vm_tight, though it is the same shape: this needs LC_BUILD_VERSION
# ABSENT too. On a host whose linker emits one, the expansion would derive a
# `load-command delete build-version` that runs first and frees at least 24
# bytes -- more than the 16 LC_VERSION_MIN_MACOSX needs -- so the run would
# succeed and this assertion would fail there and pass here. Same trap,
# same fix: make the premise true (build_main_without_build_version) rather
# than assume it.
build_main_without_build_version "$T/tgt_tight"
"$T/strip_version_min" "$T/tgt_tight" >/dev/null \
    || bad "target: fixture setup" "strip_version_min failed on tgt_tight"
tgt_pad=$(vm_pad_of "$T/tgt_tight")
if [ -z "$tgt_pad" ] || [ "$tgt_pad" -lt 32 ]; then
    bad "target: fixture setup" "pad '$tgt_pad' too small to size a filler"
    tgt_pad=32
fi
tgt_cmd=$((tgt_pad - tgt_pad % 8))
tgt_fill="/$(printf "%$((tgt_cmd - 26))s" '' | tr ' ' t)"
mtip dylib "$T/tgt_tight" -append "$tgt_fill" >/dev/null 2>"$T/tgt_fill.err" \
    || bad "target: fixture setup" "filler append failed: $(cut -c1-160 "$T/tgt_fill.err")"
tgt_left=$(vm_pad_of "$T/tgt_tight")
[ -n "$tgt_left" ] && [ "$tgt_left" -lt 16 ] \
    && ok "target: fixture setup: ${tgt_left} bytes of pad, fewer than the 16 version-min needs" \
    || bad "target: fixture setup" "expected fewer than 16 bytes of pad, got '$tgt_left'"
tgt_before_sha=$(sha "$T/tgt_tight")
tgt_run "$T/tgt_tight" "$T/tgt_tight.out" && tgt_tight_rc=0 || tgt_tight_rc=$?
[ "$tgt_tight_rc" -eq 1 ] && [ ! -e "$T/tgt_tight.out" ] \
    && [ "$(sha "$T/tgt_tight")" = "$tgt_before_sha" ] \
    && ok "target: a derived statement that needs allow-grow is refused (1) without it" \
    || bad "target (allow-grow)" "expected 1 and no OUT, got $tgt_tight_rc: $(cat "$T/tgt.err")"
grep -qF "machotool edit: refused at statement 1 of 1 (line 1);" "$T/tgt.err" \
    && ok "target: ... and the refusal names the target line, not a line nobody wrote" \
    || bad "target (allow-grow)" "not that wording: $(cat "$T/tgt.err")"
printf 'allow-grow\ntarget 10.9\n' >"$T/tgt_ag.edits"
tgt_run "$T/tgt_tight" "$T/tgt_tight.out" "$T/tgt_ag.edits" \
    || bad "target (allow-grow)" "with the directive: $(cat "$T/tgt.err")"
otool -l "$T/tgt_tight.out" 2>/dev/null | grep -q LC_VERSION_MIN_MACOSX \
    && ok "target: with allow-grow, the same expansion grows the header and lands" \
    || bad "target (allow-grow)" "no LC_VERSION_MIN_MACOSX after the grow"

# ONE TARGET PER SCRIPT, AND AN UNKNOWN ONE IS A REFUSAL -- both at parse
# time, so nothing is read and nothing is written.
printf 'target 10.9\ntarget 10.9\n' >"$T/tgt_two.edits"
rm -f "$T/tgt_two.out"
rc=0
"$MACHOTOOL" edit "$T/tgt_plain" "$T/tgt_two.out" "$T/tgt_two.edits" \
    >/dev/null 2>"$T/tgt_two.err" || rc=$?
[ "$rc" -eq 2 ] && [ ! -e "$T/tgt_two.out" ] && grep -q "line 2" "$T/tgt_two.err" \
    && ok "target: a second target is a parse error (2) naming its line" \
    || bad "target (two)" "expected 2 and 'line 2', got $rc: $(cat "$T/tgt_two.err")"
printf 'target 10.10\n' >"$T/tgt_unknown.edits"
rm -f "$T/tgt_unknown.out"
rc=0
"$MACHOTOOL" edit "$T/tgt_plain" "$T/tgt_unknown.out" "$T/tgt_unknown.edits" \
    >/dev/null 2>"$T/tgt_unknown.err" || rc=$?
[ "$rc" -eq 2 ] && [ ! -e "$T/tgt_unknown.out" ] \
    && grep -q "10.10" "$T/tgt_unknown.err" && grep -q "10.9" "$T/tgt_unknown.err" \
    && ok "target: an unknown target errors (2) rather than silently doing 10.9's work" \
    || bad "target (unknown)" "expected 2 naming 10.10 and 10.9, got $rc: $(cat "$T/tgt_unknown.err")"

# ============================================================================
# edit on a fat file, end to end
# ============================================================================
# edit on a fat file, end to end: two build_main executables in one
# container, the second labelled arm64 in its fat_arch entry (edit names a
# slice by that entry). allow-grow on the x86_64 slice alone grows it by a
# page, so the arm64 slice after it has to move -- the one consequence a
# passed-through slice can have, and the report must say so. The appended
# path is sized from the slice's own pad, not hard-coded, because each
# host's linker leaves a different pad.
build_main "$T/fat_s0"
build_main "$T/fat_s1"
"$BIN/makefat" "$T/fat_edit" "$T/fat_s0" 0x1000007 3 12 "$T/fat_s1" 0x100000c 0 12
fat_pad=$("$MACHOTOOL" info "$T/fat_s0" | sed -n 's/^header pad: \([0-9][0-9]*\) bytes available.*/\1/p')
[ -n "$fat_pad" ] || { bad "edit fat: fixture setup" "no header pad reported"; fat_pad=0; }
fat_path="/$(printf "%${fat_pad}s" '' | tr ' ' f)"
printf 'arch x86_64\nallow-grow\ndylib append %s\n' "$fat_path" >"$T/fat.edits"
rc=0
"$MACHOTOOL" edit "$T/fat_edit" "$T/fat_edit_out" "$T/fat.edits" \
    >/dev/null 2>"$T/fat.err" || rc=$?
[ "$rc" -eq 0 ] && ok "edit: a fat file's x86_64 slice is edited, growing it" \
    || bad "edit fat" "expected 0, got $rc: $(cut -c1-200 "$T/fat.err")"
grep -q "slice arm64: not selected by arch; passed through unchanged" "$T/fat.err" \
    && ok "edit: the report accounts for the unselected arm64 slice" \
    || bad "edit fat" "no pass-through line: $(cut -c1-300 "$T/fat.err")"
grep -q "slice arm64: moved from offset" "$T/fat.err" \
    && ok "edit: the report says the arm64 slice moved when the x86_64 slice grew" \
    || bad "edit fat" "no moved line: $(cut -c1-300 "$T/fat.err")"
"$BIN/fatcheck" dump "$T/fat_edit_out" 0 "$T/fat_out0"
"$BIN/fatcheck" dump "$T/fat_edit_out" 1 "$T/fat_out1"
"$MACHOTOOL" info "$T/fat_out0" | grep -qF "path=$fat_path" \
    && ok "edit: the x86_64 slice carries the appended dylib" \
    || bad "edit fat" "the appended dylib is not in slice 0"
"$MACHOTOOL" verify "$T/fat_out0" >/dev/null 2>"$T/fat_v.err" \
    && ok "edit: the grown x86_64 slice passes machotool verify" \
    || bad "edit fat" "verify refused slice 0: $(cat "$T/fat_v.err")"
cmp -s "$T/fat_out1" "$T/fat_s1" \
    && ok "edit: the arm64 slice is byte-identical, though it moved" \
    || bad "edit fat" "the arm64 slice changed"

# target 10.9 IS DETECTED PER SLICE, which is the whole of what "its meaning
# depends on the binary" means for a fat file: one container, two slices, one
# `target 10.9` line, and two different expansions. The second slice is
# mkchained's chained-fixups image, labelled arm64 in its fat_arch entry the
# way fat_s1 is above -- so exactly one slice has chained fixups, and exactly
# one `fixups set classic` may be derived across the whole run.
"$T/mkchained" make "$T/fat_tgt1"
"$BIN/makefat" "$T/fat_tgt" "$T/tgt_plain" 0x1000007 3 12 "$T/fat_tgt1" 0x100000c 0 12
rm -f "$T/fat_tgt.out"
rc=0
"$MACHOTOOL" edit "$T/fat_tgt" "$T/fat_tgt.out" "$T/tgt.edits" \
    >/dev/null 2>"$T/fat_tgt.err" || rc=$?
[ "$rc" -eq 0 ] && ok "target: a fat file's slices are each expanded" \
    || bad "target fat" "expected 0, got $rc: $(cat "$T/fat_tgt.err")"
fat_tgt_n=$(grep -c "^  target 10.9$" "$T/fat_tgt.err" || true)
fat_tgt_fx=$(grep -c "^    fixups set classic" "$T/fat_tgt.err" || true)
[ "$fat_tgt_n" -eq 2 ] && [ "$fat_tgt_fx" -eq 1 ] \
    && ok "target: ... the same line in both slices, expanding differently in each" \
    || bad "target fat" "expected 2 target lines and 1 fixups line, got $fat_tgt_n and $fat_tgt_fx: $(cat "$T/fat_tgt.err")"

printf 'arch amd64\nload-command delete uuid\n' >"$T/fat_bad.edits"
rc=0
"$MACHOTOOL" edit "$T/fat_edit" "$T/fat_edit_out" "$T/fat_bad.edits" \
    >/dev/null 2>"$T/fat_bad.err" || rc=$?
[ "$rc" -eq 2 ] && ok "edit: an unknown arch name is a parse error (2)" \
    || bad "edit fat" "arch amd64: expected 2, got $rc: $(cat "$T/fat_bad.err")"

reached_end=1
echo "cli_test: $fails failure(s)"
[ "$fails" -eq 0 ]
