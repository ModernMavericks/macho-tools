#!/bin/sh
# tests/known-callers.sh -- replay every KNOWN CALLER of the six historical
# tools, end to end, against the shell wrappers that replaced five of them.
#
#   sh tests/known-callers.sh <bindir>
#
# WHY THIS TEST IS THE GATE. The compat-retirement plan says it outright:
# "A wrapper that passes the test suite but breaks a real caller is a
# failure." It weights the exhaustive argument sweep (tests/compat-sweep.sh)
# as DISCOVERY and these replays as the decisive gate -- a surprise in the
# sweep starts a conversation, a failure here stops the work. Task 0 of that
# plan enumerated the callers from evidence (it fetched and read
# mavericksforever.com/claude/install.sh in full, and grepped every
# mavericks-* checkout on this machine); this file is that list, executed.
#
# THE CALLERS, and where each one's invocation comes from:
#
#   1. mavericksforever.com/claude/install.sh's generated /usr/local/bin/claude
#      wrapper -- the ONE production caller. Three tools in a fixed order
#      against the user's real Claude Code binary. Quoted verbatim in the plan.
#   2. mavericks-claude-ongoing/scripts/mf-wrapper-rebase.sh -- the repo
#      owner's own local rebase of that wrapper, which adds `-insert` to link
#      libavxemu.dylib as an ordinary dependency.
#   3. mavericks-magic-trackpad2's recorded, exact Bash permission entries --
#      a slightly different shape (two patch_macho runs, and three -change
#      flags with NO -strip-lc, so change_dylib translates to ONE macho9
#      command rather than two).
#
# The other callers Task 0 found are this repo's own suites, and they are
# already ctest entries in their own right rather than replays here:
# characterize (the same three-tool pipeline, digest-pinned against
# tests/EXPECTED), chained_fixups (the same pipeline over a chained-fixups
# fixture, on a host whose linker can emit one), change_dylib_test, cli_test
# and leaf_tool_crashes.
#
# HOW "OUTPUT COMPARED" WORKS NOW THAT THE C TOOLS ARE GONE. Each pipeline's
# result is pinned to the SHA-256 the C binaries built from commit 91b30b3
# (the last commit carrying all six compat/*.c files) produced from
# tests/fixture.macho, measured on real 10.9 hardware (Darwin 13.4, x86_64)
# by building that commit and running these exact command lines. tests/
# README.md's "Not run by ctest" section has the full account, including
# f500021, an earlier landmark (the last pre-extraction originals) that is
# NOT what these digests were measured against. That is the same device
# tests/EXPECTED uses, and for
# the same reason: the binaries that produced the reference cannot be kept,
# but what they produced can.
#
# The digests below are therefore evidence, not expectations invented here.
# If one moves, a wrapper has changed what a real caller gets -- fix the
# wrapper, do not update the number.
#
# WHAT EACH CALLER ACTUALLY DEPENDS ON (Task 0's evidence, quoted in the
# plan's "How the callers use stdout, stderr, and exit codes"): every one
# redirects stdout to /dev/null and checks the EXIT CODE -- `"$MF/$tool" ...
# >/dev/null || { echo "claude: $tool failed" >&2; exit 1; }`. None parses
# stdout as data. So this file asserts exit codes and resulting bytes first,
# and the two stdout properties a caller can still observe second.
#
# set -u and not set -e: a failure here has to be REPORTED with its name, not
# turned into a bare nonzero exit from whichever line tripped first.
set -u

BIN="${1:?usage: known-callers.sh <bindir>}"
HERE=$(cd "$(dirname "$0")" && pwd)
FIXTURE="$HERE/fixture.macho"

for t in macho9 patch_macho add_version_min change_dylib; do
    [ -x "$BIN/$t" ] || { echo "known-callers: $BIN/$t not found or not executable" >&2; exit 1; }
done
[ -r "$FIXTURE" ] || { echo "known-callers: $FIXTURE missing" >&2; exit 1; }

T=$(mktemp -d "${TMPDIR:-/tmp}/macho-known-callers.XXXXXX") || exit 1
trap 'rm -rf "$T"' EXIT INT TERM

pass=0; fail=0
ok()  { echo "PASS $1"; pass=$((pass + 1)); }
bad() { echo "FAIL $1: $2" >&2; fail=$((fail + 1)); }

sha() { shasum -a 256 < "$1" | cut -d' ' -f1; }

# ---- caller 1: install.sh's /usr/local/bin/claude wrapper ----------------
#
# Verbatim from the plan's "Exact invocation lines (quoted verbatim from
# source)", with $REAL/$T bound to a copy of the fixture. The `>/dev/null ||
# { ...; exit 1; }` shape is reproduced too, because the exit code is what
# actually gates that wrapper's control flow.
INSTALLSH_SHA=ad12bdd780da4131f81a808e6d08b688e2034f37e434772f81df023332b39792

cp "$FIXTURE" "$T/REAL"
step1=0
"$BIN/patch_macho"     "$T/REAL" "$T/t" >/dev/null 2>"$T/e1" || step1=1
[ "$step1" -eq 0 ] && ok "install.sh: patch_macho exits 0" \
                   || bad "install.sh: patch_macho" "exit nonzero: $(cat "$T/e1")"

step2=0
"$BIN/add_version_min" "$T/t"            >/dev/null 2>"$T/e2" || step2=1
[ "$step2" -eq 0 ] && ok "install.sh: add_version_min exits 0" \
                   || bad "install.sh: add_version_min" "exit nonzero: $(cat "$T/e2")"

step3=0
"$BIN/change_dylib"    "$T/t" -strip-lc uuid -strip-lc codesig \
    -change "/usr/lib/libSystem.B.dylib"  "@loader_path/../S.dylib" \
    -change "/usr/lib/libicucore.A.dylib" "@loader_path/../I.dylib" \
    -change "/usr/lib/libc++.1.dylib"     "@loader_path/../c++.1.dylib" \
    >/dev/null 2>"$T/e3" || step3=1
[ "$step3" -eq 0 ] && ok "install.sh: change_dylib exits 0" \
                   || bad "install.sh: change_dylib" "exit nonzero: $(cat "$T/e3")"

got=$(sha "$T/t")
[ "$got" = "$INSTALLSH_SHA" ] \
    && ok "install.sh: the converted binary is byte-identical to the C tools' output" \
    || bad "install.sh: converted bytes" "sha256 $got, want $INSTALLSH_SHA"

# The teaching message is the whole point of phase one, and it must be on
# STDERR -- stdout is redirected to /dev/null by this very caller, so a
# message on stdout would vanish. One assertion per tool: the equivalent
# macho9 command line is in that tool's stderr.
grep -q 'macho9 declassify ' "$T/e1" \
    && ok "install.sh: patch_macho taught its macho9 equivalent on stderr" \
    || bad "install.sh: patch_macho stderr" "no macho9 equivalent: $(cat "$T/e1")"
grep -q 'macho9 minos ' "$T/e2" \
    && ok "install.sh: add_version_min taught its macho9 equivalent on stderr" \
    || bad "install.sh: add_version_min stderr" "no macho9 equivalent: $(cat "$T/e2")"
# This one invocation mixes families, so its equivalent is one `macho9 edit`
# with the operations as statements -- the command, and both kinds of
# statement it carries.
grep -q 'macho9 edit ' "$T/e3" && grep -q 'load-command delete uuid' "$T/e3" \
    && grep -q 'dylib replace' "$T/e3" \
    && ok "install.sh: change_dylib taught its macho9 equivalent on stderr" \
    || bad "install.sh: change_dylib stderr" "missing an equivalent: $(cat "$T/e3")"

# IDEMPOTENCY. install.sh's wrapper decides whether to run the pipeline at all
# by grepping the target binary's own bytes, and re-runs it on a binary that
# may already have been converted. patch_macho's pass-through is what makes
# that safe: an already-converted input comes out unchanged, exit 0.
rc=0
"$BIN/patch_macho" "$T/t" "$T/t2" >"$T/o2" 2>/dev/null || rc=$?
[ "$rc" -eq 0 ] && cmp -s "$T/t" "$T/t2" \
    && ok "install.sh: patch_macho passes an already-converted binary through unchanged" \
    || bad "install.sh: patch_macho idempotency" "exit $rc, or the output differs from the input"

# ...and it says so with md_declassify's own line and NOTHING else. The C tool
# never named the file it wrote on this path; `macho9 declassify` does, and
# compat/patch_macho.sh drops that line again. This is the one stdout
# difference the wrappers actively close, so it gets its own assertion.
grep -q '^Already patched' "$T/o2" \
    && ok "install.sh: the pass-through reports itself" \
    || bad "install.sh: pass-through stdout" "no 'Already patched' line: $(cat "$T/o2")"
grep -q '^Wrote ' "$T/o2" \
    && bad "install.sh: pass-through stdout" "printed a 'Wrote ...' line the C tool never printed" \
    || ok "install.sh: the pass-through does not name the file it wrote, as before"

# ---- caller 2: mf-wrapper-rebase.sh -------------------------------------
#
# The repo owner's local rebase of the same wrapper, whose distinguishing
# feature is `-insert`: an LC_LOAD_DYLIB placed BEFORE every existing one, so
# dyld loads and initializes libavxemu.dylib first. -insert is also the one
# operation whose ordinal renumbering has historically shipped loader-crashing
# bugs, so getting it through the wrapper unchanged matters.
ONGOING_SHA=bd4b56d63514beab983f2cdf2be7666797456314776bf5466c47ed4f18d3dede

cp "$FIXTURE" "$T/b"
rc=0
{ "$BIN/patch_macho" "$T/b" "$T/b1" >/dev/null 2>&1 &&
  "$BIN/add_version_min" "$T/b1"    >/dev/null 2>&1 &&
  "$BIN/change_dylib" "$T/b1" -strip-lc uuid -strip-lc codesig \
      -change "/usr/lib/libSystem.B.dylib" "@loader_path/../S.dylib" \
      -insert "@loader_path/libavxemu.dylib" >/dev/null 2>&1; } || rc=$?
got=$(sha "$T/b1" 2>/dev/null || echo none)
[ "$rc" -eq 0 ] && [ "$got" = "$ONGOING_SHA" ] \
    && ok "mf-wrapper-rebase.sh: the -insert pipeline is byte-identical to the C tools' output" \
    || bad "mf-wrapper-rebase.sh" "exit $rc, sha256 $got, want $ONGOING_SHA"

# ---- caller 3: magic-trackpad2's recorded entries ------------------------
#
# Two patch_macho runs from the same input, then add_version_min, then a
# change_dylib with THREE -change flags and no -strip-lc -- which translates
# to a SINGLE macho9 command, so this replay covers the wrapper's non-sequence
# path where caller 1 covers the sequence path.
TRACKPAD_SHA=df2b12fe08ada595b71063ee6c6ab6821c3266e4f68579bb4a14698e466b34cd
TRACKPAD_C1_SHA=b355358e586e4828a2dbafb349f1220f1985e074e1fdc223dc0d4fe6a23f878f

cp "$FIXTURE" "$T/c"
rc=0
{ "$BIN/patch_macho" "$T/c" "$T/c1" >/dev/null 2>&1 &&
  "$BIN/patch_macho" "$T/c" "$T/c2" >/dev/null 2>&1 &&
  "$BIN/add_version_min" "$T/c2"    >/dev/null 2>&1 &&
  "$BIN/change_dylib" "$T/c2" \
      -change /usr/lib/libSystem.B.dylib  /usr/lib/sysW.dylib \
      -change /usr/lib/libicucore.A.dylib /usr/lib/icuW.dylib \
      -change /usr/lib/libc++.1.dylib     /usr/lib/cxx.dylib >/dev/null 2>&1; } || rc=$?
got=$(sha "$T/c2" 2>/dev/null || echo none)
got1=$(sha "$T/c1" 2>/dev/null || echo none)
[ "$rc" -eq 0 ] && [ "$got" = "$TRACKPAD_SHA" ] && [ "$got1" = "$TRACKPAD_C1_SHA" ] \
    && ok "magic-trackpad2: both outputs are byte-identical to the C tools'" \
    || bad "magic-trackpad2" "exit $rc, sha256 $got (want $TRACKPAD_SHA), first output $got1"

# ---- the failure behaviour these callers depend on ----------------------
#
# Every one of them aborts on a nonzero exit, and every one of them runs the
# tools against a file it intends to install. So the case that matters most
# after "it works" is what a REFUSAL leaves behind.
#
# `-strip-lc uuid -delete <a dylib something still binds to>` is that case,
# and tests/compat-sweep.sh measured it as a regression when it is run as a
# raw sequence: the C tool refused ATOMICALLY, while `macho9 lc` followed by
# `macho9 dylib` refused only AFTER the first command had already rewritten
# the file (the matrix marks those rows "both-refuse+partial"). A mixed-family
# invocation is one `macho9 edit` now, which reads the image once, applies
# every statement to it in memory and writes once at the end -- so a refusal
# at any statement writes nothing. That is what this asserts, rather than
# only describing it.
cp "$FIXTURE" "$T/atom"
before=$(sha "$T/atom")
rc=0
"$BIN/change_dylib" "$T/atom" -strip-lc uuid \
    -delete /usr/lib/libSystem.B.dylib >/dev/null 2>"$T/eatom" || rc=$?
after=$(sha "$T/atom")
[ "$rc" -ne 0 ] \
    && ok "atomicity: a mixed-family invocation that must refuse still refuses (exit $rc)" \
    || bad "atomicity" "exited 0 on an invocation the C tool refused"
[ "$before" = "$after" ] \
    && ok "atomicity: the refusal left the caller's file byte-for-byte untouched" \
    || bad "atomicity" "the file was modified despite the refusal -- the sequence wrote before it failed"
grep -q 'still binds to the dylib being deleted' "$T/eatom" \
    && ok "atomicity: the refusal gives the same reason the C tool gave" \
    || bad "atomicity" "refused for some other reason: $(cat "$T/eatom")"

# ---- the production pipeline, on a path with a space --------------------
#
# install.sh's wrapper binds $REAL to the user's real Claude Code binary and $T
# to a temporary beside it, so a space in either is a user's directory name
# away, not a hypothetical. The whole pipeline is replayed on one, and must
# produce the same bytes as the ordinary run above -- which is also the
# end-to-end check on translate.sh's quoting, through both the verb form and
# the edit script's here-document.
mkdir -p "$T/dir with space"
cp "$FIXTURE" "$T/dir with space/REAL"
rc=0
{ "$BIN/patch_macho" "$T/dir with space/REAL" "$T/dir with space/t" >/dev/null 2>&1 &&
  "$BIN/add_version_min" "$T/dir with space/t"                      >/dev/null 2>&1 &&
  "$BIN/change_dylib"    "$T/dir with space/t" -strip-lc uuid -strip-lc codesig \
      -change "/usr/lib/libSystem.B.dylib"  "@loader_path/../S.dylib" \
      -change "/usr/lib/libicucore.A.dylib" "@loader_path/../I.dylib" \
      -change "/usr/lib/libc++.1.dylib"     "@loader_path/../c++.1.dylib" \
      >/dev/null 2>&1; } || rc=$?
got=$(sha "$T/dir with space/t" 2>/dev/null || echo none)
[ "$rc" -eq 0 ] && [ "$got" = "$INSTALLSH_SHA" ] \
    && ok "install.sh: the whole pipeline works on a path containing a space" \
    || bad "install.sh spaced path" "exit $rc, sha256 $got, want $INSTALLSH_SHA"
spaceleft=$(ls -a "$T/dir with space" | grep 'macho9-compat' || true)
[ -z "$spaceleft" ] \
    && ok "install.sh: and left no temp behind in that directory" \
    || bad "install.sh spaced path" "left behind: $spaceleft"

# And nothing may be left lying around next to the caller's file.
leftovers=$(ls -a "$T" | grep 'macho9-compat' || true)
[ -z "$leftovers" ] \
    && ok "atomicity: no temporary file left beside the caller's file" \
    || bad "atomicity" "left behind: $leftovers"

echo "known-callers: $pass passed, $fail failed"
[ "$fail" -eq 0 ] || exit 1
exit 0
