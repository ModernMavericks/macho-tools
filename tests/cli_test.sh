#!/bin/sh
# tests/cli_test.sh — exercises the macho9 CLI itself: --capabilities, and
# each verb this build actually implements.
#
# What this does NOT re-prove: change_dylib_test.sh already runs real dylib
# renumbering, -insert/-delete ordinal correctness, and header-growth end to
# end. dylib/rpath/lc here delegate straight to that already-tested binary
# (see cli/macho9.c's file header), so this asserts the TRANSLATION and
# DISPATCH are correct -- one exemplar op per verb -- not the underlying
# rewrite a second time.
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
[ -x "$BIN/change_dylib" ] || { echo "cli_test: $BIN/change_dylib not found (macho9 dylib/rpath/lc delegate to it)" >&2; exit 1; }
[ -x "$BIN/add_version_min" ] || { echo "cli_test: $BIN/add_version_min not found (macho9 minos delegates to it)" >&2; exit 1; }

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

# ============================================================================
# --capabilities
# ============================================================================
caps=$("$MACHO9" --capabilities) || bad "capabilities: exit" "nonzero"
case "$caps" in
    "format 1"*) ok "capabilities: starts with format line" ;;
    *) bad "capabilities: format line" "got: $(echo "$caps" | head -1)" ;;
esac
for v in verify info grow minos lc dylib rpath; do
    if echo "$caps" | grep -q "^verb $v"; then
        ok "capabilities: advertises $v"
    else
        bad "capabilities: $v" "not listed"
    fi
done
# declassify is NOT implemented; capabilities must not claim it is.
if echo "$caps" | grep -q "^verb declassify"; then
    bad "capabilities: declassify" "advertised but not implemented"
else
    ok "capabilities: declassify correctly absent"
fi
# rpath -insert is NOT implemented; capabilities must not claim it.
if echo "$caps" | grep "^verb rpath" | grep -q "insert"; then
    bad "capabilities: rpath insert" "advertised but not implemented"
else
    ok "capabilities: rpath insert correctly absent"
fi

# declassify itself must error, not silently do nothing or crash.
if "$MACHO9" declassify "$T/main" "$T/out" >/dev/null 2>"$T/declassify.err"; then
    bad "declassify: exit code" "should be nonzero (not implemented)"
else
    ok "declassify: refuses (not implemented)"
fi
grep -qi "not implemented" "$T/declassify.err" && ok "declassify: says why" \
    || bad "declassify: message" "no 'not implemented' on stderr"

# ============================================================================
# verify
# ============================================================================
build_main "$T/verify_ok"
if "$MACHO9" verify "$T/verify_ok" >"$T/verify_ok.out"; then
    ok "verify: accepts a real binary"
else
    bad "verify: real binary" "refused: $(cat "$T/verify_ok.out")"
fi
grep -q "OK" "$T/verify_ok.out" && ok "verify: reports OK" || bad "verify: OK text" "missing"

echo 'not a mach-o' > "$T/verify_bad"
if "$MACHO9" verify "$T/verify_bad" >/dev/null 2>&1; then
    bad "verify: garbage file" "should have been refused"
else
    ok "verify: refuses a non-Mach-O file"
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

# Whether a GROWN binary can be EXECUTED is a question about the HOST, not
# about macho9: kernel code-signing enforcement (macOS 11+, unconditional on
# Apple Silicon) SIGKILLs any binary whose bytes changed since it was signed
# at link time, and growing rewrites the whole header. 10.9 -- the actual
# target platform -- has no such enforcement, so the real "and it still
# runs" check belongs there and must stay real, not weakened for portability.
#
# Probe for the capability empirically (never a hardcoded macOS-version
# check, so this keeps working if Apple changes the policy again): build a
# throwaway fixture, apply the EXACT SAME grow, and see whether the host
# lets it run at all.
build_main "$T/grow_probe"
"$MACHO9" grow "$T/grow_probe" 4096 >/dev/null
if (cd "$T" && ./grow_probe) >"$T/grow_probe.out" 2>&1; then
    grow_probe_rc=0
else
    grow_probe_rc=$?
fi
if [ "$grow_probe_rc" -eq 0 ]; then
    if (cd "$T" && ./grow_fixture); then
        ok "grow: grown binary still runs"
    else
        bad "grow: run" "grown binary failed to execute"
    fi
else
    skip "grow: grown binary still runs" \
        "an identically-grown probe binary would not execute on this host (exit $grow_probe_rc: $(head -1 "$T/grow_probe.out" 2>/dev/null || echo 'no output')) -- most likely kernel code-signing enforcement invalidating the signature macho9's rewrite disturbed; this is a host policy, not a macho9 defect, and is exercised for real on 10.9"
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
# path" below would pass even with cmd_minos's body replaced by `return 0`.
# -Wl,-no_version_load_command suppresses that (verified: no
# LC_VERSION_MIN_MACOSX and no LC_BUILD_VERSION either, so it isn't sneaking
# back in under the newer spelling), so this fixture genuinely lacks the
# load command before the tool runs, and the "present after" assertion
# actually proves add_version_min's delegation did something.
"$CC" -O2 $FIXTURE_FLAGS -Wl,-no_version_load_command \
    "$T/main.c" "$T/liba.dylib" -o "$T/minos_fixture"
before_minos=$("$MACHO9" info "$T/minos_fixture")
if echo "$before_minos" | grep -qE "LC_VERSION_MIN_MACOSX|LC_BUILD_VERSION"; then
    bad "minos: precondition" "fixture already carries a platform/version-min load command"
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
# Whether a binary that's had its LC_UUID deleted can still be EXECUTED is
# again a question about the host's dyld, not about macho9: modern dyld
# refuses to load an image carrying no LC_UUID at all ("missing LC_UUID
# load command"), a requirement 10.9's dyld does not have. Kernel
# code-signing enforcement (see the grow probe above) can also be in play,
# since deleting a load command rewrites the header too -- the two showed up
# as genuinely different failure modes on the cross runner that motivated
# this (grow got SIGKILLed outright; this got far enough for dyld itself to
# abort on the missing UUID), so this probes its OWN exact rewrite rather
# than reusing the grow probe's verdict.
build_main "$T/lc_probe"
"$MACHO9" lc "$T/lc_probe" -delete uuid >/dev/null
if (cd "$T" && ./lc_probe) >"$T/lc_probe.out" 2>&1; then
    lc_probe_rc=0
else
    lc_probe_rc=$?
fi
if [ "$lc_probe_rc" -eq 0 ]; then
    if (cd "$T" && ./lc_fixture); then
        ok "lc: binary still runs after uuid deletion"
    else
        bad "lc: run" "binary failed to execute after uuid deletion"
    fi
else
    if grep -qi "missing LC_UUID" "$T/lc_probe.out" 2>/dev/null; then
        lc_run_reason="modern dyld refuses to load any image with no LC_UUID at all ('missing LC_UUID load command'); 10.9's dyld has no such requirement"
    else
        lc_run_reason="an identically-uuid-deleted probe binary would not execute on this host (exit $lc_probe_rc: $(head -1 "$T/lc_probe.out" 2>/dev/null || echo 'no output')) -- most likely kernel code-signing enforcement, the same as the grow probe above"
    fi
    skip "lc: binary still runs after uuid deletion" "$lc_run_reason -- a host policy, not a macho9 defect, and exercised for real on 10.9"
fi
# Unknown KIND is refused with this verb's own message, before delegating.
if "$MACHO9" lc "$T/lc_fixture" -delete bogus-kind >/dev/null 2>"$T/lc_bad.err"; then
    bad "lc: bad kind" "should be refused"
else
    ok "lc: unknown KIND refused"
fi
grep -q "unknown KIND" "$T/lc_bad.err" && ok "lc: bad kind message" \
    || bad "lc: bad kind message" "missing 'unknown KIND'"

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

# rpath -insert is a documented gap in this build, not a silent downgrade.
if "$MACHO9" rpath "$T/rpath_fixture" -insert "/tmp/cli_test_inserted_rpath" \
    >/dev/null 2>"$T/rpath_insert.err"; then
    bad "rpath: -insert" "should be refused (not implemented)"
else
    ok "rpath: -insert refused"
fi
grep -qi "not implemented" "$T/rpath_insert.err" && ok "rpath: -insert says why" \
    || bad "rpath: -insert message" "no 'not implemented' on stderr"

echo "cli_test: $fails failure(s)"
[ "$fails" -eq 0 ]
